// daw_tests — CTest から実行するコンソールテスト。
// GUIなしで動くもの（データモデル・保存/読込・DLSMusicDeviceのオフラインレンダリング）だけを検証する。
// テストは一時ディレクトリのみを使い、~/Music/daw には一切触れない。

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "audio/AudioImporter.h"
#include "audio/AudioFilePreview.h"
#include "audio/BounceRenderer.h"
#include "audio/PlaybackEngine.h"
#include "audio/SamplerEngine.h"
#include "shared/Project.h"
#include "shared/SynthBank.h"
#include "shared/UndoStack.h"
#include "shared/AudioFileTypes.h"
#include "shared/AudioBrowserNavigation.h"
#include "shared/GainScale.h"
#include "ui/AppLookAndFeel.h"
#include "ui/BottomPanelHistory.h"
#include "ui/GainControls.h"
#include "ui/FileSortOrder.h"
#include "ui/PreviewPolicy.h"
#include "ui/ProjectThumbnails.h"
#include "ui/Shortcuts.h"

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
    currentTest = name;
    std::cout << "---- " << name << std::endl;
}

// 一時ディレクトリ（テストごとに一意）。~/Music/daw には一切触れない
juce::File makeTempDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("daw-tests-" + juce::Uuid().toString());
    dir.createDirectory();
    return dir;
}

// テスト用の小さなモノラルWAVを書く
bool writeTestWav (const juce::File& file, int numSamples)
{
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;

    using Opts = juce::AudioFormatWriterOptions;
    auto writer = wavFormat.createWriterFor (stream,
        Opts{}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer (1, numSamples);
    for (int i = 0; i < numSamples; ++i)
        buffer.setSample (0, i, std::sin ((float) i * 0.1f) * 0.5f);
    return writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
}

bool writeBufferWav (const juce::File& file, const juce::AudioBuffer<float>& buffer,
                     double sampleRate)
{
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;
    using Opts = juce::AudioFormatWriterOptions;
    auto writer = wavFormat.createWriterFor (stream,
        Opts{}.withSampleRate (sampleRate).withNumChannels (buffer.getNumChannels())
              .withBitsPerSample (24));
    return writer != nullptr
           && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

void testAudioFilePreview()
{
    beginTest ("Audio file browser filter and preview");
    expect (AudioFileTypes::isSupported ("/tmp/a.WAV"), "WAVを受理");
    expect (AudioFileTypes::isSupported ("/tmp/a.m4a"), "M4Aを受理");
    expect (! AudioFileTypes::isSupported ("/tmp/a.txt"), "非対応拡張子を拒否");
    expect (! AudioFileTypes::isSupported ("/tmp/.hidden"), "隠し拡張子なしを拒否");
    expect (AudioBrowserNavigation::initialDirectory().getFileName() == "Downloads",
            "初期位置はDownloads");
    AudioBrowserNavigation::History history;
    history.visit (juce::File ("/one"));
    history.visit (juce::File ("/two"));
    expect (history.canMove (-1) && ! history.canMove (1), "戻るだけが有効");
    expect (history.move (-1) == juce::File ("/one"), "戻る履歴");
    expect (history.canMove (1), "戻った後は進める");
    history.visit (juce::File ("/three"));
    expect (! history.canMove (1), "戻った後の新規移動で進む履歴を破棄");

    const auto dir = makeTempDir();
    const auto file = dir.getChildFile ("preview.wav");
    juce::AudioBuffer<float> source (2, 8);
    const float left[]  = { 0, 1, 0, -1, 0, 1, 0, -1 };
    const float right[] = { 1, 0, -1, 0, 1, 0, -1, 0 };
    for (int i = 0; i < 8; ++i)
    {
        source.setSample (0, i, left[i]);
        source.setSample (1, i, right[i]);
    }
    expect (writeBufferWav (file, source, 22050.0), "試聴用ステレオWAVを書けること");

    AudioFilePreview preview;
    preview.prepareToPlay (44100.0);
    expect (preview.start (file), "試聴デコードを開始できること");
    for (int i = 0; i < 200 && preview.status() == AudioFilePreview::Status::loading; ++i)
        juce::Thread::sleep (5);
    expect (preview.status() == AudioFilePreview::Status::playing, "デコード後に再生状態になること");

    juce::AudioBuffer<float> first (2, 5);
    first.clear();
    preview.mixInto (first, 1, 3);
    expect (std::abs (first.getSample (0, 1) - 0.0f) < 0.001f, "L先頭に遅延なし");
    expect (std::abs (first.getSample (0, 2) - 0.25f) < 0.001f, "L線形補間");
    expect (std::abs (first.getSample (0, 3) - 0.5f) < 0.001f, "L元サンプル");
    expect (std::abs (first.getSample (1, 1) - 0.5f) < 0.001f, "Rステレオ維持");
    expect (std::abs (first.getSample (1, 2) - 0.25f) < 0.001f, "R線形補間");
    expect (std::abs (first.getSample (0, 0)) < 0.001f
            && std::abs (first.getSample (0, 4)) < 0.001f,
            "指定範囲外を書き換えないこと");

    juce::AudioBuffer<float> second (2, 3);
    second.clear();
    preview.mixInto (second, 0, 3);
    expect (std::abs (second.getSample (0, 0) - 0.25f) < 0.001f,
            "コールバックをまたいでsourcePositionを維持");
    expect (std::abs (second.getSample (0, 2) + 0.25f) < 0.001f,
            "負値も線形補間");

    preview.stop();
    juce::AudioBuffer<float> stopped (2, 4);
    stopped.clear();
    preview.mixInto (stopped, 0, 4);
    expect (stopped.getMagnitude (0, 0, 4) == 0.0f, "停止後は無音");

    const auto monoFile = dir.getChildFile ("mono.wav");
    juce::AudioBuffer<float> monoSource (1, 3);
    monoSource.setSample (0, 0, 0.5f);
    monoSource.setSample (0, 1, -0.5f);
    monoSource.setSample (0, 2, 0.25f);
    expect (writeBufferWav (monoFile, monoSource, 44100.0), "モノWAVを書けること");
    expect (preview.start (monoFile), "別ファイルへ世代切替できること");
    for (int i = 0; i < 200 && preview.status() == AudioFilePreview::Status::loading; ++i)
        juce::Thread::sleep (5);
    juce::AudioBuffer<float> monoOutput (2, 8);
    monoOutput.clear();
    preview.mixInto (monoOutput, 0, 8);
    expect (std::abs (monoOutput.getSample (0, 0) - 0.25f) < 0.001f
            && std::abs (monoOutput.getSample (1, 0) - 0.25f) < 0.001f,
            "モノをL/Rへ同量で出すこと");
    expect (preview.status() == AudioFilePreview::Status::idle,
            "末尾を境界外読みせず完了すること");
    expect (std::abs (monoOutput.getSample (0, 3)) < 0.001f,
            "完了後の残りブロックは無音");

    const auto broken = dir.getChildFile ("broken.wav");
    broken.replaceWithText ("not audio");
    expect (preview.start (broken), "破損ファイルの検査を開始できること");
    for (int i = 0; i < 200 && preview.status() == AudioFilePreview::Status::loading; ++i)
        juce::Thread::sleep (5);
    expect (preview.status() == AudioFilePreview::Status::failed,
            "破損ファイルはfailed");
    expect (preview.takeError().isNotEmpty(), "破損理由をUI側で回収できること");

    preview.cancelAndWait();
    dir.deleteRecursively();
}

// ---- オーディオブラウザの試聴判定（オート/手動・予約・トランスポート・トグル） ----
void testPreviewPolicy()
{
    beginTest ("preview policy");

    const juce::File a ("/tmp/daw-policy-a.wav");
    const juce::File b ("/tmp/daw-policy-b.wav");
    const juce::File folder ("/tmp/daw-policy-dir");

    // 選択でオート予約 → デバウンス満了で開始
    {
        PreviewPolicy policy;
        auto selected = policy.selectionChanged (a, true);
        expect (selected.startTimer, "選択でデバウンスを予約すること");
        expect (! selected.startPreview, "選択の時点ではまだ鳴らさないこと");
        expect (policy.pending() == a, "予約対象が選択ファイルであること");

        auto fired = policy.takePending();
        expect (fired.startPreview && fired.startFile == a, "満了で選択ファイルを鳴らすこと");
        expect (policy.isActive (a), "試聴対象が■判定になること");
        expect (fired.repaint.contains (a), "開始した行を再描画対象にすること");

        // 連打しても最後の1件だけが残る
        policy.selectionChanged (a, true);
        policy.selectionChanged (b, true);
        expect (policy.pending() == b, "連続選択では最後の予約だけが残ること");
        expect (! policy.isActive (a), "選択が変わったら前の試聴を解除すること");
    }

    // フォルダ行・非対応ファイルは予約しない
    {
        PreviewPolicy policy;
        auto selected = policy.selectionChanged (folder, false);
        expect (! selected.startTimer, "フォルダ行では予約しないこと");
        expect (policy.pending() == juce::File(), "予約が空のままであること");
    }

    // トランスポート開始で auto だけ止まる（遷移時のみ・manualは残る）
    {
        PreviewPolicy policy;
        policy.selectionChanged (a, true);
        policy.takePending();
        auto started = policy.setTransportRunning (true);
        expect (started.stopPreview, "走行開始でオート試聴を止めること");
        expect (started.repaint.contains (a), "停止した行を再描画対象にすること");
        expect (! policy.isActive (a), "停止後は■判定が外れること");

        auto again = policy.setTransportRunning (true);
        expect (! again.stopPreview, "走行中フラグを繰り返し渡しても停止は1回だけであること");

        auto manual = policy.iconClicked (b);
        expect (manual.startPreview && manual.startFile == b, "走行中でも手動▶なら鳴らせること");
        auto stillRunning = policy.setTransportRunning (false);
        expect (! stillRunning.stopPreview, "走行終了では何も止めないこと");
        auto restart = policy.setTransportRunning (true);
        expect (! restart.stopPreview, "手動試聴は走行開始で止めないこと");
        expect (policy.isActive (b), "手動試聴が継続すること");
    }

    // 走行中は選択しても予約しない
    {
        PreviewPolicy policy;
        policy.setTransportRunning (true);
        auto selected = policy.selectionChanged (a, true);
        expect (! selected.startTimer, "走行中は選択で予約しないこと");
    }

    // トグルOFFで auto だけ止まる（予約中・実行中とも）
    {
        PreviewPolicy policy;
        policy.selectionChanged (a, true);
        auto offWhilePending = policy.setEnabled (false);
        expect (offWhilePending.stopTimer, "予約中のOFFで予約を取り消すこと");
        expect (policy.pending() == juce::File(), "予約が消えること");

        policy.setEnabled (true);
        policy.selectionChanged (a, true);
        policy.takePending();
        auto offWhilePlaying = policy.setEnabled (false);
        expect (offWhilePlaying.stopPreview, "実行中のOFFで試聴を止めること");
        expect (! policy.isActive (a), "停止後は■判定が外れること");

        auto manual = policy.iconClicked (b);
        expect (manual.startPreview, "OFF中でも手動▶なら鳴らせること");
        auto offAgain = policy.setEnabled (false);
        expect (! offAgain.stopPreview, "同じ値のOFFでは何も起きないこと");
        policy.setEnabled (true);
        auto offManual = policy.setEnabled (false);
        expect (! offManual.stopPreview, "手動試聴はトグルOFFで止めないこと");
        expect (policy.isActive (b), "手動試聴が継続すること");

        auto selected = policy.selectionChanged (a, true);
        expect (! selected.startTimer, "OFF中は選択で予約しないこと");
        expect (selected.stopPreview, "OFF中でも別の行を選べば止まること");
    }

    // 行アイコン: 同一ファイルで停止・別ファイルなら即開始
    {
        PreviewPolicy policy;
        auto first = policy.iconClicked (a);
        expect (first.startPreview && first.startFile == a, "▶で即開始すること");
        expect (! first.startTimer, "手動▶はデバウンスを挟まないこと");

        auto same = policy.iconClicked (a);
        expect (same.stopPreview && ! same.startPreview, "同じ行の■で停止すること");
        expect (same.repaint.contains (a), "停止した行を再描画対象にすること");

        policy.iconClicked (a);
        auto other = policy.iconClicked (b);
        expect (other.startPreview && other.startFile == b, "別の行の▶で切り替わること");
        expect (other.repaint.contains (a) && other.repaint.contains (b),
                "切り替え前後の2行を再描画対象にすること");
        expect (policy.isActive (b) && ! policy.isActive (a), "■が新しい行だけに付くこと");

        // 予約中に▶を押したら予約は捨てる
        PreviewPolicy other2;
        other2.selectionChanged (a, true);
        auto clicked = other2.iconClicked (b);
        expect (clicked.stopTimer, "▶で予約を取り消すこと");
        expect (other2.pending() == juce::File(), "予約が消えること");
    }

    // loading中も■・idleに落ちたら解除
    {
        PreviewPolicy policy;
        policy.iconClicked (a);
        auto loading = policy.previewStateChanged (true, false);
        expect (policy.isActive (a), "loading中も■のままであること");
        expect (loading.repaint.isEmpty(), "loading中は再描画不要であること");

        policy.previewStateChanged (false, true);
        expect (policy.isActive (a), "playing中も■のままであること");

        auto finished = policy.previewStateChanged (false, false);
        expect (! policy.isActive (a), "自然終了・失敗で■を外すこと");
        expect (finished.repaint.contains (a), "終了した行を再描画対象にすること");

        auto idle = policy.previewStateChanged (false, false);
        expect (idle.repaint.isEmpty(), "解除済みなら繰り返し再描画しないこと");

        auto restart = policy.iconClicked (a);
        expect (restart.startPreview && ! restart.stopPreview,
                "自然終了後の▶が「停止」でなく「開始」になること");
    }

    // パネルを閉じる・取り込み開始（cancelAll）は出自を問わず畳む
    {
        PreviewPolicy policy;
        policy.selectionChanged (a, true);
        auto whilePending = policy.cancelAll();
        expect (whilePending.stopTimer, "予約中のcancelAllで予約を取り消すこと");
        expect (policy.pending() == juce::File(), "予約が消えること");

        policy.iconClicked (b);
        auto whileManual = policy.cancelAll();
        expect (whileManual.stopPreview, "手動試聴中でもcancelAllで止まること");
        expect (! policy.isActive (b), "■が外れること");
    }

    // 失敗の後始末: 停止対象が無くても選択変更は停止要求を出す（フッターのエラーを畳むため）
    {
        PreviewPolicy policy;
        policy.iconClicked (a);
        policy.previewStateChanged (false, false); // failed → activeが外れた状態
        auto toFolder = policy.selectionChanged (folder, false);
        expect (toFolder.stopPreview, "失敗後にフォルダを選んでも停止要求が出ること");
        expect (! toFolder.startTimer, "フォルダでは予約しないこと");
        auto toFile = policy.selectionChanged (b, true);
        expect (toFile.stopPreview, "失敗後に別のファイルを選んでも停止要求が出ること");
        auto closed = policy.cancelAll();
        expect (closed.stopPreview, "何も鳴っていなくてもcancelAllは停止要求を出すこと");
    }
}

// ---- ファイル一覧の並び順（追加日／名前） ----
void testFileSortOrder()
{
    beginTest ("file sort order");

    const juce::Time base (2026, 5, 1, 12, 0); // 月は0起点＝6月
    juce::Array<FileSortOrder::Entry> entries;
    entries.add ({ "b-old.wav",  base });
    entries.add ({ "a-new.wav",  base + juce::RelativeTime::hours (2) });
    entries.add ({ "C-mid.wav",  base + juce::RelativeTime::hours (1) });
    entries.add ({ "d-same.wav", base }); // b-old と同時刻

    const auto byDate = FileSortOrder::sortedIndices (entries, FileSortOrder::Mode::dateAdded);
    expect (entries[byDate[0]].filename == "a-new.wav", "追加日順は新しいものが先頭に来ること");
    expect (entries[byDate[1]].filename == "C-mid.wav", "以降も新しい順に並ぶこと");
    expect (entries[byDate[2]].filename == "b-old.wav", "同時刻はファイル名で決まること");
    expect (entries[byDate[3]].filename == "d-same.wav", "同時刻の並びが安定すること");

    const auto byName = FileSortOrder::sortedIndices (entries, FileSortOrder::Mode::name);
    expect (entries[byName[0]].filename == "a-new.wav", "名前順は昇順であること");
    expect (entries[byName[1]].filename == "b-old.wav", "大文字小文字を区別しないこと(b < C)");
    expect (entries[byName[2]].filename == "C-mid.wav", "大文字小文字を区別しないこと(C < d)");
    expect (entries[byName[3]].filename == "d-same.wav", "名前順の末尾");

    juce::Array<FileSortOrder::Entry> numbered;
    numbered.add ({ "take10.wav", base });
    numbered.add ({ "take2.wav",  base });
    const auto natural = FileSortOrder::sortedIndices (numbered, FileSortOrder::Mode::name);
    expect (numbered[natural[0]].filename == "take2.wav", "数字は数値として比較されること(2 < 10)");

    expect (FileSortOrder::sortedIndices ({}, FileSortOrder::Mode::dateAdded).isEmpty(),
            "空の一覧でも壊れないこと");
}

// ---- サンプル音源GAINのスケール（dB ⇄ 線形倍率・クランプ・吸着） ----
void testGainScale()
{
    beginTest ("gain scale");

    // dB ⇄ 倍率。UIはdBで持ち、モデルは倍率で持つのでここが唯一の変換点になる
    expect (std::abs (GainScale::toLinear (0.0) - 1.0f) < 1.0e-6f, "0dB = 等倍であること");
    expect (std::abs (GainScale::toLinear (6.0) - 1.9953f) < 1.0e-3f, "+6dB ≒ 1.995倍であること");
    expect (std::abs (GainScale::toLinear (-6.0) - 0.5012f) < 1.0e-3f, "-6dB ≒ 0.501倍であること");
    expect (std::abs (GainScale::toDb (1.0f)) < 1.0e-6, "等倍 = 0dBであること");
    expect (std::abs (GainScale::toDb (GainScale::toLinear (3.5)) - 3.5) < 1.0e-3, "往復して値が保たれること");

    // 無音は -∞ でなく下端に落ちる（トリムに無音域は持たせない。消音はミュートの仕事）
    expect (std::abs (GainScale::toDb (0.0f) + GainScale::rangeDb) < 1.0e-6, "倍率0が-∞でなく-12dBになること");
    expect (std::abs (GainScale::toDb (100.0f) - GainScale::rangeDb) < 1.0e-6, "範囲を超える倍率が+12dBで頭打ちになること");

    // 読み込み時のクランプ（UIが表現できない値をモデルに残さない）
    expect (std::abs (GainScale::clampLinear (9.0f) - GainScale::maxLinear()) < 1.0e-6f,
            "上限を超える値が+12dB相当へクランプされること");
    expect (std::abs (GainScale::clampLinear (0.05f) - GainScale::minLinear()) < 1.0e-6f,
            "下限を下回る値が-12dB相当へクランプされること");
    expect (std::abs (GainScale::clampLinear (0.0f) - GainScale::minLinear()) < 1.0e-6f,
            "無音(0.0)も下限へクランプされること");
    expect (std::abs (GainScale::clampLinear (0.775f) - 0.775f) < 1.0e-6f, "範囲内の値は変わらないこと");
    expect (GainScale::minLinear() > 0.25f && GainScale::minLinear() < 0.26f, "下限が約0.251倍であること");
    expect (GainScale::maxLinear() > 3.98f && GainScale::maxLinear() < 3.99f, "上限が約3.981倍であること");

    // 0dB付近の吸着はドラッグ中だけ。プログラム同期やリセットで勝手に丸めない
    expect (GainScale::snapDb (0.2, true) == 0.0, "ドラッグ中は0dB付近が吸着すること");
    expect (GainScale::snapDb (-0.2, true) == 0.0, "下げ側からでも吸着すること");
    expect (std::abs (GainScale::snapDb (0.2, false) - 0.2) < 1.0e-9, "ドラッグ中でなければ吸着しないこと");
    expect (std::abs (GainScale::snapDb (1.0, true) - 1.0) < 1.0e-9, "吸着幅の外は動かさないこと");
    expect (std::abs (GainScale::snapDb (-6.0, true) + 6.0) < 1.0e-9, "離れた値はそのまま通ること");

    // 表示文字列（上げ側は符号を明示。-0.0 dB を出さない）
    expect (GainScale::text (0.0) == "0.0 dB", "0dBの表記");
    expect (GainScale::text (6.0) == "+6.0 dB", "上げ側に+が付くこと");
    expect (GainScale::text (-3.5) == "-3.5 dB", "下げ側の表記");
    expect (GainScale::text (-0.02) == "0.0 dB", "ごく小さい負の値が -0.0 dB にならないこと");
}

// ---- 下部エリアの履歴（E / [ / ]）----
// カーソル操作は「findValid で非破壊に探す → 復元 → 成功したら commit」の順で使う前提なので、
// findValid が呼んだだけでカーソルを動かさないことと、候補なしでカーソルが維持されることを見る
void testBottomPanelHistory()
{
    beginTest ("bottom panel history");

    using Entry = BottomPanelHistory::Entry;
    auto pianoRoll = [] (juce::uint64 trackId, juce::uint64 regionId)
    {
        Entry e;
        e.kind = Entry::Kind::pianoRoll;
        e.trackId = trackId;
        e.regionId = regionId;
        return e;
    };
    auto fxDetail = [] (const juce::String& channelKey, juce::uint64 trackId, int slot)
    {
        Entry e;
        e.kind = Entry::Kind::fxDetail;
        e.channelKey = channelKey;
        e.trackId = trackId;
        e.slot = slot;
        return e;
    };
    auto anyEntry = [] (const Entry&) { return true; };

    // 同じものを開き直しても履歴は伸びない
    {
        BottomPanelHistory h;
        h.push (pianoRoll (1, 10));
        h.push (pianoRoll (1, 10));
        expect (h.size() == 1, "同一エントリの連続pushで伸びないこと");
        h.push (fxDetail ("track", 1, 3));
        h.push (fxDetail ("track", 1, 3));
        expect (h.size() == 2, "種別違いは積まれ、その連続pushでは伸びないこと");
    }

    // 戻った状態でpushすると進む側が破棄される（ブラウザ型）
    {
        BottomPanelHistory h;
        h.push (pianoRoll (1, 10));
        h.push (pianoRoll (1, 20));
        h.push (pianoRoll (1, 30));
        h.commit (h.findValid (-1, anyEntry));
        expect (h.current() == pianoRoll (1, 20), "1つ戻れること");
        expect (h.findValid (1, anyEntry) == 2, "戻った後は進む先があること");
        h.push (pianoRoll (1, 40));
        expect (h.size() == 3, "戻った後のpushで進む側が破棄されること");
        expect (h.findValid (1, anyEntry) < 0, "破棄後は進めないこと");
        expect (h.current() == pianoRoll (1, 40), "pushしたものが現在地になること");
    }

    // 上限を超えると古い方から捨てる
    {
        BottomPanelHistory h;
        for (int i = 0; i < BottomPanelHistory::maxEntries + 4; ++i)
            h.push (pianoRoll (1, (juce::uint64) (i + 1)));
        expect (h.size() == BottomPanelHistory::maxEntries, "上限で頭打ちになること");
        expect (h.entryAt (0) == pianoRoll (1, 5), "古い方から捨てられること");
        expect (h.current() == pianoRoll (1, BottomPanelHistory::maxEntries + 4),
                "最新が現在地のままであること");
    }

    // replaceCurrent はカーソルを動かさず内容だけ差し替える（トラック選択への追従用）
    {
        BottomPanelHistory h;
        h.push (pianoRoll (1, 10));
        h.push (fxDetail ("track", 2, 3));
        const int before = h.currentPosition();
        h.replaceCurrent (fxDetail ("track", 7, 3));
        expect (h.currentPosition() == before, "replaceCurrentでカーソルが動かないこと");
        expect (h.size() == 2, "replaceCurrentで履歴が伸びないこと");
        expect (h.current() == fxDetail ("track", 7, 3), "現在エントリが差し替わること");
        expect (h.entryAt (0) == pianoRoll (1, 10), "手前のエントリが壊れないこと");
    }

    // findValid は非破壊。無効な候補は飛ばし、候補がなければカーソルを維持したまま -1
    {
        BottomPanelHistory h;
        h.push (pianoRoll (1, 10));
        h.push (pianoRoll (1, 20)); // これだけ「消えた」ことにする
        h.push (pianoRoll (1, 30));
        auto alive = [] (const Entry& e) { return e.regionId != 20; };

        const int position = h.findValid (-1, alive);
        expect (position == 0, "無効なエントリを飛ばして次の有効なものを返すこと");
        expect (h.currentPosition() == 2, "findValidがカーソルを動かさないこと");
        expect (h.entryAt (position) == pianoRoll (1, 10), "entryAtがカーソルと無関係に読めること");

        h.commit (position);
        expect (h.currentPosition() == 0 && h.current() == pianoRoll (1, 10),
                "commitで初めてカーソルが移ること");
        expect (h.findValid (-1, alive) < 0, "端まで有効な候補がなければ-1");
        expect (h.currentPosition() == 0, "候補なしでもカーソルが維持されること");
        expect (h.findValid (1, alive) == 2, "commit後のfindValidがそこ起点で探すこと");
    }

    // 有効な候補が1つもない場合
    {
        BottomPanelHistory h;
        h.push (pianoRoll (1, 10));
        h.push (pianoRoll (1, 20));
        auto none = [] (const Entry&) { return false; };
        expect (h.findValid (-1, none) < 0 && h.findValid (1, none) < 0, "全滅なら両方向とも-1");
        expect (h.currentPosition() == 1, "全滅でもカーソルが維持されること");
    }

    // 空の履歴を触っても壊れない
    {
        BottomPanelHistory h;
        expect (! h.hasCurrent(), "空なら現在地なし");
        expect (h.findValid (-1, anyEntry) < 0 && h.findValid (1, anyEntry) < 0, "空なら候補なし");
        h.replaceCurrent (pianoRoll (1, 10));
        expect (h.size() == 0, "空へのreplaceCurrentが積まないこと");
    }
}

// ---- 下部エリアのショートカットが既存キーと誤爆しないこと ----
// E は ⌘E（リージョン書き出し）と、[ / ] は ⌘[ / ⌘]（ファイルパネルの履歴）と1文字違いなので、
// 修飾の有無で確実に分かれることを固定する
void testBottomPanelShortcuts()
{
    beginTest ("bottom panel shortcuts");

    const auto none = juce::ModifierKeys();
    const auto cmd = juce::ModifierKeys (juce::ModifierKeys::commandModifier);
    const auto shift = juce::ModifierKeys (juce::ModifierKeys::shiftModifier);

    const juce::KeyPress plainE ('e', none, 'e');
    const juce::KeyPress cmdE ('e', cmd, 0);
    expect (Shortcuts::matches (plainE, Shortcuts::ID::toggleBottomPanel), "E で下部エリアのトグル");
    expect (! Shortcuts::matches (plainE, Shortcuts::ID::exportRegion), "E が⌘E（書き出し）に化けないこと");
    expect (Shortcuts::matches (cmdE, Shortcuts::ID::exportRegion), "⌘E は従来どおり書き出し");
    expect (! Shortcuts::matches (cmdE, Shortcuts::ID::toggleBottomPanel),
            "⌘E が下部エリアのトグルに化けないこと");

    const juce::KeyPress openBracket ('[', none, '[');
    const juce::KeyPress closeBracket (']', none, ']');
    const juce::KeyPress cmdOpenBracket ('[', cmd, 0);
    expect (Shortcuts::matches (openBracket, Shortcuts::ID::bottomHistory), "[ で下部エリアの履歴");
    expect (Shortcuts::matches (closeBracket, Shortcuts::ID::bottomHistory), "] で下部エリアの履歴");
    expect (! Shortcuts::matches (openBracket, Shortcuts::ID::browserHistory),
            "[ がファイルパネルの履歴に化けないこと");
    expect (Shortcuts::matches (cmdOpenBracket, Shortcuts::ID::browserHistory),
            "⌘[ は従来どおりファイルパネルの履歴");
    expect (! Shortcuts::matches (cmdOpenBracket, Shortcuts::ID::bottomHistory),
            "⌘[ が下部エリアの履歴に化けないこと");

    // Shift併用は {} になるので拾わない（noCmdCtrlAlt はShiftを許容するため、文字で弾けているかを見る）
    expect (! Shortcuts::matches (juce::KeyPress ('[', shift, '{'), Shortcuts::ID::bottomHistory),
            "Shift+[ ({) を拾わないこと");

    // テーブル全走査で「そのKeyPressにマッチする項目がちょうど1件」（VERIFY.md のキー衝突チェック）。
    // 個別の対を並べるより強く、今後キーを追加したときの衝突もここで落ちる
    auto hits = [] (const juce::KeyPress& k)
    {
        int n = 0;
        for (const auto& e : Shortcuts::table)
            if (e.matcher (k))
                ++n;
        return n;
    };
    expect (hits (plainE) == 1, "E にマッチする項目がちょうど1件");
    expect (hits (openBracket) == 1, "[ にマッチする項目がちょうど1件");
    expect (hits (closeBracket) == 1, "] にマッチする項目がちょうど1件");
    expect (hits (cmdE) == 1, "⌘E にマッチする項目がちょうど1件");
    expect (hits (cmdOpenBracket) == 1, "⌘[ にマッチする項目がちょうど1件");

    // ⌘?一覧はテーブル走査で作られるので、載っている＝一覧に出る
    expect (Shortcuts::keyText (Shortcuts::ID::toggleBottomPanel) == "E", "Eの表記");
    expect (Shortcuts::keyText (Shortcuts::ID::bottomHistory) == juce::String::fromUTF8 (u8"[ / ]"),
            "[ / ]の表記");
}

// ⌘R（リピート）と ⌘C/⌘V（コピー/ペースト）。素の R（録音）・C（サイクル）と混ざらないこと、
// 1キーにマッチする項目がちょうど1件であること（⌘C/⌘Vはノートとリージョンで共用するので分けない）
void testRegionEditShortcuts()
{
    beginTest ("region edit shortcuts");

    const auto none = juce::ModifierKeys();
    const auto cmd = juce::ModifierKeys (juce::ModifierKeys::commandModifier);

    const juce::KeyPress plainR ('r', none, 'r');
    const juce::KeyPress cmdR ('r', cmd, 0);
    const juce::KeyPress plainC ('c', none, 'c');
    const juce::KeyPress cmdC ('c', cmd, 0);
    const juce::KeyPress cmdV ('v', cmd, 0);

    expect (Shortcuts::matches (cmdR, Shortcuts::ID::repeatItem), "⌘R でリピート");
    expect (! Shortcuts::matches (cmdR, Shortcuts::ID::record), "⌘R が録音に化けないこと");
    expect (Shortcuts::matches (plainR, Shortcuts::ID::record), "R は従来どおり録音");
    expect (! Shortcuts::matches (plainR, Shortcuts::ID::repeatItem), "R がリピートに化けないこと");

    expect (Shortcuts::matches (cmdC, Shortcuts::ID::copyItem), "⌘C でコピー");
    expect (! Shortcuts::matches (cmdC, Shortcuts::ID::toggleCycle), "⌘C がサイクルに化けないこと");
    expect (Shortcuts::matches (plainC, Shortcuts::ID::toggleCycle), "C は従来どおりサイクル");
    expect (Shortcuts::matches (cmdV, Shortcuts::ID::pasteItem), "⌘V でペースト");

    auto hits = [] (const juce::KeyPress& k)
    {
        int n = 0;
        for (const auto& e : Shortcuts::table)
            if (e.matcher (k))
                ++n;
        return n;
    };
    expect (hits (cmdR) == 1, "⌘R にマッチする項目がちょうど1件");
    expect (hits (plainR) == 1, "R にマッチする項目がちょうど1件");
    expect (hits (cmdC) == 1, "⌘C にマッチする項目がちょうど1件（ノート用と分けない）");
    expect (hits (cmdV) == 1, "⌘V にマッチする項目がちょうど1件");
    expect (hits (plainC) == 1, "C にマッチする項目がちょうど1件");

    // ⌘?一覧はテーブル走査で作られるので、載っている＝一覧に出る
    expect (Shortcuts::keyText (Shortcuts::ID::repeatItem) == juce::String::fromUTF8 (u8"⌘R"), "⌘Rの表記");
    expect (Shortcuts::keyText (Shortcuts::ID::copyItem) == juce::String::fromUTF8 (u8"⌘C"), "⌘Cの表記");
    expect (Shortcuts::keyText (Shortcuts::ID::pasteItem) == juce::String::fromUTF8 (u8"⌘V"), "⌘Vの表記");
}

// ---- GAINスライダーの描画: 中央(0dB)起点の帯。トラックヘッダーの音量バーには出さない ----
void testGainSliderCenterFill()
{
    beginTest ("gain slider center fill");

    AppLookAndFeel laf;
    constexpr int w = 240, h = 26;
    const float centreX = w * 0.5f;

    // dB → 球の中心x。検証したいのは「中央を起点に左右へ伸びる」ことだけなので単純な線形写像で置く
    auto posFor = [] (double db)
    { return (float) ((db + GainScale::rangeDb) / (2.0 * GainScale::rangeDb) * w); };

    auto render = [&] (double db, bool centerFill)
    {
        juce::Slider slider;
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        if (centerFill)
            slider.getProperties().set ("centerFill", true);

        juce::Image img (juce::Image::ARGB, w, h, true);
        {
            juce::Graphics g (img);
            laf.drawLinearSlider (g, 0, 0, w, h, posFor (db), 0.0f, (float) w,
                                  juce::Slider::LinearHorizontal, slider);
        }
        return img;
    };

    // 帯（Theme::accent）が横方向のどこに何列あるか
    struct Span { int first = -1, last = -1, columns = 0; };
    auto accentSpan = [&] (const juce::Image& img)
    {
        Span s;
        for (int x = 0; x < w; ++x)
            for (int y = 0; y < h; ++y)
                if (img.getPixelAt (x, y).getARGB() == Theme::accent.getARGB())
                {
                    ++s.columns;
                    if (s.first < 0)
                        s.first = x;
                    s.last = x;
                    break;
                }
        return s;
    };

    expect (accentSpan (render (0.0, true)).columns == 0,
            "0dBでは帯を描かないこと（素のままが一目で分かる）");

    const auto up = accentSpan (render (6.0, true));
    expect (up.columns > 0, "上げたときに帯が出ること");
    expect ((float) up.first >= centreX - 1.0f, "上げた帯が中央から始まること");
    expect ((float) up.last > centreX && up.last < w, "上げた帯が中央より右へ伸びること");

    const auto down = accentSpan (render (-6.0, true));
    expect (down.columns > 0, "下げたときに帯が出ること");
    expect ((float) down.last <= centreX + 1.0f, "下げた帯が中央で終わること");
    expect (down.first > 0 && (float) down.first < centreX, "下げた帯が中央より左へ伸びること");

    expect (std::abs (up.columns - down.columns) <= 2,
            "同じ量なら上げ下げで帯の長さが揃うこと（中央が基準として読める）");

    // トラックヘッダーの音量バーは centerFill を立てないので、見た目は従来どおり（帯は出ない）
    expect (accentSpan (render (6.0, false)).columns == 0,
            "centerFillを立てないスライダーには帯を描かないこと");
}

// ---- v1プロジェクト読込 → v2保存 → 再読込のラウンドトリップ ----
void testV1ToV2Roundtrip()
{
    beginTest ("v1 -> v2 roundtrip");
    const auto dir = makeTempDir();

    expect (writeTestWav (dir.getChildFile ("clip-001.wav"), 4410), "テストWAVを書けること");

    // v1形式のJSON（ID・type・nextId無し）
    const char* v1json = R"({
        "version": 1, "bpm": 95.5, "sampleRate": 44100.0,
        "tracks": [
            { "type": "audio", "name": "Vocal", "mute": true, "solo": false, "volume": 0.6,
              "clips": [ { "file": "clip-001.wav", "startSample": 12345 } ] },
            { "type": "audio", "name": "Chorus", "mute": false, "solo": true, "volume": 0.9, "clips": [] }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (v1json);

    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr, "v1を読込めること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    expect (warnings.isEmpty(), "警告が出ないこと");
    expect (project->tracks.size() == 2, "トラック数2");
    expect (juce::approximatelyEqual (project->bpm, 95.5), "bpm維持");
    expect (project->tracks[0].id != 0 && project->tracks[1].id != 0, "IDが採番されること");
    expect (project->tracks[0].id != project->tracks[1].id, "IDが一意であること");
    expect (project->tracks[0].type == TrackType::audio, "v1トラックはaudio種別");
    expect (project->tracks[0].clips.size() == 1, "クリップが読めること");
    expect (project->tracks[0].clips[0].startSample == 12345, "startSample維持");
    expect (project->tracks[0].params->mute.load(), "mute維持");
    expect (project->tracks[1].params->solo.load(), "solo維持");

    const auto id0 = project->tracks[0].id;
    const auto id1 = project->tracks[1].id;

    expect (project->save (error), "v2で保存できること");

    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr, "v2を再読込できること");
    if (reloaded == nullptr)
        { dir.deleteRecursively(); return; }

    expect (reloaded->tracks.size() == 2, "再読込後のトラック数2");
    expect (reloaded->tracks[0].id == id0 && reloaded->tracks[1].id == id1,
            "保存→再読込でIDが安定していること");
    expect (reloaded->tracks[0].clips.size() == 1, "再読込後もクリップが読めること");
    expect (juce::approximatelyEqual (reloaded->bpm, 95.5), "再読込後のbpm維持");

    dir.deleteRecursively();
}

// ---- MIDIトラックの保存/読込ラウンドトリップ ----
void testMidiRoundtrip()
{
    beginTest ("midi track roundtrip");
    const auto dir = makeTempDir();

    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "createNewできること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    Track midiTrack;
    midiTrack.id = project->allocateId();
    midiTrack.type = TrackType::midi;
    midiTrack.name = "Drums";
    midiTrack.gmProgram = 33;
    midiTrack.drums = true;
    midiTrack.drumPitch = 36; // 固定ピッチ打楽器（Kick）

    MidiRegion region;
    region.id = project->allocateId();
    region.startPpq = Ppq::ticksPerBar * 2;
    region.lengthPpq = Ppq::ticksPerBar;

    MidiNote note1 { project->allocateId(), 36, 0, Ppq::ticksPerQuarter / 8, 127 };
    MidiNote note2 { project->allocateId(), 38, Ppq::ticksPerQuarter, 80, 1 }; // 1/32三連符=80tick, velocity最小
    region.notes.push_back (note1);
    region.notes.push_back (note2);
    midiTrack.midiRegions.push_back (region);
    project->tracks.push_back (std::move (midiTrack));

    expect (project->save (error), "保存できること");

    juce::StringArray warnings;
    auto reloaded = Project::load (project->directory, warnings, error);
    expect (reloaded != nullptr, "再読込できること");
    if (reloaded == nullptr)
        { dir.deleteRecursively(); return; }

    expect (reloaded->tracks.size() == 2, "トラック数2");
    const auto& t = reloaded->tracks[1];
    expect (t.type == TrackType::midi, "midi種別維持");
    expect (t.gmProgram == 33 && t.drums, "gmProgram/drums維持");
    expect (t.drumPitch == 36, "drumPitch維持");
    expect (t.midiRegions.size() == 1, "リージョン数1");
    const auto& r = t.midiRegions[0];
    expect (r.id == region.id, "リージョンID維持");
    expect (r.startPpq == Ppq::ticksPerBar * 2 && r.lengthPpq == Ppq::ticksPerBar, "リージョン位置維持");
    expect (r.notes.size() == 2, "ノート数2");
    expect (r.notes[0].id == note1.id && r.notes[0].pitch == 36
                && r.notes[0].lengthPpq == Ppq::ticksPerQuarter / 8 && r.notes[0].velocity == 127,
            "ノート1維持");
    expect (r.notes[1].id == note2.id && r.notes[1].startPpq == Ppq::ticksPerQuarter
                && r.notes[1].lengthPpq == 80 && r.notes[1].velocity == 1,
            "ノート2維持（1/32三連符・velocity境界）");

    dir.deleteRecursively();
}

// ループ回数の保存・読み込み。ループなしのときは loopCount を書かないので、
// 出来上がるJSONはv8以前と同じ形になる（＝旧形式を読んでもループなしで復元される）
void testLoopRoundtrip()
{
    beginTest ("loop roundtrip and legacy default");
    const auto dir = makeTempDir();

    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "createNewできること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    const auto wavFile = project->directory.getChildFile ("clip-001.wav");
    expect (writeTestWav (wavFile, 4410), "テストWAVを書けること");
    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.audio = Project::loadWav (wavFile);
    clip.lengthSamples = clip.audio != nullptr ? clip.audio->getNumSamples() : 0;
    clip.loopCount = 2;
    project->tracks[0].clips.push_back (std::move (clip));

    Track midiTrack;
    midiTrack.id = project->allocateId();
    midiTrack.type = TrackType::midi;
    MidiRegion region;
    region.id = project->allocateId();
    region.startPpq = Ppq::ticksPerBar;
    region.lengthPpq = Ppq::ticksPerBar;
    region.loopCount = 3;
    region.notes.push_back ({ project->allocateId(), 60, 0, Ppq::ticksPerQuarter, 100 });
    midiTrack.midiRegions.push_back (region);
    project->tracks.push_back (std::move (midiTrack));

    expect (project->save (error), "保存できること");
    const auto jsonFile = project->directory.getChildFile ("project.json");
    expect (jsonFile.loadFileAsString().contains ("loopCount"), "ループありはJSONに書かれること");

    juce::StringArray warnings;
    auto reloaded = Project::load (project->directory, warnings, error);
    expect (reloaded != nullptr, "再読込できること");
    if (reloaded == nullptr)
        { dir.deleteRecursively(); return; }
    expect (reloaded->tracks[0].clips.size() == 1 && reloaded->tracks[0].clips[0].loopCount == 2,
            "クリップのループ回数が復元されること");
    expect (reloaded->tracks[1].midiRegions.size() == 1
                && reloaded->tracks[1].midiRegions[0].loopCount == 3,
            "リージョンのループ回数が復元されること");

    // ループを外すと loopCount 自体がJSONから消える = v8以前と同じ形。それを読めばループなし
    reloaded->tracks[0].clips[0].loopCount = 0;
    reloaded->tracks[1].midiRegions[0].loopCount = 0;
    expect (reloaded->save (error), "ループなしで保存できること");
    expect (! jsonFile.loadFileAsString().contains ("loopCount"),
            "ループなしはJSONに書かれないこと（旧形式と同じ形）");

    auto legacy = Project::load (project->directory, warnings, error);
    expect (legacy != nullptr, "旧形式相当のJSONを読めること");
    if (legacy != nullptr)
    {
        expect (legacy->tracks[0].clips[0].loopCount == 0, "欠損時のクリップはループなし");
        expect (legacy->tracks[1].midiRegions[0].loopCount == 0, "欠損時のリージョンはループなし");
    }

    dir.deleteRecursively();
}

// リージョンゲインの保存・読み込み。ユニティのときは gain を書かないので、
// 出来上がるJSONはv9以前と同じ形になる（＝旧形式を読んでもユニティで復元される）
void testClipGainRoundtrip()
{
    beginTest ("clip gain roundtrip, clamp and legacy default");
    const auto dir = makeTempDir();

    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "createNewできること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    const auto wavFile = project->directory.getChildFile ("clip-001.wav");
    expect (writeTestWav (wavFile, 4410), "テストWAVを書けること");
    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.audio = Project::loadWav (wavFile);
    clip.lengthSamples = clip.audio != nullptr ? clip.audio->getNumSamples() : 0;
    clip.gain = GainScale::toLinear (-4.0);
    project->tracks[0].clips.push_back (std::move (clip));

    expect (project->save (error), "保存できること");
    const auto jsonFile = project->directory.getChildFile ("project.json");
    // "gain" はトラック音量の保存キーにもあるため、clips要素のプロパティ有無で判定する
    const auto clipHasGainKey = [&jsonFile]
    {
        const auto parsed = juce::JSON::parse (jsonFile.loadFileAsString());
        if (auto* tracks = parsed.getProperty ("tracks", {}).getArray())
            if (! tracks->isEmpty())
                if (auto* clips = (*tracks)[0].getProperty ("clips", {}).getArray())
                    if (! clips->isEmpty())
                        return (*clips)[0].hasProperty ("gain");
        return false;
    };
    expect (clipHasGainKey(), "ユニティ以外はJSONに書かれること");

    juce::StringArray warnings;
    auto reloaded = Project::load (project->directory, warnings, error);
    expect (reloaded != nullptr, "再読込できること");
    if (reloaded == nullptr)
        { dir.deleteRecursively(); return; }
    expect (reloaded->tracks[0].clips.size() == 1
                && std::abs (GainScale::toDb (reloaded->tracks[0].clips[0].gain) + 4.0) < 0.01,
            "クリップのゲインが復元されること");

    // ユニティに戻すと gain 自体がJSONから消える = v9以前と同じ形。それを読めばユニティ
    reloaded->tracks[0].clips[0].gain = 1.0f;
    expect (reloaded->save (error), "ユニティで保存できること");
    expect (! clipHasGainKey(), "ユニティはJSONに書かれないこと（旧形式と同じ形）");

    auto legacy = Project::load (project->directory, warnings, error);
    expect (legacy != nullptr, "旧形式相当のJSONを読めること");
    if (legacy != nullptr)
        expect (legacy->tracks[0].clips[0].gain == 1.0f, "欠損時はユニティ");

    // 範囲外の値は読み込んだ時点でクランプする（表示は-12dBなのに-20dBで鳴る、を防ぐ）
    {
        const auto outOfRange = makeTempDir();
        expect (writeTestWav (outOfRange.getChildFile ("clip-001.wav"), 4410), "テストWAVを書けること");
        const char* json = R"({
            "version": 10, "bpm": 120.0, "sampleRate": 44100.0, "nextId": 2,
            "tracks": [
                { "id": 1, "type": "audio", "name": "X",
                  "clips": [
                    { "file": "clip-001.wav", "startSample": 0, "gain": 99.0 },
                    { "file": "clip-001.wav", "startSample": 0, "gain": 0.0 }
                  ] }
            ]
        })";
        outOfRange.getChildFile ("project.json").replaceWithText (json);
        auto clamped = Project::load (outOfRange, warnings, error);
        expect (clamped != nullptr && clamped->tracks[0].clips.size() == 2, "読込めること");
        if (clamped != nullptr && clamped->tracks[0].clips.size() == 2)
        {
            expect (clamped->tracks[0].clips[0].gain == GainScale::maxLinear(), "過大値は+12dBへ");
            expect (clamped->tracks[0].clips[1].gain == GainScale::minLinear(), "0（無音）は-12dBへ");
        }
        outOfRange.deleteRecursively();
    }

    dir.deleteRecursively();
}

