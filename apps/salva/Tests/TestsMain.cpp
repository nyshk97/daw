// salva_tests — CTest から実行するコンソールテスト（LaLaの daw_tests と同じ流儀）。
// GUIなしで動く判断ロジックとread-aheadソースの時間整合を検証する。
// テストは一時ディレクトリのみを使う。

#include <iostream>

#include <juce_audio_formats/juce_audio_formats.h>

#include "audio/RegionExport.h"
#include "audio/SalvaRecorder.h"
#include "shared/BpmMath.h"
#include "shared/DiscontinuityGuard.h"
#include "shared/ReadAheadStream.h"
#include "shared/RecordingCheck.h"
#include "shared/ResampleStage.h"
#include "shared/SalvaSettings.h"
#include "shared/StemCache.h"
#include "shared/StemMix.h"

namespace
{
int failureCount = 0;
juce::String currentTest;

void expect (bool condition, const char* description)
{
    if (! condition)
    {
        ++failureCount;
        std::cout << "FAIL [" << currentTest << "] " << description << std::endl;
    }
}

void beginTest (const char* name)
{
    currentTest = juce::String::fromUTF8 (name); // 日本語テスト名（char*直代入はassertする）
    std::cout << "-- " << name << std::endl;
}

// ==== fixture: 値がそのまま絶対サンプル位置を示す32bit float WAV ====
// L[i] = i / 131072（i < 131072 なら float で正確に表現できる）、R[i] = 1 - L[i]。
// 値の一致＝サンプル精度の位置一致、として検証できる
constexpr juce::int64 fixtureLength = 100000;
constexpr double fixtureSampleRate = 44100.0;

float valueAt (juce::int64 i) { return (float) i * (1.0f / 131072.0f); }

juce::File writeFixture (const juce::File& dir)
{
    const auto file = dir.getChildFile ("fixture.wav");
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    using Opts = juce::AudioFormatWriterOptions;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
        stream,
        Opts {}.withSampleRate (fixtureSampleRate).withNumChannels (2).withBitsPerSample (32)
            .withSampleFormat (Opts::SampleFormat::floatingPoint)));
    juce::AudioBuffer<float> buf (2, (int) fixtureLength);
    for (int i = 0; i < (int) fixtureLength; ++i)
    {
        buf.setSample (0, i, valueAt (i));
        buf.setSample (1, i, 1.0f - valueAt (i));
    }
    writer->writeFromAudioSampleBuffer (buf, 0, (int) fixtureLength);
    return file;
}

// リングが受け入れる限り埋める（テストはライタースレッドを立てず同期で駆動する）
void fillFully (ReadAheadStream& stream, juce::AudioFormatReader& reader)
{
    while (stream.fillOnce (reader) > 0) {}
}

struct StereoOut
{
    std::vector<float> l, r;
    explicit StereoOut (int n) : l ((size_t) n, -999.0f), r ((size_t) n, -999.0f) {}
};

StereoOut readSamples (ReadAheadStream& stream, int n)
{
    StereoOut out (n);
    stream.readAudio (out.l.data(), out.r.data(), n);
    return out;
}

// fixtureは32bit floatなので厳密一致で検証する（意図的な==。警告抑止のためexactlyEqual経由）
bool feq (float a, float b) { return juce::exactlyEqual (a, b); }

// Goertzel: 信号中の特定周波数成分の振幅を測る（エイリアス検出用）
double goertzelAmplitude (const float* samples, int numSamples, double frequency, double sampleRate)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * frequency / sampleRate;
    const double coeff = 2.0 * std::cos (w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < numSamples; ++i)
    {
        s0 = samples[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * std::sqrt (juce::jmax (0.0, power)) / numSamples;
}

// ResampleStageへ「ソースSRの正弦波」を流し込み、出力を回収する
std::vector<float> runResample (ResampleStage& stage, double ratio, double toneHz, double sourceRate, int numOut)
{
    juce::int64 sourcePos = 0;
    std::vector<float> out ((size_t) numOut, 0.0f);
    // 実機のコールバックに合わせて512サンプルずつ生成する
    for (int done = 0; done < numOut; done += 512)
    {
        const int n = juce::jmin (512, numOut - done);
        stage.process (ratio,
                       [&sourcePos, toneHz, sourceRate] (float* l, float* r, int count)
                       {
                           for (int i = 0; i < count; ++i)
                           {
                               const auto v = (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                                * toneHz * (double) (sourcePos + i) / sourceRate);
                               l[i] = v;
                               r[i] = v;
                           }
                           sourcePos += count;
                       },
                       out.data() + done, out.data() + done, n);
    }
    return out;
}

// 定数値のステレオ24bit WAV（ステムfixture用）
juce::File writeConstWav (const juce::File& file, float value, int numSamples, double sr)
{
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    using Opts = juce::AudioFormatWriterOptions;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
        stream, Opts {}.withSampleRate (sr).withNumChannels (2).withBitsPerSample (24)));
    juce::AudioBuffer<float> buf (2, numSamples);
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::fill (buf.getWritePointer (ch), value, numSamples);
    writer->writeFromAudioSampleBuffer (buf, 0, numSamples);
    return file;
}