// ---- リージョンゲインがスナップショットへ載ること（ループ展開の各反復にも同じ値） ----
void testClipGainSnapshot()
{
    beginTest ("clip gain in snapshot");

    Project project;
    Track track;
    track.id = 1;
    Clip clip;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, 1000);
    clip.audio->clear();
    clip.lengthSamples = 500;
    clip.loopCount = 2; // 本体＋2反復 = 3実体
    clip.gain = GainScale::toLinear (-6.0);
    track.clips.push_back (std::move (clip));
    project.tracks.push_back (std::move (track));

    auto snapshot = project.buildSnapshot();
    expect (snapshot != nullptr && snapshot->tracks.size() == 1, "スナップショットが作れること");
    if (snapshot == nullptr || snapshot->tracks.empty())
        return;
    const auto& clips = snapshot->tracks[0].clips;
    expect (clips.size() == 3, "ループ分が展開されること");
    for (size_t i = 0; i < clips.size(); ++i)
        expect (std::abs (clips[i].gain - GainScale::toLinear (-6.0)) < 1.0e-6f,
                "展開された各実体に同じゲインが載ること");
}


// ---- GAINスライダーはホイールで値が変わらない ----
// undoの区切り（GainSlider::onNewClickSequence）は mouseDown 基準なので、mouseDown を伴わない
// ホイール操作を許すと「ドラッグ後にホイールで微調整すると⌘Zがドラッグ前まで戻る」不整合が出る。
// 経路を1本に絞ってあることをここで固定する（誰かが setScrollWheelEnabled を消したら落ちる）
void testGainSliderIgnoresScrollWheel()
{
    beginTest ("gain slider ignores scroll wheel");

    GainSlider slider;
    slider.setRange (-GainScale::rangeDb, GainScale::rangeDb, 0.1);
    slider.setValue (-3.0, juce::dontSendNotification);
    slider.setBounds (0, 0, 200, 20);

    int valueChanges = 0;
    slider.onValueChange = [&valueChanges] { ++valueChanges; };

    const auto& source = juce::Desktop::getInstance().getMainMouseSource();
    const juce::MouseEvent event (source, { 100.0f, 10.0f }, juce::ModifierKeys(),
                                  juce::MouseInputSource::defaultPressure,
                                  juce::MouseInputSource::defaultOrientation,
                                  juce::MouseInputSource::defaultRotation,
                                  juce::MouseInputSource::defaultTiltX,
                                  juce::MouseInputSource::defaultTiltY,
                                  &slider, &slider, juce::Time::getCurrentTime(),
                                  { 100.0f, 10.0f }, juce::Time::getCurrentTime(), 1, false);
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = 1.0f;

    slider.mouseWheelMove (event, wheel);
    expect (std::abs (slider.getValue() + 3.0) < 1.0e-9, "ホイールで値が変わらないこと");
    expect (valueChanges == 0, "onValueChangeも発火しないこと（undoが積まれない）");
}

// ---- 不正なJSONへの防御 ----
void testInvalidJson()
{
    beginTest ("invalid json");
    const auto dir = makeTempDir();
    juce::StringArray warnings;
    juce::String error;

    // 壊れたJSON → エラーで nullptr
    dir.getChildFile ("project.json").replaceWithText ("{ not valid json !!");
    expect (Project::load (dir, warnings, error) == nullptr, "壊れたJSONはnullptr");
    expect (error.isNotEmpty(), "エラーメッセージが入ること");

    // 型違い・欠損・範囲外 → クラッシュせずデフォルト/クランプで読む
    const char* weird = R"({
        "version": 2, "bpm": 99999, "sampleRate": -5,
        "tracks": [
            42,
            { "type": "midi", "name": "X", "gmProgram": 999, "drums": false,
              "regions": [
                  "not an object",
                  { "id": 7, "startPpq": -100, "lengthPpq": 0,
                    "notes": [ { "id": 7, "pitch": 200, "startPpq": -5, "lengthPpq": 0, "velocity": 0 } ] }
              ] },
            { "type": "audio", "name": "Y", "clips": "nope" }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (weird);

    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr, "型違い混在でも読込めること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    expect (project->tracks.size() == 2, "非オブジェクトのトラックは読み飛ばす");
    expect (project->bpm <= 300.0, "bpmがクランプされること");
    expect (project->sampleRate >= 0.0, "sampleRateがクランプされること");

    const auto& t = project->tracks[0];
    expect (t.gmProgram == 127, "gmProgramクランプ");
    expect (t.midiRegions.size() == 1, "非オブジェクトのリージョンは読み飛ばす");
    const auto& r = t.midiRegions[0];
    expect (r.startPpq == 0 && r.lengthPpq == 1, "リージョンの負開始・長さ0がクランプされること");
    expect (r.notes.size() == 1, "ノートが読めること");
    expect (r.notes[0].pitch == 127 && r.notes[0].velocity == 1
                && r.notes[0].startPpq == 0 && r.notes[0].lengthPpq == 1,
            "ノートの範囲外値がクランプされること");
    // id重複 (region 7 / note 7) はどちらかが振り直される
    expect (r.id != r.notes[0].id, "重複IDが振り直されること");

    dir.deleteRecursively();
}

// ---- clampNote の境界規則 ----
void testClampNoteBoundaries()
{
    beginTest ("clampNote boundaries");

    MidiRegion region;
    region.lengthPpq = Ppq::ticksPerBar; // 3840

    MidiNote note;
    note.startPpq = Ppq::ticksPerBar + 100; // リージョン外
    note.lengthPpq = 0;
    note.pitch = -3;
    note.velocity = 300;
    region.clampNote (note);

    expect (note.startPpq == Ppq::ticksPerBar - 1, "開始はリージョン末尾-1へクランプ");
    expect (note.lengthPpq == 1, "長さ最小1tick");
    expect (note.pitch == 0, "pitch下限0");
    expect (note.velocity == 127, "velocity上限127");

    // リージョン端を越えて伸びるノートは許容（再生時マスク）
    MidiNote longNote;
    longNote.startPpq = Ppq::ticksPerBar - 10;
    longNote.lengthPpq = Ppq::ticksPerQuarter * 8;
    region.clampNote (longNote);
    expect (longNote.startPpq == Ppq::ticksPerBar - 10 && longNote.lengthPpq == Ppq::ticksPerQuarter * 8,
            "リージョン端を越える長さは維持されること");
}

// ---- v2プロジェクト（offset/length無し）の読込 → v3保存 → 再読込 ----
void testClipOffsetsV2Migration()
{
    beginTest ("clip offsets v2 migration");
    const auto dir = makeTempDir();

    constexpr int wavLength = 4410;
    expect (writeTestWav (dir.getChildFile ("clip-001.wav"), wavLength), "テストWAVを書けること");

    const char* v2json = R"({
        "version": 2, "bpm": 120.0, "sampleRate": 44100.0, "nextId": 5,
        "tracks": [
            { "id": 1, "type": "audio", "name": "Vocal",
              "clips": [ { "file": "clip-001.wav", "startSample": 100 } ] }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (v2json);

    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr && project->tracks.size() == 1 && project->tracks[0].clips.size() == 1,
            "v2を読込めること");
    if (project == nullptr || project->tracks.empty() || project->tracks[0].clips.empty())
        { dir.deleteRecursively(); return; }

    expect (project->tracks[0].clips[0].offsetSamples == 0, "v2読込時はoffset=0");
    expect (project->tracks[0].clips[0].lengthSamples == wavLength, "v2読込時はlength=WAV全長");

    // 参照範囲を狭めて保存 → v3として再読込で維持される
    project->tracks[0].clips[0].offsetSamples = 1000;
    project->tracks[0].clips[0].lengthSamples = 2000;
    expect (project->save (error), "v3で保存できること");

    const auto saved = juce::JSON::parse (dir.getChildFile ("project.json").loadFileAsString());
    expect ((int) saved.getProperty ("version", 0) == Project::currentVersion,
            "現行バージョンで保存されること");

    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr && reloaded->tracks.size() == 1 && reloaded->tracks[0].clips.size() == 1,
            "v3を再読込できること");
    if (reloaded != nullptr && ! reloaded->tracks.empty() && ! reloaded->tracks[0].clips.empty())
    {
        expect (reloaded->tracks[0].clips[0].offsetSamples == 1000, "offset維持");
        expect (reloaded->tracks[0].clips[0].lengthSamples == 2000, "length維持");
        expect (reloaded->tracks[0].clips[0].startSample == 100, "startSample維持");
    }

    dir.deleteRecursively();
}

// ---- v3の不正なoffset/lengthのクランプ（オーバーフロー誘発値を含む）----
void testClipOffsetClamp()
{
    beginTest ("clip offset/length clamp");
    const auto dir = makeTempDir();

    constexpr int wavLength = 4410;
    expect (writeTestWav (dir.getChildFile ("clip-001.wav"), wavLength), "テストWAVを書けること");

    // clip1: int64最大値（offset+lengthを先に足すとオーバーフローする）→ 範囲ゼロでスキップ
    // clip2: 負のoffset → 0 に。lengthはそのまま収まる
    // clip3: length省略 → offset以降の残り全部
    // clip4: length過大 → 残り範囲へクランプ
    // clip5: length 0 → スキップ
    const char* v3json = R"({
        "version": 3, "bpm": 120.0, "sampleRate": 44100.0, "nextId": 5,
        "tracks": [
            { "id": 1, "type": "audio", "name": "X",
              "clips": [
                { "file": "clip-001.wav", "startSample": 0,
                  "offsetSamples": 9223372036854775807, "lengthSamples": 9223372036854775807 },
                { "file": "clip-001.wav", "startSample": 0, "offsetSamples": -100, "lengthSamples": 200 },
                { "file": "clip-001.wav", "startSample": 0, "offsetSamples": 400 },
                { "file": "clip-001.wav", "startSample": 0, "offsetSamples": 4000, "lengthSamples": 99999 },
                { "file": "clip-001.wav", "startSample": 0, "offsetSamples": 0, "lengthSamples": 0 }
              ] }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (v3json);

    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr && project->tracks.size() == 1, "読込めること（クラッシュしない）");
    if (project == nullptr || project->tracks.empty())
        { dir.deleteRecursively(); return; }

    const auto& clips = project->tracks[0].clips;
    expect (clips.size() == 3, "範囲ゼロの2クリップはスキップされること");
    expect (warnings.size() == 2, "スキップ分の警告が出ること");
    if (clips.size() == 3)
    {
        expect (clips[0].offsetSamples == 0 && clips[0].lengthSamples == 200, "負offsetは0へ");
        expect (clips[1].offsetSamples == 400 && clips[1].lengthSamples == wavLength - 400,
                "length省略はoffset以降の全部");
        expect (clips[2].offsetSamples == 4000 && clips[2].lengthSamples == wavLength - 4000,
                "過大lengthは残り範囲へクランプ");
    }

    dir.deleteRecursively();
}

// ---- 同一WAVを参照するクリップ間のバッファ共有 ----
void testSharedWavBufferOnLoad()
{
    beginTest ("shared wav buffer on load");
    const auto dir = makeTempDir();

    expect (writeTestWav (dir.getChildFile ("clip-001.wav"), 4410), "テストWAVを書けること");

    // 分割後相当: 同じWAVを参照する2クリップ（別トラックにも1つ）
    const char* v3json = R"({
        "version": 3, "bpm": 120.0, "sampleRate": 44100.0, "nextId": 5,
        "tracks": [
            { "id": 1, "type": "audio", "name": "A",
              "clips": [
                { "file": "clip-001.wav", "startSample": 0, "offsetSamples": 0, "lengthSamples": 2000 },
                { "file": "clip-001.wav", "startSample": 2000, "offsetSamples": 2000, "lengthSamples": 2410 }
              ] },
            { "id": 2, "type": "audio", "name": "B",
              "clips": [ { "file": "clip-001.wav", "startSample": 0 } ] }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (v3json);

    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr && project->tracks.size() == 2, "読込めること");
    if (project == nullptr || project->tracks.size() != 2
        || project->tracks[0].clips.size() != 2 || project->tracks[1].clips.size() != 1)
        { dir.deleteRecursively(); return; }

    expect (project->tracks[0].clips[0].audio.get() == project->tracks[0].clips[1].audio.get(),
            "同一トラック内の同一WAV参照はバッファ共有");
    expect (project->tracks[0].clips[0].audio.get() == project->tracks[1].clips[0].audio.get(),
            "トラックを跨いでもバッファ共有");

    // 保存 → 再読込でも共有が保たれる
    expect (project->save (error), "保存できること");
    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr, "再読込できること");
    if (reloaded != nullptr && reloaded->tracks.size() == 2
        && reloaded->tracks[0].clips.size() == 2 && reloaded->tracks[1].clips.size() == 1)
    {
        expect (reloaded->tracks[0].clips[0].audio.get() == reloaded->tracks[0].clips[1].audio.get()
                    && reloaded->tracks[0].clips[0].audio.get() == reloaded->tracks[1].clips[0].audio.get(),
                "再読込後もバッファ共有");
    }

    dir.deleteRecursively();
}

// ---- buildSnapshot が offset/length を ClipPlayback へ伝播すること ----
void testBuildSnapshotClipOffsets()
{
    beginTest ("buildSnapshot clip offsets");

    Project project;
    Track track;
    track.id = 1;

    Clip clip;
    clip.startSample = 500;
    clip.offsetSamples = 128;
    clip.lengthSamples = 256;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, 1024);
    track.clips.push_back (clip);

    // 範囲外のoffset/lengthはオーディオスレッドに渡る前に除外/クランプされる
    Clip broken = clip;
    broken.offsetSamples = 2000;
    track.clips.push_back (broken);
    project.tracks.push_back (std::move (track));

    auto snapshot = project.buildSnapshot();
    expect (snapshot->tracks.size() == 1 && snapshot->tracks[0].clips.size() == 1,
            "範囲ゼロのクリップはスナップショットに載らないこと");
    if (snapshot->tracks.size() == 1 && snapshot->tracks[0].clips.size() == 1)
    {
        const auto& playback = snapshot->tracks[0].clips[0];
        expect (playback.startSample == 500, "startSample伝播");
        expect (playback.offsetSamples == 128, "offsetSamples伝播");
        expect (playback.lengthSamples == 256, "lengthSamples伝播");
    }
}

// ---- PlaybackEngine が offset付きの左右クリップを連続した元音源として読むこと ----
void testEngineReadsClipOffsets()
{
    beginTest ("engine reads clip offsets");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int totalSamples = blockSize * 4;
    constexpr int splitAt = blockSize + 100; // ブロック境界とズラした分割点

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // ソース: サンプル値 = 位置に比例するランプ波（読み出し位置のズレを1サンプル単位で検出できる）
    auto source = std::make_shared<juce::AudioBuffer<float>> (1, totalSamples);
    for (int i = 0; i < totalSamples; ++i)
        source->setSample (0, i, (float) i / (float) totalSamples);

    Project project;
    Track track;
    track.id = 1;
    track.params->gain.store (1.0f);
    Clip left;
    left.startSample = 0;
    left.offsetSamples = 0;
    left.lengthSamples = splitAt;
    left.audio = source;
    Clip right;
    right.startSample = splitAt;
    right.offsetSamples = splitAt;
    right.lengthSamples = totalSamples - splitAt;
    right.audio = source;
    track.clips.push_back (std::move (left));
    track.clips.push_back (std::move (right));
    project.tracks.push_back (std::move (track));
    snapshots.push (project.buildSnapshot());

    juce::AudioBuffer<float> buffer (2, blockSize);
    engine.play();
    int mismatches = 0;
    for (int block = 0; block < 4; ++block)
    {
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        for (int i = 0; i < blockSize; ++i)
            if (std::abs (buffer.getSample (0, i) - source->getSample (0, block * blockSize + i)) > 1.0e-6f)
                ++mismatches;
    }
    engine.stop();

    expect (mismatches == 0, "分割された左右クリップの出力が元音源と全サンプル一致すること");
    snapshots.deleteRetired();
}

// ---- splitClip: 左右のoffset/length・バッファ共有・境界no-op ----
void testSplitClip()
{
    beginTest ("splitClip");

    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.startSample = 1000;
    clip.offsetSamples = 50;
    clip.lengthSamples = 400;
    clip.muted = true;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, 1000);
    for (int i = 0; i < 1000; ++i)
        clip.audio->setSample (0, i, (float) i);
    clip.buildPeakCache();

    Clip left, right;
    expect (splitClip (clip, 1100, left, right), "内側の分割点で分割できること");
    expect (left.startSample == 1000 && left.offsetSamples == 50 && left.lengthSamples == 100,
            "左: start/offset維持・length=分割点まで");
    expect (right.startSample == 1100 && right.offsetSamples == 150 && right.lengthSamples == 300,
            "右: 分割点から開始・offsetが左のぶん進む");
    expect (left.audio.get() == clip.audio.get() && right.audio.get() == clip.audio.get(),
            "左右ともソースバッファを共有すること");
    expect (left.fileName == clip.fileName && right.fileName == clip.fileName, "fileName共有");
    expect (left.muted && right.muted, "mutedが両方に引き継がれること");

    // peakCacheは参照範囲のみ: 右の先頭ピークはソースのoffset位置以降の値になる
    expect (left.peakCache.size() == 1 && right.peakCache.size() == 1, "peakCacheが再構築されること");
    if (! right.peakCache.empty())
        expect (juce::approximatelyEqual (right.peakCache[0], 449.0f), // max(|150..449|)
                "右のpeakCacheが自分の参照範囲から作られること");

    Clip unused1, unused2;
    expect (! splitClip (clip, 1000, unused1, unused2), "開始境界ちょうどはno-op");
    expect (! splitClip (clip, 1400, unused1, unused2), "終端境界ちょうどはno-op");
    expect (! splitClip (clip, 999, unused1, unused2), "範囲外はno-op");
}

// ---- splitMidiRegion: またぎノートKeep・右への相対シフト移動・境界no-op ----
void testSplitMidiRegion()
{
    beginTest ("splitMidiRegion");

    MidiRegion region;
    region.id = 10;
    region.startPpq = 3840; // 2小節目
    region.lengthPpq = 3840;
    region.muted = true;
    region.notes.push_back ({ 1, 60, 0, 100, 100 });      // 左に残る
    region.notes.push_back ({ 2, 62, 1900, 400, 90 });    // 分割点(相対1920)をまたぐ → Keep
    region.notes.push_back ({ 3, 64, 1920, 10, 80 });     // 分割点ちょうどから → 右へ
    region.notes.push_back ({ 4, 65, 3000, 100, 70 });    // 右へ

    MidiRegion left, right;
    expect (splitMidiRegion (region, 3840 + 1920, left, right), "内側の分割点で分割できること");
    expect (left.id == 10 && left.startPpq == 3840 && left.lengthPpq == 1920, "左: id/start維持");
    expect (right.id == 0, "右のidは未採番（呼び出し側で採番）");
    expect (right.startPpq == 3840 + 1920 && right.lengthPpq == 1920, "右: 分割点から残り");
    expect (left.muted && right.muted, "mutedが両方に引き継がれること");

    expect (left.notes.size() == 2, "左に2ノート");
    if (left.notes.size() == 2)
    {
        expect (left.notes[0].id == 1 && left.notes[0].startPpq == 0, "左ノート1維持");
        expect (left.notes[1].id == 2 && left.notes[1].startPpq == 1900 && left.notes[1].lengthPpq == 400,
                "またぎノートはフル長のまま左に残ること（Keep）");
    }
    expect (right.notes.size() == 2, "右に2ノート");
    if (right.notes.size() == 2)
    {
        expect (right.notes[0].id == 3 && right.notes[0].startPpq == 0 && right.notes[0].lengthPpq == 10,
                "分割点ちょうどのノートは右の先頭へ");
        expect (right.notes[1].id == 4 && right.notes[1].startPpq == 1080 && right.notes[1].velocity == 70,
                "右ノートは相対シフトされること");
    }

    MidiRegion unused1, unused2;
    expect (! splitMidiRegion (region, 3840, unused1, unused2), "開始境界ちょうどはno-op");
    expect (! splitMidiRegion (region, 3840 + 3840, unused1, unused2), "終端境界ちょうどはno-op");
    expect (! splitMidiRegion (region, 0, unused1, unused2), "範囲外はno-op");
}

// ---- セクションマーカー: ヘルパー・保存/読込ラウンドトリップ・不正値除外 ----
void testSectionMarkers()
{
    beginTest ("section markers");

    // set は昇順を保ち、同一位置への追加は種別変更として働く（位置は曲頭からの拍数・4拍=1小節）
    std::vector<SectionMarker> markers;
    SectionMarkers::set (markers, 32, SectionType::verse);   // bar 9
    SectionMarkers::set (markers, 0, SectionType::intro);    // bar 1
    SectionMarkers::set (markers, 64, SectionType::hook);    // bar 17
    expect (markers.size() == 3, "3個追加されること");
    expect (markers[0].startBeats == 0 && markers[1].startBeats == 32 && markers[2].startBeats == 64,
            "startBeats昇順を保つこと");
    expect (markers[1].bar() == 9 && markers[1].beat() == 0, "bar/beat換算が正しいこと");
    SectionMarkers::set (markers, 32, SectionType::bridge);
    expect (markers.size() == 3 && markers[1].type == SectionType::bridge,
            "同一位置へのsetは種別変更になること");

    // 自動採番: 1個だけなら番号なし、2個以上で出現順
    SectionMarkers::set (markers, 32, SectionType::verse); // 戻す
    SectionMarkers::set (markers, 96, SectionType::verse); // bar 25
    expect (SectionMarkers::displayName (markers, 0) == "intro", "1個だけの種別は番号なし");
    expect (SectionMarkers::displayName (markers, 1) == "verse1", "2個以上は出現順に採番(1)");
    expect (SectionMarkers::displayName (markers, 3) == "verse2", "2個以上は出現順に採番(2)");
    SectionMarkers::removeAt (markers, 1); // verse1を削除 → 残りが繰り上がる
    expect (SectionMarkers::displayName (markers, 2) == "verse", "1個に戻ったら番号が消えること");

    // clampStartBeats: 隣のマーカーの手前・>=0にクランプ（適用はしない）
    // markers = [0:intro, 64:hook, 96:verse]
    expect (SectionMarkers::clampStartBeats (markers, 1, 120) == 95, "次のマーカーの手前まで");
    expect (SectionMarkers::clampStartBeats (markers, 1, 0) == 1, "前のマーカーの直後まで");
    expect (SectionMarkers::clampStartBeats (markers, 0, -5) == 0, "先頭は曲頭まで");
    expect (SectionMarkers::clampStartBeats (markers, 2, 9999) == 9999, "最後のマーカーは上限なし");

    // 全6種＋拍オフセット付きの保存→読込ラウンドトリップ
    const auto dir = makeTempDir();
    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "createNewできること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    int beats = 0;
    for (auto type : SectionMarkers::allTypes)
        SectionMarkers::set (project->markers, beats += 32, type);
    SectionMarkers::set (project->markers, 34, SectionType::other); // bar 9 beat 2（拍オフセット）
    expect (project->save (error), "マーカー付きで保存できること");

    juce::StringArray warnings;
    auto reloaded = Project::load (project->directory, warnings, error);
    expect (reloaded != nullptr && warnings.isEmpty(), "警告なく再読込できること");
    if (reloaded != nullptr)
    {
        expect (reloaded->markers.size() == 7, "全6種＋拍オフセットが読み戻せること");
        for (size_t i = 0; i < reloaded->markers.size(); ++i)
        {
            expect (reloaded->markers[i].startBeats == project->markers[i].startBeats,
                    "startBeats（bar+beat）が維持されること");
            expect (reloaded->markers[i].type == project->markers[i].type,
                    "typeが維持されること");
        }
    }
    dir.deleteRecursively();
}

void testSectionMarkersInvalidLoad()
{
    beginTest ("section markers invalid load");
    const auto dir = makeTempDir();

    // bar 0・beat範囲外・未知type・重複位置 は警告付きで捨てる。
    // beat省略=0（旧形式）、同barでもbeat違いは別位置、bar 999（上限なし）は残す
    const char* json = R"({
        "version": 3, "bpm": 120.0, "sampleRate": 44100.0, "tracks": [],
        "markers": [
            { "bar": 0, "type": "intro" },
            { "bar": 5, "type": "chorus" },
            { "bar": 9, "type": "verse" },
            { "bar": 9, "type": "hook" },
            { "bar": 9, "beat": 2, "type": "bridge" },
            { "bar": 2, "beat": 4, "type": "intro" },
            { "bar": 2, "beat": -1, "type": "intro" },
            { "bar": 999, "type": "outro" }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (json);

    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr, "読込自体は成功すること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    expect (warnings.size() == 5, "不正マーカー5件の警告が出ること");
    expect (project->markers.size() == 3, "有効なマーカーだけ残ること");
    if (project->markers.size() == 3)
    {
        expect (project->markers[0].startBeats == 32 && project->markers[0].type == SectionType::verse,
                "重複位置は先勝ちであること（beat省略=0）");
        expect (project->markers[1].startBeats == 34 && project->markers[1].type == SectionType::bridge,
                "同barでもbeat違いは別位置として残ること");
        expect (project->markers[2].startBeats == 3992 && project->markers[2].type == SectionType::outro,
                "barの上限がないこと");
    }
    dir.deleteRecursively();
}

// ---- UndoStack: 構造編集の巻き戻し・ミキサー値の非対象・WAV GC保護 ----
void testUndoStack()
{
    beginTest ("UndoStack");

    Project project;
    Track first;
    first.id = 1;
    first.name = "A";
    project.tracks.push_back (std::move (first));

    UndoStack undo;
    expect (! undo.canUndo() && ! undo.canRedo(), "初期状態は履歴なし");

    // 編集種別を捨てて呼ぶ薄いラッパー（種別自体は下の専用ブロックで検証する）
    auto undoIt = [&undo, &project]
    { UndoStack::EditKind kind {}; return undo.undo (project, kind); };
    auto redoIt = [&undo, &project]
    { UndoStack::EditKind kind {}; return undo.redo (project, kind); };

    // トラック追加 → undo → redo
    undo.begin (project);
    Track second;
    second.id = 2;
    second.name = "B";
    project.tracks.push_back (std::move (second));

    expect (undoIt(), "undoできること");
    expect (project.tracks.size() == 1 && project.tracks[0].name == "A", "追加前に戻ること");
    expect (redoIt(), "redoできること");
    expect (project.tracks.size() == 2 && project.tracks[1].name == "B", "redoで復元されること");

    // MIDIリージョン編集のundo
    project.tracks[1].type = TrackType::midi;
    undo.begin (project);
    MidiRegion region;
    region.id = 3;
    project.tracks[1].midiRegions.push_back (region);
    expect (undoIt(), "リージョン追加をundoできること");
    expect (project.tracks[1].midiRegions.empty(), "リージョンが消えること");

    // ミキサー値（TrackParams）はundo対象外: begin後の変更がundoで巻き戻らない
    undo.begin (project);
    project.tracks.pop_back();
    project.tracks[0].params->mute.store (true);
    undoIt();
    expect (project.tracks.size() == 2, "構造は戻ること");
    expect (project.tracks[0].params->mute.load(), "ミキサー値はundoで巻き戻らないこと");

    // セクションマーカーの追加/種別変更/移動/削除のundo/redo
    undo.begin (project);
    SectionMarkers::set (project.markers, 0, SectionType::intro);
    expect (undoIt() && project.markers.empty(), "マーカー追加をundoできること");
    expect (redoIt() && project.markers.size() == 1, "マーカー追加をredoできること");

    undo.begin (project);
    SectionMarkers::set (project.markers, 0, SectionType::verse); // 同一位置 = 種別変更
    undoIt();
    expect (project.markers.size() == 1 && project.markers[0].type == SectionType::intro,
            "種別変更をundoできること");

    undo.begin (project);
    project.markers[0].startBeats = 18; // 移動（クランプ済みの値を直接書くUI側の操作と同じ）
    undoIt();
    expect (project.markers[0].startBeats == 0, "移動をundoできること");

    undo.begin (project);
    SectionMarkers::removeAt (project.markers, 0);
    expect (undoIt() && project.markers.size() == 1, "削除をundoできること");

    // マーカーを含むスナップショット化後もトラック編集のundoが維持されること（markersは巻き戻らない）
    undo.begin (project);
    Track third;
    third.id = 4;
    project.tracks.push_back (std::move (third));
    undoIt();
    expect (project.tracks.size() == 2, "トラック編集undoが維持されること");
    expect (project.markers.size() == 1 && project.markers[0].type == SectionType::intro,
            "トラック編集undoでマーカーが壊れないこと");

    // 編集種別: サンプル値だけの編集は「スナップショット再pushが不要」を呼び出し側へ伝える
    // （再pushすると発音中の音が頭から鳴り直すため、undo/redoでも区別が要る）
    {
        Project sampleProject;
        Track samplerTrack;
        samplerTrack.id = 1;
        samplerTrack.type = TrackType::midi;
        samplerTrack.instrument = InstrumentKind::sample;
        samplerTrack.sampleGain = 1.0f;
        sampleProject.tracks.push_back (std::move (samplerTrack));

        UndoStack kinds;
        kinds.begin (sampleProject, UndoStack::EditKind::sampleValue);
        sampleProject.tracks[0].sampleGain = 0.5f;

        auto kind = UndoStack::EditKind::structure;
        expect (kinds.undo (sampleProject, kind) && kind == UndoStack::EditKind::sampleValue,
                "サンプル値編集の種別がundoで返ること");
        expect (std::abs (sampleProject.tracks[0].sampleGain - 1.0f) < 1.0e-6f, "値が戻ること");

        kind = UndoStack::EditKind::structure;
        expect (kinds.redo (sampleProject, kind) && kind == UndoStack::EditKind::sampleValue,
                "redoでも同じ種別が返ること");
        expect (std::abs (sampleProject.tracks[0].sampleGain - 0.5f) < 1.0e-6f, "redoで値が進むこと");

        kinds.begin (sampleProject); // 既定は structure
        sampleProject.tracks.clear();
        expect (kinds.undo (sampleProject, kind) && kind == UndoStack::EditKind::structure,
                "構造編集の種別は structure になること");
    }

    // クリップ値（リージョンゲイン）の種別。復元時に「MIDI世代を据え置くpush」を選ぶための区別
    {
        Project clipProject;
        Track audioTrack;
        audioTrack.id = 1;
        Clip clip;
        clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, 100);
        clip.lengthSamples = 100;
        clip.gain = 1.0f;
        audioTrack.clips.push_back (std::move (clip));
        clipProject.tracks.push_back (std::move (audioTrack));

        UndoStack kinds;
        kinds.begin (clipProject, UndoStack::EditKind::clipValue);
        clipProject.tracks[0].clips[0].gain = 2.0f;

        auto kind = UndoStack::EditKind::structure;
        expect (kinds.undo (clipProject, kind) && kind == UndoStack::EditKind::clipValue,
                "クリップ値編集の種別がundoで返ること");
        expect (std::abs (clipProject.tracks[0].clips[0].gain - 1.0f) < 1.0e-6f, "ゲインが戻ること");

        kind = UndoStack::EditKind::structure;
        expect (kinds.redo (clipProject, kind) && kind == UndoStack::EditKind::clipValue,
                "redoでも同じ種別が返ること");
        expect (std::abs (clipProject.tracks[0].clips[0].gain - 2.0f) < 1.0e-6f, "redoでゲインが進むこと");
    }
}

void testSaveGcProtectsUndoWavs()
{
    beginTest ("save GC protects undo-referenced wavs");
    const auto dir = makeTempDir();

    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "createNewできること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    const auto wavFile = project->directory.getChildFile ("clip-001.wav");
    expect (writeTestWav (wavFile, 4410), "テストWAVを書けること");

    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.audio = Project::loadWav (wavFile);
    clip.lengthSamples = clip.audio != nullptr ? clip.audio->getNumSamples() : 0;
    project->tracks[0].clips.push_back (std::move (clip));

    UndoStack undo;
    undo.begin (*project);
    project->tracks[0].clips.clear(); // クリップ削除（undo履歴には残っている）

    expect (project->save (error, undo.referencedWavs()), "保存できること");
    expect (wavFile.existsAsFile(), "undo履歴が参照するWAVはGCされないこと");

    // 履歴なしで保存すればGCされる（従来どおり）
    UndoStack emptyUndo;
    expect (project->save (error, emptyUndo.referencedWavs()), "再保存できること");
    expect (! wavFile.existsAsFile(), "未参照WAVはGCされること");

    dir.deleteRecursively();
}

// ⌘C/⌘V のクリップボードが参照するWAVもGCから保護されること。
// undo履歴は深さ上限で押し出されるため、履歴とは別にクリップボードを保護源にする必要がある
void testSaveGcProtectsClipboardWav()
{
    beginTest ("save GC protects clipboard wav");
    const auto dir = makeTempDir();

    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "createNewできること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    const auto wavFile = project->directory.getChildFile ("clip-001.wav");
    expect (writeTestWav (wavFile, 4410), "テストWAVを書けること");

    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.audio = Project::loadWav (wavFile);
    clip.lengthSamples = clip.audio != nullptr ? clip.audio->getNumSamples() : 0;
    project->tracks[0].clips.push_back (std::move (clip));

    // ⌘C: クリップを丸ごとクリップボードへ（MainComponent::ItemClipboard 相当）
    const Clip clipboard = project->tracks[0].clips[0];

    // 元クリップを削除して保存。undo履歴が空でも（履歴から押し出された状況でも）
    // クリップボードが参照しているWAVは消えてはいけない
    project->tracks[0].clips.clear();
    juce::StringArray keep;
    keep.addIfNotAlreadyThere (clipboard.fileName);
    expect (project->save (error, keep), "保存できること");
    expect (wavFile.existsAsFile(), "クリップボードが参照するWAVはGCされないこと");

    // ⌘V: モデルへ戻せば以降はモデル参照で守られる（保護リストなしでも残る）
    project->tracks[0].clips.push_back (clipboard);
    expect (project->save (error), "ペースト後に保存できること");
    expect (wavFile.existsAsFile(), "ペースト後はモデル参照で守られること");
    expect (project->tracks[0].clips[0].audio != nullptr, "ペーストしたクリップが音声バッファを共有すること");

    dir.deleteRecursively();
}

// ---- ループ展開（appendRegionNotes / appendClipPlaybacks）----
// 通常再生・⌘B（buildSnapshot）と ⌘E（buildItemRender）が共有するヘルパー。
// 「各反復の末尾でマスクする」ことと「ミュートを判断しない」ことが要（前者を最終終端だけで
// 見ると境界をまたぐノートが次の反復へ持ち越され、後者を入れると⌘Eの明示選択優先が壊れる）
void testLoopExpansion()
{
    beginTest ("loop expansion");

    const auto bar = Ppq::ticksPerBar;

    MidiRegion region;
    region.startPpq = bar;      // 2小節目から
    region.lengthPpq = bar;     // 1小節ぶん
    region.notes.push_back ({ 1, 60, 0, bar * 3 / 2, 100 });               // 境界を越えて伸びるノート
    region.notes.push_back ({ 2, 64, Ppq::ticksPerQuarter, Ppq::ticksPerQuarter, 90 }); // 収まるノート

    // ループなし = 従来と同じ（1回・境界マスクあり）
    {
        std::vector<MidiNotePlayback> out;
        appendRegionNotes (region, -1, out);
        expect (out.size() == 2, "ループなしはノート2つ");
        expect (out[0].startPpq == bar && out[0].endPpq == bar * 2, "越境ノートは本体終端でマスク");
        expect (out[1].pitch == 64, "ピッチはそのまま");
    }

    // 2回ループ（本体＋2反復 = 3回鳴る）
    region.loopCount = 2;
    expect (region.totalLengthPpq() == bar * 3, "総再生長は本体長×3");
    {
        std::vector<MidiNotePlayback> out;
        appendRegionNotes (region, -1, out);
        expect (out.size() == 6, "3反復ぶんのノート数");
        bool startsOk = out.size() == 6, masksOk = startsOk, shortOk = startsOk;
        for (int r = 0; r < 3 && startsOk; ++r)
        {
            const auto repStart = bar + (juce::int64) r * bar;
            const auto& longNote = out[(size_t) r * 2];
            const auto& shortNote = out[(size_t) r * 2 + 1];
            startsOk = startsOk && longNote.startPpq == repStart;
            // 越境ノートは各反復の末尾で切れる。最終ループ終端だけを見る検査では通ってしまう
            masksOk = masksOk && longNote.endPpq == repStart + bar;
            shortOk = shortOk && shortNote.startPpq == repStart + Ppq::ticksPerQuarter;
        }
        expect (startsOk, "各反復の開始位置が本体長ずつ進むこと");
        expect (masksOk, "越境ノートが各反復の末尾で切れ、次の反復へ持ち越さないこと");
        expect (shortOk, "各反復の通常ノートが同じ相対位置に来ること");
    }

    // 固定ピッチ置換（GMドラム規則）とミュート非判断
    {
        std::vector<MidiNotePlayback> out;
        MidiRegion muted = region;
        muted.muted = true;
        appendRegionNotes (muted, 36, out);
        expect (! out.empty(), "ヘルパーはミュートを見ないこと（⌘Eの明示選択優先を壊さない）");
        expect (out[0].pitch == 36 && out[1].pitch == 36, "fixedPitchで全ノートを置換");
    }

    // ---- オーディオクリップ ----
    Clip clip;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, 4000);
    clip.audio->clear();
    clip.startSample = 1000;
    clip.offsetSamples = 500;
    clip.lengthSamples = 800;
    clip.loopCount = 3;
    expect (clip.totalLengthSamples() == 3200, "総再生長は本体長×4");
    {
        std::vector<ClipPlayback> out;
        appendClipPlaybacks (clip, out);
        expect (out.size() == 4, "4反復ぶんのエントリ");
        bool startsOk = out.size() == 4, offsetsOk = startsOk, lengthsOk = startsOk, sharedOk = startsOk;
        for (int r = 0; r < 4 && startsOk; ++r)
        {
            startsOk = startsOk && out[(size_t) r].startSample == 1000 + (juce::int64) r * 800;
            offsetsOk = offsetsOk && out[(size_t) r].offsetSamples == 500;
            lengthsOk = lengthsOk && out[(size_t) r].lengthSamples == 800;
            sharedOk = sharedOk && out[(size_t) r].audio == clip.audio;
        }
        expect (startsOk, "各反復の開始位置が本体長ずつ進むこと");
        expect (offsetsOk, "ソース参照位置は全反復で共通");
        expect (lengthsOk, "各反復の長さは本体長のまま");
        expect (sharedOk, "バッファは全反復で共有参照");
    }

    // 範囲外読みのクランプ（バッファ末尾を越える参照は詰められる。反復間隔はモデル長のまま）
    {
        Clip over = clip;
        over.offsetSamples = 3800;   // 残り200サンプルしかない
        over.loopCount = 1;
        std::vector<ClipPlayback> out;
        appendClipPlaybacks (over, out);
        expect (out.size() == 2, "クランプされても反復数は変わらない");
        expect (out[0].lengthSamples == 200, "バッファ残量へクランプ");
        expect (out[1].startSample == over.startSample + over.lengthSamples,
                "反復間隔はクランプ後でなくモデルの lengthSamples（描画と一致させる）");
    }
}