// テスト用のidentityディレクトリ＋manifest＋ステムWAVを組み立てる
juce::File makeStemFixture (const juce::File& root, const juce::String& hash,
                            const StemCache::SourceIdentity& identity, const juce::String& runId,
                            const juce::String& status, int numGroups, double sr, int lengthSamples)
{
    const auto dir = root.getChildFile (hash);
    juce::Array<juce::var> groups;
    const char* groupIds[] = { "htdemucs", "htdemucs_6s" };
    const char* stemNames[] = { "drums", "bass" };
    for (int g = 0; g < numGroups; ++g)
    {
        juce::Array<juce::var> stems;
        for (int s = 0; s < 2; ++s)
        {
            const auto rel = "runs/" + runId + "/" + groupIds[g] + "/" + stemNames[s] + ".wav";
            writeConstWav (dir.getChildFile (rel), 0.1f * (float) (s + 1), lengthSamples, sr);
            auto* stem = new juce::DynamicObject();
            stem->setProperty ("name", stemNames[s]);
            stem->setProperty ("file", rel);
            stems.add (juce::var (stem));
        }
        auto* group = new juce::DynamicObject();
        group->setProperty ("id", groupIds[g]);
        group->setProperty ("name", juce::String (g == 0 ? 4 : 6) + juce::String::fromUTF8 (u8"ステム"));
        group->setProperty ("stems", stems);
        groups.add (juce::var (group));
    }
    auto* src = new juce::DynamicObject();
    src->setProperty ("path", identity.path);
    src->setProperty ("size", identity.size);
    src->setProperty ("mtimeMs", identity.mtimeMs);
    auto* m = new juce::DynamicObject();
    m->setProperty ("contractVersion", StemCache::contractVersion);
    m->setProperty ("model", "test");
    m->setProperty ("source", juce::var (src));
    m->setProperty ("sampleRate", sr);
    m->setProperty ("lengthSamples", lengthSamples);
    m->setProperty ("status", status);
    m->setProperty ("run", "runs/" + runId);
    m->setProperty ("groups", groups);
    dir.getChildFile ("manifest.json").replaceWithText (juce::JSON::toString (juce::var (m)));
    return dir;
}
} // namespace

// 異常終了耐性テストの子プロセスモード: 実時間ペースで録音し続ける（親がSIGKILLする）
static int recordChildMain (const juce::File& outFile)
{
    constexpr double sr = 48000.0;
    SalvaRecorder recorder;
    if (! recorder.start (outFile, sr))
        return 1;
    std::vector<float> l (4800), r (4800); // 0.1秒ぶん
    for (int i = 0; i < 4800; ++i)
        l[(size_t) i] = r[(size_t) i] = 0.5f;
    const float* channels[2] = { l.data(), r.data() };
    const auto heartbeat = outFile.getSiblingFile (outFile.getFileNameWithoutExtension() + ".attempted");
    for (int block = 0; block < 600; ++block) // 最大60秒（親のkillで死ぬ想定）
    {
        recorder.pushBlock (channels, 2, 4800);
        // 親が損失量を検算できるよう、書こうとした総数を随時残す（kill後も直前値が読める）
        heartbeat.replaceWithText (juce::String (recorder.attemptedSamples()));
        juce::Thread::sleep (100);
    }
    return 0;
}