// プロジェクト選択画面のミニ波形: ループの転写（spreadLoopedBins）。
// ループ回数ぶん元サンプルを走査し直すと O(サンプル数 × 回数) でワーカーを占有するため、
// 「1反復ぶんの集約」と「反復ぶんの転写」を分けてある。ここは転写側の正しさと軽さを見る
void testSpreadLoopedBins()
{
    beginTest ("thumbnail loop bins");

    constexpr int bins = 240;
    // クリップ長 = 全体の 1/4。ローカルビンは先頭だけにピークがある形にする
    const juce::int64 clipLength = 1000, total = 4000;
    std::vector<float> local ((size_t) bins, 0.0f);
    local[0] = 1.0f;

    {
        std::vector<float> out ((size_t) bins, 0.0f);
        spreadLoopedBins (local, bins, 0, clipLength, 1, total, out);
        expect (out[0] > 0.0f, "ループなしでも先頭ビンへ転写されること");
        int nonZero = 0;
        for (auto v : out)
            nonZero += v > 0.0f;
        expect (nonZero >= 1 && nonZero <= 2, "1反復ぶんのピークが広がりすぎないこと");
    }

    {
        // 4反復 = 全長を埋める。各反復の先頭（全体の 0/4, 1/4, 2/4, 3/4 位置）にピークが出る
        std::vector<float> out ((size_t) bins, 0.0f);
        spreadLoopedBins (local, bins, 0, clipLength, 4, total, out);
        expect (out[0] > 0.0f && out[(size_t) (bins / 4)] > 0.0f
                    && out[(size_t) (bins / 2)] > 0.0f && out[(size_t) (bins * 3 / 4)] > 0.0f,
                "各反復の先頭位置へ転写されること");
    }

    {
        // 上限回数でも転写だけで済む（O(回数 × ビン数)）。数十msで返ること＝サンプル再走査していない
        std::vector<float> out ((size_t) bins, 0.0f);
        std::vector<float> dense ((size_t) bins, 0.5f);
        const auto start = juce::Time::getMillisecondCounterHiRes();
        spreadLoopedBins (dense, bins, 0, 1000, maxLoopCount, 1000LL * maxLoopCount, out);
        const auto elapsed = juce::Time::getMillisecondCounterHiRes() - start;
        // サンプルを再走査していれば桁違いに遅くなる（旧実装は48kHz・1分×999回で約8.7秒）
        expect (elapsed < 200.0, "上限回数でも200ms未満で終わること");
        int nonZero = 0;
        for (auto v : out)
            nonZero += v > 0.0f;
        expect (nonZero == bins, "全ビンが埋まること");
    }

    {
        // 丸め方向: 集約が floor(sample * bins / len) なので逆算は切り上げでないと1サンプルずれる。
        // 割り切れない長さ（241サンプルを240ビン）だと、そのずれがグローバルビンを跨いで見える。
        // ローカルbin 1 に入るのは sample 2（floor(2*240/241)=1）で、sample 1 は bin 0 に入る
        const juce::int64 oddLength = 241;
        std::vector<float> oddLocal ((size_t) bins, 0.0f);
        oddLocal[1] = 1.0f;

        std::vector<float> out ((size_t) bins, 0.0f);
        spreadLoopedBins (oddLocal, bins, 0, oddLength, 1, oddLength, out);
        expect (out[1] > 0.0f, "ローカルbin1がグローバルbin1へ転写されること（切り上げ逆算）");
        expect (out[0] == 0.0f, "1サンプル手前のビンへ漏れないこと（切り捨て逆算だとここが落ちる）");
    }

    {
        // 短いクリップ（サンプル数 < ビン数）でも、ピークのあるビンの位置がずれないこと
        const juce::int64 shortLength = 10;
        std::vector<float> shortLocal ((size_t) bins, 0.0f);
        shortLocal[(size_t) 24] = 1.0f; // sample 1 が入るビン（floor(1*240/10)=24）

        std::vector<float> out ((size_t) bins, 0.0f);
        spreadLoopedBins (shortLocal, bins, 0, shortLength, 1, shortLength, out);
        // sample 1 → グローバルビン floor(1*240/10) = 24
        expect (out[24] > 0.0f, "短いクリップでも対応するグローバルビンへ転写されること");
    }

    {
        // abortFlagで途中終了できること（終了フローの応答性）
        std::vector<float> out ((size_t) bins, 0.0f);
        std::atomic<bool> abort { true };
        spreadLoopedBins (local, bins, 0, clipLength, 4, total, out, &abort);
        int nonZero = 0;
        for (auto v : out)
            nonZero += v > 0.0f;
        expect (nonZero == 0, "abort済みなら何も書かないこと");
    }
}

// ---- buildSnapshot のノートフラット化（絶対PPQ変換・リージョン境界マスク・ソート）----
void testBuildSnapshotFlattensNotes()
{
    beginTest ("buildSnapshot flattens midi notes");

    Project project;
    Track track;
    track.id = 1;
    track.type = TrackType::midi;

    MidiRegion region;
    region.id = 2;
    region.startPpq = Ppq::ticksPerBar;      // 2小節目
    region.lengthPpq = Ppq::ticksPerBar;

    // リージョン相対: 3拍目から2小節分（リージョン端を大きくはみ出す→マスクされる）
    region.notes.push_back ({ 3, 60, Ppq::ticksPerQuarter * 2, Ppq::ticksPerBar * 2, 100 });
    // 1拍目（後ろのノートより先に鳴る。ソート確認用に後から追加）
    region.notes.push_back ({ 4, 64, 0, Ppq::ticksPerQuarter, 90 });
    track.midiRegions.push_back (region);
    project.tracks.push_back (std::move (track));

    auto snapshot = project.buildSnapshot();
    expect (snapshot->tracks.size() == 1, "トラック数1");
    const auto& notes = snapshot->tracks[0].notes;
    expect (notes.size() == 2, "ノート2つ");
    if (notes.size() != 2)
        return;

    expect (notes[0].startPpq == Ppq::ticksPerBar && notes[0].pitch == 64,
            "startPpq昇順にソートされること");
    expect (notes[0].endPpq == Ppq::ticksPerBar + Ppq::ticksPerQuarter, "絶対PPQ変換");
    expect (notes[1].startPpq == Ppq::ticksPerBar + Ppq::ticksPerQuarter * 2, "絶対PPQ変換2");
    expect (notes[1].endPpq == Ppq::ticksPerBar * 2, "リージョン端でマスクされること");

    // 固定ピッチ打楽器（Kick等）は再生時に全ノートのピッチが置き換わる
    project.tracks[0].drums = true;
    project.tracks[0].drumPitch = 36;
    auto drumSnapshot = project.buildSnapshot();
    expect (drumSnapshot->tracks[0].notes.size() == 2, "固定ピッチでもノート数は同じ");
    for (auto& note : drumSnapshot->tracks[0].notes)
        expect (note.pitch == 36, "固定ピッチに置き換わること");
}

// ---- SynthBank: プロジェクトのMIDIトラックに応じた生成・差し替え・破棄と発音 ----
void testSynthBank()
{
    beginTest ("SynthBank lifecycle and sound");

    Project project;
    Track track;
    track.id = 10;
    track.type = TrackType::midi;
    track.gmProgram = 0;
    project.tracks.push_back (std::move (track));

    SynthBank bank;
    expect (! bank.sync (project, 0.0, 512), "sampleRate未確定の間は何もしないこと");
    expect (bank.get (10) == nullptr, "未確定中はsynthなし");

    expect (bank.sync (project, 44100.0, 512), "レート確定後の初回syncで生成されること");
    auto synth = bank.get (10);
    expect (synth != nullptr && synth->plugin != nullptr, "synthが生成されること");
    expect (! bank.sync (project, 44100.0, 512), "変更がなければsyncはfalse");

    if (synth != nullptr && synth->plugin != nullptr)
    {
        expect (synth->midiChannel == 1, "非ドラムはch1");
        expect (synth->totalOutputChannels >= 2, "出力チャンネル数が記録されること");

        juce::AudioBuffer<float> buffer (synth->totalOutputChannels, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (synth->midiChannel, 60, (juce::uint8) 100), 0);
        float magnitude = 0.0f;
        for (int i = 0; i < 20; ++i)
        {
            buffer.clear();
            synth->plugin->processBlock (buffer, midi);
            midi.clear();
            magnitude = juce::jmax (magnitude, buffer.getMagnitude (0, buffer.getNumSamples()));
        }
        expect (magnitude > 0.001f, "SynthBank生成のsynthで音が出ること");
    }

    // 楽器変更 → インスタンス差し替え
    project.tracks[0].gmProgram = 48;
    expect (bank.sync (project, 44100.0, 512), "楽器変更でsyncがtrue");
    auto replaced = bank.get (10);
    expect (replaced != nullptr && replaced != synth, "別インスタンスに差し替わること");

    // ドラム指定 → ch10
    project.tracks[0].drums = true;
    bank.sync (project, 44100.0, 512);
    auto drumSynth = bank.get (10);
    expect (drumSynth != nullptr && drumSynth->midiChannel == 10, "ドラムはch10");

    // トラック削除 → エントリ破棄
    project.tracks.clear();
    expect (bank.sync (project, 44100.0, 512), "トラック削除でsyncがtrue");
    expect (bank.get (10) == nullptr, "削除後はsynthなし");
}

// ---- PlaybackEngine: MIDIトラックの再生・シーク再発音・停止消音・ミュート時のイベント継続 ----
void testPlaybackEngineMidi()
{
    beginTest ("PlaybackEngine midi rendering");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // bpm 120 / Strings（持続音で判定しやすい）。ノートは曲頭から8小節伸ばす
    Project project;
    Track track;
    track.id = 20;
    track.type = TrackType::midi;
    track.gmProgram = 48;
    MidiRegion region;
    region.id = 21;
    region.startPpq = 0;
    region.lengthPpq = Ppq::ticksPerBar * 8;
    region.notes.push_back ({ 22, 60, 0, Ppq::ticksPerBar * 8, 100 });
    track.midiRegions.push_back (region);
    project.tracks.push_back (std::move (track));

    SynthBank bank;
    bank.sync (project, sr, blockSize);
    auto pushSnapshot = [&]
    {
        auto snapshot = project.buildSnapshot();
        snapshot->tracks[0].synth = bank.get (20);
        snapshots.push (std::move (snapshot));
    };
    pushSnapshot();

    juce::AudioBuffer<float> buffer (2, blockSize);
    auto processBlocks = [&] (int count)
    {
        float magnitude = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            magnitude = juce::jmax (magnitude, buffer.getMagnitude (0, 0, blockSize));
        }
        return magnitude;
    };

    // 停止中は無音
    expect (processBlocks (5) < 0.0001f, "停止中は無音");

    // 再生でノートが鳴る
    engine.play();
    expect (processBlocks (20) > 0.001f, "再生でMIDIノートが鳴ること");

    // 一旦止めて減衰させ、ノートの途中（2小節目）へシーク → 跨ぎノートが再発音される
    engine.stop();
    processBlocks (100); // 停止エッジの消音＋リリース減衰
    transport.seekRequest.store ((juce::int64) (sr * 2.0)); // 2秒 = 2小節目頭
    engine.play();
    expect (processBlocks (20) > 0.001f, "シーク途中の持続音が再発音されること");

    // 停止で鳴り止む（リリース減衰後にほぼ無音）
    engine.stop();
    processBlocks (150);
    expect (processBlocks (5) < 0.01f, "停止後は減衰して静かになること");

    // ミュート中もイベントは処理される（ミックスだけ0）: ミュートで再生開始→無音、
    // 途中でミュート解除→ノートオンを取りこぼしていなければ即musicが聞こえる
    project.tracks[0].params->mute.store (true);
    transport.seekRequest.store (0);
    engine.play();
    expect (processBlocks (20) < 0.0001f, "ミュート中は完全に無音（加算ゲイン0）");
    project.tracks[0].params->mute.store (false);
    expect (processBlocks (10) > 0.001f, "ミュート解除直後から鳴ること（イベント送信は止まっていない）");
    engine.stop();
    processBlocks (200); // 消音＋リリース減衰

    // プレビュー発音: 停止中にFIFO経由でノートオン → 音が出る → 固定発音長（0.5秒）後に自動オフ
    auto synth = bank.get (20);
    expect (synth != nullptr, "synth取得");
    const auto countActive = [&synth] (int pitch)
    {
        int count = 0;
        if (synth != nullptr)
            for (int i = 0; i < synth->numActiveNotes; ++i)
                if (synth->activeNotes[i].pitch == pitch)
                    ++count;
        return count;
    };

    previewFifo.push ({ PreviewFifo::Command::Type::noteOn, 20, 72, 100 });
    expect (processBlocks (10) > 0.001f, "停止中のプレビュー発音が鳴ること");
    expect (countActive (72) == 1, "プレビュー中はactiveNotesに載ること");
    processBlocks (60); // 0.5秒（約43ブロック）を超えて回す → 自動ノートオフ
    expect (countActive (72) == 0, "固定発音長の経過で自動ノートオフされること");

    // ペインを閉じたときの打ち消し（allNotesOff）
    previewFifo.push ({ PreviewFifo::Command::Type::noteOn, 20, 60, 100 });
    processBlocks (3);
    previewFifo.push ({ PreviewFifo::Command::Type::allNotesOff, 20, 0, 0 });
    processBlocks (3);
    expect (countActive (60) == 0, "allNotesOffでプレビューが打ち消されること");

    // 再生中はプレビューコマンドを破棄する
    engine.play();
    processBlocks (2);
    previewFifo.push ({ PreviewFifo::Command::Type::noteOn, 20, 84, 100 });
    processBlocks (3);
    expect (countActive (84) == 0, "再生中のプレビューは破棄されること");
    engine.stop();
    processBlocks (10);

    snapshots.deleteRetired();
}

// ---- PlaybackEngine: トラックメーターは重なるクリップの「合算後」ピークを測る（混入なし）----
void testTrackLevelMeter()
{
    beginTest ("track level meter");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    auto makeClip = [] (float amplitude, int numSamples)
    {
        Clip clip;
        clip.startSample = 0;
        clip.lengthSamples = numSamples;
        clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            clip.audio->setSample (0, i, amplitude);
        return clip;
    };

    Project project;
    {
        Track track; // 0.6+0.6の重なり → 合算後ピークは約1.2（個別maxの0.6では0.9を超えない）
        track.id = 1;
        track.name = "overlap";
        track.params->gain.store (1.0f);
        track.clips.push_back (makeClip (0.6f, blockSize * 8));
        track.clips.push_back (makeClip (0.6f, blockSize * 8));
        project.tracks.push_back (std::move (track));
    }
    {
        Track track; // 0.3単独 → trackScratchのclear漏れがあると前トラックの1.2が混入する
        track.id = 2;
        track.name = "single";
        track.params->gain.store (1.0f);
        track.clips.push_back (makeClip (0.3f, blockSize * 8));
        project.tracks.push_back (std::move (track));
    }
    snapshots.push (project.buildSnapshot());

    juce::AudioBuffer<float> buffer (2, blockSize);
    engine.play();
    for (int i = 0; i < 4; ++i)
    {
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
    }
    engine.stop();

    const float peak0 = juce::jmax (project.tracks[0].params->peakL.exchange (0.0f),
                                    project.tracks[0].params->peakR.exchange (0.0f));
    const float peak1 = juce::jmax (project.tracks[1].params->peakL.exchange (0.0f),
                                    project.tracks[1].params->peakR.exchange (0.0f));
    expect (peak0 > 0.9f, "重なったクリップは合算後ピークで測ること（0.6+0.6 > 0.9）");
    expect (peak0 > 1.15f && peak0 < 1.25f, "合算後ピークが約1.2であること");
    expect (peak1 > 0.25f && peak1 < 0.35f, "他トラックの音が混入しないこと（clear漏れ検知）");

    // 出力自体も従来どおり両トラック合算で鳴っていること（スクラッチ経由への置き換えで無音化していない）
    buffer.clear();
    engine.play();
    juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
    engine.process (info);
    engine.stop();
    expect (buffer.getMagnitude (0, 0, blockSize) > 1.4f, "出力は全トラック合算（1.2+0.3）で鳴ること");

    snapshots.deleteRetired();
}

// ---- 再生中のノート編集: スナップショット差し替え時に消音→跨ぎノート再発音（鳴りっぱなし防止）----
void testSnapshotSwapDuringPlayback()
{
    beginTest ("snapshot swap during playback");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    Project project;
    Track track;
    track.id = 30;
    track.type = TrackType::midi;
    track.gmProgram = 48; // Strings（持続音）
    MidiRegion region;
    region.id = 31;
    region.startPpq = 0;
    region.lengthPpq = Ppq::ticksPerBar * 16;
    region.notes.push_back ({ 32, 60, 0, Ppq::ticksPerBar * 16, 100 });
    track.midiRegions.push_back (region);
    project.tracks.push_back (std::move (track));

    SynthBank bank;
    bank.sync (project, sr, blockSize);
    auto synth = bank.get (30);
    expect (synth != nullptr, "synth取得");
    const auto countActive = [&synth] (int pitch)
    {
        int count = 0;
        if (synth != nullptr)
            for (int i = 0; i < synth->numActiveNotes; ++i)
                if (synth->activeNotes[i].pitch == pitch)
                    ++count;
        return count;
    };
    auto pushSnapshot = [&]
    {
        auto snapshot = project.buildSnapshot();
        snapshot->tracks[0].synth = bank.get (30);
        snapshots.push (std::move (snapshot));
    };
    pushSnapshot();

    juce::AudioBuffer<float> buffer (2, blockSize);
    auto processBlocks = [&] (int count)
    {
        float magnitude = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            magnitude = juce::jmax (magnitude, buffer.getMagnitude (0, 0, blockSize));
        }
        return magnitude;
    };

    engine.play();
    expect (processBlocks (20) > 0.001f, "ロングノートが鳴ること");
    expect (countActive (60) == 1, "発音中はactiveNotesに載ること");

    // 再生中にノートを削除（新スナップショットへ差し替え）→ 鳴りっぱなしにならない
    project.tracks[0].midiRegions[0].notes.clear();
    snapshots.deleteRetired();
    pushSnapshot();
    processBlocks (5);
    expect (countActive (60) == 0, "再生中の削除で発音が止まること（鳴りっぱなし防止）");
    processBlocks (200); // リリース減衰
    expect (processBlocks (5) < 0.01f, "削除後は減衰して静かになること");

    // 再生中にノートを戻す → 跨ぎノートとして再発音される
    project.tracks[0].midiRegions[0].notes.push_back ({ 33, 60, 0, Ppq::ticksPerBar * 16, 100 });
    snapshots.deleteRetired();
    pushSnapshot();
    expect (processBlocks (10) > 0.001f, "再生中の追加で跨ぎノートが再発音されること");
    expect (countActive (60) == 1, "再発音後はactiveNotesに載ること");

    engine.stop();
    processBlocks (10);
    snapshots.deleteRetired();
}

// ---- オーディオ値のみの差し替え（Project::SnapshotChange::audioValuesOnly）----
// リージョンゲインをドラッグ中に音へ反映させるための経路。MIDIの構成世代を据え置くので、
// エンジンの「消音＋跨ぎノート再発音」が走らない＝鳴っているMIDIを乱さない
void testAudioValuesOnlySnapshot()
{
    beginTest ("audio-only snapshot swap");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    // ---- オーディオ: 世代を据え置いてもクリップゲインの変更が音に反映されること ----
    {
        TransportState transport;
        SnapshotExchange snapshots;
        PreviewFifo previewFifo;
        PlaybackEngine engine (transport, snapshots, previewFifo);
        engine.prepareToPlay (blockSize, sr);

        Project project;
        Track track;
        track.id = 1;
        track.params->gain.store (1.0f);
        Clip clip;
        clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, blockSize * 4);
        for (int i = 0; i < clip.audio->getNumSamples(); ++i)
            clip.audio->setSample (0, i, 0.4f);
        clip.lengthSamples = blockSize * 4;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
        snapshots.push (project.buildSnapshot());

        juce::AudioBuffer<float> buffer (2, blockSize);
        const auto measure = [&]
        {
            transport.seekRequest.store (0);
            engine.play();
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            const auto magnitude = buffer.getMagnitude (0, 0, blockSize);
            engine.stop();
            buffer.clear();
            engine.process (info);
            return magnitude;
        };

        expect (std::abs (measure() - 0.4f) < 0.001f, "ユニティでは素通し");

        project.tracks[0].clips[0].gain = 0.5f;
        snapshots.deleteRetired();
        snapshots.push (project.buildSnapshot (Project::SnapshotChange::audioValuesOnly));
        expect (std::abs (measure() - 0.2f) < 0.001f, "世代据え置きでもクリップゲインが音に反映されること");

        snapshots.deleteRetired();
    }

    // ---- MIDI: 世代据え置きでは消音＋再発音が走らないこと（＋通常pushでは走ること） ----
    {
        TransportState transport;
        SnapshotExchange snapshots;
        PreviewFifo previewFifo;
        PlaybackEngine engine (transport, snapshots, previewFifo);
        engine.prepareToPlay (blockSize, sr);

        Project project;
        Track track;
        track.id = 30;
        track.type = TrackType::midi;
        track.gmProgram = 48; // Strings（持続音）
        MidiRegion region;
        region.id = 31;
        region.startPpq = 0;
        region.lengthPpq = Ppq::ticksPerBar * 16;
        region.notes.push_back ({ 32, 60, 0, Ppq::ticksPerBar * 16, 100 });
        track.midiRegions.push_back (region);
        project.tracks.push_back (std::move (track));

        SynthBank bank;
        bank.sync (project, sr, blockSize);
        auto synth = bank.get (30);
        expect (synth != nullptr, "synth取得");
        const auto countActive = [&synth] (int pitch)
        {
            int count = 0;
            if (synth != nullptr)
                for (int i = 0; i < synth->numActiveNotes; ++i)
                    if (synth->activeNotes[i].pitch == pitch)
                        ++count;
            return count;
        };
        const auto push = [&] (Project::SnapshotChange change)
        {
            snapshots.deleteRetired();
            auto snapshot = project.buildSnapshot (change);
            snapshot->tracks[0].synth = bank.get (30);
            snapshots.push (std::move (snapshot));
        };
        juce::AudioBuffer<float> buffer (2, blockSize);
        const auto processBlocks = [&] (int count)
        {
            for (int i = 0; i < count; ++i)
            {
                buffer.clear();
                juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
                engine.process (info);
            }
        };

        push (Project::SnapshotChange::midiStructure);
        engine.play();
        processBlocks (20);
        expect (countActive (60) == 1, "ロングノートが発音中であること");

        // ノートを消したスナップショットを audioValuesOnly で渡す＝消音がスキップされる。
        // 「MIDI構成を変えていない」という契約を破ると鳴りっぱなしになる、という裏付けでもある
        project.tracks[0].midiRegions[0].notes.clear();
        push (Project::SnapshotChange::audioValuesOnly);
        processBlocks (5);
        expect (countActive (60) == 1, "世代据え置きでは消音＋再発音が走らないこと");

        // 同じ状態を通常pushで渡すと、従来どおり安全機構（消音）が働く
        push (Project::SnapshotChange::midiStructure);
        processBlocks (5);
        expect (countActive (60) == 0, "通常pushでは消音されること（鳴りっぱなし防止は健在）");

        // ⌘Bのバウンス用構築（offlineRender）はエンジンへ渡さないので世代に触らない。
        // ここで進んでしまうと「バウンス → リージョンゲイン調整」の順で鳴っているMIDIが鳴り直す
        project.tracks[0].midiRegions[0].notes.push_back ({ 33, 60, 0, Ppq::ticksPerBar * 16, 100 });
        push (Project::SnapshotChange::midiStructure);
        processBlocks (10);
        expect (countActive (60) == 1, "ノートを戻して再発音されること");

        auto discarded = project.buildSnapshot (Project::SnapshotChange::offlineRender); // ⌘B相当
        discarded.reset();
        project.tracks[0].midiRegions[0].notes.clear(); // 消音が走ったかを見るための細工
        push (Project::SnapshotChange::audioValuesOnly);
        processBlocks (5);
        expect (countActive (60) == 1,
                "バウンス用の構築を挟んでも据え置きが効くこと（世代を進めていない）");

        engine.stop();
        processBlocks (10);
        snapshots.deleteRetired();
    }
}

// ---- イベント上限超過: 捨てたノートの終端で同ピッチの別ノートを誤って止めない ----
void testOverflowDoesNotKillOtherNotes()
{
    beginTest ("note-on overflow does not kill other notes");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // 1ブロック ≈ 22.3 tick（bpm120/44.1kHz）。全ノートをtick 0に置き、
    // [A: pitch60ロング] → [フィラー1023個: pitch62・10tickの短ノート] → [C: pitch60・50tick]
    // の順で上限1024を消費させ、Cのノートオンだけを捨てさせる（stable_sortで並び順は維持される）
    Project project;
    Track track;
    track.id = 40;
    track.type = TrackType::midi;
    track.gmProgram = 48;
    MidiRegion region;
    region.id = 41;
    region.startPpq = 0;
    region.lengthPpq = Ppq::ticksPerBar * 16;
    juce::uint64 nextNoteId = 100;
    region.notes.push_back ({ nextNoteId++, 60, 0, Ppq::ticksPerBar * 8, 100 }); // A
    for (int i = 0; i < 1023; ++i)
        region.notes.push_back ({ nextNoteId++, 62, 0, 10, 100 });               // フィラー（ブロック内で完結）
    region.notes.push_back ({ nextNoteId++, 60, 0, 50, 100 });                   // C（捨てられる・終端はブロック3）
    track.midiRegions.push_back (std::move (region));
    project.tracks.push_back (std::move (track));

    SynthBank bank;
    bank.sync (project, sr, blockSize);
    auto synth = bank.get (40);
    expect (synth != nullptr, "synth取得");
    {
        auto snapshot = project.buildSnapshot();
        snapshot->tracks[0].synth = bank.get (40);
        snapshots.push (std::move (snapshot));
    }

    juce::AudioBuffer<float> buffer (2, blockSize);
    auto processBlocks = [&] (int count)
    {
        for (int i = 0; i < count; ++i)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
        }
    };

    engine.play();
    processBlocks (10); // Cの終端（50tick ≈ ブロック3）を確実に通過させる

    expect (transport.midiDroppedNoteOns.load() > 0, "上限超過で捨てたノートオンが計上されること");
    int activeAt60 = 0;
    for (int i = 0; i < synth->numActiveNotes; ++i)
        if (synth->activeNotes[i].pitch == 60)
            ++activeAt60;
    expect (activeAt60 == 1, "捨てたノートの終端で、送信済みの同ピッチノートが止められないこと");

    engine.stop();
    processBlocks (5);
    snapshots.deleteRetired();
}

// ---- サンプル音源のテスト用ヘルパー ----

// 全サンプルが同じ値のバッファ（レベル判定を単純にする）
std::shared_ptr<juce::AudioBuffer<float>> makeFlatSample (int numSamples, float value,
                                                          int numChannels = 1)
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>> (numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
        juce::FloatVectorOperations::fill (buffer->getWritePointer (ch), value, numSamples);
    return buffer;
}

// 0→1へ上がるランプ（サンプル単位の一致・startOffsetの検証用）
std::shared_ptr<juce::AudioBuffer<float>> makeRampSample (int numSamples)
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>> (1, numSamples);
    for (int i = 0; i < numSamples; ++i)
        buffer->setSample (0, i, (float) i / (float) numSamples);
    return buffer;
}

// ボイスが全部消えるまでのブロック数（超えたら-1）
int blocksUntilSilent (SamplerEngine& sampler, juce::AudioBuffer<float>& buffer, int limit)
{
    juce::MidiBuffer empty;
    for (int i = 0; i < limit; ++i)
    {
        buffer.clear();
        sampler.processBlock (buffer, empty);
        if (sampler.numActiveVoices() == 0)
            return i + 1;
    }
    return -1;
}

// PlaybackEngine結合テスト用の SynthInstance（SynthBank を介さず手で組む）
std::shared_ptr<SynthInstance> makeSamplerInstance (std::shared_ptr<juce::AudioBuffer<float>> audio,
                                                    double sourceRate, double deviceRate,
                                                    int blockSize, bool pitchFollow)
{
    auto synth = std::make_shared<SynthInstance>();
    synth->sampler = std::make_unique<SamplerEngine> (audio, sourceRate, deviceRate);
    synth->sampler->setPitchFollow (pitchFollow);
    synth->midiChannel = 1;
    synth->preparedSampleRate = deviceRate;
    synth->preparedBlockSize = juce::jmax (4096, blockSize);
    synth->totalOutputChannels = 2;
    synth->oneShot.store (! pitchFollow);
    return synth;
}

// ---- v8: サンプル音源の保存/読込ラウンドトリップ（クランプ・欠損・SR復元）とGC ----
void testSamplerProjectRoundtrip()
{
    beginTest ("sampler project roundtrip and gc");
    const auto dir = makeTempDir();

    // 44.1kHz素材（プロジェクトSRは48kHz＝サンプルは元SRのまま保存される）
    constexpr double sampleRate = 44100.0;
    auto audio = makeFlatSample ((int) sampleRate, 0.5f);
    expect (writeBufferWav (dir.getChildFile ("instr-001.wav"), *audio, sampleRate),
            "サンプルWAVを書けること");

    {
        Project project;
        project.directory = dir;
        project.sampleRate = 48000.0;

        Track track;
        track.id = 1;
        track.type = TrackType::midi;
        track.name = "kick track";
        track.gmProgram = 48; // GM側の設定も保持されること
        track.instrument = InstrumentKind::sample;
        track.sampleFile = "instr-001.wav";
        track.sampleName = "kick";
        track.samplePitchFollow = true;
        track.sampleMono = true;
        track.sampleRootNote = 48;
        track.sampleGain = 0.5f;
        track.sampleStartOffset = 100;
        track.sampleAudio = audio;
        track.sampleSourceRate = sampleRate;
        project.tracks.push_back (std::move (track));

        juce::String error;
        expect (project.save (error), "保存できること");
    }

    const auto json = juce::JSON::parse (dir.getChildFile ("project.json").loadFileAsString());
    expect ((int) json.getProperty ("version", 0) == Project::currentVersion,
            "現行バージョンで保存されること");

    {
        juce::StringArray warnings;
        juce::String error;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr && loaded->tracks.size() == 1, "読み込めること");
        if (loaded == nullptr || loaded->tracks.empty())
        {
            dir.deleteRecursively();
            return;
        }
        const auto& track = loaded->tracks[0];
        expect (track.instrument == InstrumentKind::sample, "instrument=sample が復元されること");
        expect (track.sampleFile == "instr-001.wav", "sampleFileが復元されること");
        expect (track.sampleName == "kick", "sampleNameが復元されること");
        expect (track.samplePitchFollow, "samplePitchFollowが復元されること");
        expect (track.sampleMono, "sampleMonoが復元されること");
        expect (track.sampleRootNote == 48, "sampleRootNoteが復元されること");
        expect (std::abs (track.sampleGain - 0.5f) < 1.0e-6f, "sampleGainが復元されること");
        expect (track.sampleStartOffset == 100, "sampleStartOffsetが復元されること");
        expect (track.gmProgram == 48, "GM側の設定も保持されること");
        expect (track.sampleAudio != nullptr, "サンプルWAVがメモリへ載ること");
        expect (juce::approximatelyEqual (track.sampleSourceRate, sampleRate),
                "元SR（44.1k）がWAVから復元されること");
        expect (! track.samplePeakCache.empty(), "ピークキャッシュが作られること");
        expect (warnings.isEmpty(), "正常なプロジェクトで警告が出ないこと");

        // SR復元の意味: 48kHz環境での再生長が保存前と同じ比率になる（sampleSourceRateが0なら壊れる）
        SamplerEngine sampler (track.sampleAudio, track.sampleSourceRate, 48000.0);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, track.sampleRootNote, (juce::uint8) 127), 0);
        buffer.clear();
        sampler.processBlock (buffer, midi);
        const int blocks = 1 + blocksUntilSilent (sampler, buffer, 300);
        const int expected = (int) std::ceil (sampleRate * 48000.0 / sampleRate / 512.0);
        expect (std::abs (blocks - expected) <= 2, "再読込後も再生長が比率どおりであること");
    }

    // ---- 既存v8ファイル互換: sampleMono キーが無ければOFF（後から足したキー）----
    {
        auto file = dir.getChildFile ("project.json");
        auto text = file.loadFileAsString();
        expect (text.contains ("\"sampleMono\""), "保存JSONにsampleMonoがあること");
        // キーごと削除（後ろのキーとの区切りも一緒に消す）
        const auto removed = text.replace ("\"sampleMono\": true,", "");
        expect (! removed.contains ("\"sampleMono\""), "テスト用にキーを削除できること");
        file.replaceWithText (removed);

        juce::StringArray warnings;
        juce::String error;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr && ! loaded->tracks.empty(), "キー欠損でも読めること");
        if (loaded != nullptr && ! loaded->tracks.empty())
            expect (! loaded->tracks[0].sampleMono, "sampleMonoが無いv8ファイルはOFFとして読むこと");

        file.replaceWithText (text); // 後続のクランプ検証のために戻す
    }

    // ---- 不正値のクランプ（手編集JSONへの防御）----
    {
        auto text = dir.getChildFile ("project.json").loadFileAsString();
        text = text.replace ("\"sampleRootNote\": 48", "\"sampleRootNote\": 999")
                   .replace ("\"sampleGain\": 0.5", "\"sampleGain\": 9.0")
                   .replace ("\"sampleStartOffset\": 100", "\"sampleStartOffset\": 99999999");
        dir.getChildFile ("project.json").replaceWithText (text);

        juce::StringArray warnings;
        juce::String error;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr && ! loaded->tracks.empty(), "クランプ検証用に読めること");
        if (loaded != nullptr && ! loaded->tracks.empty())
        {
            const auto& track = loaded->tracks[0];
            expect (track.sampleRootNote == 127, "sampleRootNoteが0..127へクランプされること");
            expect (std::abs (track.sampleGain - GainScale::maxLinear()) < 1.0e-6f,
                    "sampleGainが+12dB相当へクランプされること");
            expect (track.sampleStartOffset < (juce::int64) track.sampleAudio->getNumSamples(),
                    "sampleStartOffsetがバッファ長へクランプされること");
        }
    }

    // ---- 下限のクランプ（UIが表現できない小さい値を残すと表示と実音が食い違う）----
    {
        auto text = dir.getChildFile ("project.json").loadFileAsString()
                        .replace ("\"sampleGain\": 9.0", "\"sampleGain\": 0.05");
        dir.getChildFile ("project.json").replaceWithText (text);

        juce::StringArray warnings;
        juce::String error;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr && ! loaded->tracks.empty(), "下限クランプ検証用に読めること");
        if (loaded != nullptr && ! loaded->tracks.empty())
            expect (std::abs (loaded->tracks[0].sampleGain - GainScale::minLinear()) < 1.0e-6f,
                    "sampleGainが-12dB相当へクランプされること");
    }

    // ---- GAIN範囲の端が保存→読込で変わらないこと ----
    {
        juce::StringArray warnings;
        juce::String error;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr && ! loaded->tracks.empty(), "ラウンドトリップ検証用に読めること");
        if (loaded != nullptr && ! loaded->tracks.empty())
        {
            for (const float gain : { GainScale::minLinear(), 1.0f, GainScale::maxLinear() })
            {
                loaded->tracks[0].sampleGain = gain;
                juce::String saveError;
                expect (loaded->save (saveError), "GAINを変えて保存できること");

                juce::StringArray warnings2;
                auto reloaded = Project::load (dir, warnings2, saveError);
                expect (reloaded != nullptr && ! reloaded->tracks.empty(), "保存したものを再読込できること");
                if (reloaded != nullptr && ! reloaded->tracks.empty())
                    expect (std::abs (reloaded->tracks[0].sampleGain - gain) < 1.0e-6f,
                            "±12dBの端がクランプで削られずに復元されること");
            }
        }
    }

    // ---- ファイル欠損: 警告を出してそのトラックは無音（勝手にGMへ戻さない）----
    {
        auto text = dir.getChildFile ("project.json").loadFileAsString()
                        .replace ("instr-001.wav", "instr-missing.wav");
        dir.getChildFile ("project.json").replaceWithText (text);

        juce::StringArray warnings;
        juce::String error;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr && ! loaded->tracks.empty(), "欠損でもプロジェクトは開けること");
        if (loaded != nullptr && ! loaded->tracks.empty())
        {
            const auto& track = loaded->tracks[0];
            expect (track.instrument == InstrumentKind::sample, "欠損でも勝手にGMへ戻さないこと");
            expect (track.sampleAudio == nullptr, "欠損時はバッファなし（無音）");
            expect (juce::approximatelyEqual (track.sampleSourceRate, sampleRate),
                    "欠損でもJSONに記録した元SRが復元されること（診断用）");
            expect (! track.hasSample(), "バッファが無ければ hasSample は false");

            // 欠損状態で保存し直しても記録値が0で上書きされないこと
            juce::String saveError;
            expect (loaded->save (saveError), "欠損状態でも保存できること");
            juce::StringArray warnings2;
            auto reloaded = Project::load (dir, warnings2, saveError);
            expect (reloaded != nullptr && ! reloaded->tracks.empty(), "再読込できること");
            if (reloaded != nullptr && ! reloaded->tracks.empty())
                expect (juce::approximatelyEqual (reloaded->tracks[0].sampleSourceRate, sampleRate),
                        "欠損状態の保存→再読込でも元SRが残ること");
        }
        expect (! warnings.isEmpty(), "欠損サンプルは警告されること");
    }

    dir.deleteRecursively();

    // ---- GC: 未参照の instr-*.wav は消し、参照中・undo履歴のものは残す ----
    {
        const auto gcDir = makeTempDir();
        expect (writeBufferWav (gcDir.getChildFile ("instr-001.wav"), *audio, sampleRate), "参照中サンプル");
        expect (writeBufferWav (gcDir.getChildFile ("instr-002.wav"), *audio, sampleRate), "undo履歴のサンプル");
        expect (writeBufferWav (gcDir.getChildFile ("instr-003.wav"), *audio, sampleRate), "孤児サンプル");

        Project project;
        project.directory = gcDir;
        Track track;
        track.id = 1;
        track.type = TrackType::midi;
        track.instrument = InstrumentKind::sample;
        track.sampleFile = "instr-002.wav"; // まずこれを参照した状態を履歴へ積む
        track.sampleAudio = audio;
        track.sampleSourceRate = sampleRate;
        project.tracks.push_back (std::move (track));

        UndoStack undoStack;
        undoStack.begin (project);                        // instr-002.wav を履歴が参照
        project.tracks[0].sampleFile = "instr-001.wav";   // 差し替え（現行は instr-001.wav）

        juce::String error;
        expect (project.save (error, undoStack.referencedWavs()), "保存できること");
        expect (gcDir.getChildFile ("instr-001.wav").existsAsFile(), "参照中のサンプルは残ること");
        expect (gcDir.getChildFile ("instr-002.wav").existsAsFile(),
                "undo履歴から参照されているサンプルは残ること");
        expect (! gcDir.getChildFile ("instr-003.wav").existsAsFile(), "未参照のサンプルは消えること");
        gcDir.deleteRecursively();
    }
}

// ---- SynthBank: サンプル音源の生成・差し替え・atomic更新・音程モード切替の停止要求 ----
void testSynthBankSampler()
{
    beginTest ("SynthBank sampler instances");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    Project project;
    Track track;
    track.id = 50;
    track.type = TrackType::midi;
    track.instrument = InstrumentKind::sample;
    track.sampleFile = "instr-001.wav";
    track.sampleName = "kick";
    track.sampleAudio = makeFlatSample ((int) sr, 0.5f); // 1秒
    track.sampleSourceRate = sr;
    project.tracks.push_back (std::move (track));
    auto& model = project.tracks[0];

    SynthBank bank;
    expect (bank.sync (project, sr, blockSize), "初回syncでサンプラーが生成されること");
    auto synth = bank.get (50);
    expect (synth != nullptr && synth->sampler != nullptr, "samplerが生成されること");
    expect (synth != nullptr && synth->plugin == nullptr, "サンプル音源のときpluginは持たないこと");
    expect (synth != nullptr && synth->oneShot.load(), "既定（固定モード）はoneShot=true");
    expect (synth != nullptr && synth->totalOutputChannels == 2, "サンプラーの出力は2ch");
    expect (! bank.sync (project, sr, blockSize), "変更がなければsyncはfalse");
    if (synth == nullptr || synth->sampler == nullptr)
        return;

    // 音量・頭カット・ルート音はインスタンスを作り直さない（発音中の音を切らないため）
    model.sampleGain = 0.5f;
    model.sampleStartOffset = 100;
    model.sampleRootNote = 48;
    expect (! bank.sync (project, sr, blockSize), "音量・頭カット・ルート音の変更ではsyncはfalse");
    expect (bank.get (50) == synth, "同じインスタンスのまま（atomic更新のみ）");

    // 音程モードの変化は「同じインスタンスのまま停止要求を立てる」
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
    buffer.clear();
    synth->sampler->processBlock (buffer, midi);
    midi.clear();
    expect (synth->sampler->numActiveVoices() == 1, "発音中のボイスがあること");

    model.samplePitchFollow = true;
    expect (bank.sync (project, sr, blockSize), "音程モードの変化でsyncがtrue（再pushが必要）");
    expect (bank.get (50) == synth, "音程モード変更でもインスタンスは作り直さないこと");
    expect (! synth->oneShot.load(), "追従モードではoneShot=false");
    buffer.clear();
    synth->sampler->processBlock (buffer, midi);
    expect (synth->sampler->numActiveVoices() == 0,
            "音程モード切替でrequestStopAllが立ち、全ボイスが止まること");

    // サンプルの差し替えはインスタンスごと交換
    model.sampleFile = "instr-002.wav";
    expect (bank.sync (project, sr, blockSize), "サンプル差し替えでsyncがtrue");
    auto replaced = bank.get (50);
    expect (replaced != nullptr && replaced != synth, "別インスタンスに差し替わること");
    expect (replaced != nullptr && replaced->sampler != nullptr, "差し替え後もsamplerであること");

    // デバイスSR変更もインスタンス差し替え（再生比率が変わる）
    expect (bank.sync (project, 48000.0, blockSize), "SR変更でsyncがtrue");
    expect (bank.get (50) != replaced, "SR変更で別インスタンスになること");

    // GMへ戻す → pluginを持つインスタンスへ
    model.instrument = InstrumentKind::gm;
    model.gmProgram = 48;
    expect (bank.sync (project, 48000.0, blockSize), "GMへ戻すとsyncがtrue");
    auto gm = bank.get (50);
    expect (gm != nullptr && gm->plugin != nullptr, "GMへ戻すとpluginを持つこと");
    expect (gm != nullptr && gm->sampler == nullptr, "GMのときsamplerは持たないこと");
    expect (gm != nullptr && ! gm->oneShot.load(), "GMはoneShot=false（resoundは従来どおり）");

    // サンプル欠損（読込失敗）は無音: synthを作らず、再試行もしない
    model.instrument = InstrumentKind::sample;
    model.sampleAudio = nullptr;
    model.sampleSourceRate = 0.0;
    expect (bank.sync (project, 48000.0, blockSize), "欠損サンプルへの切り替えでもsyncがtrue");
    expect (bank.get (50) == nullptr, "サンプル欠損時はsynthなし（トラックは無音）");
    expect (! bank.sync (project, 48000.0, blockSize), "失敗はキャッシュされ再試行しないこと");
}

// ---- SamplerEngine: 発音・消音・レート・ボイス管理（オーディオスレッド側の単体検証）----
void testSamplerEngine()
{
    beginTest ("SamplerEngine voices and rates");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    const int sampleLength = (int) sr;                      // 1秒
    const int blocksPerSample = sampleLength / blockSize;    // 86ブロック
    auto flat = makeFlatSample (sampleLength, 0.5f);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    // 1ブロック分レンダリングしてch0のピークを返す（midiは消費される）
    auto renderInto = [&buffer, &midi] (SamplerEngine& sampler, int blocks)
    {
        float mag = 0.0f;
        for (int i = 0; i < blocks; ++i)
        {
            buffer.clear();
            sampler.processBlock (buffer, midi);
            midi.clear();
            mag = juce::jmax (mag, buffer.getMagnitude (0, 0, blockSize));
        }
        return mag;
    };

    // ---- 固定モード（One Shot）----
    {
        SamplerEngine sampler (flat, sr, sr);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        expect (renderInto (sampler, 1) > 0.49f, "固定: noteOnで鳴ること");

        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0); // 8分音符相当で来るノートオフ
        expect (renderInto (sampler, 1) > 0.49f, "固定: noteOffでレベルが落ちないこと");
        expect (renderInto (sampler, blocksPerSample - 10) > 0.49f,
                "固定: ノート長を無視してサンプル末尾まで鳴ること");
        expect (blocksUntilSilent (sampler, buffer, 20) > 0, "固定: サンプル末尾で自然に停止すること");
    }

    // CC123（停止・シーク・サイクル折返しで送られる）では止まる
    {
        SamplerEngine sampler (flat, sr, sr);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        renderInto (sampler, 1);
        midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
        renderInto (sampler, 1); // このブロック内で5msフェード
        expect (sampler.numActiveVoices() == 0, "固定: CC123でボイスが消えること");
        expect (renderInto (sampler, 2) < 1.0e-6f, "固定: CC123の後は無音になること");
    }

    // requestStopAll（音程モード切替時にSynthBankが立てる）も同じく止める
    {
        SamplerEngine sampler (flat, sr, sr);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        renderInto (sampler, 1);
        sampler.requestStopAll();
        renderInto (sampler, 1);
        expect (sampler.numActiveVoices() == 0, "requestStopAllで全ボイスが止まること");
    }

    // ---- 追従モード ----
    {
        SamplerEngine sampler (flat, sr, sr);
        sampler.setPitchFollow (true);
        sampler.setRootNote (60);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        expect (renderInto (sampler, 1) > 0.49f, "追従: noteOnで鳴ること");
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        renderInto (sampler, 1);
        expect (sampler.numActiveVoices() == 0, "追従: noteOffでノート長どおり止まること");
        expect (renderInto (sampler, 2) < 1.0e-6f, "追従: noteOff後は無音になること");
    }

    // rootNote+12 は再生長がおよそ半分
    {
        SamplerEngine sampler (flat, sr, sr);
        sampler.setPitchFollow (true);
        sampler.setRootNote (60);
        midi.addEvent (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 127), 0);
        renderInto (sampler, 1);
        const int blocks = 1 + blocksUntilSilent (sampler, buffer, blocksPerSample);
        expect (std::abs (blocks - blocksPerSample / 2) <= 2,
                "追従: 1オクターブ上は再生長がおよそ半分になること");
    }

    // 離れた音程でも 2^((pitch-rootNote)/12) が効く（レートのクランプで頭打ちにならない）。
    // ルート0＋ノート84 = 7オクターブ上 = 128倍速。上限64倍のままだと再生長が2倍になって落ちる
    {
        const int longFrames = (int) (sr * 4.0);
        SamplerEngine sampler (makeFlatSample (longFrames, 0.5f), sr, sr);
        sampler.setPitchFollow (true);
        sampler.setRootNote (0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 84, (juce::uint8) 127), 0);
        renderInto (sampler, 1);
        const int blocks = 1 + blocksUntilSilent (sampler, buffer, 400);
        const int expected = (int) std::ceil ((double) longFrames / 128.0 / (double) blockSize);
        expect (std::abs (blocks - expected) <= 1,
                "7オクターブ上でも再生レートが 2^((pitch-root)/12) どおりであること");
    }

    // rootNoteちょうどはサンプル単位で元のサンプルと一致（velocity127・gain1.0）
    {
        auto ramp = makeRampSample (sampleLength);
        SamplerEngine sampler (ramp, sr, sr);
        sampler.setPitchFollow (true);
        sampler.setRootNote (60);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        buffer.clear();
        sampler.processBlock (buffer, midi);
        midi.clear();

        float maxDiff = 0.0f, maxLrDiff = 0.0f;
        for (int i = 0; i < blockSize; ++i)
        {
            maxDiff = juce::jmax (maxDiff, std::abs (buffer.getSample (0, i) - ramp->getSample (0, i)));
            maxLrDiff = juce::jmax (maxLrDiff,
                                    std::abs (buffer.getSample (0, i) - buffer.getSample (1, i)));
        }
        expect (maxDiff < 1.0e-6f, "追従: ルート音は元のサンプルと一致すること");
        expect (maxLrDiff < 1.0e-6f, "モノソースはL/Rへ複製されること");
    }

    // 同ピッチ連打のnoteOff対象は「未リリースのうち最古」1本だけ
    {
        SamplerEngine sampler (flat, sr, sr);
        sampler.setPitchFollow (true);
        sampler.setRootNote (60);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        renderInto (sampler, 1);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        expect (renderInto (sampler, 1) > 0.99f, "追従: 同ピッチ連打は重なって鳴ること");
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        renderInto (sampler, 1);
        expect (sampler.numActiveVoices() == 1, "追従: 同ピッチのnoteOffで止まるのは最古の1本だけ");
        const auto level = renderInto (sampler, 1);
        expect (level > 0.49f && level < 0.51f, "追従: 後の発音は鳴り続けること");
    }

    // Mono: 新しい打点で前の音を切る（Logicの Polyphony: 1 相当。長い音の連打で重ならない）
    {
        SamplerEngine sampler (flat, sr, sr);
        sampler.setMono (true);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        expect (renderInto (sampler, 1) > 0.49f, "Mono: 1発目が鳴ること");

        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        renderInto (sampler, 1); // このブロック内で旧ボイスが5msフェードして消える
        expect (sampler.numActiveVoices() == 1, "Mono: 連打しても1ボイスだけ残ること");
        const auto level = renderInto (sampler, 1);
        expect (level > 0.49f && level < 0.51f, "Mono: 重ならないので振幅が2倍にならないこと");

        // 8連打しても積み上がらない。リリース長（5ms）以上の間隔なら同時に鳴るのは
        // 「現在の1本＋尾1本」＝最大2ボイス分（この定数サンプルでは 0.5×2 = 1.0）。
        // 重ねる場合の8ボイス分（4.0）とは桁が違う
        float peak = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
            peak = juce::jmax (peak, renderInto (sampler, 4));
        }
        expect (peak <= 1.01f, "Mono: 連打しても2ボイス分（フェードの重なり）を超えないこと");
        expect (sampler.numActiveVoices() == 1, "Mono: 8連打後も1ボイス");
    }

    // Mono × 追従: Monoが先に切ったノートのオフが、後から届いて新しいボイスを止めないこと
    // （MIDIのnoteOffにノート個体のIDがないため、飲む仕組みが要る）
    {
        SamplerEngine sampler (flat, sr, sr);
        sampler.setMono (true);
        sampler.setPitchFollow (true);
        sampler.setRootNote (60);

        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0); // ノートA
        renderInto (sampler, 2);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0); // ノートB（Aを切る）
        renderInto (sampler, 2);
        expect (sampler.numActiveVoices() == 1, "追従+Mono: Bだけになっていること");

        // ここでAのnoteOffが届く（Aは既にMonoで切られている）→ Bは鳴り続けるべき
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        renderInto (sampler, 2);
        expect (sampler.numActiveVoices() == 1, "追従+Mono: 古いノートのオフでBが止まらないこと");
        expect (renderInto (sampler, 1) > 0.49f, "追従+Mono: Bが鳴り続けること");

        // B自身のnoteOffではちゃんと止まる
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        renderInto (sampler, 2);
        expect (sampler.numActiveVoices() == 0, "追従+Mono: 自分のオフでは止まること");
    }

    // Mono: 同じ位置に複数の noteOn（同時刻の和音等）が来ても尾が積み上がらないこと
    {
        SamplerEngine sampler (flat, sr, sr);
        sampler.setMono (true);
        for (int i = 0; i < SamplerEngine::maxVoices; ++i)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0); // 全部同じオフセット
        const auto level = renderInto (sampler, 1);
        expect (sampler.numActiveVoices() == 1, "Mono: 同一位置の連続noteOnは1ボイスに畳まれること");
        expect (level > 0.49f && level < 0.51f,
                "Mono: 同一位置の連続noteOnで振幅が積み上がらないこと（未出力ボイスは即停止）");
    }

    // 連打（固定モード・Mono OFF）はボイスを奪わず加算される
    {
        SamplerEngine sampler (flat, sr, sr);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        renderInto (sampler, 1);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        expect (renderInto (sampler, 1) > 0.99f, "固定: 連打は前の音を切らず重なること");
        expect (sampler.numActiveVoices() == 2, "固定: 連打で2ボイスになること");
    }

    // ボイス上限超え: 17音目で最古（velocity127の1本目）が奪われる
    {
        SamplerEngine sampler (makeFlatSample (sampleLength, 1.0f), sr, sr);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0); // 最古（寄与1.0）
        renderInto (sampler, 1);
        for (int i = 0; i < SamplerEngine::maxVoices - 1; ++i)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 32), 0);
        renderInto (sampler, 1);
        expect (sampler.numActiveVoices() == SamplerEngine::maxVoices, "ボイスが上限まで埋まること");

        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 32), 0); // 17音目
        const auto level = renderInto (sampler, 1);
        expect (sampler.numActiveVoices() == SamplerEngine::maxVoices, "上限超えでもボイス数は上限のまま");
        const float expected = 16.0f * 32.0f / 127.0f; // 最古が奪われた＝全ボイスがvelocity32
        expect (std::abs (level - expected) < 0.05f, "上限超えでは最古のボイスが奪われること");
    }

    // startOffset（頭の無音カット）とgain・velocity
    {
        auto ramp = makeRampSample (sampleLength);
        SamplerEngine sampler (ramp, sr, sr);
        sampler.setStartOffset (1000);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        buffer.clear();
        sampler.processBlock (buffer, midi);
        midi.clear();
        expect (std::abs (buffer.getSample (0, 0) - ramp->getSample (0, 1000)) < 1.0e-6f,
                "startOffsetの分だけ先頭が飛ぶこと");
    }
    {
        SamplerEngine sampler (flat, sr, sr);
        sampler.setGain (0.5f);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        const auto level = renderInto (sampler, 1);
        expect (std::abs (level - 0.25f) < 0.001f, "gainが線形に効くこと");
    }
    {
        SamplerEngine sampler (flat, sr, sr);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 64), 0);
        const auto level = renderInto (sampler, 1);
        expect (std::abs (level - 0.5f * 64.0f / 127.0f) < 0.001f,
                "velocity 64 が velocity 127 のおよそ半分になること");
    }

    // デバイスSR≠ソースSR: 44.1k素材を48kで鳴らすと再生長が比率どおりに伸びる
    {
        SamplerEngine sampler (flat, sr, 48000.0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        renderInto (sampler, 1);
        const int blocks = 1 + blocksUntilSilent (sampler, buffer, 200);
        const int expected = (int) std::ceil (sampleLength * 48000.0 / sr / blockSize);
        expect (std::abs (blocks - expected) <= 2, "デバイスSRに応じて再生長が変わること");
    }
}

// ---- SamplerEngine × PlaybackEngine: 消音・再発音・モード切替の結合検証 ----
void testSamplerThroughPlaybackEngine()
{
    beginTest ("Sampler through PlaybackEngine");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    auto flat = makeFlatSample ((int) (sr * 2.0), 0.5f); // 2秒のワンショット

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // 曲頭に1つだけ「1小節分の長さ」のノート（固定モードならノート長は無視される）
    Project project;
    Track track;
    track.id = 40;
    track.type = TrackType::midi;
    MidiRegion region;
    region.id = 41;
    region.startPpq = 0;
    region.lengthPpq = Ppq::ticksPerBar * 8;
    region.notes.push_back ({ 42, 60, 0, Ppq::ticksPerBar * 4, 127 });
    track.midiRegions.push_back (region);
    project.tracks.push_back (std::move (track));
    project.tracks[0].params->gain.store (1.0f); // 振幅をサンプル値そのままで読む

    auto synth = makeSamplerInstance (flat, sr, sr, blockSize, false);
    auto pushSnapshot = [&]
    {
        auto snapshot = project.buildSnapshot();
        snapshot->tracks[0].synth = synth;
        snapshots.push (std::move (snapshot));
    };
    pushSnapshot();

    juce::AudioBuffer<float> buffer (2, blockSize);
    auto processBlocks = [&] (int count)
    {
        float magnitude = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            magnitude = juce::jmax (magnitude, buffer.getMagnitude (0, 0, blockSize));
        }
        return magnitude;
    };

    // 再生 → ワンショットが鳴る（ノート長より長く鳴り続ける）
    engine.play();
    expect (processBlocks (2) > 0.49f, "固定: 再生でワンショットが鳴ること");
    expect (synth->sampler->numActiveVoices() == 1, "1ボイスだけ鳴っていること");

    // 再生中のスナップショット差し替え（ノート追加相当）で二重発音しない（resoundスキップ）
    snapshots.deleteRetired();
    pushSnapshot();
    const auto swapped = processBlocks (3);
    expect (swapped < 0.6f, "固定: 差し替えでワンショットが二重に鳴らないこと");
    expect (synth->sampler->numActiveVoices() == 1, "固定: 差し替え後もボイスは1本");

    // 停止 → CC123で止まる
    engine.stop();
    processBlocks (2);
    expect (synth->sampler->numActiveVoices() == 0, "固定: 停止でボイスが止まること");
    expect (processBlocks (2) < 1.0e-6f, "固定: 停止後は無音");

    // シーク（ノートを跨いだ位置へ）→ ワンショットは途中から鳴り出さない
    transport.seekRequest.store ((juce::int64) (sr * 0.5));
    engine.play();
    expect (processBlocks (3) < 1.0e-6f, "固定: シーク先で跨ぎノートが途中から鳴り出さないこと");
    engine.stop();
    processBlocks (2);

    // サイクル折返し: 範囲頭のノートが鳴り直しても重ならない（折返しの消音が効いている）
    transport.seekRequest.store (0);
    const auto barSamples = (juce::int64) (sr * 2.0); // 120bpm/4拍 = 2秒
    transport.cycleRange.store (TransportState::packCycle (0, barSamples));
    transport.cycleEnabled.store (true);
    engine.play();
    const auto looped = processBlocks ((int) (barSamples * 3 / blockSize)); // 3周ぶん
    expect (looped < 0.6f, "サイクル折返しでワンショットが積み重ならないこと");
    expect (synth->sampler->numActiveVoices() <= 1, "折返し後もボイスは1本以下");
    engine.stop();
    transport.cycleEnabled.store (false);
    processBlocks (2);

    // ---- 非整数サンプル境界（127BPM）でも、グリッド上のOne Shotが消えないこと ----
    // シーク位置は「グリッドをサンプル整数へ丸めた値」なので、pos*tps でPPQへ戻すと
    // ノートのPPQよりわずかに大きくなる。PPQ同士で比べていると境界ちょうどのノートが
    // 「過去のノート」に落ち、One Shotは resound もしないので完全に消える
    {
        constexpr double oddBpm = 127.0;
        transport.bpm.store (oddBpm);
        project.tracks[0].midiRegions[0].notes.clear();
        // 16分音符1つ目（PPQ 240）に短いノートを置く
        project.tracks[0].midiRegions[0].notes.push_back ({ 50, 60, Ppq::ticksPerQuarter / 4,
                                                            Ppq::ticksPerQuarter / 4, 127 });
        synth->sampler->setPitchFollow (false);
        synth->oneShot.store (true);
        synth->sampler->requestStopAll();
        snapshots.deleteRetired();
        pushSnapshot();
        processBlocks (2);

        const auto gridSample = (juce::int64) std::llround (sr * 60.0 / oddBpm / 4.0); // = 5209
        transport.seekRequest.store (gridSample);
        engine.play();
        expect (processBlocks (3) > 0.49f, "127BPMでもグリッド上のOne Shotがシーク直後に鳴ること");
        engine.stop();
        processBlocks (2);

        transport.bpm.store (120.0);
        project.tracks[0].midiRegions[0].notes.clear();
        project.tracks[0].midiRegions[0].notes.push_back ({ 42, 60, 0, Ppq::ticksPerBar * 4, 127 });
        snapshots.deleteRetired();
        pushSnapshot();
        processBlocks (2);
    }

    // ---- 追従モードは従来どおり resound で持続音が復元される ----
    synth->sampler->setPitchFollow (true);
    synth->sampler->setRootNote (60);
    synth->oneShot.store (false);
    synth->sampler->requestStopAll();
    snapshots.deleteRetired();
    pushSnapshot();
    processBlocks (2);

    transport.seekRequest.store ((juce::int64) (sr * 0.5)); // ノートを跨いだ位置
    engine.play();
    expect (processBlocks (3) > 0.49f, "追従: シーク先で跨ぎノートが復元されること");

    // 再生中の固定⇄追従切替: 旧ボイスを止めてから復元するので二重発音しない
    synth->sampler->setPitchFollow (false);
    synth->oneShot.store (true);
    synth->sampler->requestStopAll(); // SynthBank::sync()が立てるフラグ相当
    snapshots.deleteRetired();
    pushSnapshot();
    processBlocks (3);
    expect (synth->sampler->numActiveVoices() == 0,
            "追従→固定: 全ボイス停止＋resoundスキップで鳴り残らないこと");

    synth->sampler->setPitchFollow (true);
    synth->oneShot.store (false);
    synth->sampler->requestStopAll();
    snapshots.deleteRetired();
    pushSnapshot();
    const auto afterSwitch = processBlocks (3);
    expect (afterSwitch > 0.49f && afterSwitch < 0.6f,
            "固定→追従: 持続音が1本だけ復元されること（二重発音しない）");
    expect (synth->sampler->numActiveVoices() == 1, "固定→追従: ボイスは1本");

    engine.stop();
    processBlocks (2);
    snapshots.deleteRetired();
}