int main (int argc, char* argv[])
{
    if (argc == 3 && juce::String (argv[1]) == "--record-child")
        return recordChildMain (juce::File (juce::String (argv[2])));

    std::cout << "salva_tests" << std::endl;

    const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("salva_tests_" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    tempDir.createDirectory();

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    const auto fixture = writeFixture (tempDir);

    // ==================== fixtureの健全性 ====================
    {
        beginTest ("fixture: 32bit float WAVが正確な値でラウンドトリップする");
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (fixture));
        expect (reader != nullptr, "reader created");
        expect (reader->lengthInSamples == fixtureLength, "length");
        juce::AudioBuffer<float> buf (2, 10);
        reader->read (&buf, 0, 10, 12345, true, true);
        expect (feq (buf.getSample (0, 0), valueAt (12345)), "L exact");
        expect (feq (buf.getSample (1, 0), 1.0f - valueAt (12345)), "R exact");
    }

    // ==================== 連続読み・区間境界のサンプル精度 ====================
    {
        beginTest ("read-ahead: 連続読みが絶対位置と一致する");
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (fixture));
        ReadAheadStream stream;
        stream.prepare (fixtureLength);
        fillFully (stream, *reader);

        const int n = 10000;
        const auto out = readSamples (stream, n);
        bool ok = true;
        for (int i = 0; i < n; ++i)
            ok = ok && feq (out.l[(size_t) i], valueAt (i)) && feq (out.r[(size_t) i], 1.0f - valueAt (i));
        expect (ok, "先頭10000サンプルが位置どおり");
        expect (stream.starvedSamples() == 0, "枯渇なし");
        expect (stream.playheadPosition() == n, "playhead");
    }

    // ==================== ループ折り返しの連続性（先読みでギャップゼロ） ====================
    {
        beginTest ("read-ahead: ループ端で隙間なく折り返す");
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (fixture));
        ReadAheadStream stream;
        stream.prepare (fixtureLength);
        const juce::int64 ls = 1000, le = 5000; // 4000サンプルのループ
        stream.setLoop (ls, le, true);
        stream.requestSeek (ls);
        fillFully (stream, *reader);

        const int n = 12000; // ループ3周分
        const auto out = readSamples (stream, n);
        bool ok = true;
        for (int i = 0; i < n; ++i)
        {
            const auto expected = ls + (juce::int64) i % (le - ls);
            ok = ok && feq (out.l[(size_t) i], valueAt (expected));
        }
        expect (ok, "3周ぶん全サンプルが折り返し位置どおり（無音の混入なし）");
        expect (stream.starvedSamples() == 0, "折り返しで枯渇していない");
        // 境界の直接確認: 折り返し直前=le-1、直後=ls
        expect (feq (out.l[3999], valueAt (le - 1)), "折り返し直前サンプル");
        expect (feq (out.l[4000], valueAt (ls)), "折り返し直後サンプル");
    }

    // ==================== 枯渇: 無音＋カウンタ、復帰後の時間整合 ====================
    {
        beginTest ("read-ahead: 枯渇は無音＋カウンタ、復帰後も絶対位置がずれない");
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (fixture));
        ReadAheadStream stream;
        stream.prepare (fixtureLength);
        // 埋めずに読む → 全部無音
        const int starvedN = 5000;
        const auto silent = readSamples (stream, starvedN);
        bool allZero = true;
        for (const auto v : silent.l)
            allZero = allZero && feq (v, 0.0f);
        expect (allZero, "枯渇区間は無音");
        expect (stream.starvedSamples() == (juce::uint64) starvedN, "枯渇カウンタ加算");
        expect (stream.playheadPosition() == starvedN, "枯渇中も位置は進む（時間を保つ）");

        // ライターが遅れて追いつく（位置0からのブロックは「過去」なので破棄されるべき）
        fillFully (stream, *reader);
        const int n = 4000;
        const auto out = readSamples (stream, n);
        bool ok = true;
        for (int i = 0; i < n; ++i)
            ok = ok && feq (out.l[(size_t) i], valueAt (starvedN + i));
        expect (ok, "復帰後のサンプルが絶対位置どおり（遅延ブロックの過去部分を再生していない）");
        expect (stream.starvedSamples() == (juce::uint64) starvedN, "復帰後は枯渇が増えない");
    }

    // ==================== シーク: generationによる旧データ無効化 ====================
    {
        beginTest ("read-ahead: シークで旧世代データが無効になる");
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (fixture));
        ReadAheadStream stream;
        stream.prepare (fixtureLength);
        fillFully (stream, *reader); // 位置0からの旧世代データでリングが満杯

        const juce::int64 target = 50000;
        stream.requestSeek (target);
        expect (stream.uiPosition() == target, "未適用シークはuiPositionに即反映");

        // ライターを回さず読む → 旧世代は破棄され無音（旧データが鳴らないことの検証）
        const int n1 = 1000;
        const auto out1 = readSamples (stream, n1);
        bool noOldData = true;
        for (const auto v : out1.l)
            noOldData = noOldData && feq (v, 0.0f);
        expect (noOldData, "シーク直後に旧世代データを再生しない");

        // ライターが新世代を書く → シーク先の続きから正しい値
        fillFully (stream, *reader);
        const int n2 = 4000;
        const auto out2 = readSamples (stream, n2);
        bool ok = true;
        for (int i = 0; i < n2; ++i)
            ok = ok && feq (out2.l[(size_t) i], valueAt (target + n1 + i));
        expect (ok, "シーク適用後は新しい位置の続きから読める");
    }

    // ==================== 非ループ終端 ====================
    {
        beginTest ("read-ahead: 非ループの終端で無音＋endフラグ（枯渇に数えない）");
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (fixture));
        ReadAheadStream stream;
        stream.prepare (fixtureLength);
        stream.requestSeek (fixtureLength - 100);
        fillFully (stream, *reader);
        const auto out = readSamples (stream, 300);
        expect (feq (out.l[99], valueAt (fixtureLength - 1)), "最終サンプルまで読める");
        expect (feq (out.l[100], 0.0f) && feq (out.l[299], 0.0f), "終端以降は無音");
        expect (stream.reachedEnd(), "endフラグが立つ");
        expect (stream.starvedSamples() == 0, "終端の無音は枯渇に数えない");
    }

    // ==================== BPM逆算 ====================
    {
        beginTest ("BPM逆算: 候補選択・境界・最近接フォールバック");
        // 10.43秒: 16拍で92.0 BPM（範囲内は高々1つ）
        expect (BpmMath::autoBeats (10.43) == 16, "10.43s -> 16拍");
        expect (BpmMath::bpmDisplayText (16, 10.43) == "92.0", "小数1桁表示");
        // 境界ちょうど: 4拍で110.0 / 70.0
        expect (BpmMath::autoBeats (4.0 / 110.0 * 60.0) == 4, "110ちょうどは範囲内");
        expect (BpmMath::autoBeats (4.0 / 70.0 * 60.0) == 4, "70ちょうどは範囲内");
        // 範囲外: 最近接の候補
        expect (BpmMath::autoBeats (1.0) == 4, "1s -> 全候補が高すぎ、最も近い4拍");
        expect (BpmMath::autoBeats (60.0) == 64, "60s -> 64拍(64bpm)が範囲に最近接");
        // 同距離タイ（4拍=60bpm と 8拍=120bpm が範囲から等距離）は先勝ち=低いBPM側
        expect (BpmMath::autoBeats (4.0) == 4, "等距離タイは低BPM側");
        // 候補循環
        expect (BpmMath::nextBeats (4) == 8 && BpmMath::nextBeats (64) == 4, "候補の循環");
    }

    // ==================== 書き出しファイル名 ====================
    {
        beginTest ("ファイル名生成: <base>_<bars>bars_<bpm>bpm.wav");
        expect (BpmMath::exportFileName ("side-a", 16, 10.43) == "side-a_4bars_92bpm.wav",
                "四捨五入の整数bpm");
        expect (BpmMath::exportFileName ("rec", 8, 5.0) == "rec_2bars_96bpm.wav", "2小節");
        expect (BpmMath::exportFileName ("x", 4, 2.6) == "x_1bars_92bpm.wav", "92.3->92");
    }

    // ==================== 設定の保存・読込 ====================
    {
        beginTest ("SalvaSettings: JSONラウンドトリップと最近ファイルの重複・上限");
        const auto file = tempDir.getChildFile ("settings.json");
        SalvaSettings s;
        s.outputDeviceName = "MOTU UltraLite mk5";
        s.inputChannelPairStart = 2;
        s.exportDirectory = "/tmp/out";
        for (int i = 0; i < 12; ++i)
            s.addRecentFile ("/path/file" + juce::String (i) + ".wav");
        s.addRecentFile ("/path/file5.wav"); // 重複 → 先頭へ
        s.save (file);

        const auto loaded = SalvaSettings::load (file);
        expect (loaded.outputDeviceName == s.outputDeviceName, "outputDevice");
        expect (loaded.inputChannelPairStart == 2, "inputChannelPairStart");
        expect (loaded.exportDirectory == "/tmp/out", "exportDirectory");
        expect (loaded.recentFiles.size() == SalvaSettings::maxRecentFiles, "上限8件");
        expect (loaded.recentFiles[0] == "/path/file5.wav", "重複は先頭へ移動");
        expect (! loaded.recentFiles.contains ("/path/file0.wav"), "古いものから落ちる");

        const auto empty = SalvaSettings::load (tempDir.getChildFile ("missing.json"));
        expect (empty.outputDeviceName.isEmpty() && empty.recentFiles.isEmpty(), "ファイル無しは既定値");
        expect (empty.inputChannelPairStart == 2, "既定の入力ペアは3-4ch（mk5背面Line In想定）");
    }

    // ==================== 録音: WAVフォーマットと内容 ====================
    {
        beginTest ("録音: 24bitステレオ・デバイスSR・全サンプルが書かれる");
        const auto file = tempDir.getChildFile ("rec-format.wav");
        constexpr double sr = 48000.0;
        SalvaRecorder recorder;
        expect (recorder.start (file, sr), "start成功");

        // 既知値（小さいランプ）を1秒ぶんプッシュ
        constexpr int total = 48000, block = 4800;
        std::vector<float> l (block), r (block);
        for (int b = 0; b < total / block; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                const auto v = (float) (b * block + i) / (float) total * 0.5f;
                l[(size_t) i] = v;
                r[(size_t) i] = -v;
            }
            const float* channels[2] = { l.data(), r.data() };
            recorder.pushBlock (channels, 2, block);
            // 実際の入力は実時間ペースで来る。テストは一瞬で1秒分を積むため、
            // FIFO(1秒容量)を追い越さないよう背景スレッドのドレインを待つ
            juce::Thread::sleep (10);
        }
        expect (recorder.attemptedSamples() == total, "attemptedが総数と一致");
        expect (recorder.droppedWrites() == 0, "このペースではドロップしない");
        const auto actual = recorder.stop();
        expect (actual == total, "実WAV長が総数と一致");
        expect (RecordingCheck::mismatchWarning (total, actual, sr).isEmpty(), "一致なら警告なし");

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        expect (reader != nullptr, "確定WAVが開ける");
        expect ((int) reader->bitsPerSample == 24, "24bit");
        expect ((int) reader->numChannels == 2, "ステレオ");
        expect (juce::exactlyEqual (reader->sampleRate, sr), "デバイスSR");
        // 内容の照合（24bit量子化の誤差込み）
        juce::AudioBuffer<float> buf (2, 100);
        reader->read (&buf, 0, 100, 24000, true, true);
        bool ok = true;
        for (int i = 0; i < 100; ++i)
        {
            const auto expected = (float) (24000 + i) / (float) total * 0.5f;
            ok = ok && std::abs (buf.getSample (0, i) - expected) < 1.0e-6f
                 && std::abs (buf.getSample (1, i) + expected) < 1.0e-6f;
        }
        expect (ok, "書いた値が読み戻せる（24bit精度内）");
    }

    // ==================== 録音: サンプル数不一致の検出 ====================
    {
        beginTest ("録音: 欠落の注入で警告経路が動く");
        const auto warning = RecordingCheck::mismatchWarning (48000, 47000, 48000.0);
        expect (warning.isNotEmpty(), "欠落があれば警告");
        expect (warning.contains (juce::String::fromUTF8 (u8"秒")), "欠落秒数を含む");
        expect (RecordingCheck::mismatchWarning (48000, 48000, 48000.0).isEmpty(), "一致は警告なし");
        expect (RecordingCheck::mismatchWarning (48000, 48123, 48000.0).isEmpty(), "actualが多い側は警告しない");
    }

    // ==================== 録音: 異常終了の耐性（SIGKILL後もWAVが有効） ====================
    {
        beginTest ("録音: プロセスkill後のWAVが開けて損失が2秒以内");
        const auto wav = tempDir.getChildFile ("rec-kill.wav");
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        juce::ChildProcess child;
        expect (child.start (juce::StringArray { exe.getFullPathName(), "--record-child", wav.getFullPathName() }),
                "子プロセス起動");
        juce::Thread::sleep (4000); // 実時間ペースで約4秒録音させる
        expect (child.kill(), "SIGKILL"); // ヘッダ確定処理は走らない＝クラッシュ相当
        juce::Thread::sleep (300);

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (wav));
        expect (reader != nullptr, "kill後のWAVが開ける（定期flushでヘッダが有効）");
        if (reader != nullptr)
        {
            const auto heartbeat = wav.getSiblingFile ("rec-kill.attempted");
            const auto attempted = heartbeat.loadFileAsString().getLargeIntValue();
            expect (attempted > 0, "heartbeatが残っている");
            const auto missing = attempted - reader->lengthInSamples;
            // 損失上限 ≒ flush間隔(1s)＋FIFO容量(1s)。heartbeatのラグぶん0.3秒の余裕を足す
            expect (missing <= (juce::int64) (2.3 * 48000.0),
                    "損失がflush間隔＋FIFO容量相当（2秒+余裕）以内");
            expect (reader->lengthInSamples > 0, "flush済みデータが残っている");
        }
    }

    // ==================== ステム: M/S競合規則 ====================
    {
        beginTest ("StemMix: Solo集合＋Mute優先");
        using V = std::vector<bool>;
        // Soloなし: Muteだけが効く
        expect (StemMix::audible (V { false, true, false }, V { false, false, false }) == (V { true, false, true }),
                "Soloなし時のMute");
        // Solo1つ: その集合のみ
        expect (StemMix::audible (V { false, false, false }, V { false, true, false }) == (V { false, true, false }),
                "単独Solo");
        // 複数Soloは加算
        expect (StemMix::audible (V { false, false, false }, V { true, false, true }) == (V { true, false, true }),
                "複数Solo加算");
        // MuteはSoloより優先（Solo中でもMuteされていれば鳴らない）
        expect (StemMix::audible (V { true, false, false }, V { true, true, false }) == (V { false, true, false }),
                "Solo中Muteの消音");
    }

    // ==================== ステム: manifest検証（stale/別ファイルは無視） ====================
    {
        beginTest ("StemCache: manifestの検証とグループ数");
        const auto root = tempDir.getChildFile ("stems-manifest");
        const StemCache::SourceIdentity id { "/tmp/a.wav", 1000, 123456 };
        constexpr double sr = 44100.0;

        // 正常（2群）
        const auto dir = makeStemFixture (root, "aaaa", id, "run1", "complete", 2, sr, 100);
        auto manifest = StemCache::parseManifest (dir.getChildFile ("manifest.json"));
        expect (manifest.valid, "parse成功");
        expect ((int) manifest.groups.size() == 2, "2群");
        expect (StemCache::manifestUsable (manifest, id, dir), "identity一致・completeで使用可");

        // 1群のみ（モデルラボでBS-RoFormer単独が勝っても矛盾しない分岐）
        const auto dir1 = makeStemFixture (root, "bbbb", id, "run1", "complete", 1, sr, 100);
        const auto m1 = StemCache::parseManifest (dir1.getChildFile ("manifest.json"));
        expect (m1.valid && (int) m1.groups.size() == 1, "1群でもvalid（UIは切替を隠す）");

        // identity不一致（元音源の上書き相当）は無視
        const StemCache::SourceIdentity other { "/tmp/a.wav", 1000, 999999 };
        expect (! StemCache::manifestUsable (manifest, other, dir), "identity不一致は使用不可");

        // 未完了は無視
        const auto dirIncomplete = makeStemFixture (root, "cccc", id, "run1", "running", 2, sr, 100);
        const auto mi = StemCache::parseManifest (dirIncomplete.getChildFile ("manifest.json"));
        expect (! StemCache::manifestUsable (mi, id, dirIncomplete), "未完了は使用不可");

        // WAV欠損は無視
        dir.getChildFile ("runs/run1/htdemucs/drums.wav").deleteFile();
        expect (! StemCache::manifestUsable (manifest, id, dir), "ステムWAV欠損は使用不可");
    }

    // ==================== ステム: ロック競合と生存判定 ====================
    {
        beginTest ("StemCache: ロック競合・PID/PGID生存判定・未完成lockの保持");
        const auto root = tempDir.getChildFile ("stems-lock");
        const auto dir = root.getChildFile ("idhash");
        const auto now = juce::Time::getCurrentTime();
        const auto alivePid = [] (juce::int64 pid) { return pid == 100; };
        const auto alivePgid = [] (juce::int64 pgid) { return pgid == 200; };

        expect (StemCache::acquireLock (dir, 100), "初回取得");
        expect (! StemCache::acquireLock (dir, 101), "二重取得は失敗（ロック競合時の抑止）");

        // owner生存 → 生
        expect (StemCache::lockIsAlive (StemCache::readLock (dir), alivePid, alivePgid, now),
                "owner生存で生");

        // アプリ死亡・worker生存（PGIDメンバー残存）→ 生
        dir.getChildFile ("lock/owner.json").replaceWithText ("{\"pid\": 999}"); // 死んだowner
        dir.getChildFile ("lock/worker.json").replaceWithText ("{\"pid\": 998, \"pgid\": 200}");
        expect (StemCache::lockIsAlive (StemCache::readLock (dir), alivePid, alivePgid, now),
                "アプリ死亡でもworker PGIDにメンバーが残っていれば生");

        // worker側も死んでいる → 死
        dir.getChildFile ("lock/worker.json").replaceWithText ("{\"pid\": 998, \"pgid\": 999}");
        expect (! StemCache::lockIsAlive (StemCache::readLock (dir), alivePid, alivePgid, now),
                "owner・workerとも死で死");

        // 未完成lock（worker未記録）: 直近なら安全側で生、1時間より古ければ死
        dir.getChildFile ("lock/worker.json").deleteFile();
        expect (StemCache::lockIsAlive (StemCache::readLock (dir), alivePid, alivePgid, now),
                "未完成lockは安全側で保持");
        expect (! StemCache::lockIsAlive (StemCache::readLock (dir), alivePid, alivePgid,
                                          now + juce::RelativeTime::hours (2)),
                "古い未完成lockは解放対象");
    }

    // ==================== ステム: 起動時掃除と削除の排他 ====================
    {
        beginTest ("StemCache: 掃除が生きているロック/runを消さない・削除の排他");
        const auto root = tempDir.getChildFile ("stems-sweep");
        const auto now = juce::Time::getCurrentTime();
        const StemCache::SourceIdentity id { "/tmp/src.wav", 10, 20 };
        const auto alivePid = [] (juce::int64 pid) { return pid == 100; };
        const auto alivePgid = [] (juce::int64) { return false; };

        // A: 生きているロック＋中途run（分離実行中の想定）→ 触らない
        const auto dirA = makeStemFixture (root, "aliveid", id, "oldrun", "complete", 1, 44100.0, 50);
        dirA.getChildFile ("runs/newrun/htdemucs").createDirectory(); // 中途run
        StemCache::acquireLock (dirA, 100);

        // B: 死んだロック＋非参照run → 解放・削除される
        // （worker.jsonまで記録済み＝未完成lockの安全側保持ルールの対象外にする）
        const auto dirB = makeStemFixture (root, "deadid", id, "run1", "complete", 1, 44100.0, 50);
        dirB.getChildFile ("runs/orphan/htdemucs").createDirectory();
        StemCache::acquireLock (dirB, 999);
        dirB.getChildFile ("lock/worker.json").replaceWithText ("{\"pid\": 998, \"pgid\": 999}");

        StemCache::sweep (root, 12345, alivePid, alivePgid, now);

        expect (dirA.getChildFile ("lock").isDirectory(), "生きているロックは保持");
        expect (dirA.getChildFile ("runs/newrun").isDirectory(), "実行中identityの中途runは保持");
        expect (! dirB.getChildFile ("lock").isDirectory(), "死んだロックは解放");
        expect (! dirB.getChildFile ("runs/orphan").isDirectory(), "非参照・ロックなしの中途runは削除");
        expect (dirB.getChildFile ("runs/run1").isDirectory(), "manifest参照中のrunは保持");

        // 別プロセスが分離中（=生きているロック）に全キャッシュ削除 → そのidentityは残る
        const auto result = StemCache::deleteAll (root, 12345, alivePid, alivePgid, now);
        expect (result.skippedInUse == 1, "使用中はスキップ（件数表示）");
        expect (result.deleted == 1, "使用中でない側は削除");
        expect (dirA.isDirectory() && dirA.getChildFile ("runs/oldrun").isDirectory(),
                "分離中のlock/runが残る");
        expect (! dirB.exists(), "非使用中identityは削除");
    }

    // ==================== 書き出し: 聴こえている構成＝内容・クリップ収め・失敗時 ====================
    {
        beginTest ("書き出し: ミックス一致・SR/24bit・クリップゲイン・途中失敗");
        const auto dir = tempDir.getChildFile ("export");
        constexpr double sr = 48000.0;
        constexpr int len = 4800;
        const auto stemA = writeConstWav (dir.getChildFile ("a.wav"), 0.25f, len, sr);
        const auto stemB = writeConstWav (dir.getChildFile ("b.wav"), 0.50f, len, sr);

        // 聴こえている構成（A+B）のミックスが書き出し内容と一致
        const auto out = dir.getChildFile ("mix.wav");
        auto result = RegionExport::renderMix ({ { stemA, 1.0f }, { stemB, 1.0f } }, 1000, 3000, sr, out);
        expect (result.ok, "書き出し成功");
        expect (feq (result.appliedGain, 1.0f), "0dBFS未満は等倍");
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (out));
            expect (reader != nullptr && (int) reader->bitsPerSample == 24
                        && juce::exactlyEqual (reader->sampleRate, sr) && (int) reader->numChannels == 2,
                    "24bit・元SR・ステレオ");
            expect (reader->lengthInSamples == 2000, "区間長");
            juce::AudioBuffer<float> buf (2, 10);
            reader->read (&buf, 0, 10, 500, true, true);
            expect (std::abs (buf.getSample (0, 0) - 0.75f) < 1.0e-5f, "A+Bの加算値（聴こえている構成＝内容）");
        }

        // Solo相当（Aのみ）は値が変わる
        const auto outSolo = dir.getChildFile ("solo.wav");
        RegionExport::renderMix ({ { stemA, 1.0f } }, 1000, 3000, sr, outSolo);
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (outSolo));
            juce::AudioBuffer<float> buf (2, 4);
            reader->read (&buf, 0, 4, 0, true, true);
            expect (std::abs (buf.getSample (0, 0) - 0.25f) < 1.0e-5f, "Soloの構成が反映される");
        }

        // クリップ: 0.8+0.8=1.6 → 全体を1/1.6に下げて収める
        const auto hotA = writeConstWav (dir.getChildFile ("hotA.wav"), 0.8f, len, sr);
        const auto hotB = writeConstWav (dir.getChildFile ("hotB.wav"), 0.8f, len, sr);
        const auto outHot = dir.getChildFile ("hot.wav");
        result = RegionExport::renderMix ({ { hotA, 1.0f }, { hotB, 1.0f } }, 0, len, sr, outHot);
        expect (result.ok && result.appliedGain < 1.0f, "クリップ時はゲインを下げる");
        expect (std::abs (result.appliedGain - 1.0f / 1.6f) < 1.0e-4f, "ちょうど収まる倍率");
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (outHot));
            juce::AudioBuffer<float> buf (2, 4);
            reader->read (&buf, 0, 4, 100, true, true);
            expect (buf.getSample (0, 0) <= 1.0f && buf.getSample (0, 0) > 0.99f, "ピークが0dBFSに収まる");
        }

        // 途中失敗（書けない出力先）で壊れた成果物・一時ファイルを残さない
        const auto badOut = dir.getChildFile ("no-such-dir/x.wav");
        result = RegionExport::renderMix ({ { stemA, 1.0f } }, 0, len, sr, badOut);
        expect (! result.ok && result.error.isNotEmpty(), "失敗を報告");
        expect (! badOut.exists(), "成果物が残らない");
        expect (dir.getChildFile ("no-such-dir").findChildFiles (juce::File::findFiles, true).isEmpty(),
                "一時ファイルも残らない");
    }

    // ==================== リサンプル: 帯域制限（アンチエイリアス）と不連続リセット ====================
    {
        beginTest ("ResampleStage: ダウンサンプリングの折り返し抑圧");
        // 96kHz→48kHz（比率2.0）。96k側の36kHz正弦波は無対策だと48k出力の12kHzへ折り返す
        constexpr double srcRate = 96000.0, ratio = 2.0;
        constexpr int numOut = 48000;
        ResampleStage stage;
        stage.prepare (1024, 8.0);

        const auto aliased = runResample (stage, ratio, 36000.0, srcRate, numOut);
        const auto alias12k = goertzelAmplitude (aliased.data() + 1000, numOut - 1000, 12000.0, 48000.0);

        stage.reset();
        const auto highAlias = runResample (stage, ratio, 44000.0, srcRate, numOut);
        const auto alias4k = goertzelAmplitude (highAlias.data() + 1000, numOut - 1000, 4000.0, 48000.0);

        stage.reset();
        const auto passband = runResample (stage, ratio, 5000.0, srcRate, numOut);
        const auto pass5k = goertzelAmplitude (passband.data() + 1000, numOut - 1000, 5000.0, 48000.0);

        expect (pass5k > 0.85, "通過帯域（5kHz）はほぼ素通し");
        // ローパス無しだとLagrange単体の減衰のみ（0.5超）で折り返す。2次Butterworthで大幅に沈む
        expect (alias12k < 0.25, "36kHz→12kHz折り返しが抑圧される");
        expect (alias4k < 0.10, "44kHz→4kHz折り返しは強く抑圧される（カットオフから遠い）");
    }

    {
        beginTest ("ResampleStage: アップサンプリングのイメージング抑圧");
        // 48kHz→96kHz（比率0.5）。10kHzは通り、48k±10kのイメージ（38kHz）は後段フィルタで沈む
        ResampleStage stage;
        stage.prepare (1024, 8.0);
        constexpr int numOut = 96000;
        const auto out = runResample (stage, 0.5, 10000.0, 48000.0, numOut);
        const auto pass10k = goertzelAmplitude (out.data() + 1000, numOut - 1000, 10000.0, 96000.0);
        const auto image38k = goertzelAmplitude (out.data() + 1000, numOut - 1000, 38000.0, 96000.0);
        expect (pass10k > 0.85, "通過帯域（10kHz）はほぼ素通し");
        expect (image38k < 0.2, "イメージング（38kHz）が抑圧される");
    }

    {
        beginTest ("ResampleStage: reset()で不連続前のサンプルが漏れない");
        // DC=1.0を流して内部状態を汚し、reset後にゼロ入力 → 出力は完全にゼロ
        ResampleStage stage;
        stage.prepare (1024, 8.0);
        std::vector<float> out (2000, -1.0f);
        auto feedDC = [] (float* l, float* r, int n)
        {
            juce::FloatVectorOperations::fill (l, 1.0f, n);
            if (r != l)
                juce::FloatVectorOperations::fill (r, 1.0f, n);
        };
        auto feedZero = [] (float* l, float* r, int n)
        {
            juce::FloatVectorOperations::clear (l, n);
            if (r != l)
                juce::FloatVectorOperations::clear (r, n);
        };
        stage.process (2.0, feedDC, out.data(), out.data(), 2000);

        // 対照: resetしないままゼロ入力 → キャリー・フィルタ履歴からDCが漏れる（テストの弁別力の証明）
        stage.process (2.0, feedZero, out.data(), out.data(), 64);
        float leak = 0.0f;
        for (int i = 0; i < 64; ++i)
            leak = juce::jmax (leak, std::abs (out[(size_t) i]));
        expect (leak > 0.0f, "reset無しでは旧サンプルが漏れる（対照）");

        // 本題: DCで汚し直してからreset → ゼロ入力の出力は厳密にゼロ
        stage.process (2.0, feedDC, out.data(), out.data(), 2000);
        stage.reset();
        stage.process (2.0, feedZero, out.data(), out.data(), 512);
        bool allZero = true;
        for (int i = 0; i < 512; ++i)
            allZero = allZero && feq (out[(size_t) i], 0.0f);
        expect (allZero, "reset後は旧位置のサンプルが混ざらない");
    }

    // ==================== read-ahead: ソース差し替え競合（世代上限ガード） ====================
    {
        beginTest ("read-ahead: 旧readerは自分の世代上限を超えるseekに応じない");
        // レビュー指摘の順序: writerが旧readerでfillしようとする直前に、
        // openFile（新reader保留）＋play（世代++）が走る。上限ガードが無いと
        // 旧ファイルのサンプルを新世代としてリングへ公開してしまう
        std::unique_ptr<juce::AudioFormatReader> oldReader (formatManager.createReaderFor (fixture));
        ReadAheadStream stream;
        stream.prepare (fixtureLength);
        // writerが「pending無し」を確認した時点のスナップショット
        const auto snapshot = stream.latestGeneration();
        fillFully (stream, *oldReader);
        readSamples (stream, 256); // 平常再生

        // UI: openFile→play相当（新readerはまだ採用されていないのに世代だけ進む）
        stream.requestSeek (70000);

        // 旧readerのfillはスナップショット上限で拒否される（採用もしない）
        expect (stream.fillOnce (*oldReader, snapshot) == 0, "上限超の世代には応じない");
        // リーダー側は新世代を採用し、旧世代ブロックを破棄 → 旧ファイルの音は出ない（無音）
        const auto out = readSamples (stream, 256);
        bool silent = true;
        for (const auto v : out.l)
            silent = silent && feq (v, 0.0f);
        expect (silent, "旧readerのデータが新世代として鳴らない");

        // 新readerの採用後（=新しい上限で）fillすれば、新しい位置から正しく出る
        std::unique_ptr<juce::AudioFormatReader> newReader (formatManager.createReaderFor (fixture));
        fillFully (stream, *newReader); // 引数なし版 = 最新世代を上限にする（採用後の状態）
        const auto out2 = readSamples (stream, 256);
        expect (feq (out2.l[0], valueAt (70000 + 256)), "採用後は新しい位置の続きから出る");
    }

    // ==================== リサンプル: 遷移epochと処理の重なり検出 ====================
    {
        beginTest ("DiscontinuityGuard: 遷移epochが処理と重なったブロックを取りこぼさない");
        DiscontinuityGuard guard;

        // 連続再生: version不変・active=0 → 何も起きない
        expect (! guard.preBlock (0, 0), "変化なしならpre-resetしない");
        expect (! guard.postBlock (0), "重なりなしなら何もしない");

        // 通常経路: 遷移がブロック間で完了 → pre-resetのみ（無音化なし）
        expect (guard.preBlock (1, 0), "完了済み遷移はpre-reset");
        expect (! guard.postBlock (1), "処理と重なっていなければミュート不要");

        // 処理中にbegin（レビュー指摘の順序: preBlockが旧versionを読んだ後に遷移開始）
        expect (! guard.preBlock (1, 0), "versionは旧値のまま");
        expect (guard.postBlock (2), "処理中にepochが進んだ＝競合ブロックとして無音化");
        // 次のブロック: versionの変化はpre-resetで消化。二重ミュートしない
        expect (guard.preBlock (2, 0), "遅れて見えたepochでpre-reset");
        expect (! guard.postBlock (2), "追加のミュートなし");

        // ブロック開始時点で遷移が進行中（active>0）→ resetもミュートも行う
        expect (guard.preBlock (3, 1), "進行中の遷移はpre-resetも行う");
        expect (guard.postBlock (3), "進行中に処理したブロックは無音化");
    }

    {
        beginTest ("DiscontinuityGuard: 複数ステムの途中で遷移した順序を検出する");
        // レビュー指摘の実順序をストリーム2本＋ガードで再現する:
        //   1. preBlockが旧epochを読む → 2. stem0を読む → 3. UIが全ステムをシーク（遷移）
        //   → 4. stem1を読む（新位置を採用）→ 一部だけ新位置のブロックができる
        std::unique_ptr<juce::AudioFormatReader> reader0 (formatManager.createReaderFor (fixture));
        std::unique_ptr<juce::AudioFormatReader> reader1 (formatManager.createReaderFor (fixture));
        ReadAheadStream stem0, stem1;
        stem0.prepare (fixtureLength);
        stem1.prepare (fixtureLength);
        fillFully (stem0, *reader0);
        fillFully (stem1, *reader1);

        DiscontinuityGuard guard;
        juce::uint32 version = 0;
        int active = 0;

        // 前のブロックで両ステムを位置0から読んでおく（平常状態）
        expect (! guard.preBlock (version, active), "平常");
        readSamples (stem0, 256);
        readSamples (stem1, 256);
        expect (! guard.postBlock (version), "平常ブロックはミュートなし");

        // 競合ブロック: stem0を読んだ**後**にUIの遷移（全ステムへseek）が走る
        expect (! guard.preBlock (version, active), "ブロック開始時点では遷移なし");
        const auto out0 = readSamples (stem0, 256); // 旧位置(256..)
        {
            ++active; ++version; // begin（エンジンのTransitionScope相当）
            stem0.requestSeek (50000);
            stem1.requestSeek (50000);
            --active;            // end
        }
        stem1.discardStale();
        fillFully (stem1, *reader1);
        const auto out1 = readSamples (stem1, 256); // 新位置(50000..)を採用
        // → ステムごとに違う位置の音が混ざっている（これが問題のブロック）
        expect (feq (out0.l[0], valueAt (256)), "stem0は旧位置");
        expect (feq (out1.l[0], valueAt (50000)), "stem1は新位置（不整合の実証）");
        // ガードはこのブロックを検出する（masterだけの監視では取りこぼす順序）
        expect (guard.postBlock (version), "途中遷移ブロックをepochで検出＝SRに関係なく無音化");
    }

    tempDir.deleteRecursively();

    if (failureCount > 0)
    {
        std::cout << failureCount << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "All tests passed" << std::endl;
    return 0;
}