// ---- スパイク: DLSMusicDevice をホストしてノートオン→無音でないことを確認 ----
void testDlsMusicDeviceRendersAudio()
{
    beginTest ("DLSMusicDevice renders audio");

    juce::AudioUnitPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, "AudioUnit:Synths/aumu,dls ,appl");
    expect (found.size() > 0, "DLSMusicDevice が見つかること");
    if (found.isEmpty())
        return;

    juce::String error;
    auto instance = format.createInstanceFromDescription (*found[0], 44100.0, 512, error);
    expect (instance != nullptr, "インスタンス化できること");
    if (instance == nullptr)
    {
        std::cout << "  error: " << error << std::endl;
        return;
    }

    instance->setNonRealtime (true);
    instance->prepareToPlay (44100.0, 512);

    const int numChannels = juce::jmax (2, instance->getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer (numChannels, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);

    float magnitude = 0.0f;
    for (int block = 0; block < 20; ++block)
    {
        buffer.clear();
        instance->processBlock (buffer, midi);
        midi.clear();
        magnitude = juce::jmax (magnitude, buffer.getMagnitude (0, buffer.getNumSamples()));
    }
    expect (magnitude > 0.001f, "ノートオン後に無音でないこと");
    std::cout << "  peak magnitude: " << magnitude << std::endl;

    instance->releaseResources();
}
// ---- バウンス: 完了までポーリング（ワーカースレッドの終了待ち）----
bool waitForBounce (BounceRenderer& renderer, int timeoutMs = 30000)
{
    const auto start = juce::Time::getMillisecondCounter();
    while (renderer.status() == BounceRenderer::Status::running)
    {
        if (juce::Time::getMillisecondCounter() - start > (juce::uint32) timeoutMs)
            return false;
        juce::Thread::sleep (10);
    }
    return true;
}

// ---- バウンス基本: クリップ×gainのミックス・24bit/2ch/レート・原子的置換・一時ファイル掃除 ----
void testBounceRendererBasic()
{
    beginTest ("BounceRenderer basic render");
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce.wav");
    target.replaceWithText ("stale junk"); // 既存ファイルが置換されることの確認用

    // クリップ: サンプル値0.5×1000サンプルを位置500へ、gain 0.5 → 出力は0.25
    auto audio = std::make_shared<juce::AudioBuffer<float>> (1, 1000);
    for (int i = 0; i < 1000; ++i)
        audio->setSample (0, i, 0.5f);

    BounceRenderer::Request request;
    request.sampleRate = 44100.0;
    request.bpm = 120.0;
    request.endSample = 2000;
    request.targetFile = target;
    BounceRenderer::TrackRender track;
    track.gain = 0.5f;
    track.clips.push_back ({ audio, 500, 0, 1000 });
    request.tracks.push_back (std::move (track));

    BounceRenderer renderer;
    expect (renderer.start (std::move (request)), "startできること");
    expect (waitForBounce (renderer), "タイムアウトせず完了すること");
    const auto result = renderer.takeResult();
    expect (result.status == BounceRenderer::Status::success, "successで終わること");
    expect (! result.scaled, "ピーク1.0以下ならスケールしないこと");
    expect (result.writtenSamples == 2000, "テールなし=endSampleちょうどの長さ");

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::FileInputStream (target), true));
    expect (reader != nullptr, "書き出したWAVを読めること（junkが置換されている）");
    if (reader != nullptr)
    {
        expect ((int) reader->numChannels == 2, "ステレオであること");
        expect ((int) reader->bitsPerSample == 24, "24bitであること");
        expect (juce::approximatelyEqual (reader->sampleRate, 44100.0), "サンプルレートが一致すること");
        expect (reader->lengthInSamples == 2000, "長さがendSampleと一致すること");

        juce::AudioBuffer<float> readBack (2, 2000);
        reader->read (&readBack, 0, 2000, 0, true, true);
        expect (std::abs (readBack.getSample (0, 800) - 0.25f) < 0.001f, "クリップ区間はgain適用値（L）");
        expect (std::abs (readBack.getSample (1, 800) - 0.25f) < 0.001f, "クリップ区間はgain適用値（R）");
        expect (std::abs (readBack.getSample (0, 100)) < 0.0001f, "クリップ前は無音");
        expect (std::abs (readBack.getSample (0, 1800)) < 0.0001f, "クリップ後は無音");
    }

    expect (dir.getNumberOfChildFiles (juce::File::findFiles, "*.tmp") == 0
                && dir.getNumberOfChildFiles (juce::File::findFiles, ".*") == 0,
            "一時ファイルが残らないこと");
    dir.deleteRecursively();
}

// ---- バウンス: ピーク>1.0のときだけ全体スケールダウン（オーバーロード保護）----
void testBounceRendererClippingProtection()
{
    beginTest ("BounceRenderer clipping protection");
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce.wav");

    // 0.8のクリップを同位置に2枚重ねて加算1.6 → 0.999/1.6にスケールされる
    auto audio = std::make_shared<juce::AudioBuffer<float>> (1, 500);
    for (int i = 0; i < 500; ++i)
        audio->setSample (0, i, 0.8f);

    BounceRenderer::Request request;
    request.sampleRate = 44100.0;
    request.endSample = 500;
    request.targetFile = target;
    BounceRenderer::TrackRender track;
    track.gain = 1.0f;
    track.clips.push_back ({ audio, 0, 0, 500 });
    track.clips.push_back ({ audio, 0, 0, 500 });
    request.tracks.push_back (std::move (track));

    BounceRenderer renderer;
    expect (renderer.start (std::move (request)), "startできること");
    expect (waitForBounce (renderer), "タイムアウトせず完了すること");
    const auto result = renderer.takeResult();
    expect (result.status == BounceRenderer::Status::success, "successで終わること");
    expect (result.scaled, "ピーク>1.0でスケールされること");
    expect (std::abs (result.peak - 1.6f) < 0.001f, "スケール前ピークが記録されること");

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::FileInputStream (target), true));
    expect (reader != nullptr, "書き出したWAVを読めること");
    if (reader != nullptr)
    {
        juce::AudioBuffer<float> readBack (2, 500);
        reader->read (&readBack, 0, 500, 0, true, true);
        const float peak = readBack.getMagnitude (0, 500);
        expect (peak <= 1.0f, "出力ピークが1.0以下に収まること");
        expect (std::abs (peak - 0.999f) < 0.005f, "0.999へ正規化されること");
    }
    dir.deleteRecursively();
}

// ---- バウンス: MIDIトラック（DLS専用インスタンス）のレンダリングと余韻テール ----
void testBounceRendererMidiTail()
{
    beginTest ("BounceRenderer midi + tail");
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce.wav");

    SynthBank bank;
    Track gmTrack; // GM音源（Piano）の設定だけを持つダミー
    gmTrack.type = TrackType::midi;
    auto synth = bank.createIndependent (gmTrack, 44100.0, BounceRenderer::renderBlockSize);
    expect (synth != nullptr, "バウンス専用DLSインスタンスを作れること");
    if (synth == nullptr)
    {
        dir.deleteRecursively();
        return;
    }

    // 1拍のノート（120BPMで0.5秒=22050サンプル）。endSampleはノート終端ちょうど
    BounceRenderer::Request request;
    request.sampleRate = 44100.0;
    request.bpm = 120.0;
    request.endSample = 22050;
    request.wantTail = true;
    request.targetFile = target;
    BounceRenderer::TrackRender track;
    track.gain = 0.8f;
    track.synth = synth;
    track.notes.push_back ({ 0, Ppq::ticksPerQuarter, 60, 100 });
    request.tracks.push_back (std::move (track));

    BounceRenderer renderer;
    expect (renderer.start (std::move (request)), "startできること");
    expect (waitForBounce (renderer), "タイムアウトせず完了すること");
    const auto result = renderer.takeResult();
    expect (result.status == BounceRenderer::Status::success, "successで終わること");
    expect (result.peak > 0.001f, "無音でないこと");
    expect (result.writtenSamples >= 22050, "テールで曲末より長くなること（最低でも切り捨てない）");
    expect (result.writtenSamples <= 22050 + (juce::int64) (44100 * 5.0) + BounceRenderer::renderBlockSize,
            "テール上限（5秒）を超えないこと");

    dir.deleteRecursively();
}

// ---- バウンス: サンプル音源（範囲延長・厳密範囲・テールでのレンダリング・再生との一致）----
void testBounceSampler()
{
    beginTest ("BounceRenderer sampler");

    constexpr double sr = 44100.0;
    const auto sampleFrames = (int) (sr * 6.0); // 曲末に置く6秒のワンショット

    Track track;
    track.id = 1;
    track.type = TrackType::midi;
    track.instrument = InstrumentKind::sample;
    track.sampleFile = "instr-001.wav";
    track.sampleName = "long808";
    track.sampleAudio = makeFlatSample (sampleFrames, 0.4f);
    track.sampleSourceRate = sr;

    // 曲頭の16分音符1つ（固定モードではノート長を無視して6秒鳴る）
    const std::vector<MidiNotePlayback> notes { { 0, Ppq::ticksPerQuarter / 4, 60, 127 } };

    // ---- 範囲延長の計算（純関数）----
    expect (BounceRenderer::oneShotEndSample (track, notes, 120.0, sr) == (juce::int64) sampleFrames,
            "固定モードの終端がサンプル全長になること");
    {
        auto follow = track;
        follow.samplePitchFollow = true;
        expect (BounceRenderer::oneShotEndSample (follow, notes, 120.0, sr) == 0,
                "追従モードは延長しないこと");

        auto trimmed = track;
        trimmed.sampleStartOffset = (juce::int64) sr; // 頭1秒カット
        expect (BounceRenderer::oneShotEndSample (trimmed, notes, 120.0, sr)
                    == (juce::int64) (sr * 5.0),
                "頭カットの分だけ終端が手前になること");

        auto gm = track;
        gm.instrument = InstrumentKind::gm;
        expect (BounceRenderer::oneShotEndSample (gm, notes, 120.0, sr) == 0,
                "GM音源では延長しないこと");

        // 非整数SR比: SamplerEngineは「読み出し位置が末尾に達するまで」出力するので必要長はceil。
        // 四捨五入だと末尾1サンプルが切れる（残り2サンプル・44.1k→48kは実際に3サンプル出る）
        auto tiny = track;
        tiny.sampleAudio = makeFlatSample (2, 0.5f);
        tiny.sampleSourceRate = sr;
        const auto tinyEnd = BounceRenderer::oneShotEndSample (tiny, notes, 120.0, 48000.0);
        expect (tinyEnd == 3, "非整数SR比の終端はceilで数えること");

        // 実際のエンジン出力数と一致していること（helperの計算がエンジンの規則と同じか）
        SamplerEngine tinySampler (tiny.sampleAudio, sr, 48000.0);
        juce::AudioBuffer<float> tinyBuffer (2, 64);
        juce::MidiBuffer tinyMidi;
        tinyMidi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        tinyBuffer.clear();
        tinySampler.processBlock (tinyBuffer, tinyMidi);
        int sounded = 0;
        for (int i = 0; i < tinyBuffer.getNumSamples(); ++i)
            if (std::abs (tinyBuffer.getSample (0, i)) > 1.0e-6f)
                ++sounded;
        expect (sounded == (int) tinyEnd, "helperの終端がエンジンの実出力サンプル数と一致すること");
    }

    const auto dir = makeTempDir();
    SynthBank bank;

    auto runBounce = [&] (const juce::File& target, juce::int64 endSample, bool wantTail,
                          const std::vector<MidiNotePlayback>& notesToRender)
    {
        BounceRenderer::Request request;
        request.sampleRate = sr;
        request.bpm = 120.0;
        request.targetFile = target;
        request.endSample = endSample;
        request.wantTail = wantTail;

        BounceRenderer::TrackRender render;
        render.gain = 1.0f;
        render.notes = notesToRender;
        render.synth = bank.createIndependent (track, sr, BounceRenderer::renderBlockSize);
        expect (render.synth != nullptr && render.synth->sampler != nullptr,
                "バウンス専用サンプラーを作れること");
        request.tracks.push_back (std::move (render));

        BounceRenderer renderer;
        expect (renderer.start (std::move (request)), "startできること");
        expect (waitForBounce (renderer), "タイムアウトせず完了すること");
        return renderer.takeResult();
    };

    auto readWav = [] (const juce::File& file, juce::AudioBuffer<float>& out)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (
            wav.createReaderFor (new juce::FileInputStream (file), true));
        if (reader == nullptr)
            return false;
        out.setSize (2, (int) reader->lengthInSamples);
        return reader->read (&out, 0, (int) reader->lengthInSamples, 0, true, true);
    };

    // ---- 全体バウンス: 延長した範囲でワンショットが全長書き出される ----
    {
        const auto target = dir.getChildFile ("full.wav");
        const auto result = runBounce (target, (juce::int64) sampleFrames, true, notes);
        expect (result.status == BounceRenderer::Status::success, "全体バウンスが成功すること");
        expect (result.writtenSamples >= (juce::int64) sampleFrames, "全長ぶん書き出されること");

        juce::AudioBuffer<float> out;
        expect (readWav (target, out), "出力を読めること");
        if (out.getNumSamples() >= sampleFrames)
        {
            // 5秒〜6秒の区間（現行テール上限5秒では切れていた領域）に音が残っていること
            const int from = (int) (sr * 5.0);
            expect (out.getMagnitude (0, from, (int) (sr * 0.9)) > 0.3f,
                    "5秒以降もサンプルが鳴っていること（テール上限に依存していない）");
        }
    }

    // ---- リージョン/サイクル書き出し: 指定範囲で厳密に切る（延長しない）----
    {
        const auto target = dir.getChildFile ("strict.wav");
        const auto strictEnd = (juce::int64) (sr * 0.5);
        const auto result = runBounce (target, strictEnd, false, notes);
        expect (result.status == BounceRenderer::Status::success, "厳密範囲の書き出しが成功すること");
        expect (result.writtenSamples == strictEnd, "指定範囲ちょうどで切れること");

        juce::AudioBuffer<float> out;
        expect (readWav (target, out), "出力を読めること");
        expect (out.getNumSamples() == (int) strictEnd, "WAVの長さも指定範囲ちょうど");
        expect (out.getMagnitude (0, 0, out.getNumSamples()) > 0.3f, "範囲内では鳴っていること");
    }

    // ---- サイクル範囲書き出し: 範囲頭を跨ぐOne Shotは鳴らさない（リアルタイム再生と一致させる）----
    // 再生側は oneShot のとき resound しない＝シーク先で途中から鳴り出さないので、書き出しでも
    // 範囲頭より前に始まったワンショットを鳴らしてはいけない（叩いていない打点が増える）
    {
        auto runRange = [&] (const juce::File& target, juce::int64 startSample, juce::int64 endSample,
                             const std::vector<MidiNotePlayback>& notesToRender, double bpm = 120.0)
        {
            BounceRenderer::Request request;
            request.sampleRate = sr;
            request.bpm = bpm;
            request.targetFile = target;
            request.startSample = startSample;
            request.endSample = endSample;
            request.wantTail = false;

            BounceRenderer::TrackRender render;
            render.gain = 1.0f;
            render.notes = notesToRender;
            render.synth = bank.createIndependent (track, sr, BounceRenderer::renderBlockSize);
            request.tracks.push_back (std::move (render));

            BounceRenderer renderer;
            expect (renderer.start (std::move (request)), "startできること");
            expect (waitForBounce (renderer), "タイムアウトせず完了すること");
            return renderer.takeResult();
        };

        // 1小節目頭のノート。終端は範囲頭より**後**（= 本当に範囲頭を跨いで鳴っている状態）にする。
        // 終端が範囲頭ちょうどだと「鳴り終わったノート」の読み飛ばしでも除外されてしまい、
        // One Shot専用の読み飛ばし規則を検証できない
        const auto barSamples = (juce::int64) (sr * 2.0); // 120BPMの1小節
        const std::vector<MidiNotePlayback> crossingOneShot { { 0, Ppq::ticksPerBar * 3, 60, 127 } };
        const auto target = dir.getChildFile ("cycle-crossing.wav");
        const auto result = runRange (target, barSamples, barSamples * 2, crossingOneShot);
        expect (result.status == BounceRenderer::Status::success, "サイクル範囲の書き出しが成功すること");
        expect (result.peak < 0.001f, "範囲頭を跨ぐOne Shotは鳴らさないこと（再生と一致）");

        // 範囲頭ちょうどに始まるノートは鳴る（読み飛ばしが行き過ぎていないことの確認）
        {
            const double tps = Ppq::ticksPerSample (120.0, sr);
            const auto atRangeStart = (juce::int64) std::llround ((double) barSamples * tps);
            const std::vector<MidiNotePlayback> onRangeStart { { atRangeStart,
                                                                atRangeStart + Ppq::ticksPerBar, 60, 127 } };
            const auto onStart = dir.getChildFile ("cycle-on-start.wav");
            const auto onStartResult = runRange (onStart, barSamples, barSamples * 2, onRangeStart);
            expect (onStartResult.status == BounceRenderer::Status::success, "境界ちょうどでも成功すること");
            expect (onStartResult.peak > 0.3f, "範囲頭ちょうどに始まるOne Shotは鳴ること");
        }

        // 非整数サンプル境界（127BPM/44.1kHz: 16分音符1つ目はPPQ240に対しサンプル5209で、
        // tpsでPPQへ戻すと240.0156になる）。PPQ同士で比べていると境界ちょうどのノートが
        // 「範囲頭より前」に落ちて消える
        {
            constexpr double oddBpm = 127.0;
            const double sixteenth = sr * 60.0 / oddBpm / 4.0;
            const auto rangeStart = (juce::int64) std::llround (sixteenth);       // = 5209
            const auto rangeEnd = (juce::int64) std::llround (sixteenth * 5.0);
            const std::vector<MidiNotePlayback> atGrid { { Ppq::ticksPerQuarter / 4,
                                                           Ppq::ticksPerQuarter / 4 + Ppq::ticksPerQuarter,
                                                           60, 127 } };
            const auto oddTarget = dir.getChildFile ("cycle-odd-bpm.wav");
            const auto oddResult = runRange (oddTarget, rangeStart, rangeEnd, atGrid, oddBpm);
            expect (oddResult.status == BounceRenderer::Status::success, "127BPMでも成功すること");
            expect (oddResult.peak > 0.3f,
                    "非整数サンプル境界でも範囲頭ちょうどのOne Shotが鳴ること（PPQ丸めで消えない）");
        }
    }

    // ---- テール: 範囲を跨ぐ追従ノートの余韻がサンプラーでもレンダリングされること ----
    {
        const auto saved = track.samplePitchFollow;
        track.samplePitchFollow = true; // runBounceは現在のtrackを見る
        const auto target = dir.getChildFile ("tail.wav");
        const auto rangeEnd = (juce::int64) (sr * 0.25);
        // 範囲末尾を跨いで鳴り続けるノート（テールの先頭でnoteOffが送られる）
        const std::vector<MidiNotePlayback> crossing { { 0, Ppq::ticksPerBar, 60, 127 } };
        const auto result = runBounce (target, rangeEnd, true, crossing);
        track.samplePitchFollow = saved;

        expect (result.status == BounceRenderer::Status::success, "テール付き書き出しが成功すること");
        // テール先頭でnoteOff→5msフェード。サンプラーがテールで回っていれば
        // 「音のあるブロック＋無音ブロック」の2ブロック書かれる（回らないと無音1ブロックで打ち切られる）
        expect (result.writtenSamples >= rangeEnd + 2 * BounceRenderer::renderBlockSize,
                "サンプラーがテールでもレンダリングされること");

        juce::AudioBuffer<float> out;
        expect (readWav (target, out), "出力を読めること");
        if (out.getNumSamples() > (int) rangeEnd)
            expect (out.getMagnitude (0, (int) rangeEnd, BounceRenderer::renderBlockSize) > 0.05f,
                    "テール先頭に余韻（リリースのフェード）が入ること");
    }

    // ---- 再生（PlaybackEngine）とバウンスの出力一致 ----
    {
        constexpr int blockSize = 512;
        const int totalSamples = blockSize * 40; // 約0.46秒

        TransportState transport;
        SnapshotExchange snapshots;
        PreviewFifo previewFifo;
        PlaybackEngine engine (transport, snapshots, previewFifo);
        engine.prepareToPlay (blockSize, sr);

        Project project;
        Track playTrack = track;
        MidiRegion region;
        region.id = 2;
        region.startPpq = 0;
        region.lengthPpq = Ppq::ticksPerBar;
        region.notes.push_back ({ 3, 60, 0, Ppq::ticksPerQuarter / 4, 127 });
        playTrack.midiRegions.push_back (region);
        playTrack.params->gain.store (1.0f);
        project.tracks.push_back (std::move (playTrack));

        SynthBank playBank;
        playBank.sync (project, sr, blockSize);
        auto snapshot = project.buildSnapshot();
        snapshot->tracks[0].synth = playBank.get (1);
        expect (snapshot->tracks[0].synth != nullptr, "再生用サンプラーが生成されること");
        snapshots.push (std::move (snapshot));

        juce::AudioBuffer<float> engineOut (2, totalSamples);
        engineOut.clear();
        engine.play();
        juce::AudioBuffer<float> buffer (2, blockSize);
        for (int block = 0; block < totalSamples / blockSize; ++block)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            for (int ch = 0; ch < 2; ++ch)
                engineOut.copyFrom (ch, block * blockSize, buffer, ch, 0, blockSize);
        }
        engine.stop();
        snapshots.deleteRetired();

        const auto target = dir.getChildFile ("consistency.wav");
        const auto result = runBounce (target, (juce::int64) totalSamples, false, notes);
        expect (result.status == BounceRenderer::Status::success, "一致検証用バウンスが成功すること");

        juce::AudioBuffer<float> bounceOut;
        expect (readWav (target, bounceOut), "出力を読めること");
        if (bounceOut.getNumSamples() == totalSamples)
        {
            float maxDiff = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < totalSamples; ++i)
                    maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (ch, i)
                                                             - bounceOut.getSample (ch, i)));
            expect (maxDiff < 1.0e-4f, "再生とバウンスのサンプル値が一致（24bit量子化誤差内）すること");
        }
    }

    dir.deleteRecursively();
}

// ---- ⌘Eリージョン書き出し: 選択1アイテムのTrackRender組み立て（buildItemRender）----
void testBuildItemRender()
{
    beginTest ("buildItemRender (region export)");

    auto makeAudio = [] (int numSamples, float value)
    {
        auto audio = std::make_shared<juce::AudioBuffer<float>> (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            audio->setSample (0, i, value);
        return audio;
    };

    // オーディオトラック: 信号値の異なる3クリップ（1枚目はmuted）から2枚目だけを選択
    Track track;
    track.type = TrackType::audio;
    Clip a;
    a.audio = makeAudio (1000, 0.1f);
    a.startSample = 0;
    a.lengthSamples = 1000;
    a.muted = true;
    Clip b;
    b.audio = makeAudio (2000, 0.2f);
    b.startSample = 5000;
    b.offsetSamples = 300;
    b.lengthSamples = 1200;
    Clip c;
    c.audio = makeAudio (1000, 0.3f);
    c.startSample = 9000;
    c.lengthSamples = 1000;
    track.clips = { a, b, c };
    track.params->gain.store (0.6f);
    track.params->pan.store (-0.25f);
    track.params->sends[1].store (0.4f);
    track.params->mute.store (true); // トラックのmute/soloは無視される（明示選択が優先）
    track.params->solo.store (true);

    BounceRenderer::TrackRender out;
    juce::int64 rangeStart = -1, rangeEnd = -1;
    expect (BounceRenderer::buildItemRender (track, 1, 120.0, 44100.0, out, rangeStart, rangeEnd),
            "クリップを組み立てられること");
    expect (out.clips.size() == 1, "選択した1クリップだけが入ること");
    if (out.clips.size() == 1)
    {
        expect (out.clips[0].audio == b.audio, "選択クリップのバッファであること（他クリップが混ざらない）");
        expect (out.clips[0].startSample == 5000 && out.clips[0].offsetSamples == 300
                    && out.clips[0].lengthSamples == 1200,
                "位置・オフセット・長さがモデルどおりであること");
    }
    expect (out.notes.empty(), "オーディオトラックはノートなし");
    expect (rangeStart == 5000 && rangeEnd == 6200, "範囲がクリップ区間ちょうどであること");
    expect (juce::approximatelyEqual (out.gain, 0.6f) && juce::approximatelyEqual (out.pan, -0.25f)
                && juce::approximatelyEqual (out.sends[1], 0.4f),
            "gain/pan/sendsが焼き込まれること（mute/soloは無視）");

    expect (BounceRenderer::buildItemRender (track, 0, 120.0, 44100.0, out, rangeStart, rangeEnd)
                && out.clips.size() == 1,
            "mutedクリップも書き出せること（明示選択が優先）");

    // offsetSamples/長さのクランプ（buildSnapshotと同じ最終防衛線）。範囲はモデル上の区間のまま
    Clip overrun;
    overrun.audio = makeAudio (500, 0.1f);
    overrun.startSample = 100;
    overrun.offsetSamples = 200;
    overrun.lengthSamples = 900; // バッファ残りは300しかない
    track.clips = { overrun };
    expect (BounceRenderer::buildItemRender (track, 0, 120.0, 44100.0, out, rangeStart, rangeEnd),
            "はみ出しクリップも組み立てられること");
    expect (out.clips.size() == 1 && out.clips[0].lengthSamples == 300,
            "再生長がバッファ残りへクランプされること");
    expect (rangeStart == 100 && rangeEnd == 1000, "範囲はモデル上の区間（クランプ分は末尾無音）");

    expect (! BounceRenderer::buildItemRender (track, 1, 120.0, 44100.0, out, rangeStart, rangeEnd),
            "index範囲外はfalse");

    // MIDIトラック: 2リージョンから1つ目（muted）を選択。他リージョンのノートが混ざらない
    Track midi;
    midi.type = TrackType::midi;
    MidiRegion r1;
    r1.startPpq = Ppq::ticksPerBar; // 2小節目頭
    r1.lengthPpq = Ppq::ticksPerBar;
    r1.muted = true;                // リージョンのmutedも無視される
    r1.notes.push_back ({ 0, 60, 0, Ppq::ticksPerQuarter, 100 });
    // リージョン端をはみ出すノート → 境界でマスクされる
    r1.notes.push_back ({ 0, 64, Ppq::ticksPerBar - Ppq::ticksPerQuarter, Ppq::ticksPerQuarter * 2, 90 });
    MidiRegion r2;
    r2.startPpq = Ppq::ticksPerBar * 3;
    r2.lengthPpq = Ppq::ticksPerBar;
    r2.notes.push_back ({ 0, 72, 0, Ppq::ticksPerQuarter, 100 });
    midi.midiRegions = { r1, r2 };

    expect (BounceRenderer::buildItemRender (midi, 0, 120.0, 44100.0, out, rangeStart, rangeEnd),
            "MIDIリージョンを組み立てられること");
    expect (out.clips.empty(), "MIDIトラックはクリップなし");
    expect (out.notes.size() == 2, "選択リージョンのノートだけが入ること（mutedでも書き出す）");
    if (out.notes.size() == 2)
    {
        expect (out.notes[0].startPpq == Ppq::ticksPerBar
                    && out.notes[0].endPpq == Ppq::ticksPerBar + Ppq::ticksPerQuarter
                    && out.notes[0].pitch == 60,
                "ノートが絶対PPQへフラット化されること");
        expect (out.notes[1].endPpq == Ppq::ticksPerBar * 2,
                "リージョン端をはみ出すノートは境界でマスクされること");
    }
    // 120BPM・44100Hz: 1小節（3840tick）= 2秒 = 88200サンプル
    expect (rangeStart == 88200 && rangeEnd == 176400,
            "PPQ→サンプルの厳密長換算（1小節=88200サンプル）");

    midi.drums = true;
    midi.drumPitch = 36;
    expect (BounceRenderer::buildItemRender (midi, 1, 120.0, 44100.0, out, rangeStart, rangeEnd)
                && out.notes.size() == 1 && out.notes[0].pitch == 36,
            "固定ピッチ打楽器はピッチが置き換わること");

    // ノート空リージョンは notes 空で成功（入口ガードは呼び出し側の責務）
    MidiRegion emptyRegion;
    emptyRegion.lengthPpq = Ppq::ticksPerBar;
    midi.midiRegions = { emptyRegion };
    expect (BounceRenderer::buildItemRender (midi, 0, 120.0, 44100.0, out, rangeStart, rangeEnd)
                && out.notes.empty(),
            "ノート空リージョンはnotes空で成功すること");

    // ---- ループ: ⌘Eの書き出しはループ終端まで伸びる ----
    midi.drums = false;
    midi.drumPitch = -1;
    MidiRegion looped;
    looped.startPpq = Ppq::ticksPerBar;
    looped.lengthPpq = Ppq::ticksPerBar;
    looped.loopCount = 2; // 本体＋2反復 = 3小節ぶん
    looped.notes.push_back ({ 0, 60, 0, Ppq::ticksPerQuarter, 100 });
    midi.midiRegions = { looped };
    expect (BounceRenderer::buildItemRender (midi, 0, 120.0, 44100.0, out, rangeStart, rangeEnd),
            "ループ付きリージョンを組み立てられること");
    expect (out.notes.size() == 3, "反復ぶんのノートが入ること");
    expect (rangeStart == 88200 && rangeEnd == 88200 * 4,
            "書き出し範囲がループ終端（3小節ぶん）まで伸びること");

    Clip loopedClip;
    loopedClip.audio = makeAudio (1000, 0.4f);
    loopedClip.startSample = 2000;
    loopedClip.lengthSamples = 1000;
    loopedClip.loopCount = 1; // 本体＋1反復
    track.clips = { loopedClip };
    expect (BounceRenderer::buildItemRender (track, 0, 120.0, 44100.0, out, rangeStart, rangeEnd),
            "ループ付きクリップを組み立てられること");
    expect (out.clips.size() == 2, "反復ぶんのクリップエントリが入ること");
    expect (rangeStart == 2000 && rangeEnd == 4000, "書き出し範囲がループ終端まで伸びること");
}

// ---- v7: プロジェクトメモの保存・読込と、旧形式の空文字補完 ----
void testProjectMemoRoundtrip()
{
    beginTest ("project memo roundtrip and v6 default");

    auto dir = makeTempDir();
    Project project;
    project.directory = dir;
    project.memo = juce::String::fromUTF8 (u8"仮歌\n・2番を録り直す\n\"引用\" と \\\\");

    juce::String error;
    expect (project.save (error), "メモ入りプロジェクトを保存できること");

    const auto parsed = juce::JSON::parse (dir.getChildFile ("project.json").loadFileAsString());
    expect ((int) parsed.getProperty ("version", 0) == Project::currentVersion,
            "メモ保存時のversionが現行値であること");
    expect (parsed.getProperty ("memo", "").toString() == project.memo,
            "JSON上で改行・日本語・引用符・バックスラッシュが維持されること");

    juce::StringArray warnings;
    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr && reloaded->memo == project.memo,
            "メモが保存→再読込で一致すること");

    dir.getChildFile ("project.json").replaceWithText (R"({
        "version": 6, "bpm": 120.0, "sampleRate": 0.0, "nextId": 1,
        "tracks": []
    })");
    warnings.clear();
    error.clear();
    auto legacy = Project::load (dir, warnings, error);
    expect (legacy != nullptr && legacy->memo.isEmpty(),
            "v6のmemo欠損は空文字で補完されること");

    dir.deleteRecursively();
}

// ---- v4: pan/sends/バス/Masterの保存・読込と、v3以前のデフォルト補完 ----
void testMixerParamsRoundtrip()
{
    beginTest ("mixer params roundtrip");

    // 新規Projectのデフォルト: バス・Masterはユニティ（TrackParamsの既定0.8を引き継がない）
    Project fresh;
    for (int b = 0; b < numSendBuses; ++b)
        expect (juce::approximatelyEqual (fresh.busParams[b]->gain.load(), 1.0f),
                "新規Projectのバスgainは1.0");
    expect (juce::approximatelyEqual (fresh.masterParams->gain.load(), 1.0f),
            "新規ProjectのMaster gainは1.0");

    // v3形式（pan/sends/buses/masterなし）の読込 → デフォルト補完
    auto dir = makeTempDir();
    dir.getChildFile ("project.json").replaceWithText (R"({
        "version": 3, "bpm": 120.0, "sampleRate": 0.0, "nextId": 2,
        "tracks": [ { "id": 1, "type": "audio", "name": "t",
                      "mute": false, "solo": false, "volume": 0.5, "clips": [] } ]
    })");
    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr && project->tracks.size() == 1, "v3を読込めること");
    if (project == nullptr || project->tracks.empty())
    {
        dir.deleteRecursively();
        return;
    }
    auto& params = *project->tracks[0].params;
    expect (juce::approximatelyEqual (params.pan.load(), 0.0f), "v3読込: pan=0");
    expect (params.eqEnabled.load() && params.compEnabled.load(), "v3読込: FXはON補完");
    for (int b = 0; b < numSendBuses; ++b)
        expect (juce::approximatelyEqual (params.sends[b].load(), 0.0f), "v3読込: send=0");
    for (int b = 0; b < numSendBuses; ++b)
        expect (juce::approximatelyEqual (project->busParams[b]->gain.load(), 1.0f)
                    && ! project->busParams[b]->mute.load(),
                "v3読込: バスgain=1.0・mute=false");
    expect (juce::approximatelyEqual (project->masterParams->gain.load(), 1.0f),
            "v3読込: Master gain=1.0");

    // 値を入れて保存 → v4になり、再読込で維持される
    params.pan.store (-0.5f);
    params.sends[0].store (0.3f);
    params.sends[2].store (1.0f);
    params.eqEnabled.store (false);
    project->busParams[1]->gain.store (0.7f);
    project->busParams[1]->mute.store (true);
    project->masterParams->gain.store (0.9f);
    expect (project->save (error), "v4で保存できること");

    const auto parsed = juce::JSON::parse (dir.getChildFile ("project.json").loadFileAsString());
    expect ((int) parsed.getProperty ("version", 0) == Project::currentVersion,
            "現行バージョンで保存されること");

    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr && reloaded->tracks.size() == 1, "v4を再読込できること");
    if (reloaded != nullptr && ! reloaded->tracks.empty())
    {
        auto& p = *reloaded->tracks[0].params;
        expect (juce::approximatelyEqual (p.pan.load(), -0.5f), "pan維持");
        expect (juce::approximatelyEqual (p.sends[0].load(), 0.3f)
                    && juce::approximatelyEqual (p.sends[1].load(), 0.0f)
                    && juce::approximatelyEqual (p.sends[2].load(), 1.0f),
                "sends維持");
        expect (! p.eqEnabled.load() && p.compEnabled.load(), "FXのON/OFF維持（eq=OFF/comp=ON）");
        expect (juce::approximatelyEqual (reloaded->busParams[1]->gain.load(), 0.7f)
                    && reloaded->busParams[1]->mute.load(),
                "バスgain/mute維持");
        expect (juce::approximatelyEqual (reloaded->masterParams->gain.load(), 0.9f), "Master gain維持");

        // buildSnapshotにバス・Masterが載ること
        auto snapshot = reloaded->buildSnapshot();
        expect (snapshot->busParams[0] == reloaded->busParams[0]
                    && snapshot->masterParams == reloaded->masterParams,
                "スナップショットがバス/Masterのparamsを共有すること");
    }
    dir.deleteRecursively();
}

// ---- エンジン: pan法則・post-fader send（素通しバス）・busGain/mute・Masterゲイン・メーター ----
void testEnginePanSendsMaster()
{
    beginTest ("engine pan/sends/master");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // 定数振幅0.5のクリップ（レベル検証がしやすい）
    Project project;
    Track track;
    track.id = 1;
    track.params->gain.store (1.0f);
    Clip clip;
    clip.startSample = 0;
    clip.lengthSamples = blockSize * 64;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, blockSize * 64);
    for (int i = 0; i < clip.audio->getNumSamples(); ++i)
        clip.audio->setSample (0, i, 0.5f);
    track.clips.push_back (std::move (clip));
    project.tracks.push_back (std::move (track));
    auto& params = *project.tracks[0].params;
    snapshots.push (project.buildSnapshot());

    juce::AudioBuffer<float> buffer (2, blockSize);
    auto measure = [&] (float& left, float& right)
    {
        transport.seekRequest.store (0);
        engine.play();
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        left = buffer.getMagnitude (0, 0, blockSize);
        right = buffer.getMagnitude (1, 0, blockSize);
        engine.stop();
        buffer.clear();
        engine.process (info); // 停止エッジの消化
    };

    float left = 0.0f, right = 0.0f;

    // panセンター: 両ch 0.5（等パワー補正型はセンター0dB = 既存プロジェクトの音量を変えない）
    measure (left, right);
    expect (std::abs (left - 0.5f) < 0.001f && std::abs (right - 0.5f) < 0.001f,
            "panセンターは両ch等量（0.5）");

    // pan右振り切り: 左ほぼ0・右は+3dB（0.5×√2≈0.707）
    params.peakL.exchange (0.0f); // センター測定の蓄積ピーク（CAS max）をリセット
    params.peakR.exchange (0.0f);
    params.pan.store (1.0f);
    measure (left, right);
    expect (left < 0.001f, "pan右振り切りで左chは無音");
    expect (std::abs (right - 0.7071f) < 0.005f, "pan右振り切りで右chは+3dB（約0.707）");
    expect (params.peakR.exchange (0.0f) > 0.7f, "Rメーターはpost-panピーク（約0.707）");
    expect (params.peakL.exchange (0.0f) < 0.001f, "pan右振り切りでLメーターは振れないこと");

    // send（素通しバス）: pan中央・send100% → 原音と二重加算で1.0
    params.pan.store (0.0f);
    params.sends[0].store (1.0f);
    measure (left, right);
    expect (std::abs (left - 1.0f) < 0.002f, "send100%は素通しバスで二重加算（1.0）");
    expect (project.busParams[0]->peakL.exchange (0.0f) > 0.45f, "バスメーターが振れること");

    // バスミュートでsend分が消える
    project.busParams[0]->mute.store (true);
    measure (left, right);
    expect (std::abs (left - 0.5f) < 0.002f, "バスMでsend分が消えること");
    project.busParams[0]->mute.store (false);

    // バスのリターン量（gain 0.5 → 0.5 + 0.25 = 0.75）
    project.busParams[0]->gain.store (0.5f);
    measure (left, right);
    expect (std::abs (left - 0.75f) < 0.002f, "バスgainがリターン量として効くこと");
    project.busParams[0]->gain.store (1.0f);

    // Masterゲイン（全体 1.0 → 0.5）とMasterメーター
    project.masterParams->gain.store (0.5f);
    project.masterParams->peakL.exchange (0.0f); // 前シナリオの蓄積ピーク（CAS max）をリセット
    project.masterParams->peakR.exchange (0.0f);
    measure (left, right);
    expect (std::abs (left - 0.5f) < 0.002f, "Masterゲインで全体が半減すること");
    expect (std::abs (project.masterParams->peakL.exchange (0.0f) - 0.5f) < 0.01f,
            "Masterメーターはpost-masterピーク");

    snapshots.deleteRetired();
}

// ---- エンジン: 最終出力ルール（ch0/1のみ・1chはダウンミックス・余剰chは無音。クリック含む）----
void testEngineOutputChannelRule()
{
    beginTest ("engine output channel rule");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    Project project;
    Track track;
    track.id = 1;
    track.params->gain.store (1.0f);
    track.params->sends[0].store (1.0f); // バス経路も通す（余剰ch漏れの検査対象に含める）
    Clip clip;
    clip.startSample = 0;
    clip.lengthSamples = blockSize * 64;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, blockSize * 64);
    for (int i = 0; i < clip.audio->getNumSamples(); ++i)
        clip.audio->setSample (0, i, 0.5f);
    track.clips.push_back (std::move (clip));
    project.tracks.push_back (std::move (track));
    snapshots.push (project.buildSnapshot());

    // 4ch出力: 通常音（クリップ＋バス）とクリック（曲頭=拍頭で必ず鳴る）がch0/1のみに出ること
    transport.clickEnabled.store (true);
    {
        juce::AudioBuffer<float> buffer (4, blockSize);
        transport.seekRequest.store (0);
        engine.play();
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        engine.stop();
        expect (buffer.getMagnitude (0, 0, blockSize) > 0.9f
                    && buffer.getMagnitude (1, 0, blockSize) > 0.9f,
                "4ch出力: ch0/1に音が出ること（クリップ＋バス＋クリック）");
        expect (buffer.getMagnitude (2, 0, blockSize) == 0.0f
                    && buffer.getMagnitude (3, 0, blockSize) == 0.0f,
                "4ch出力: ch2以降は完全に無音（クリックも漏れない）");
        buffer.clear();
        engine.process (info); // 停止エッジの消化
    }

    // 1ch出力: クラッシュせずL+R等分ダウンミックスになること（クリックは切って振幅を検証）
    transport.clickEnabled.store (false);
    project.tracks[0].params->sends[0].store (0.0f);
    {
        juce::AudioBuffer<float> buffer (1, blockSize);
        transport.seekRequest.store (0);
        engine.play();
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        engine.stop();
        expect (std::abs (buffer.getMagnitude (0, 0, blockSize) - 0.5f) < 0.002f,
                "1ch出力: L+R等分ダウンミックス（panセンターの0.5が保たれる）");
        buffer.clear();
        engine.process (info);
    }

    snapshots.deleteRetired();
}

// ---- エンジン: 停止中のMIDIプレビュー発音もMaster（pan/sendバス経路）を通ること ----
void testPreviewThroughMaster()
{
    beginTest ("preview routes through master");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    Project project;
    Track track;
    track.id = 30;
    track.type = TrackType::midi;
    track.gmProgram = 48; // Strings（持続音）
    project.tracks.push_back (std::move (track));

    SynthBank bank;
    bank.sync (project, sr, blockSize);
    auto snapshot = project.buildSnapshot();
    snapshot->tracks[0].synth = bank.get (30);
    snapshots.push (std::move (snapshot));

    juce::AudioBuffer<float> buffer (2, blockSize);
    auto processBlocks = [&] (int count)
    {
        float magnitude = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            magnitude = juce::jmax (magnitude, buffer.getMagnitude (0, 0, blockSize));
        }
        return magnitude;
    };

    // Master 0 でプレビュー → 無音（Masterを迂回していない証拠）。イベント自体は処理されている
    project.masterParams->gain.store (0.0f);
    previewFifo.push ({ PreviewFifo::Command::Type::noteOn, 30, 72, 100 });
    expect (processBlocks (5) < 0.0001f, "Master 0ならプレビューも無音");
    auto synth = bank.get (30);
    bool active = false;
    if (synth != nullptr)
        for (int i = 0; i < synth->numActiveNotes; ++i)
            active = active || synth->activeNotes[i].pitch == 72;
    expect (active, "無音でもプレビューのイベントは処理されていること");

    // Master 1 に戻すと（同じ発音中ノートが）聞こえる
    project.masterParams->gain.store (1.0f);
    expect (processBlocks (5) > 0.001f, "Masterを戻すとプレビューが聞こえること");

    processBlocks (60); // 発音長を消化してから片付け
    snapshots.deleteRetired();
}

// ---- v5: サイクル範囲の保存・読込と、v4以前のデフォルト補完・不正値の防御 ----
void testCycleRoundtrip()
{
    beginTest ("cycle range roundtrip and v4 defaults");

    // v4形式（cycleキーなし）の読込 → 既定値: 範囲なし・OFF
    auto dir = makeTempDir();
    dir.getChildFile ("project.json").replaceWithText (R"({
        "version": 4, "bpm": 120.0, "sampleRate": 0.0, "nextId": 2,
        "tracks": [ { "id": 1, "type": "audio", "name": "t",
                      "mute": false, "solo": false, "volume": 0.5, "clips": [] } ]
    })");
    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr, "v4を読込めること");
    if (project == nullptr)
    {
        dir.deleteRecursively();
        return;
    }
    expect (! project->hasCycleRange() && ! project->cycleEnabled, "v4読込: サイクルは範囲なし・OFF");

    // 値を入れて保存 → v5になり、再読込で維持される
    project->cycleStartSixteenths = 16; // 2小節目頭
    project->cycleEndSixteenths = 48;   // 4小節目頭
    project->cycleEnabled = true;
    expect (project->save (error), "保存できること");
    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr, "再読込できること");
    if (reloaded != nullptr)
    {
        expect (reloaded->cycleStartSixteenths == 16 && reloaded->cycleEndSixteenths == 48,
                "サイクル範囲が維持されること");
        expect (reloaded->cycleEnabled, "enabledが維持されること");
    }
    dir.deleteRecursively();

    // 不正な範囲（start >= end）は範囲なしへ落とし、enabledも立てない
    dir = makeTempDir();
    dir.getChildFile ("project.json").replaceWithText (R"({
        "version": 5, "bpm": 120.0, "sampleRate": 0.0, "nextId": 1, "tracks": [],
        "cycle": { "start": 32, "end": 32, "enabled": true }
    })");
    auto invalid = Project::load (dir, warnings, error);
    expect (invalid != nullptr, "不正cycle付きでも読込めること");
    if (invalid != nullptr)
        expect (! invalid->hasCycleRange() && ! invalid->cycleEnabled,
                "start>=endは範囲なし・OFFに落ちること");
    dir.deleteRecursively();

    // packCycle/unpackの整合（32bit超のクランプ含む）
    const auto packed = TransportState::packCycle (12345, 678901);
    expect (TransportState::cycleStartOf (packed) == 12345
                && TransportState::cycleEndOf (packed) == 678901,
            "pack/unpackが往復すること");
    expect (TransportState::cycleEndOf (TransportState::packCycle (0, (juce::int64) 1 << 40)) == 0xffffffff,
            "32bit超はクランプされること");
}

// ---- エンジン: サイクルのループ再生（ブロック跨ぎ・等号境界・範囲長<blockSizeの複数回ラップ）----
void testPlaybackEngineCycleLoop()
{
    beginTest ("PlaybackEngine cycle loop");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int totalSamples = 4096;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // ランプ波（サンプル値=位置に比例）で読み出し位置のズレを1サンプル単位で検出する
    auto source = std::make_shared<juce::AudioBuffer<float>> (1, totalSamples);
    for (int i = 0; i < totalSamples; ++i)
        source->setSample (0, i, (float) i / (float) totalSamples);

    Project project;
    Track track;
    track.id = 1;
    track.params->gain.store (1.0f);
    Clip clip;
    clip.startSample = 0;
    clip.offsetSamples = 0;
    clip.lengthSamples = totalSamples;
    clip.audio = source;
    track.clips.push_back (std::move (clip));
    project.tracks.push_back (std::move (track));
    snapshots.push (project.buildSnapshot());

    juce::AudioBuffer<float> buffer (2, blockSize);
    const auto processBlock = [&]
    {
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
    };
    const auto expectRamp = [&] (int bufOffset, int count, int srcStart, const char* description)
    {
        int mismatches = 0;
        for (int i = 0; i < count; ++i)
            if (std::abs (buffer.getSample (0, bufOffset + i)
                          - source->getSample (0, srcStart + i)) > 1.0e-6f)
                ++mismatches;
        expect (mismatches == 0, description);
    };

    // ---- ブロック途中でラップ: サイクル[200, 1000)・位置200から ----
    transport.cycleRange.store (TransportState::packCycle (200, 1000));
    transport.cycleEnabled.store (true);
    transport.seekRequest.store (200);
    engine.play();

    processBlock(); // 200..712
    expectRamp (0, blockSize, 200, "1ブロック目: 範囲内をそのまま再生");
    expect (transport.playheadSamplePos.load() == 712, "1ブロック目の最終位置");

    processBlock(); // 712..1000（288）＋ラップして 200..424（224）
    expectRamp (0, 288, 712, "境界前セグメントが正しい内容");
    expectRamp (288, 224, 200, "ラップ後セグメントがバッファ後半の正しい位置に正しい内容");
    expect (transport.playheadSamplePos.load() == 424, "ラップ後の最終位置");
    engine.stop();
    processBlock(); // 停止エッジを消化

    // ---- 等号境界: ブロックが終端ちょうどで終わる → 次の再生位置は範囲頭（終端排他）----
    transport.cycleRange.store (TransportState::packCycle (200, 712)); // 範囲長=blockSize
    transport.seekRequest.store (200);
    engine.play();
    processBlock(); // 200..712 = 終端ちょうど
    expectRamp (0, blockSize, 200, "等号境界ブロックの内容");
    expect (transport.playheadSamplePos.load() == 200, "終端ちょうどで終わっても範囲頭へ戻ること");
    processBlock();
    expectRamp (0, blockSize, 200, "次ブロックは範囲頭から再生されること");
    engine.stop();
    processBlock();

    // ---- 範囲長 < blockSize: 1コールバックで複数回ラップ ----
    transport.cycleRange.store (TransportState::packCycle (0, 128));
    transport.seekRequest.store (0);
    engine.play();
    processBlock(); // 0..128 を4周
    for (int rep = 0; rep < 4; ++rep)
        expectRamp (rep * 128, 128, 0, "範囲長<blockSize: 各セグメントが正しい位置・内容で書かれること");
    expect (transport.playheadSamplePos.load() == 0, "複数回ラップ後の最終位置が範囲頭に戻ること");
    engine.stop();
    processBlock();

    snapshots.deleteRetired();
}

// ---- バウンス: サイクル範囲の書き出し（開始位置・厳密なサンプル長・MIDIありテールなし）----
void testBounceCycleRange()
{
    beginTest ("BounceRenderer cycle range");

    // クリップのみ: 範囲[500, 1500)を書き出すと出力先頭=ソース位置500・長さ1000ちょうど
    {
        const auto dir = makeTempDir();
        const auto target = dir.getChildFile ("bounce.wav");

        auto audio = std::make_shared<juce::AudioBuffer<float>> (1, 2000);
        for (int i = 0; i < 2000; ++i)
            audio->setSample (0, i, 0.4f * (float) i / 2000.0f); // ランプ波

        BounceRenderer::Request request;
        request.sampleRate = 44100.0;
        request.bpm = 120.0;
        request.startSample = 500;
        request.endSample = 1500;
        request.wantTail = false;
        request.targetFile = target;
        BounceRenderer::TrackRender track;
        track.gain = 1.0f;
        track.clips.push_back ({ audio, 0, 0, 2000 });
        request.tracks.push_back (std::move (track));

        BounceRenderer renderer;
        expect (renderer.start (std::move (request)), "startできること");
        expect (waitForBounce (renderer), "タイムアウトせず完了すること");
        const auto result = renderer.takeResult();
        expect (result.status == BounceRenderer::Status::success, "successで終わること");
        expect (result.writtenSamples == 1000, "出力長=範囲サンプル長ちょうど");

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (
            wav.createReaderFor (new juce::FileInputStream (target), true));
        expect (reader != nullptr, "書き出したWAVを読めること");
        if (reader != nullptr)
        {
            expect (reader->lengthInSamples == 1000, "WAVの長さ=範囲サンプル長");
            juce::AudioBuffer<float> readBack (2, 1000);
            reader->read (&readBack, 0, 1000, 0, true, true);
            int mismatches = 0;
            for (int i = 0; i < 1000; ++i)
                if (std::abs (readBack.getSample (0, i) - audio->getSample (0, 500 + i)) > 1.0e-4f)
                    ++mismatches;
            expect (mismatches == 0, "出力内容がソースの範囲[500,1500)と一致すること");
        }
        dir.deleteRecursively();
    }

    // MIDIあり: 範囲頭を跨ぐ持続ノートが範囲頭から鳴り、テールなしで厳密長になること
    {
        const auto dir = makeTempDir();
        const auto target = dir.getChildFile ("bounce.wav");

        SynthBank bank;
        Track gmTrack; // Strings（持続音）
        gmTrack.type = TrackType::midi;
        gmTrack.gmProgram = 48;
        auto synth = bank.createIndependent (gmTrack, 44100.0, BounceRenderer::renderBlockSize);
        expect (synth != nullptr, "バウンス専用DLSインスタンスを作れること");
        if (synth == nullptr)
        {
            dir.deleteRecursively();
            return;
        }

        BounceRenderer::Request request;
        request.sampleRate = 44100.0;
        request.bpm = 120.0;
        request.startSample = 500;
        request.endSample = 500 + 22050; // 0.5秒
        request.wantTail = false; // サイクル書き出しはMIDIがあってもテールなし
        request.targetFile = target;
        BounceRenderer::TrackRender track;
        track.gain = 0.8f;
        track.synth = synth;
        track.notes.push_back ({ 0, Ppq::ticksPerQuarter * 4, 60, 100 }); // 範囲頭より前に始まり範囲全体を跨ぐ
        request.tracks.push_back (std::move (track));

        BounceRenderer renderer;
        expect (renderer.start (std::move (request)), "startできること");
        expect (waitForBounce (renderer), "タイムアウトせず完了すること");
        const auto result = renderer.takeResult();
        expect (result.status == BounceRenderer::Status::success, "successで終わること");
        expect (result.writtenSamples == 22050, "MIDIありでもテールなし＝範囲サンプル長ちょうど");
        expect (result.peak > 0.001f, "範囲頭を跨ぐノートが範囲内で鳴っていること");
        dir.deleteRecursively();
    }
}

// ---- ステレオ: loadWavのch規則（1ch=モノ・2ch以上=先頭2ch）とv6の表示名ラウンドトリップ ----
void testStereoClipLoadAndV6()
{
    beginTest ("stereo clip load and v6 name roundtrip");
    const auto dir = makeTempDir();

    // 任意ch数のテストWAV（ch番号で値を変える: ch0=0.1, ch1=0.2, ch2=0.3...）
    auto writeWav = [&] (const char* fileName, int numChannels, int numSamples)
    {
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::OutputStream> stream (dir.getChildFile (fileName).createOutputStream());
        if (stream == nullptr)
            return false;
        using Opts = juce::AudioFormatWriterOptions;
        auto writer = wavFormat.createWriterFor (stream,
            Opts{}.withSampleRate (44100.0).withNumChannels (numChannels).withBitsPerSample (16));
        if (writer == nullptr)
            return false;
        juce::AudioBuffer<float> buffer (numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, 0.1f * (float) (ch + 1));
        return writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
    };

    expect (writeWav ("clip-001.wav", 2, 2000), "ステレオWAVを書けること");
    expect (writeWav ("clip-002.wav", 4, 2000), "4chWAVを書けること");
    expect (writeWav ("clip-003.wav", 1, 2000), "モノWAVを書けること");

    // loadWavのch規則
    const auto stereo = Project::loadWav (dir.getChildFile ("clip-001.wav"));
    const auto multi = Project::loadWav (dir.getChildFile ("clip-002.wav"));
    const auto mono = Project::loadWav (dir.getChildFile ("clip-003.wav"));
    expect (stereo != nullptr && stereo->getNumChannels() == 2, "2chはステレオで読むこと");
    expect (multi != nullptr && multi->getNumChannels() == 2, "3ch以上は先頭2chだけ読むこと");
    expect (mono != nullptr && mono->getNumChannels() == 1, "1chはモノのまま読むこと");
    if (stereo != nullptr && stereo->getNumChannels() == 2)
    {
        expect (std::abs (stereo->getSample (0, 100) - 0.1f) < 0.001f, "L=ソースch0");
        expect (std::abs (stereo->getSample (1, 100) - 0.2f) < 0.001f, "R=ソースch1");
    }
    if (multi != nullptr && multi->getNumChannels() == 2)
        expect (std::abs (multi->getSample (1, 100) - 0.2f) < 0.001f, "4chでも先頭2ch目=ソースch1");

    // ステレオのピークキャッシュ: L/Rのmax合成（L=0.1, R=0.2 → 0.2）
    {
        Clip clip;
        clip.audio = stereo;
        clip.lengthSamples = 2000;
        clip.buildPeakCache();
        expect (! clip.peakCache.empty(), "ピークキャッシュが作られること");
        bool allMax = true;
        for (float peak : clip.peakCache)
            if (std::abs (peak - 0.2f) > 0.001f)
                allMax = false;
        expect (allMax, "ステレオのピークはL/Rのmax（0.2）");
    }

    // ステレオクリップの分割: audio共有・ピークキャッシュ再構築
    {
        Clip clip;
        clip.audio = stereo;
        clip.startSample = 0;
        clip.lengthSamples = 2000;
        clip.buildPeakCache();
        Clip left, right;
        expect (splitClip (clip, 800, left, right), "ステレオクリップを分割できること");
        expect (left.audio.get() == right.audio.get() && left.audio.get() == stereo.get(),
                "分割後も同じステレオバッファを共有すること");
        expect (right.offsetSamples == 800 && right.lengthSamples == 1200, "右側の参照範囲");
    }

    // v6: 表示名の保存・読込ラウンドトリップ（録音クリップ=空は書かない）
    {
        Project project;
        project.directory = dir;
        Track track;
        track.id = 1;
        Clip imported;
        imported.fileName = "clip-001.wav";
        imported.name = "dark-trap-140";
        imported.audio = stereo;
        imported.lengthSamples = 2000;
        Clip recorded;
        recorded.fileName = "clip-003.wav";
        recorded.audio = mono;
        recorded.lengthSamples = 2000;
        track.clips.push_back (std::move (imported));
        track.clips.push_back (std::move (recorded));
        project.tracks.push_back (std::move (track));

        // 取り込み中の一時ファイル（clip-*.wavのGCパターン外）が保存時のGCに消されないこと
        const auto importTemp = dir.getChildFile (".import-abc.wav.tmp");
        importTemp.replaceWithText ("partial data");
        // 参照されていないclip-*.wavはGCされること（対照群）
        const auto orphan = dir.getChildFile ("clip-099.wav");
        orphan.replaceWithText ("orphan");

        juce::String error;
        expect (project.save (error), "保存できること");
        expect (importTemp.existsAsFile(), "取り込み一時ファイルは保存時GCに消されないこと");
        expect (! orphan.existsAsFile(), "未参照のclip-*.wavはGCされること（対照群）");
        importTemp.deleteFile();

        const auto json = juce::JSON::parse (dir.getChildFile ("project.json").loadFileAsString());
        expect ((int) json.getProperty ("version", 0) == Project::currentVersion,
                "現行バージョンで保存されること");

        juce::StringArray warnings;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr, "読込できること");
        if (loaded != nullptr && ! loaded->tracks.empty() && loaded->tracks[0].clips.size() == 2)
        {
            expect (loaded->tracks[0].clips[0].name == "dark-trap-140", "取り込みクリップの表示名が復元されること");
            expect (loaded->tracks[0].clips[0].audio->getNumChannels() == 2, "ステレオで読み戻されること");
            expect (loaded->tracks[0].clips[1].name.isEmpty(), "録音クリップは無名のままなこと");
        }

        // buildSnapshotのhasStereoClip判定
        auto snapshot = loaded != nullptr ? loaded->buildSnapshot() : nullptr;
        expect (snapshot != nullptr && ! snapshot->tracks.empty() && snapshot->tracks[0].hasStereoClip,
                "ステレオクリップを含むトラックはhasStereoClipが立つこと");
    }

    dir.deleteRecursively();
}

// ---- エンジン: ステレオクリップのpan（バランス型）・モノとの同トラック混在・send・メーター ----
void testEngineStereoPan()
{
    beginTest ("engine stereo clip pan and mono mix");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // Lだけに信号があるステレオクリップ（L=0.5, R=0.0）
    Project project;
    Track track;
    track.id = 1;
    track.params->gain.store (1.0f);
    Clip stereoClip;
    stereoClip.audio = std::make_shared<juce::AudioBuffer<float>> (2, blockSize * 4);
    stereoClip.audio->clear();
    for (int i = 0; i < stereoClip.audio->getNumSamples(); ++i)
        stereoClip.audio->setSample (0, i, 0.5f);
    stereoClip.lengthSamples = blockSize * 4;
    track.clips.push_back (std::move (stereoClip));
    project.tracks.push_back (std::move (track));
    auto& params = *project.tracks[0].params;
    snapshots.push (project.buildSnapshot());

    juce::AudioBuffer<float> buffer (2, blockSize);
    auto measure = [&] (float& left, float& right)
    {
        transport.seekRequest.store (0);
        engine.play();
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        left = buffer.getMagnitude (0, 0, blockSize);
        right = buffer.getMagnitude (1, 0, blockSize);
        engine.stop();
        buffer.clear();
        engine.process (info); // 停止エッジの消化
    };

    float left = 0.0f, right = 0.0f;

    // panセンター: バランス型はセンター0dB → L=0.5, R=0（ソースがLのみ）
    measure (left, right);
    expect (std::abs (left - 0.5f) < 0.001f && right < 0.001f,
            "ステレオクリップはセンターで素通し（L=0.5, R=0）");
    expect (params.peakL.exchange (0.0f) > 0.45f, "Lメーターは真のLピーク");
    expect (params.peakR.exchange (0.0f) < 0.001f, "Rメーターはソース通り無音");

    // pan左振り切り: balL=1, balR=0 → L=0.5のまま（バランス型は増幅しない）
    params.pan.store (-1.0f);
    measure (left, right);
    expect (std::abs (left - 0.5f) < 0.001f && right < 0.001f,
            "左振り切りでもLは0.5のまま（バランス型・+3dBしない）");

    // pan右振り切り: balL=0 → L側ソースが消え、R側ソースは元々無音 → 両ch無音
    params.pan.store (1.0f);
    measure (left, right);
    expect (left < 0.001f && right < 0.001f, "右振り切りでLのみソースは無音になること");

    // モノクリップ（0.25）を同トラックへ重ねる: モノは等パワー型・ステレオはバランス型で共存
    params.pan.store (0.0f);
    {
        Clip monoClip;
        monoClip.audio = std::make_shared<juce::AudioBuffer<float>> (1, blockSize * 4);
        for (int i = 0; i < monoClip.audio->getNumSamples(); ++i)
            monoClip.audio->setSample (0, i, 0.25f);
        monoClip.lengthSamples = blockSize * 4;
        project.tracks[0].clips.push_back (std::move (monoClip));
    }
    snapshots.push (project.buildSnapshot());
    measure (left, right);
    expect (std::abs (left - 0.75f) < 0.001f, "混在トラックのL=ステレオL+モノ（0.5+0.25）");
    expect (std::abs (right - 0.25f) < 0.001f, "混在トラックのR=モノのみ（0.25）");

    // send: post-fader（gain・pan適用後）を素通しバス経由で二重加算
    params.sends[0].store (1.0f);
    measure (left, right);
    expect (std::abs (left - 1.5f) < 0.002f, "send100%でLが二重加算（1.5）");
    expect (std::abs (right - 0.5f) < 0.002f, "send100%でRが二重加算（0.5）");

    snapshots.deleteRetired();
}

// ---- リージョンゲインが再生に効くこと（モノ経路・ステレオ経路・重なりクリップ個別） ----
// トラックゲインでは再現できない「1トラック内のクリップ単位の差」を検証する
void testEngineClipGain()
{
    beginTest ("engine applies per-clip gain");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    juce::AudioBuffer<float> buffer (2, blockSize);
    const auto measure = [&] (float& left, float& right)
    {
        transport.seekRequest.store (0);
        engine.play();
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        left = buffer.getMagnitude (0, 0, blockSize);
        right = buffer.getMagnitude (1, 0, blockSize);
        engine.stop();
        buffer.clear();
        engine.process (info); // 停止エッジの消化
    };
    const auto makeConst = [] (int channels, float value)
    {
        auto audio = std::make_shared<juce::AudioBuffer<float>> (channels, blockSize * 2);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < audio->getNumSamples(); ++i)
                audio->setSample (ch, i, value);
        return audio;
    };

    float left = 0.0f, right = 0.0f;

    // ---- モノ経路: -6.02dB（0.5倍）のクリップは振幅が半分になる ----
    {
        Project project;
        Track track;
        track.id = 1;
        track.params->gain.store (1.0f);
        Clip clip;
        clip.audio = makeConst (1, 0.4f);
        clip.lengthSamples = blockSize * 2;
        clip.gain = 0.5f;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
        // acquire() は retired が空のときだけ pending を取り込む（PlaybackSnapshot.h）。
        // このテストは3構成を続けて差し替えるので、毎回メッセージスレッド側の掃除を挟む
        snapshots.deleteRetired();
        snapshots.push (project.buildSnapshot());

        measure (left, right);
        // モノは等パワー・センター補正型で、panセンターでは両ch 1.0倍（Pan::monoGains）
        expect (std::abs (left - 0.4f * 0.5f) < 0.001f, "モノ経路: L がクリップゲイン分だけ下がること");
        expect (std::abs (right - 0.4f * 0.5f) < 0.001f, "モノ経路: R も同じだけ下がること");
    }

    // ---- ステレオ経路: L/Rの比を保ったままスケールされる ----
    {
        Project project;
        Track track;
        track.id = 1;
        track.params->gain.store (1.0f);
        Clip clip;
        clip.audio = std::make_shared<juce::AudioBuffer<float>> (2, blockSize * 2);
        for (int i = 0; i < clip.audio->getNumSamples(); ++i)
        {
            clip.audio->setSample (0, i, 0.4f);
            clip.audio->setSample (1, i, 0.2f); // L:R = 2:1
        }
        clip.lengthSamples = blockSize * 2;
        clip.gain = 0.5f;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
        snapshots.deleteRetired();
        snapshots.push (project.buildSnapshot());

        measure (left, right);
        expect (std::abs (left - 0.2f) < 0.001f, "ステレオ経路: L = 0.4 * 0.5");
        expect (std::abs (right - 0.1f) < 0.001f, "ステレオ経路: R = 0.2 * 0.5（L:Rの比が保たれる）");
    }

    // ---- 重なりクリップ: 同一トラックでもクリップごとに別のゲインが効く ----
    {
        Project project;
        Track track;
        track.id = 1;
        track.params->gain.store (1.0f);
        Clip quiet;      // 0.4 を 0.5倍 → 0.2
        quiet.audio = makeConst (1, 0.4f);
        quiet.lengthSamples = blockSize * 2;
        quiet.gain = 0.5f;
        Clip loud;       // 0.1 を 2.0倍 → 0.2
        loud.audio = makeConst (1, 0.1f);
        loud.lengthSamples = blockSize * 2;
        loud.gain = 2.0f;
        track.clips.push_back (std::move (quiet));
        track.clips.push_back (std::move (loud));
        project.tracks.push_back (std::move (track));
        snapshots.deleteRetired();
        snapshots.push (project.buildSnapshot());

        measure (left, right);
        // 加算後 = 0.2 + 0.2（panセンターは両ch 1.0倍）。トラックゲイン1本ではこの組み合わせは作れない
        expect (std::abs (left - 0.4f) < 0.001f, "重なり: クリップごとのゲイン適用後に加算されること");
        expect (std::abs (right - 0.4f) < 0.001f, "重なり: R も同じ");
    }

    snapshots.deleteRetired();
}

// ---- エンジンとバウンスの一致: 同一のモノ＋ステレオ混在構成でL/Rサンプルが一致すること ----
void testEngineBounceStereoConsistency()
{
    beginTest ("engine vs bounce stereo consistency");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 8;
    constexpr int totalSamples = blockSize * numBlocks;

    auto makeAudio = [] (int channels, int len, float scale)
    {
        auto buffer = std::make_shared<juce::AudioBuffer<float>> (channels, len);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < len; ++i)
                buffer->setSample (ch, i, std::sin ((float) i * 0.07f + (float) ch * 1.5f) * scale);
        return buffer;
    };

    Project project;
    {
        Track track; // ステレオクリップ・pan右寄り・send・リージョンゲイン下げ
        track.id = 1;
        track.params->gain.store (0.8f);
        track.params->pan.store (0.4f);
        track.params->sends[1].store (0.3f);
        Clip clip;
        clip.audio = makeAudio (2, totalSamples, 0.4f);
        clip.lengthSamples = totalSamples;
        clip.gain = GainScale::toLinear (-3.0); // 2経路の一致をクリップゲイン込みで見る
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }
    {
        Track track; // モノクリップ・pan左寄り・リージョンゲイン上げ
        track.id = 2;
        track.params->gain.store (0.7f);
        track.params->pan.store (-0.6f);
        Clip clip;
        clip.audio = makeAudio (1, totalSamples, 0.3f);
        clip.startSample = 1000;
        clip.lengthSamples = totalSamples - 1000;
        clip.gain = GainScale::toLinear (2.0);
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }
    project.busParams[1]->gain.store (0.9f);
    project.masterParams->gain.store (0.85f);

    // エンジン側レンダリング
    juce::AudioBuffer<float> engineOut (2, totalSamples);
    {
        TransportState transport;
        SnapshotExchange snapshots;
        PreviewFifo previewFifo;
        PlaybackEngine engine (transport, snapshots, previewFifo);
        engine.prepareToPlay (blockSize, sr);
        snapshots.push (project.buildSnapshot());
        transport.seekRequest.store (0);
        engine.play();

        juce::AudioBuffer<float> buffer (2, blockSize);
        for (int blockIndex = 0; blockIndex < numBlocks; ++blockIndex)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            for (int ch = 0; ch < 2; ++ch)
                engineOut.copyFrom (ch, blockIndex * blockSize, buffer, ch, 0, blockSize);
        }
        engine.stop();
        snapshots.deleteRetired();
    }

    // バウンス側レンダリング
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce.wav");
    {
        BounceRenderer::Request request;
        request.sampleRate = sr;
        request.bpm = 120.0;
        request.endSample = totalSamples;
        request.targetFile = target;
        request.busGain[1] = 0.9f;
        request.masterGain = 0.85f;
        for (auto& track : project.tracks)
        {
            BounceRenderer::TrackRender render;
            render.gain = track.params->gain.load();
            render.pan = track.params->pan.load();
            for (int busIndex = 0; busIndex < numSendBuses; ++busIndex)
                render.sends[busIndex] = track.params->sends[busIndex].load();
            for (auto& clip : track.clips)
                appendClipPlaybacks (clip, render.clips); // 本番（⌘B/⌘E）と同じ変換経路を通す
            request.tracks.push_back (std::move (render));
        }
        BounceRenderer renderer;
        expect (renderer.start (std::move (request)), "startできること");
        expect (waitForBounce (renderer), "タイムアウトせず完了すること");
        expect (renderer.takeResult().status == BounceRenderer::Status::success, "successで終わること");
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::FileInputStream (target), true));
    expect (reader != nullptr && reader->lengthInSamples == totalSamples, "バウンス出力を読めること");
    if (reader != nullptr && reader->lengthInSamples == totalSamples)
    {
        juce::AudioBuffer<float> bounceOut (2, totalSamples);
        reader->read (&bounceOut, 0, totalSamples, 0, true, true);
        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < totalSamples; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (ch, i)
                                                         - bounceOut.getSample (ch, i)));
        // 24bit量子化＋浮動小数点の積和順序差ぶんの許容誤差
        expect (maxDiff < 1.0e-4f, "エンジンとバウンスのL/Rがサンプル一致（許容誤差内）すること");
    }
    reader.reset();
    dir.deleteRecursively();
}

// ---- AudioImporter: 変換コピーの仕様（ch規則・出力長・末尾・SR一致バイパス・失敗系）----
bool waitForImport (AudioImporter& importer)
{
    for (int i = 0; i < 600; ++i) // 最大60秒
    {
        if (importer.status() != AudioImporter::Status::running)
            return true;
        juce::Thread::sleep (100);
    }
    return false;
}

void testAudioImporter()
{
    beginTest ("AudioImporter conversion rules");
    const auto dir = makeTempDir();

    // サンプル値を自由に埋められるテストWAVライター（32bit floatで書き量子化誤差を避ける）
    auto writeWav = [&] (const char* fileName, const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::OutputStream> stream (dir.getChildFile (fileName).createOutputStream());
        if (stream == nullptr)
            return false;
        using Opts = juce::AudioFormatWriterOptions;
        auto writer = wavFormat.createWriterFor (stream,
            Opts{}.withSampleRate (sampleRate).withNumChannels (buffer.getNumChannels())
                  .withBitsPerSample (32));
        return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    };

    auto runImport = [&] (const char* sourceName, const char* tempName, double targetRate)
    {
        AudioImporter importer;
        AudioImporter::Request request;
        request.sourceFile = dir.getChildFile (sourceName);
        request.tempFile = dir.getChildFile (tempName);
        request.targetSampleRate = targetRate;
        expect (importer.start (std::move (request)), "startできること");
        expect (waitForImport (importer), "タイムアウトせず完了すること");
        return importer.takeResult();
    };

    auto readWav = [&] (const char* fileName, juce::AudioBuffer<float>& out, double& sampleRate)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (
            wav.createReaderFor (new juce::FileInputStream (dir.getChildFile (fileName)), true));
        if (reader == nullptr)
            return false;
        sampleRate = reader->sampleRate;
        out.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
        return reader->read (&out, 0, (int) reader->lengthInSamples, 0, true, reader->numChannels >= 2);
    };

    // ---- SR一致: リサンプルバイパス・内容がそのまま（24bit量子化誤差のみ）----
    {
        juce::AudioBuffer<float> source (2, 5000);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 5000; ++i)
                source.setSample (ch, i, std::sin ((float) i * 0.05f + (float) ch) * 0.6f);
        expect (writeWav ("same-sr.wav", source, 44100.0), "ソースWAVを書けること");

        const auto result = runImport ("same-sr.wav", "same-sr-out.tmp", 44100.0);
        expect (result.status == AudioImporter::Status::success, "SR一致で成功すること");
        expect (result.outputFrames == 5000, "SR一致は入力と同じ長さ");
        expect (result.numChannels == 2, "ステレオ維持");

        juce::AudioBuffer<float> out;
        double outRate = 0.0;
        expect (readWav ("same-sr-out.tmp", out, outRate), "出力を読めること");
        expect (juce::approximatelyEqual (outRate, 44100.0), "出力SR=ターゲット");
        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 5000; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (out.getSample (ch, i) - source.getSample (ch, i)));
        expect (maxDiff < 1.0e-4f, "SR一致は内容がそのまま（24bit量子化誤差のみ）");
    }

    // ---- SR変換: 出力長 = llround(in × target / source)・L/Rの内容が独立していること ----
    {
        constexpr int inputFrames = 44100;
        juce::AudioBuffer<float> source (2, inputFrames);
        for (int i = 0; i < inputFrames; ++i)
        {
            source.setSample (0, i, std::sin ((float) i * 0.02f) * 0.5f); // L: 低めの周波数
            source.setSample (1, i, 0.0f);                                // R: 無音（ch独立の検証）
        }
        expect (writeWav ("resample.wav", source, 44100.0), "ソースWAVを書けること");

        const auto result = runImport ("resample.wav", "resample-out.tmp", 48000.0);
        expect (result.status == AudioImporter::Status::success, "SR変換で成功すること");
        const auto expectedFrames = (juce::int64) std::llround ((double) inputFrames * 48000.0 / 44100.0);
        expect (result.outputFrames == expectedFrames, "出力長=llround(in×48000/44100)");

        juce::AudioBuffer<float> out;
        double outRate = 0.0;
        expect (readWav ("resample-out.tmp", out, outRate), "出力を読めること");
        expect (out.getNumSamples() == (int) expectedFrames, "WAVの長さも一致");
        expect (out.getMagnitude (0, 1000, 40000) > 0.4f, "Lに信号があること");
        expect (out.getMagnitude (1, 0, out.getNumSamples()) < 1.0e-3f,
                "Rは無音のまま（chごとに独立した補間器）");
    }

    // ---- パルス位置: 末尾パルスの残存（ゼロ埋め欠落防止）と中間パルスの位置（レイテンシ補償）----
    {
        constexpr int inputFrames = 44100;
        constexpr int midPulseIn = 22050;
        juce::AudioBuffer<float> source (1, inputFrames);
        source.clear();
        source.setSample (0, midPulseIn, 0.5f);
        source.setSample (0, inputFrames - 1, 0.8f);
        expect (writeWav ("tail-pulse.wav", source, 44100.0), "ソースWAVを書けること");

        const auto result = runImport ("tail-pulse.wav", "tail-pulse-out.tmp", 48000.0);
        expect (result.status == AudioImporter::Status::success, "成功すること");

        juce::AudioBuffer<float> out;
        double outRate = 0.0;
        expect (readWav ("tail-pulse-out.tmp", out, outRate), "出力を読めること");
        const int outLen = out.getNumSamples();

        // 末尾: 入力最終サンプルのパルスが出力末尾付近に残ること
        const float tailPeak = out.getMagnitude (0, juce::jmax (0, outLen - 100),
                                                 juce::jmin (100, outLen));
        expect (tailPeak > 0.3f, "末尾パルスが出力末尾付近に残ること（無音化・欠落なし）");

        // 中間: パルスの出力位置が換算位置±2サンプルにあること（補間器のレイテンシが補償されている）
        const int expectedPos = (int) std::llround ((double) midPulseIn * 48000.0 / 44100.0);
        int peakPos = 0;
        float peakValue = 0.0f;
        for (int i = juce::jmax (0, expectedPos - 300); i < juce::jmin (outLen, expectedPos + 300); ++i)
        {
            const float value = std::abs (out.getSample (0, i));
            if (value > peakValue)
            {
                peakValue = value;
                peakPos = i;
            }
        }
        expect (std::abs (peakPos - expectedPos) <= 2,
                "中間パルスが換算位置±2サンプルに出ること（全体シフトなし）");
    }

    // ---- targetSampleRate = 0（元のSRを保つ。サンプル音源の取り込み経路）----
    {
        constexpr int inputFrames = 3000;
        juce::AudioBuffer<float> source (1, inputFrames);
        for (int i = 0; i < inputFrames; ++i)
            source.setSample (0, i, std::sin ((float) i * 0.03f) * 0.7f);
        expect (writeWav ("keep-sr.wav", source, 44100.0), "44.1kソースを書けること");

        const auto result = runImport ("keep-sr.wav", "keep-sr-out.tmp", 0.0);
        expect (result.status == AudioImporter::Status::success, "SR保持経路で成功すること");
        expect (result.outputFrames == inputFrames, "SR保持は入力と同じフレーム数（リサンプルなし）");
        expect (juce::approximatelyEqual (result.sourceSampleRate, 44100.0), "元SRが結果に載ること");

        juce::AudioBuffer<float> out;
        double outRate = 0.0;
        expect (readWav ("keep-sr-out.tmp", out, outRate), "出力を読めること");
        expect (juce::approximatelyEqual (outRate, 44100.0), "出力WAVのSRが元のまま（44.1k）");
        expect (out.getNumSamples() == inputFrames, "出力WAVの長さも入力と一致");
        float maxDiff = 0.0f;
        for (int i = 0; i < inputFrames; ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (out.getSample (0, i) - source.getSample (0, i)));
        expect (maxDiff < 1.0e-4f, "SR保持は内容がそのまま（リサンプルをバイパス）");
    }

    // ---- 4ch → 先頭2chのみ ----
    {
        juce::AudioBuffer<float> source (4, 2000);
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < 2000; ++i)
                source.setSample (ch, i, 0.1f * (float) (ch + 1));
        expect (writeWav ("multi.wav", source, 44100.0), "4chソースを書けること");

        const auto result = runImport ("multi.wav", "multi-out.tmp", 44100.0);
        expect (result.status == AudioImporter::Status::success, "成功すること");
        expect (result.numChannels == 2, "4chは先頭2chだけ採用");

        juce::AudioBuffer<float> out;
        double outRate = 0.0;
        expect (readWav ("multi-out.tmp", out, outRate), "出力を読めること");
        expect (out.getNumChannels() == 2, "出力WAVは2ch");
        expect (std::abs (out.getSample (1, 500) - 0.2f) < 0.001f, "2ch目=ソースch1");
    }

    // ---- 失敗系: 存在しない・壊れたファイル ----
    {
        auto result = runImport ("missing.wav", "missing-out.tmp", 44100.0);
        expect (result.status == AudioImporter::Status::failed, "存在しないファイルはfailed");
        expect (result.errorMessage.isNotEmpty(), "エラーメッセージがあること");

        dir.getChildFile ("broken.wav").replaceWithText ("not a wav file");
        result = runImport ("broken.wav", "broken-out.tmp", 44100.0);
        expect (result.status == AudioImporter::Status::failed, "壊れたファイルはfailed");
        expect (! dir.getChildFile ("broken-out.tmp").existsAsFile(), "失敗時は一時ファイルが残らないこと");
    }

    dir.deleteRecursively();
}






// ---- 回帰ハッシュ: モノのみ構成の決定的レンダリング結果のMD5をstdoutへ出す ----
// ステレオ対応リファクタの前後で「モノのみトラックの出力がビット一致で不変」であることを、
// このハッシュの目視比較で確認する。期待値はハードコードしない（浮動小数点の積和順序は
// コンパイラ・環境で変わりうるため、同一環境での変更前後比較にのみ使う）。
// 重なりクリップ×pan×send×Masterの全経路を通し、積和順序の変化を検出できる構成にする
void testMonoRenderRegressionHash()
{
    beginTest ("mono render regression hash");

    // FNV-1a 64bit（juce_cryptographyを増やさないための自前ハッシュ。比較用途には十分）
    auto fnv1a = [] (const void* data, size_t size)
    {
        const auto* bytes = static_cast<const juce::uint8*> (data);
        juce::uint64 hash = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 0x100000001b3ULL;
        return juce::String::toHexString ((juce::int64) hash);
    };

    auto makeAudio = [] (int len, float scale, float phase)
    {
        auto buffer = std::make_shared<juce::AudioBuffer<float>> (1, len);
        for (int i = 0; i < len; ++i)
            buffer->setSample (0, i, std::sin ((float) i * 0.13f + phase) * scale);
        return buffer;
    };

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 32;

    // トラック1: 重なりクリップ2つ・pan左・send2本 / トラック2: pan右
    Project project;
    {
        Track track;
        track.id = 1;
        track.params->gain.store (0.7f);
        track.params->pan.store (-0.3f);
        track.params->sends[0].store (0.5f);
        track.params->sends[2].store (0.25f);
        Clip a;
        a.audio = makeAudio (8000, 0.5f, 0.0f);
        a.startSample = 0;
        a.lengthSamples = 8000;
        Clip b;
        b.audio = makeAudio (8000, 0.4f, 1.0f);
        b.startSample = 4000;
        b.lengthSamples = 8000;
        track.clips.push_back (std::move (a));
        track.clips.push_back (std::move (b));
        project.tracks.push_back (std::move (track));
    }
    {
        Track track;
        track.id = 2;
        track.params->gain.store (0.9f);
        track.params->pan.store (0.5f);
        Clip c;
        c.audio = makeAudio (8000, 0.3f, 2.0f);
        c.startSample = 2000;
        c.lengthSamples = 8000;
        track.clips.push_back (std::move (c));
        project.tracks.push_back (std::move (track));
    }
    project.busParams[0]->gain.store (0.8f);
    project.masterParams->gain.store (0.9f);

    // エンジン経路
    {
        TransportState transport;
        SnapshotExchange snapshots;
        PreviewFifo previewFifo;
        PlaybackEngine engine (transport, snapshots, previewFifo);
        engine.prepareToPlay (blockSize, sr);
        snapshots.push (project.buildSnapshot());

        transport.seekRequest.store (0);
        engine.play();

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MemoryBlock rendered;
        for (int blockIndex = 0; blockIndex < numBlocks; ++blockIndex)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            for (int ch = 0; ch < 2; ++ch)
                rendered.append (buffer.getReadPointer (ch), sizeof (float) * (size_t) blockSize);
        }
        engine.stop();
        std::cout << "hash-engine: " << fnv1a (rendered.getData(), rendered.getSize()) << std::endl;
        snapshots.deleteRetired();
    }

    // バウンス経路（gain/pan/sendはRequestへ焼き込み）
    {
        const auto dir = makeTempDir();
        const auto target = dir.getChildFile ("bounce.wav");

        BounceRenderer::Request request;
        request.sampleRate = sr;
        request.bpm = 120.0;
        request.endSample = blockSize * numBlocks;
        request.targetFile = target;
        request.busGain[0] = 0.8f;
        request.masterGain = 0.9f;
        for (auto& track : project.tracks)
        {
            BounceRenderer::TrackRender render;
            render.gain = track.params->gain.load();
            render.pan = track.params->pan.load();
            for (int busIndex = 0; busIndex < numSendBuses; ++busIndex)
                render.sends[busIndex] = track.params->sends[busIndex].load();
            for (auto& clip : track.clips)
                render.clips.push_back ({ clip.audio, clip.startSample, clip.offsetSamples, clip.lengthSamples });
            request.tracks.push_back (std::move (render));
        }

        BounceRenderer renderer;
        expect (renderer.start (std::move (request)), "startできること");
        expect (waitForBounce (renderer), "タイムアウトせず完了すること");
        expect (renderer.takeResult().status == BounceRenderer::Status::success, "successで終わること");

        juce::MemoryBlock fileData;
        expect (target.loadFileAsData (fileData), "書き出したWAVを読めること");
        std::cout << "hash-bounce: " << fnv1a (fileData.getData(), fileData.getSize()) << std::endl;
        dir.deleteRecursively();
    }
}


} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager 初期化（AUインスタンス化に必要）

    testV1ToV2Roundtrip();
    testMidiRoundtrip();
    testInvalidJson();
    testClampNoteBoundaries();
    testClipOffsetsV2Migration();
    testClipOffsetClamp();
    testSharedWavBufferOnLoad();
    testBuildSnapshotClipOffsets();
    testEngineReadsClipOffsets();
    testSplitClip();
    testSplitMidiRegion();
    testSectionMarkers();
    testSectionMarkersInvalidLoad();
    testUndoStack();
    testSaveGcProtectsUndoWavs();
    testSaveGcProtectsClipboardWav();
    testRegionEditShortcuts();
    testLoopExpansion();
    testLoopRoundtrip();
    testClipGainRoundtrip();
    testClipGainSnapshot();
    testSpreadLoopedBins();
    testBuildSnapshotFlattensNotes();
    testSynthBank();
    testPlaybackEngineMidi();
    testTrackLevelMeter();
    testSnapshotSwapDuringPlayback();
    testAudioValuesOnlySnapshot();
    testOverflowDoesNotKillOtherNotes();
    testSamplerProjectRoundtrip();
    testSynthBankSampler();
    testSamplerEngine();
    testSamplerThroughPlaybackEngine();
    testDlsMusicDeviceRendersAudio();
    testBounceRendererBasic();
    testBounceRendererClippingProtection();
    testBounceRendererMidiTail();
    testBounceSampler();
    testBuildItemRender();
    testProjectMemoRoundtrip();
    testMixerParamsRoundtrip();
    testEnginePanSendsMaster();
    testEngineOutputChannelRule();
    testPreviewThroughMaster();
    testCycleRoundtrip();
    testPlaybackEngineCycleLoop();
    testBounceCycleRange();
    testStereoClipLoadAndV6();
    testEngineStereoPan();
    testEngineClipGain();
    testEngineBounceStereoConsistency();
    testAudioImporter();
    testAudioFilePreview();
    testPreviewPolicy();
    testFileSortOrder();
    testBottomPanelHistory();
    testBottomPanelShortcuts();
    testGainScale();
    testGainSliderIgnoresScrollWheel();
    testGainSliderCenterFill();
    testMonoRenderRegressionHash();

    if (failureCount > 0)
    {
        std::cout << failureCount << " test(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "all tests passed" << std::endl;
    return 0;
}
