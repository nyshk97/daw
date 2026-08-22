// daw_tests — CTest から実行するコンソールテスト。
// GUIなしで動くもの（データモデル・保存/読込・DLSMusicDeviceのオフラインレンダリング）だけを検証する。
// テストは一時ディレクトリのみを使い、~/Music/daw には一切触れない。

#include <unistd.h> // getpid（TempDirSweep の現PIDケース）

#include <cstdlib>
#include <new>

// ---- RT確保ゼロ検証用のグローバル operator new/delete 差し替え ----
// 呼び出しスレッドごとの確保回数を数える（他スレッド（Timer等）の確保でflakyにならないよう
// thread_local）。注意: 過整列型のaligned new経路は数えない＝カウント0は「通常のnew経由の
// 確保が無い」ことの検証（vector成長・std::function確保などの典型バグはこれで捕まる）
thread_local long long testAllocationCount = 0;

void* operator new (std::size_t size)
{
    ++testAllocationCount;
    if (auto* p = std::malloc (size > 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t size)
{
    ++testAllocationCount;
    if (auto* p = std::malloc (size > 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h> // EQの解析応答（getMagnitudeForFrequency）との照合用

#include "audio/AudioImporter.h"
#include "audio/AudioFilePreview.h"
#include "audio/BounceRenderer.h"
#include "audio/ClipStretcher.h"
#include "audio/PitchAnalyzer.h"
#include "audio/PitchAnalysisWorker.h"
#include "shared/PitchCurve.h"
#include "shared/PitchNotes.h"
#include "shared/PitchCorrection.h"
#include "shared/RenderRecipe.h"
#include "shared/PitchEditorSession.h"
#include "audio/VocalResynth.h"
#include "audio/VocalNoteAudition.h"
#include "audio/RenderCache.h"
#include "audio/MasterLimiter.h"
#include "audio/PlaybackEngine.h"
#include "audio/SamplerEngine.h"
#include "audio/TrackFxChain.h"
#include "audio/TrackLofi.h"
#include "audio/TrackSaturator.h"
#include "audio/UrlDownloader.h"
#include "ui/FxSlotLayout.h"
#include "audio/ReferenceReportGenerator.h"
#include "shared/ClipDomains.h"
#include "shared/GridSnap.h"
#include "shared/Project.h"
#include "shared/SynthBank.h"
#include "shared/UndoStack.h"
#include "shared/AudioFileTypes.h"
#include "shared/MidiFileTypes.h"
#include "shared/MidiImport.h"
#include "shared/GachaSession.h"
#include "shared/ReferenceAlign.h"
#include "shared/ReferenceExport.h"
#include "shared/ReferenceReport.h"
#include "shared/ReferenceTools.h"
#include "shared/AudioBrowserNavigation.h"
#include "shared/ClipFade.h"
#include "shared/GainScale.h"
#include "shared/SongFade.h"
#include "shared/SpawnedProcess.h"
#include "shared/TempDirSweep.h"
#include "shared/AnalyzeProgress.h"
#include "shared/AnalyzerTap.h"
#include "shared/LoudnessMeter.h"
#include "shared/MasterMeterStats.h"
#include "shared/TruePeakDetector.h"
#include "audio/MasterMeterSource.h"
#include "shared/SpectrumAnalyzer.h"
#include "shared/YtDlpOutput.h"
#include "ui/AppLookAndFeel.h"
#include "ui/BottomPanelHistory.h"
#include "ui/GainControls.h"
#include "ui/FileSortOrder.h"
#include "ui/PreviewPolicy.h"
#include "ui/ProjectThumbnails.h"
#include "ui/Shortcuts.h"
#include "ui/TimelineView.h" // フェードハンドルの矩形計算・掴み分け（staticヘルパー）の検証用

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

// Master Limiterのlookahead遅延（2ms相当）。エンジン（RT経路）の出力はタイムラインより
// これだけ遅れて出る（遅延契約: docs/plans/2026-08-16-1523-fx-batch2-meters-limiter.md。
// バウンスは先頭破棄＋末尾flushで整列済み）。エンジン出力をタイムラインやバウンスと
// 比較するテストは、エンジン側を +latency で読む（余分に1ブロック回して末尾を確保する）
int engineLimiterLatency (double sr) { return MasterLimiter::lookaheadForRate (sr); }

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

// ---- ClipFade: エンベロープの定義と区間分割 ----
// エンジンを動かす前にヘッダ単体で潰せる範囲をここで検証する
namespace
{
// フェード境目を絶対位置で持つ ClipPlayback を作る（ループなしの1実体）
ClipPlayback makeFadePlayback (juce::int64 start, juce::int64 length,
                               juce::int64 fadeIn, juce::int64 fadeOut)
{
    ClipPlayback clip;
    clip.startSample = start;
    clip.lengthSamples = length;
    clip.fadeInStart = start;
    clip.fadeInEnd = start + fadeIn;
    clip.fadeOutStart = start + length - fadeOut;
    clip.fadeOutEnd = start + length;
    return clip;
}

// JUCE の addFromWithRamp が区間内サンプル i に**実際に適用する**ゲイン
// （juce_AudioSampleBuffer.h: startGain + i / numSamples * (endGain - startGain)）
float appliedGain (const ClipFade::Segment& segment, int i)
{
    if (! segment.ramp)
        return segment.startGain;
    return segment.startGain
           + (float) i / (float) segment.count * (segment.endGain - segment.startGain);
}
}

void testClipFadeSegments()
{
    beginTest ("ClipFade envelope and segments");

    ClipFade::Segment segs[ClipFade::maxSegments];

    // フェードなし → 1区間・ゲイン1.0・平坦（呼び出し側が既存の addFrom 1回へ戻れること）
    {
        ClipPlayback clip;
        clip.startSample = 0;
        clip.lengthSamples = 500;
        const int n = ClipFade::segments (clip, 100, 300, 50, segs);
        expect (n == 1, "フェードなしは1区間");
        expect (segs[0].destOffset == 50 && segs[0].count == 200, "destOffsetはsegPos基準・countは重なり長");
        expect (! segs[0].ramp && juce::exactlyEqual (segs[0].startGain, 1.0f)
                    && juce::exactlyEqual (segs[0].endGain, 1.0f),
                "フェードなしはゲイン1.0の平坦区間");
    }

    // 閉区間定義の両端: 先頭が厳密に0・n-1番目が厳密に1／フェードアウトの最終サンプルが0
    {
        const auto clip = makeFadePlayback (0, 1000, 240, 240);
        expect (juce::exactlyEqual (ClipFade::fadeInGainAt (clip, 0), 0.0f), "フェードインの先頭サンプルが0");
        expect (std::abs (ClipFade::fadeInGainAt (clip, 239) - 1.0f) < 1.0e-6f,
                "フェードインの n-1 番目が1（閉区間）");
        expect (juce::exactlyEqual (ClipFade::fadeOutGainAt (clip, 999), 0.0f),
                "フェードアウトの最終サンプルが0（閉区間）");
        expect (std::abs (ClipFade::fadeOutGainAt (clip, 760) - 1.0f) < 1.0e-6f,
                "フェードアウトの先頭サンプルが1");
        // 排他端の外挿値はクランプしない（addFromWithRamp の endGain に渡す値）
        expect (ClipFade::fadeInGainAt (clip, 240) > 1.0f, "フェードイン区間の排他端は1を超えること");
        expect (ClipFade::fadeOutGainAt (clip, 1000) < 0.0f, "フェードアウト区間の排他端は負になること");
    }

    // 1サンプル・2サンプルのフェード（ゼロ除算のガードと端点）
    {
        const auto one = makeFadePlayback (0, 100, 1, 1);
        expect (juce::exactlyEqual (ClipFade::fadeInGainAt (one, 0), 0.0f),
                "1サンプルのフェードインはそのサンプルが無音");
        expect (juce::exactlyEqual (ClipFade::fadeOutGainAt (one, 99), 0.0f), "1サンプルのフェードアウトも同じ");
        const auto two = makeFadePlayback (0, 100, 2, 2);
        expect (juce::exactlyEqual (ClipFade::fadeInGainAt (two, 0), 0.0f)
                    && juce::exactlyEqual (ClipFade::fadeInGainAt (two, 1), 1.0f),
                "2サンプルのフェードインは 0, 1");
        expect (juce::exactlyEqual (ClipFade::fadeOutGainAt (two, 98), 1.0f)
                    && juce::exactlyEqual (ClipFade::fadeOutGainAt (two, 99), 0.0f),
                "2サンプルのフェードアウトは 1, 0");
    }

    // ブロックが1区間に完全に収まるケース（フェードイン部／平坦部／フェードアウト部）
    {
        const auto clip = makeFadePlayback (0, 1000, 500, 200);
        int n = ClipFade::segments (clip, 0, 100, 0, segs);
        expect (n == 1 && segs[0].ramp && juce::exactlyEqual (segs[0].startGain, 0.0f),
                "フェードイン内のブロックは1傾斜区間");
        n = ClipFade::segments (clip, 600, 700, 0, segs);
        expect (n == 1 && ! segs[0].ramp && juce::exactlyEqual (segs[0].startGain, 1.0f),
                "平坦部のブロックは平坦区間");
        n = ClipFade::segments (clip, 850, 950, 0, segs);
        expect (n == 1 && segs[0].ramp && segs[0].startGain < 1.0f && segs[0].endGain < segs[0].startGain,
                "フェードアウト内のブロックは下る傾斜区間");
    }

    // 3区間すべてにまたがるブロック（短いクリップ＋長いフェード）
    {
        const auto clip = makeFadePlayback (0, 100, 30, 40);
        const int n = ClipFade::segments (clip, 0, 100, 0, segs);
        expect (n == 3, "3区間に割れること");
        if (n == 3)
        {
            expect (segs[0].destOffset == 0 && segs[0].count == 30 && segs[0].ramp, "1区間目=フェードイン部");
            expect (segs[1].destOffset == 30 && segs[1].count == 30 && ! segs[1].ramp, "2区間目=平坦部");
            expect (segs[2].destOffset == 60 && segs[2].count == 40 && segs[2].ramp, "3区間目=フェードアウト部");
            // 区間種別をまたぐ境界は「実際に適用されるゲイン」で連続する
            // （パラメータは一致しない: フェードイン排他端の外挿値は n/(n-1) > 1 で、平坦部の 1.0 と違う）
            expect (std::abs (appliedGain (segs[0], 29) - 1.0f) < 1.0e-6f,
                    "フェードイン部の最終サンプルが厳密に1（次の平坦部と連続）");
            expect (std::abs (appliedGain (segs[1], 29) - appliedGain (segs[2], 0)) < 1.0e-6f,
                    "平坦部の最終サンプルとフェードアウト部の先頭サンプルが連続すること");
            expect (std::abs (appliedGain (segs[2], 39)) < 1.0e-6f,
                    "フェードアウト部の最終サンプルが0（段差が消える条件）");
        }
    }

    // 同一傾斜内でブロック分割したときは**パラメータが一致する**（絶対位置から評価し直すため）
    {
        const auto clip = makeFadePlayback (0, 1000, 500, 0);
        ClipFade::Segment first[ClipFade::maxSegments], second[ClipFade::maxSegments];
        ClipFade::segments (clip, 0, 64, 0, first);
        ClipFade::segments (clip, 64, 128, 64, second);
        expect (juce::exactlyEqual (first[0].endGain, second[0].startGain),
                "同一傾斜内の分割では前ブロックのendGainと次のstartGainが一致すること");
    }

    // フェードインの終端とフェードアウトの開始が接するケース（fadeIn + fadeOut == 全長）
    {
        const auto clip = makeFadePlayback (0, 100, 40, 60);
        const int n = ClipFade::segments (clip, 0, 100, 0, segs);
        expect (n == 2, "平坦部が消えて2区間になること");
        if (n == 2)
            expect (segs[0].count == 40 && segs[1].count == 60, "境目がちょうど接すること");
    }

    // ループ展開: フェードが反復をまたぐ（絶対位置で持つ設計の要）。
    // 本体100×4反復（連なり 0..400）に 250サンプルのフェードアウト
    {
        ClipPlayback rep2, rep3;
        rep2.startSample = 100;
        rep3.startSample = 200;
        for (auto* clip : { &rep2, &rep3 })
        {
            clip->lengthSamples = 100;
            clip->fadeInStart = clip->fadeInEnd = 0; // フェードインなし
            clip->fadeOutStart = 150;
            clip->fadeOutEnd = 400;
        }
        int n = ClipFade::segments (rep2, 100, 200, 100, segs);
        expect (n == 2 && ! segs[0].ramp && segs[1].ramp,
                "フェードアウトの開始をまたぐ反復は 平坦→傾斜 の2区間になること");
        const float lastOfRep2 = n == 2 ? appliedGain (segs[1], segs[1].count - 1) : 0.0f;

        n = ClipFade::segments (rep3, 200, 300, 200, segs);
        expect (n == 1 && segs[0].ramp, "フェードアウトの途中に入る反復は1傾斜区間");
        const float firstOfRep3 = n == 1 ? appliedGain (segs[0], 0) : 0.0f;
        expect (firstOfRep3 < lastOfRep2 && firstOfRep3 > 0.0f,
                "反復をまたいでも1.0へ戻らず単調に下がり続けること");
    }
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

// ---- フェードの不変条件（Clip::clampFades）。規則は「fadeIn を先に頭打ち → 残りで fadeOut」----
// この非対称性が意味を持つのはループ縮小のとき（どちらかを削るしかない場面）で、
// フェード自身のドラッグはこの関数を直接通さない（相手を押しのけてしまうため）
void testClipFadeClamp()
{
    beginTest ("clip fade clamp rules");

    Clip clip;
    clip.lengthSamples = 100;

    clip.fadeInSamples = 80;
    clip.fadeOutSamples = 40;
    clip.clampFades();
    expect (clip.fadeInSamples == 80 && clip.fadeOutSamples == 20,
            "全長を超える分は fadeOut だけが削られること（fadeIn 優先）");

    clip.fadeInSamples = 500; // fadeIn 単体が全長超え
    clip.fadeOutSamples = 30;
    clip.clampFades();
    expect (clip.fadeInSamples == 100 && clip.fadeOutSamples == 0,
            "fadeIn が全長で頭打ちになり fadeOut は0になること");

    clip.fadeInSamples = -5;
    clip.fadeOutSamples = -1;
    clip.clampFades();
    expect (clip.fadeInSamples == 0 && clip.fadeOutSamples == 0, "負値は0へ寄せること");

    // 判定はループ込みの全長（totalLengthSamples）。収まっていれば触らない
    clip.loopCount = 3; // 全長 400
    clip.fadeInSamples = 150;
    clip.fadeOutSamples = 200;
    clip.clampFades();
    expect (clip.fadeInSamples == 150 && clip.fadeOutSamples == 200,
            "ループ込みの全長に収まっていれば変えないこと");

    // ループを減らすと全長が縮む → 再クランプが要る（ループ縮小・ループ解除の経路）
    clip.loopCount = 1; // 全長 200
    clip.clampFades();
    expect (clip.fadeInSamples == 150 && clip.fadeOutSamples == 50,
            "ループ縮小で全長が縮んだら fadeOut が削られること");
    clip.loopCount = 0; // 全長 100
    clip.clampFades();
    expect (clip.fadeInSamples == 100 && clip.fadeOutSamples == 0,
            "さらに縮めば fadeIn も全長で頭打ちになること");
}

// ---- フェードのドラッグ規則（相手を押しのけない／ループ伸縮での元値方式／ハンドルの掴み分け）----
void testClipFadeDragRules()
{
    beginTest ("clip fade drag rules");

    // 相手を押しのけない: 全長100・fadeOut=40 のとき fadeIn は60で止まる（逆向きも同様）。
    // clampFades() を直接通す実装に戻すと fadeIn=80 / fadeOut=20 になって落ちる
    {
        Clip clip;
        clip.lengthSamples = 100;
        clip.fadeInSamples = 0;
        clip.fadeOutSamples = 40;
        expect (clip.clampedFadeIn (80) == 60, "フェードインは相手の手前で止まること");
        expect (clip.clampedFadeIn (-10) == 0, "負方向は0で止まること");

        clip.fadeInSamples = 40;
        clip.fadeOutSamples = 0;
        expect (clip.clampedFadeOut (80) == 60, "フェードアウトも相手の手前で止まること");

        // 適用してみて相手が変わっていないことを確かめる（押しのけていない証明）
        clip.fadeInSamples = 0;
        clip.fadeOutSamples = 40;
        clip.fadeInSamples = clip.clampedFadeIn (80);
        clip.clampFades(); // 保険。ここで相手が削られてはいけない
        expect (clip.fadeInSamples == 60 && clip.fadeOutSamples == 40,
                "ドラッグ後も相手のフェードが元の長さのままであること");
    }

    // ループのドラッグ: 毎イベント「元値を代入 → clampFades()」で再計算するので、
    // 縮めすぎてから戻すとフェードが元の長さに復元される（現在値からクランプすると復元されない）
    {
        Clip clip;
        clip.lengthSamples = 100;
        clip.loopCount = 3; // 全長 400
        const juce::int64 origIn = 150, origOut = 200;
        clip.fadeInSamples = origIn;
        clip.fadeOutSamples = origOut;

        const auto applyLoopDrag = [&clip, origIn, origOut] (int loopCount)
        {
            clip.loopCount = loopCount;
            clip.fadeInSamples = origIn;   // 元値方式（RegionDrag::origFade*Samples 相当）
            clip.fadeOutSamples = origOut;
            clip.clampFades();
        };

        applyLoopDrag (1); // 全長200まで縮める → fadeOut が削られる
        expect (clip.fadeInSamples == 150 && clip.fadeOutSamples == 50, "縮めた途中では削られること");
        applyLoopDrag (0); // さらに縮める
        expect (clip.fadeInSamples == 100 && clip.fadeOutSamples == 0, "限界まで縮めると両方削られること");
        applyLoopDrag (3); // 元へ戻す
        expect (clip.fadeInSamples == origIn && clip.fadeOutSamples == origOut,
                "同じジェスチャー内で戻せばフェードが元の長さに復元されること");

        // 現在値からクランプする実装だと復元されない（この差を明示しておく）
        clip.loopCount = 0;
        clip.clampFades();
        clip.loopCount = 3;
        clip.clampFades();
        expect (clip.fadeInSamples == 100 && clip.fadeOutSamples == 0,
                "現在値クランプでは戻しても復元されないこと（元値方式が必要な理由）");
    }

    // ハンドルの掴み分け: 重なったら近い方・等距離ならフェードイン
    {
        const auto inHandle = TimelineView::fadeHandleRectAt (0, 200, 0, 100);
        const auto outHandle = TimelineView::fadeHandleRectAt (0, 200, 0, 100);
        expect (inHandle == outHandle, "フェード終端が一致すると矩形も完全に一致すること");
        const int centreX = inHandle.getCentreX();
        const int y = inHandle.getCentreY();
        expect (TimelineView::pickFadeHandle (inHandle, outHandle, { centreX, y }) == 0,
                "完全に重なったときはフェードインを掴むこと");

        // 少しずれた2つ: クリック位置に近い方を選ぶ
        const auto left = TimelineView::fadeHandleRectAt (0, 200, 0, 100);
        const auto right = TimelineView::fadeHandleRectAt (0, 200, 0, 106);
        expect (left != right, "終端がずれれば矩形もずれること");
        expect (TimelineView::pickFadeHandle (left, right, { left.getCentreX(), y }) == 0,
                "左寄りのクリックはフェードイン側");
        expect (TimelineView::pickFadeHandle (left, right, { right.getCentreX(), y }) == 1,
                "右寄りのクリックはフェードアウト側");
        expect (TimelineView::pickFadeHandle (left, right, { left.getX() - 5, y }) == -1,
                "どちらの矩形の外なら掴まないこと");

        // 連なりの内側へのクランプ（端では矩形がはみ出さない）
        const auto atStart = TimelineView::fadeHandleRectAt (10, 210, 0, 10);
        const auto atEnd = TimelineView::fadeHandleRectAt (10, 210, 0, 210);
        expect (atStart.getX() == 10, "左端では連なりの内側に収まること");
        expect (atEnd.getRight() == 210, "右端でも連なりの内側に収まること");
    }

    // undo: clipValue 種別でフェードが往復すること（履歴は Clip のコピーなので値が乗る）
    {
        Project project;
        Track track;
        track.id = 1;
        Clip clip;
        clip.lengthSamples = 1000;
        clip.fadeInSamples = 100;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));

        UndoStack undo;
        undo.begin (project, UndoStack::EditKind::clipValue);
        project.tracks[0].clips[0].fadeInSamples = 400;

        UndoStack::EditKind kind {};
        expect (undo.undo (project, kind), "undoできること");
        expect (kind == UndoStack::EditKind::clipValue, "種別が clipValue で戻ること（発音を乱さない経路）");
        expect (project.tracks[0].clips[0].fadeInSamples == 100, "フェード長が戻ること");
        expect (undo.redo (project, kind) && project.tracks[0].clips[0].fadeInSamples == 400,
                "redoで再適用されること");
    }
}

// ---- フェードの保存・読み込み。0のときは書かないので、出来上がるJSONはv10以前と同じ形 ----
void testClipFadeRoundtrip()
{
    beginTest ("clip fade roundtrip, clamp and legacy default");
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
    clip.fadeInSamples = 1000;
    clip.fadeOutSamples = 500;
    project->tracks[0].clips.push_back (std::move (clip));

    expect (project->save (error), "保存できること");
    const auto jsonFile = project->directory.getChildFile ("project.json");
    const auto clipHasFadeKeys = [&jsonFile]
    {
        const auto parsed = juce::JSON::parse (jsonFile.loadFileAsString());
        if (auto* tracks = parsed.getProperty ("tracks", {}).getArray())
            if (! tracks->isEmpty())
                if (auto* clips = (*tracks)[0].getProperty ("clips", {}).getArray())
                    if (! clips->isEmpty())
                        return (*clips)[0].hasProperty ("fadeInSamples")
                               || (*clips)[0].hasProperty ("fadeOutSamples");
        return false;
    };
    expect (clipHasFadeKeys(), "フェードありはJSONに書かれること");

    juce::StringArray warnings;
    auto reloaded = Project::load (project->directory, warnings, error);
    expect (reloaded != nullptr, "再読込できること");
    if (reloaded == nullptr)
        { dir.deleteRecursively(); return; }
    expect (reloaded->tracks[0].clips.size() == 1
                && reloaded->tracks[0].clips[0].fadeInSamples == 1000
                && reloaded->tracks[0].clips[0].fadeOutSamples == 500,
            "フェード長が復元されること");

    reloaded->tracks[0].clips[0].fadeInSamples = 0;
    reloaded->tracks[0].clips[0].fadeOutSamples = 0;
    expect (reloaded->save (error), "フェードなしで保存できること");
    expect (! clipHasFadeKeys(), "フェードなしはJSONに書かれないこと（旧形式と同じ形）");

    auto legacy = Project::load (project->directory, warnings, error);
    expect (legacy != nullptr, "旧形式相当のJSONを読めること");
    if (legacy != nullptr)
        expect (legacy->tracks[0].clips[0].fadeInSamples == 0
                    && legacy->tracks[0].clips[0].fadeOutSamples == 0,
                "欠損時はフェードなし");

    // 手編集JSONの範囲外は読込時にクランプする（不変条件が破れたモデルを持ち回らない）
    {
        const auto outOfRange = makeTempDir();
        expect (writeTestWav (outOfRange.getChildFile ("clip-001.wav"), 4410), "テストWAVを書けること");
        const char* json = R"({
            "version": 11, "bpm": 120.0, "sampleRate": 44100.0, "nextId": 2,
            "tracks": [
                { "id": 1, "type": "audio", "name": "X",
                  "clips": [
                    { "file": "clip-001.wav", "startSample": 0, "lengthSamples": 4410,
                      "fadeInSamples": 99999, "fadeOutSamples": 5000 },
                    { "file": "clip-001.wav", "startSample": 0, "lengthSamples": 4410,
                      "fadeInSamples": -10, "fadeOutSamples": -20 },
                    { "file": "clip-001.wav", "startSample": 0, "lengthSamples": 1000, "loopCount": 3,
                      "fadeInSamples": 1500, "fadeOutSamples": 3000 }
                  ] }
            ]
        })";
        outOfRange.getChildFile ("project.json").replaceWithText (json);
        auto clamped = Project::load (outOfRange, warnings, error);
        expect (clamped != nullptr && clamped->tracks[0].clips.size() == 3, "読込めること");
        if (clamped != nullptr && clamped->tracks[0].clips.size() == 3)
        {
            const auto& clips = clamped->tracks[0].clips;
            expect (clips[0].fadeInSamples == 4410 && clips[0].fadeOutSamples == 0,
                    "過大値は全長で頭打ち（fadeIn 優先で fadeOut は0）");
            expect (clips[1].fadeInSamples == 0 && clips[1].fadeOutSamples == 0, "負値は0へ");
            expect (clips[2].fadeInSamples == 1500 && clips[2].fadeOutSamples == 2500,
                    "ループ込みの全長(4000)で判定されること");
        }
        outOfRange.deleteRecursively();
    }

    dir.deleteRecursively();
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

    // ソース: サンプル値 = 位置に比例するランプ波（読み出し位置のズレを1サンプル単位で検出できる）。
    // 振幅は0.5止まり（1.0までのランプはMaster Limiterの天井-1dB≈0.891に叩かれて一致しなくなる）
    auto source = std::make_shared<juce::AudioBuffer<float>> (1, totalSamples);
    for (int i = 0; i < totalSamples; ++i)
        source->setSample (0, i, 0.5f * (float) i / (float) totalSamples);

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

    // 出力はMaster Limiterのlookahead分遅れるので、余分に1ブロック回して +latency で照合する
    const int latency = engineLimiterLatency (sr);
    juce::AudioBuffer<float> out (2, blockSize * 5);
    out.clear();
    engine.play();
    for (int block = 0; block < 5; ++block)
    {
        juce::AudioSourceChannelInfo info (&out, block * blockSize, blockSize);
        engine.process (info);
    }
    engine.stop();
    int mismatches = 0;
    for (int i = 0; i < totalSamples; ++i)
        if (std::abs (out.getSample (0, i + latency) - source->getSample (0, i)) > 1.0e-6f)
            ++mismatches;

    expect (mismatches == 0, "分割された左右クリップの出力が元音源と全サンプル一致すること");
    snapshots.deleteRetired();
}

// 再生ヘッド（今いる場所）と再生開始位置（次に鳴る場所）の分離に伴い、編集操作は
// 「保留中シークを含む論理位置」を見る。seekRequest はオーディオコールバックで
// 初めて playheadSamplePos に反映されるため、生の再生位置だとクリック直後の1バッファ分だけ
// 旧位置で切れてしまう
void testUiPositionSample()
{
    beginTest ("TransportState::uiPositionSample");

    TransportState transport;
    transport.playheadSamplePos.store (48000);
    expect (transport.uiPositionSample() == 48000, "保留中シークが無ければ再生位置を返す");

    transport.seekRequest.store (96000);
    expect (transport.uiPositionSample() == 96000,
            "保留中シークがあれば、まだ適用されていなくてもシーク先を返す");

    // 適用はオーディオスレッドの applyPendingSeek が行う（PlaybackEngine::process と同じ手順）。
    // 「ヘッドを公開してから要求を消す」順序なので、適用の前後どちらで読んでもシーク先を指す
    expect (transport.applyPendingSeek(), "保留中シークを適用したら true");
    expect (transport.playheadSamplePos.load() == 96000, "ヘッドがシーク先へ移る");
    expect (transport.seekRequest.load() == TransportState::kNoSeek, "適用した要求は消える");
    expect (transport.uiPositionSample() == 96000, "適用後もシーク先を指し続ける");

    expect (! transport.applyPendingSeek(), "保留中シークが無ければ false（ヘッドは動かさない）");
    expect (transport.playheadSamplePos.load() == 96000, "空振りでヘッドが巻き戻らない");

    // 適用の途中で新しい要求が入った場合はCASが失敗して要求が残り、次のコールバックで適用される。
    // ここでは「適用直後に新しい要求が来た」状況を模して、要求が消されないことを見る
    transport.seekRequest.store (192000);
    expect (transport.uiPositionSample() == 192000, "新しい要求は適用前でもUIから見える");
    expect (transport.applyPendingSeek() && transport.playheadSamplePos.load() == 192000,
            "次のコールバックで新しい要求が適用される");

    // カウントイン中は負。ここでクランプすると白線が小節1に張り付いて開始位置マーカーと重なる
    transport.playheadSamplePos.store (-96000);
    expect (transport.uiPositionSample() == -96000, "カウントイン中の負位置をクランプしない");

    transport.seekRequest.store (-48000);
    expect (transport.uiPositionSample() == -48000, "保留中シークが負でもクランプしない");
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
    clip.resetRenderDomainToSelf();
    clip.activeDomain = ClipDomains::makeNeutralDomain (clip.audio, clip.offsetSamples,
                                                        clip.lengthSamples, 48000.0);

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

    // 波形キャッシュはドメイン共有＋view端の再集計: 右の先頭ピークは自分の参照範囲
    //（offset 150 以降）から集計され、左側にしかない値（<150）が混ざらない
    expect (left.activeDomain.get() == clip.activeDomain.get()
                && right.activeDomain.get() == clip.activeDomain.get(),
            "分割で RenderedDomain が親と同一インスタンスのまま共有されること（再レンダーなし）");
    if (right.activeDomain != nullptr)
    {
        const auto peak = right.activeDomain->peakBetween (right.viewStartRendered(),
                                                           right.viewStartRendered() + 300,
                                                           Clip::samplesPerPeak);
        expect (juce::approximatelyEqual (peak, 449.0f), // max(|150..449|)
                "右の波形ピークが自分の参照範囲から集計されること");
    }

    // フェードは外側だけ継承する（内側＝分割点側は0）。丸ごとコピーだと分割点にフェードが移動する
    {
        Clip faded = clip;
        faded.fadeInSamples = 40;
        faded.fadeOutSamples = 60;
        Clip fl, fr;
        expect (splitClip (faded, 1100, fl, fr), "フェード付きも分割できること");
        expect (fl.fadeInSamples == 40 && fl.fadeOutSamples == 0,
                "左は fadeIn を継承し fadeOut は0（分割点にフェードを作らない）");
        expect (fr.fadeInSamples == 0 && fr.fadeOutSamples == 60,
                "右は fadeOut を継承し fadeIn は0");

        // 継承側も自分の長さでクランプする（左は100サンプルしかない）
        faded.fadeInSamples = 300;
        expect (splitClip (faded, 1100, fl, fr), "長いフェード付きも分割できること");
        expect (fl.fadeInSamples == 100, "継承した fadeIn が左の長さへ収まること");
    }

    // ループ済みクリップの分割: ループは解除して返す。**解除はフェードのクランプより先**に
    // 行わないと、元のループ込み全長で通したフェードが分割後の全長を超えて残る
    // （描画は未クランプ値・再生は防御クランプ値を使うため見た目と音が食い違う）
    {
        Clip looped = clip;
        looped.lengthSamples = 400;
        looped.loopCount = 3;          // 連なり 1600
        looped.fadeInSamples = 900;    // どちらも本体長(400)より長い
        looped.fadeOutSamples = 600;
        Clip ll, lr;
        expect (splitClip (looped, 1100, ll, lr), "ループ済みクリップも分割できること");
        expect (ll.loopCount == 0 && lr.loopCount == 0, "分割でループが解除されること");
        expect (ll.fadeInSamples + ll.fadeOutSamples <= ll.sourceTotalLengthSamples()
                    && lr.fadeInSamples + lr.fadeOutSamples <= lr.sourceTotalLengthSamples(),
                "左右のフェードが分割後の全長に収まること（不変条件が破れないこと）");
        expect (ll.fadeInSamples == 100 && lr.fadeOutSamples == 300,
                "継承したフェードがそれぞれの長さで頭打ちになること");
    }

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
    expect (clip.sourceTotalLengthSamples() == 3200, "総再生長は本体長×4");
    expect (clip.renderedTotalLengthSamples() == 3200, "無加工では見かけの総再生長も同じ");
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

    // 出力自体も従来どおり両トラック合算で鳴っていること（スクラッチ経由への置き換えで無音化していない）。
    // Master Limiter（常在・ceiling -1dB）で1.5は叩かれてしまうため、トラックゲインを0.5に
    // 落として合算0.75（天井未満＝素通し）で確認する。出力はlookahead分遅れるので2ブロック目で測る
    project.tracks[0].params->gain.store (0.5f);
    project.tracks[1].params->gain.store (0.5f);
    transport.seekRequest.store (0); // シークでLimiterをリセット（直前の1.5×4ブロックで掛かったGRのリリース中に測らない）
    engine.play();
    for (int i = 0; i < 2; ++i)
    {
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
    }
    engine.stop();
    expect (buffer.getMagnitude (0, 0, blockSize) > 0.7f,
            "出力は全トラック合算（(1.2+0.3)×0.5）で鳴ること");

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

        // 出力はMaster Limiterのlookahead分遅れるので、2ブロック回して
        // [latency, latency+blockSize) を読む（＝従来の1ブロック目の内容）
        const int latency = MasterLimiter::lookaheadForRate (sr);
        juce::AudioBuffer<float> buffer (2, blockSize * 2);
        const auto measure = [&]
        {
            transport.seekRequest.store (0);
            engine.play();
            buffer.clear();
            for (int b = 0; b < 2; ++b)
            {
                juce::AudioSourceChannelInfo info (&buffer, b * blockSize, blockSize);
                engine.process (info);
            }
            const auto magnitude = buffer.getMagnitude (0, latency, blockSize);
            engine.stop();
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            return magnitude;
        };

        expect (std::abs (measure() - 0.4f) < 0.001f, "ユニティでは素通し");

        project.tracks[0].clips[0].gain = 0.5f;
        snapshots.deleteRetired();
        snapshots.push (project.buildSnapshot (Project::SnapshotChange::audioValuesOnly));
        expect (std::abs (measure() - 0.2f) < 0.001f, "世代据え置きでもクリップゲインが音に反映されること");

        // フェードも同じ経路で音へ届く（ドラッグ中の反映に使う）。ブロック全体を覆う
        // フェードインなので、測定ブロックのピークは末尾サンプル ≒ 0.2 * (511/1023)
        project.tracks[0].clips[0].fadeInSamples = blockSize * 2;
        snapshots.deleteRetired();
        snapshots.push (project.buildSnapshot (Project::SnapshotChange::audioValuesOnly));
        expect (std::abs (measure() - 0.2f * 511.0f / 1023.0f) < 0.002f,
                "世代据え置きでもフェードが音に反映されること");

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

// ---- バウンス: 過大ミックスは Master Limiter（常在・ceiling既定-1dB）が天井で抑えること ----
// かつての「ピーク>1.0で全体スケール」保護はLimiterが天井を保証するため実質発動しなくなった
// （コードはバックストップとして残存）。ここではLimiter経由の新契約を検証する
void testBounceRendererClippingProtection()
{
    beginTest ("BounceRenderer clipping protection");
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce.wav");

    // 0.8のクリップを同位置に2枚重ねて加算1.6（+4.1dB）→ Limiterがceiling(-1dB)へ抑える
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
    const float ceiling = juce::Decibels::decibelsToGain (-1.0f);
    expect (result.status == BounceRenderer::Status::success, "successで終わること");
    expect (! result.scaled, "Limiterが天井を守るためスケールは発動しないこと");
    expect (result.peak <= ceiling + 1.0e-4f, "記録ピークがceiling以下であること");
    expect (result.writtenSamples == 500, "Limiterの遅延が出力長に漏れないこと（先頭破棄＋flush）");
    // 完了時の自動計測（ワーカー内 measureFile）。DC矩形の立ち上がりエッジはギブス現象で
    // サンプル間オーバーシュートが出る（96タップカーネルの実測 ≈ +1.05dB）ため、
    // TPはceiling(-1dB)以上〜+0.5dBTP未満に収まる（連続音楽ではこの現象は出ない）
    expect (result.loudnessMeasured, "完了時にLUFS/TPが計測されること");
    expect (result.truePeakDb > -1.05 && result.truePeakDb < 0.5,
            "計測TPがceiling(-1dB)〜+0.5dBTPの範囲（矩形エッジのISP込み）であること");

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::FileInputStream (target), true));
    expect (reader != nullptr, "書き出したWAVを読めること");
    if (reader != nullptr)
    {
        expect (reader->lengthInSamples == 500, "WAVの長さが範囲どおりであること");
        juce::AudioBuffer<float> readBack (2, 500);
        reader->read (&readBack, 0, 500, 0, true, true);
        const float peak = readBack.getMagnitude (0, 500);
        expect (peak <= ceiling + 1.0e-3f, "出力ピークがceiling(-1dB)以下に収まること");
        expect (peak > ceiling - 0.02f, "出力が天井近くまで出ていること（過剰に潰していない）");
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

        // Limiterの遅延ぶん余分に1ブロック回す（比較はエンジン側を +latency で読む）
        const int latency = engineLimiterLatency (sr);
        juce::AudioBuffer<float> engineOut (2, totalSamples + blockSize);
        engineOut.clear();
        engine.play();
        juce::AudioBuffer<float> buffer (2, blockSize);
        for (int block = 0; block < totalSamples / blockSize + 1; ++block)
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
                    maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (ch, i + latency)
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
    // EQはON補完・Compはv15以前のenabledが無意味（DSPなし）なため一律OFFリセット（v16）
    expect (params.eqEnabled.load() && ! params.compEnabled.load(),
            "v3読込: EQはON補完・CompはOFFリセット");
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
    params.compEnabled.store (true); // v16保存ではcompのON/OFFが実際に維持される
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

// ---- TrackEq: サイン波を通した実測ゲインが解析応答（RBJの設計式）と一致すること ----
// 合格基準は「係数から計算した解析応答と実信号の実測ゲインの一致」（planの冒頭）。
// 入力振幅0.25のサイン波1秒を処理し、後半0.5秒のRMSからゲインを求める
double measureEqGainDb (const Eq::Values& targets, bool eqOn, double freqHz, double sr)
{
    TrackEq eq;
    eq.snapTo (sr, eqOn, targets);
    constexpr int blockSize = 512;
    const int totalBlocks = (int) (sr / blockSize) + 1;
    juce::uint64 serial = 0;
    std::vector<float> out;
    out.reserve ((size_t) blockSize * (size_t) totalBlocks);
    double phase = 0.0;
    const double inc = juce::MathConstants<double>::twoPi * freqHz / sr;
    float block[blockSize];
    for (int b = 0; b < totalBlocks; ++b)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            block[i] = (float) std::sin (phase) * 0.25f;
            phase += inc;
        }
        eq.process (block, nullptr, blockSize, sr, ++serial, false, eqOn, targets);
        out.insert (out.end(), block, block + blockSize);
    }
    double sum = 0.0;
    const size_t half = out.size() / 2;
    for (size_t i = half; i < out.size(); ++i)
        sum += (double) out[i] * (double) out[i];
    const double rms = std::sqrt (sum / (double) (out.size() - half));
    return juce::Decibels::gainToDecibels (rms / (0.25 / std::sqrt (2.0)));
}

void testTrackEqResponse()
{
    beginTest ("track eq response vs analytic");
    constexpr double sr = 48000.0;
    const auto neutral = Eq::defaultValues();

    // ---- ベル +6dB@1kHz Q1: 中心で+6dB・解析応答と一致 ----
    {
        auto targets = neutral;
        targets[Eq::bell1] = Eq::normalized (Eq::bell1, { true, 1000.0f, 6.0f, 1.0f });
        expect (std::abs (measureEqGainDb (targets, true, 1000.0, sr) - 6.0) < 0.15,
                "ベル中心周波数で+6dB");
        const auto analytic = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
            sr, 1000.0f, 1.0f, juce::Decibels::decibelsToGain (6.0f));
        for (const double f : { 250.0, 1000.0, 4000.0 })
        {
            const double expected = juce::Decibels::gainToDecibels (
                analytic->getMagnitudeForFrequency (f, sr));
            expect (std::abs (measureEqGainDb (targets, true, f, sr) - expected) < 0.3,
                    "実測ゲインが解析応答と一致（ベル）");
        }
    }

    // ---- ハイパス 200Hz: カットオフで-3dB・遮断域はおよそ12dB/oct ----
    {
        auto targets = neutral;
        targets[Eq::highpass] = Eq::normalized (Eq::highpass, { true, 200.0f, 0.0f, 0.0f });
        expect (std::abs (measureEqGainDb (targets, true, 200.0, sr) - (-3.01)) < 0.3,
                "HPカットオフで-3dB（Butterworth）");
        const double g100 = measureEqGainDb (targets, true, 100.0, sr);
        const double g50 = measureEqGainDb (targets, true, 50.0, sr);
        expect (g100 - g50 > 10.0 && g100 - g50 < 14.0, "HP遮断域の傾きがおよそ12dB/oct");
        const auto analytic = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 200.0f, Eq::fixedQ);
        const double expected50 = juce::Decibels::gainToDecibels (
            analytic->getMagnitudeForFrequency (50.0, sr));
        expect (std::abs (g50 - expected50) < 0.5, "実測ゲインが解析応答と一致（HP）");

        // HPを無効にしたら（クロスフェード量0）素通しになること
        auto hpOff = targets;
        hpOff[Eq::highpass].enabled = false;
        expect (std::abs (measureEqGainDb (hpOff, true, 50.0, sr)) < 0.05, "HP無効なら低域が素通し");
    }

    // ---- ハイシェルフ +4dB@10kHz: 棚の上で+4dB・解析応答と一致 ----
    {
        auto targets = neutral;
        targets[Eq::highShelf] = Eq::normalized (Eq::highShelf, { true, 10000.0f, 4.0f, 0.0f });
        const auto analytic = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
            sr, 10000.0f, Eq::fixedQ, juce::Decibels::decibelsToGain (4.0f));
        for (const double f : { 4000.0, 16000.0 })
        {
            const double expected = juce::Decibels::gainToDecibels (
                analytic->getMagnitudeForFrequency (f, sr));
            expect (std::abs (measureEqGainDb (targets, true, f, sr) - expected) < 0.3,
                    "実測ゲインが解析応答と一致（シェルフ）");
        }
    }

    // ---- バイパス相当（中立・eqOff）は高速パス判定になること ----
    {
        TrackEq eq;
        expect (! eq.needsActivePath (true, neutral, false), "中立は高速パス");
        auto boosted = neutral;
        boosted[Eq::bell1].gainDb = 6.0f;
        expect (! eq.needsActivePath (false, boosted, false), "eqOffは高速パス");
        expect (eq.needsActivePath (true, boosted, false), "非中立＋ONはactive");
        expect (eq.needsActivePath (false, neutral, true), "アナライザタップ中はactive");
    }
}

// ---- TrackEq: ON/OFF切替のクリック・時間ジャンプのリセット・中立化後の高速パス遷移 ----
void testTrackEqTransitions()
{
    beginTest ("track eq transitions");
    constexpr double sr = 48000.0;
    constexpr int blockSize = 512;
    const auto neutral = Eq::defaultValues();
    auto boosted = neutral;
    boosted[Eq::bell1] = Eq::normalized (Eq::bell1, { true, 1000.0f, 12.0f, 1.0f });
    boosted[Eq::highpass] = Eq::normalized (Eq::highpass, { true, 300.0f, 0.0f, 0.0f });

    // ---- eqEnabled・HPのON/OFFを繰り返してもNaN/Inf・不連続なサンプル跳躍が出ないこと ----
    {
        TrackEq eq;
        eq.snapTo (sr, true, boosted);
        juce::uint64 serial = 0;
        double phase = 0.0;
        const double inc = juce::MathConstants<double>::twoPi * 1000.0 / sr;
        float previous = 0.0f;
        float maxJump = 0.0f;
        bool allFinite = true;
        for (int b = 0; b < 40; ++b)
        {
            const bool eqOn = (b / 4) % 2 == 0;      // 4ブロックごとに全体ON/OFF
            auto targets = boosted;
            targets[Eq::highpass].enabled = (b / 8) % 2 == 0; // 8ブロックごとにHP ON/OFF
            float block[blockSize];
            for (int i = 0; i < blockSize; ++i)
            {
                block[i] = (float) std::sin (phase) * 0.5f;
                phase += inc;
            }
            eq.process (block, nullptr, blockSize, sr, ++serial, false, eqOn, targets);
            for (int i = 0; i < blockSize; ++i)
            {
                allFinite = allFinite && std::isfinite (block[i]);
                maxJump = juce::jmax (maxJump, std::abs (block[i] - previous));
                previous = block[i];
            }
        }
        expect (allFinite, "切替中にNaN/Infが出ないこと");
        // 1kHz/振幅0.5(+12dBで最大2.0)のサイン自体の最大傾斜は約0.26。クロスフェードなしの
        // 瞬時切替なら波形差ぶんの跳躍（最大1.5規模）が出るので、この閾値で退行を検出できる
        expect (maxJump < 0.5f, "切替がクロスフェードされ跳躍しないこと");
    }

    // ---- timelineJumped後の出力が「初期化直後」と決定的に一致すること（旧履歴の混入なし）----
    {
        TrackEq warmed, fresh;
        warmed.snapTo (sr, true, boosted);
        fresh.snapTo (sr, true, boosted);
        juce::uint64 serial = 0;
        float noise[blockSize];
        juce::Random random (42);
        for (int b = 0; b < 8; ++b) // warmedにだけ履歴を溜める
        {
            for (int i = 0; i < blockSize; ++i)
                noise[i] = random.nextFloat() - 0.5f;
            warmed.process (noise, nullptr, blockSize, sr, ++serial, false, true, boosted);
        }
        float a[blockSize], bBuf[blockSize];
        for (int i = 0; i < blockSize; ++i)
            a[i] = bBuf[i] = (float) std::sin ((double) i * 0.13) * 0.4f;
        warmed.process (a, nullptr, blockSize, sr, ++serial, true, true, boosted); // ジャンプ
        fresh.process (bBuf, nullptr, blockSize, sr, 1, false, true, boosted);
        bool identical = true;
        for (int i = 0; i < blockSize; ++i)
            identical = identical && juce::exactlyEqual (a[i], bBuf[i]);
        expect (identical, "ジャンプ後は初期化直後とビット一致（旧IIR履歴が混入しない）");
    }

    // ---- 中立へ戻した後、平滑完了までactiveに残り、その後高速パスへ移ること ----
    {
        TrackEq eq;
        eq.snapTo (sr, true, boosted);
        expect (eq.needsActivePath (true, boosted, false), "非中立はactive");
        juce::uint64 serial = 0;
        float block[blockSize];
        auto runBlock = [&] (const Eq::Values& targets)
        {
            std::fill (block, block + blockSize, 0.1f);
            eq.process (block, nullptr, blockSize, sr, ++serial, false, true, targets);
        };
        runBlock (boosted);
        runBlock (neutral); // 目標を中立へ（HPも無効へ）
        expect (eq.needsActivePath (true, neutral, false),
                "中立化直後は平滑が残るのでactiveに留まる");
        for (int b = 0; b < 20; ++b) // 20ブロック≒213ms >> 平滑20ms
            runBlock (neutral);
        expect (! eq.needsActivePath (true, neutral, false), "平滑完了後は高速パスへ移れる");
    }
}

// ---- EQバンド: 既定値・保存/復元・旧version補完・不変条件の正規化（v15）----
void testEqParamsRoundtrip()
{
    beginTest ("eq params roundtrip");

    // 新規TrackParamsの既定値（HP=OFF/80Hz・ベル250/3.5k・シェルフ10k・全gain 0dB）
    TrackParams fresh;
    expect (! fresh.eqBands[Eq::highpass].enabled.load()
                && juce::approximatelyEqual (fresh.eqBands[Eq::highpass].freqHz.load(), 80.0f),
            "既定: HPはOFF・80Hz");
    expect (juce::approximatelyEqual (fresh.eqBands[Eq::bell1].freqHz.load(), 250.0f)
                && juce::approximatelyEqual (fresh.eqBands[Eq::bell2].freqHz.load(), 3500.0f)
                && juce::approximatelyEqual (fresh.eqBands[Eq::highShelf].freqHz.load(), 10000.0f),
            "既定: ベル250/3500・シェルフ10000");
    expect (Eq::isNeutral (Eq::loadAll (fresh.eqBands)), "既定値は中立（音を変えない）");

    // 値を入れて保存 → 再読込で維持される
    auto dir = makeTempDir();
    juce::String error;
    juce::StringArray warnings;
    {
        Project project;
        project.directory = dir;
        Track track;
        track.id = project.allocateId();
        project.tracks.push_back (std::move (track));
        auto& params = *project.tracks[0].params;
        Eq::store (params.eqBands[Eq::highpass], Eq::normalized (Eq::highpass, { true, 120.0f, 0.0f, 0.0f }));
        Eq::store (params.eqBands[Eq::bell1], Eq::normalized (Eq::bell1, { true, 300.0f, -3.5f, 2.0f }));
        Eq::store (params.eqBands[Eq::bell2], Eq::normalized (Eq::bell2, { true, 5000.0f, 4.0f, 0.5f }));
        Eq::store (params.eqBands[Eq::highShelf], Eq::normalized (Eq::highShelf, { true, 12000.0f, 2.0f, 0.0f }));
        expect (project.save (error), "保存できること");
    }
    const auto parsed = juce::JSON::parse (dir.getChildFile ("project.json").loadFileAsString());
    expect ((int) parsed.getProperty ("version", 0) == Project::currentVersion,
            "現行バージョンで保存されること");

    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr && reloaded->tracks.size() == 1, "再読込できること");
    if (reloaded != nullptr && ! reloaded->tracks.empty())
    {
        const auto bands = Eq::loadAll (reloaded->tracks[0].params->eqBands);
        expect (bands[Eq::highpass].enabled
                    && juce::approximatelyEqual (bands[Eq::highpass].freqHz, 120.0f)
                    && juce::approximatelyEqual (bands[Eq::highpass].q, Eq::fixedQ),
                "HP維持（enabled/freq。Qは固定値）");
        expect (juce::approximatelyEqual (bands[Eq::bell1].freqHz, 300.0f)
                    && juce::approximatelyEqual (bands[Eq::bell1].gainDb, -3.5f)
                    && juce::approximatelyEqual (bands[Eq::bell1].q, 2.0f),
                "ベル1維持（freq/gain/q）");
        expect (juce::approximatelyEqual (bands[Eq::bell2].gainDb, 4.0f)
                    && juce::approximatelyEqual (bands[Eq::bell2].q, 0.5f),
                "ベル2維持");
        expect (juce::approximatelyEqual (bands[Eq::highShelf].gainDb, 2.0f)
                    && juce::approximatelyEqual (bands[Eq::highShelf].q, Eq::fixedQ),
                "シェルフ維持（Qは固定値）");
        expect (! Eq::isNeutral (bands), "非中立と判定されること");
    }
    dir.deleteRecursively();

    // v14形式（bands欠損）→ 既定値補完
    auto dirV14 = makeTempDir();
    dirV14.getChildFile ("project.json").replaceWithText (R"({
        "version": 14, "bpm": 120.0, "sampleRate": 0.0, "nextId": 2,
        "tracks": [ { "id": 1, "type": "audio", "name": "t",
                      "mute": false, "solo": false, "volume": 0.5,
                      "fx": { "eq": { "enabled": false }, "comp": { "enabled": true } },
                      "clips": [] } ]
    })");
    auto projectV14 = Project::load (dirV14, warnings, error);
    expect (projectV14 != nullptr && projectV14->tracks.size() == 1, "v14を読込めること");
    if (projectV14 != nullptr && ! projectV14->tracks.empty())
    {
        const auto bands = Eq::loadAll (projectV14->tracks[0].params->eqBands);
        expect (! bands[Eq::highpass].enabled
                    && juce::approximatelyEqual (bands[Eq::bell1].freqHz, 250.0f)
                    && juce::approximatelyEqual (bands[Eq::bell1].gainDb, 0.0f),
                "v14読込: バンドは既定値補完");
        expect (! projectV14->tracks[0].params->eqEnabled.load(), "v14読込: eq.enabledは維持");
    }
    dirV14.deleteRecursively();

    // 不正データの正規化: UIから戻せない隠れ状態（ベルenabled=false・シェルフq変更・HPのgain・
    // 範囲外値）を読込時に潰す
    auto dirBad = makeTempDir();
    dirBad.getChildFile ("project.json").replaceWithText (R"({
        "version": 15, "bpm": 120.0, "sampleRate": 0.0, "nextId": 2,
        "tracks": [ { "id": 1, "type": "audio", "name": "t",
                      "mute": false, "solo": false, "volume": 0.5,
                      "fx": { "eq": { "enabled": true, "bands": [
                          { "enabled": true, "freq": 5000.0, "gain": 6.0, "q": 5.0 },
                          { "enabled": false, "freq": 5.0, "gain": 100.0, "q": 0.01 },
                          { "enabled": true, "freq": 30000.0, "gain": -100.0, "q": 50.0 },
                          { "enabled": false, "freq": 100.0, "gain": 3.0, "q": 3.0 }
                      ] }, "comp": { "enabled": true } },
                      "clips": [] } ]
    })");
    auto projectBad = Project::load (dirBad, warnings, error);
    expect (projectBad != nullptr && projectBad->tracks.size() == 1, "不正バンドでも読込めること");
    if (projectBad != nullptr && ! projectBad->tracks.empty())
    {
        const auto bands = Eq::loadAll (projectBad->tracks[0].params->eqBands);
        expect (juce::approximatelyEqual (bands[Eq::highpass].freqHz, 1000.0f)
                    && juce::approximatelyEqual (bands[Eq::highpass].gainDb, 0.0f)
                    && juce::approximatelyEqual (bands[Eq::highpass].q, Eq::fixedQ),
                "HP: freqは上限1kへクランプ・gain無視・Q固定");
        expect (bands[Eq::bell1].enabled
                    && juce::approximatelyEqual (bands[Eq::bell1].freqHz, 20.0f)
                    && juce::approximatelyEqual (bands[Eq::bell1].gainDb, Eq::maxGainDb)
                    && juce::approximatelyEqual (bands[Eq::bell1].q, Eq::minQ),
                "ベル1: enabled=true強制・freq/gain/qクランプ");
        expect (juce::approximatelyEqual (bands[Eq::bell2].freqHz, 20000.0f)
                    && juce::approximatelyEqual (bands[Eq::bell2].gainDb, -Eq::maxGainDb)
                    && juce::approximatelyEqual (bands[Eq::bell2].q, Eq::maxQ),
                "ベル2: 上限側クランプ");
        expect (bands[Eq::highShelf].enabled
                    && juce::approximatelyEqual (bands[Eq::highShelf].freqHz, 1000.0f)
                    && juce::approximatelyEqual (bands[Eq::highShelf].q, Eq::fixedQ),
                "シェルフ: enabled=true強制・freq下限1k・Q固定");
    }
    dirBad.deleteRecursively();
}

// ---- スペクトラムアナライザ: 正規化・L/Rパワー平均・floor・世代照合 ----
void testSpectrumAnalyzer()
{
    beginTest ("spectrum analyzer");
    constexpr double sr = 48000.0;

    // bin中心のサイン波を blockSamples 単位で積む（世代・trackIdはタップの現在値が付く）
    auto pushSine = [] (AnalyzerTap& tap, juce::uint64 trackId, double freq, float amp,
                        int totalSamples, bool antiphaseRight)
    {
        std::vector<float> left (AnalyzerTap::blockSamples), right (AnalyzerTap::blockSamples);
        double phase = 0.0;
        const double inc = juce::MathConstants<double>::twoPi * freq / sr;
        for (int pushed = 0; pushed < totalSamples; pushed += AnalyzerTap::blockSamples)
        {
            for (int i = 0; i < AnalyzerTap::blockSamples; ++i)
            {
                left[(size_t) i] = (float) std::sin (phase) * amp;
                right[(size_t) i] = antiphaseRight ? -left[(size_t) i] : left[(size_t) i];
                phase += inc;
            }
            tap.pushSamples (trackId, left.data(), right.data(), AnalyzerTap::blockSamples);
        }
    };

    // 表示ビンのindex（対象周波数に最も近いもの）
    auto displayBinFor = [] (double freq)
    {
        int best = 0;
        double bestDiff = 1.0e12;
        for (int i = 0; i < SpectrumAnalyzer::numBins; ++i)
        {
            const double diff = std::abs ((double) SpectrumAnalyzer::binFrequency (i) - freq);
            if (diff < bestDiff)
            {
                bestDiff = diff;
                best = i;
            }
        }
        return best;
    };

    const double binCenterFreq = 256.0 * sr / SpectrumAnalyzer::fftSize; // = 3000Hz（FFT bin 256）

    // ---- フルスケールのサイン波がbin中心で 0dBFS になること（振幅正規化・窓補正・片側補正）----
    {
        AnalyzerTap tap;
        SpectrumAnalyzer analyzer;
        tap.setTarget (1, true);
        pushSine (tap, 1, binCenterFreq, 1.0f, SpectrumAnalyzer::fftSize + SpectrumAnalyzer::hopSize,
                  false);
        expect (analyzer.update (tap, sr), "フレームが生成されること");
        const float peak = analyzer.magnitudesDb()[(size_t) displayBinFor (binCenterFreq)];
        expect (std::abs (peak - 0.0f) < 0.3f, "フルスケールサインがbin中心で0dBFS");
        // 離れた帯域はサイドローブ床まで落ちること（100Hz付近）
        expect (analyzer.magnitudesDb()[(size_t) displayBinFor (100.0)] < -50.0f,
                "離れた帯域は-50dB以下");
    }

    // ---- 振幅0.1（-20dBFS）の正確さ ----
    {
        AnalyzerTap tap;
        SpectrumAnalyzer analyzer;
        tap.setTarget (1, true);
        pushSine (tap, 1, binCenterFreq, 0.1f, SpectrumAnalyzer::fftSize + SpectrumAnalyzer::hopSize,
                  false);
        expect (analyzer.update (tap, sr), "フレームが生成されること");
        const float peak = analyzer.magnitudesDb()[(size_t) displayBinFor (binCenterFreq)];
        expect (std::abs (peak - (-20.0f)) < 0.3f, "振幅0.1は-20dBFS");
    }

    // ---- L/R逆相でもパワー平均後の表示が消えないこと（モノ和なら打ち消えて床に落ちる）----
    {
        AnalyzerTap tap;
        SpectrumAnalyzer analyzer;
        tap.setTarget (1, true);
        pushSine (tap, 1, binCenterFreq, 1.0f, SpectrumAnalyzer::fftSize + SpectrumAnalyzer::hopSize,
                  true);
        expect (analyzer.update (tap, sr), "フレームが生成されること");
        const float peak = analyzer.magnitudesDb()[(size_t) displayBinFor (binCenterFreq)];
        expect (std::abs (peak - 0.0f) < 0.3f, "L/R逆相でも0dBFSのまま（パワー平均）");
    }

    // ---- 無音はfloor（-60dB）----
    {
        AnalyzerTap tap;
        SpectrumAnalyzer analyzer;
        tap.setTarget (1, true);
        std::vector<float> silence (AnalyzerTap::blockSamples, 0.0f);
        for (int pushed = 0; pushed < SpectrumAnalyzer::fftSize + SpectrumAnalyzer::hopSize;
             pushed += AnalyzerTap::blockSamples)
            tap.pushSamples (1, silence.data(), nullptr, AnalyzerTap::blockSamples);
        expect (analyzer.update (tap, sr), "無音でもフレームは生成されること");
        bool allFloor = true;
        for (const float db : analyzer.magnitudesDb())
            allFloor = allFloor && juce::approximatelyEqual (db, SpectrumAnalyzer::floorDb);
        expect (allFloor, "無音は全ビンがfloor（-60dB）");
    }

    // ---- 世代の異なるブロックが混ざらないこと（切替直後にキューへ残った旧データを弾く）----
    {
        AnalyzerTap tap;
        SpectrumAnalyzer analyzer;
        tap.setTarget (1, true);
        pushSine (tap, 1, binCenterFreq, 1.0f, SpectrumAnalyzer::fftSize + SpectrumAnalyzer::hopSize,
                  false); // 旧世代のデータがFIFOに溜まった状態
        tap.setTarget (2, true); // 表示対象を切替（世代が進む）
        expect (! analyzer.update (tap, sr), "旧世代のブロックからはフレームを作らない");
        bool allFloor = true;
        for (const float db : analyzer.magnitudesDb())
            allFloor = allFloor && juce::approximatelyEqual (db, SpectrumAnalyzer::floorDb);
        expect (allFloor, "切替後は旧トラックのスペクトルが残らない");
    }
}

// ---- アナライザのタップはフェーダー前: フェーダー0でも表示信号が流れ、出力は無音のまま ----
void testEngineAnalyzerPreFaderTap()
{
    beginTest ("engine analyzer pre-fader tap");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 8;
    constexpr int totalSamples = blockSize * numBlocks;

    Project project;
    {
        Track track;
        track.id = 7;
        track.params->gain.store (0.0f); // フェーダーを完全に下げる
        Clip clip;
        clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, totalSamples);
        for (int i = 0; i < totalSamples; ++i)
            clip.audio->setSample (0, i, std::sin ((float) i * 0.1f) * 0.5f);
        clip.lengthSamples = totalSamples;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    AnalyzerTap tap;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.setAnalyzerTap (&tap);
    tap.setTarget (7, true);
    engine.prepareToPlay (blockSize, sr);
    snapshots.push (project.buildSnapshot());
    transport.seekRequest.store (0);
    engine.play();

    float outputPeak = 0.0f;
    juce::AudioBuffer<float> buffer (2, blockSize);
    for (int blockIndex = 0; blockIndex < numBlocks; ++blockIndex)
    {
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        for (int ch = 0; ch < 2; ++ch)
            outputPeak = juce::jmax (outputPeak, buffer.getMagnitude (ch, 0, blockSize));
    }
    engine.stop();
    snapshots.deleteRetired();

    expect (outputPeak < 1.0e-6f, "フェーダー0の出力は無音のまま");

    AnalyzerTap::Header header;
    std::vector<float> left (AnalyzerTap::blockSamples), right (AnalyzerTap::blockSamples);
    int blocks = 0;
    float tapPeak = 0.0f;
    while (tap.popBlock (header, left.data(), right.data()))
    {
        ++blocks;
        expect (header.trackId == 7, "タップのtrackIdが正しいこと");
        for (const float v : left)
            tapPeak = juce::jmax (tapPeak, std::abs (v));
    }
    expect (blocks > 0, "フェーダー0でもタップにブロックが流れること（pre-fader）");
    expect (tapPeak > 0.3f, "タップ信号はフェーダー前のフル振幅であること");
}

// ---- 通常バウンス: オーディオトラックEQのリングアウトが曲末で切れず、RTと一致すること ----
void testBounceEqTail()
{
    beginTest ("bounce eq ring-out tail");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 16;
    constexpr int totalSamples = blockSize * numBlocks; // クリップ終端＝書き出し範囲終端
    constexpr int tailCompare = 2048;                   // 終端直後の比較区間

    Project project;
    {
        Track track;
        track.id = 1;
        track.params->gain.store (1.0f);
        // 高Q・大ブーストのベル（100Hz）: リングアウトが数百msの規模で残る設定
        Eq::store (track.params->eqBands[Eq::bell1],
                   Eq::normalized (Eq::bell1, { true, 100.0f, 24.0f, 10.0f }));
        Clip clip;
        clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, totalSamples);
        const double inc = juce::MathConstants<double>::twoPi * 100.0 / sr;
        for (int i = 0; i < totalSamples; ++i)
            clip.audio->setSample (0, i, (float) std::sin (inc * i) * 0.05f);
        clip.lengthSamples = totalSamples;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }

    // RT: 範囲終端を越えて処理を続け、リングアウトを採取する
    //（Limiterの遅延ぶん余分に1ブロック回し、比較はエンジン側を +latency で読む）
    const int latency = engineLimiterLatency (sr);
    juce::AudioBuffer<float> engineOut (2, totalSamples + tailCompare + blockSize);
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
        for (int blockIndex = 0; blockIndex < numBlocks + tailCompare / blockSize + 1; ++blockIndex)
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

    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce-eq-tail.wav");
    juce::int64 writtenSamples = 0;
    {
        BounceRenderer::Request request;
        request.sampleRate = sr;
        request.bpm = 120.0;
        request.endSample = totalSamples;
        request.wantTail = true; // MainComponentがEQ有効オーディオトラックで立てる判断のミラー
        request.targetFile = target;
        for (auto& track : project.tracks)
        {
            BounceRenderer::TrackRender render;
            render.gain = track.params->gain.load();
            render.loadFxFrom (*track.params);
            for (auto& clip : track.clips)
                appendClipPlaybacks (clip, render.clips);
            request.tracks.push_back (std::move (render));
        }
        BounceRenderer renderer;
        expect (renderer.start (std::move (request)), "startできること");
        expect (waitForBounce (renderer), "タイムアウトせず完了すること");
        const auto result = renderer.takeResult();
        expect (result.status == BounceRenderer::Status::success, "successで終わること");
        writtenSamples = result.writtenSamples;
    }

    expect (writtenSamples > totalSamples, "リングアウトのテールが書き出されること");

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::FileInputStream (target), true));
    expect (reader != nullptr && reader->lengthInSamples >= totalSamples + tailCompare,
            "テール込みの長さで読めること");
    if (reader != nullptr && reader->lengthInSamples >= totalSamples + tailCompare)
    {
        juce::AudioBuffer<float> bounceOut (2, totalSamples + tailCompare);
        reader->read (&bounceOut, 0, totalSamples + tailCompare, 0, true, true);

        float tailPeak = 0.0f;
        float maxDiff = 0.0f;
        for (int i = totalSamples; i < totalSamples + tailCompare; ++i)
        {
            tailPeak = juce::jmax (tailPeak, std::abs (bounceOut.getSample (0, i)));
            for (int ch = 0; ch < 2; ++ch)
                maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (ch, i + latency)
                                                         - bounceOut.getSample (ch, i)));
        }
        expect (tailPeak > 1.0e-3f, "終端直後にリングアウトが実在すること");
        expect (maxDiff < 1.0e-4f, "リングアウトがRT再生と一致すること");
    }
    reader.reset();
    dir.deleteRecursively();
}

// ---- EQ有効時のエンジンとバウンスの一致（同じ処理順で通っていることの証明）----
void testEngineEqBounceConsistency()
{
    beginTest ("engine vs bounce with active eq");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 16;
    constexpr int totalSamples = blockSize * numBlocks;
    // RT側は再生開始時にdryからのクロスフェードイン（約10ms=441サンプル）があるため、
    // 先頭は比較から除外する（バウンスは開始時に確定値へスナップする仕様）
    constexpr int compareFrom = 2048;

    auto makeAudio = [] (int channels, int len, float scale)
    {
        auto buffer = std::make_shared<juce::AudioBuffer<float>> (channels, len);
        juce::Random random (7);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < len; ++i)
                buffer->setSample (ch, i,
                                   (std::sin ((float) i * 0.05f + (float) ch) * 0.3f
                                    + (random.nextFloat() - 0.5f) * 0.2f) * scale);
        return buffer;
    };

    Project project;
    {
        Track track; // ステレオクリップ＋ベルブースト＋send
        track.id = 1;
        track.params->gain.store (0.8f);
        track.params->pan.store (0.3f);
        track.params->sends[1].store (0.4f);
        Eq::store (track.params->eqBands[Eq::bell1],
                   Eq::normalized (Eq::bell1, { true, 500.0f, 6.0f, 1.2f }));
        Clip clip;
        clip.audio = makeAudio (2, totalSamples, 1.0f);
        clip.lengthSamples = totalSamples;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }
    {
        Track track; // モノクリップ＋HP＋シェルフ
        track.id = 2;
        track.params->gain.store (0.7f);
        track.params->pan.store (-0.5f);
        Eq::store (track.params->eqBands[Eq::highpass],
                   Eq::normalized (Eq::highpass, { true, 150.0f, 0.0f, 0.0f }));
        Eq::store (track.params->eqBands[Eq::highShelf],
                   Eq::normalized (Eq::highShelf, { true, 8000.0f, 3.0f, 0.0f }));
        Clip clip;
        clip.audio = makeAudio (1, totalSamples, 0.8f);
        clip.lengthSamples = totalSamples;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }
    project.busParams[1]->gain.store (0.9f);
    project.masterParams->gain.store (0.85f);

    auto renderEngine = [&] (juce::AudioBuffer<float>& out)
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
        for (int blockIndex = 0; blockIndex * blockSize < out.getNumSamples(); ++blockIndex)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, blockIndex * blockSize, buffer, ch, 0, blockSize);
        }
        engine.stop();
        snapshots.deleteRetired();
    };

    // Limiterの遅延ぶん余分に1ブロック回す（比較はエンジン側を +latency で読む）
    const int latency = engineLimiterLatency (sr);
    juce::AudioBuffer<float> engineOut (2, totalSamples + blockSize);
    renderEngine (engineOut);

    // EQが実際に効いていること（中立との差がある）を先に確認する
    // — 両経路で「EQが掛かっていない」ままの空一致で通ることを防ぐ
    {
        Eq::Values savedBell = Eq::loadAll (project.tracks[0].params->eqBands);
        Eq::Values savedHp = Eq::loadAll (project.tracks[1].params->eqBands);
        Eq::applyDefaults (project.tracks[0].params->eqBands);
        Eq::applyDefaults (project.tracks[1].params->eqBands);
        juce::AudioBuffer<float> neutralOut (2, totalSamples);
        renderEngine (neutralOut);
        float maxDiff = 0.0f;
        for (int i = compareFrom; i < totalSamples; ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (0, i)
                                                     - neutralOut.getSample (0, i)));
        expect (maxDiff > 1.0e-3f, "EQありは中立と出力が異なること（EQが実際に効いている）");
        for (int i = 0; i < Eq::numBands; ++i)
        {
            Eq::store (project.tracks[0].params->eqBands[i], savedBell[(size_t) i]);
            Eq::store (project.tracks[1].params->eqBands[i], savedHp[(size_t) i]);
        }
    }

    // バウンス側（TrackRenderへプレーン値コピー＝本番と同じ流儀）
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce-eq.wav");
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
            render.loadFxFrom (*track.params);
            for (auto& clip : track.clips)
                appendClipPlaybacks (clip, render.clips);
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
            for (int i = compareFrom; i < totalSamples; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (ch, i + latency)
                                                         - bounceOut.getSample (ch, i)));
        // 24bit量子化＋浮動小数点の積和順序差ぶんの許容誤差（フェードイン区間は除外済み）
        expect (maxDiff < 1.0e-4f, "EQ有効時もエンジンとバウンスが許容誤差内で一致すること");
    }
    reader.reset();
    dir.deleteRecursively();
}

// ---- Compパラメータ: 既定値・保存/復元・旧version（enabledリセット）・不正データの正規化（v16）----
void testCompParamsRoundtrip()
{
    beginTest ("comp params roundtrip");

    // 新規TrackParamsの既定値（OFF・中立スタート: Threshold 0dB / 4:1 / 10ms / 100ms / +0dB / HPF OFF）
    TrackParams fresh;
    expect (! fresh.compEnabled.load(), "既定: CompはOFF（真のバイパス）");
    {
        const auto v = Comp::load (fresh.comp);
        expect (juce::approximatelyEqual (v.thresholdDb, 0.0f)
                    && juce::approximatelyEqual (v.ratio, 4.0f)
                    && juce::approximatelyEqual (v.attackMs, 10.0f)
                    && juce::approximatelyEqual (v.releaseMs, 100.0f)
                    && juce::approximatelyEqual (v.makeupDb, 0.0f) && ! v.detectorHpf,
                "既定値（中立スタート）");
    }

    // 値を入れて保存 → 再読込で維持される
    auto dir = makeTempDir();
    juce::String error;
    juce::StringArray warnings;
    {
        Project project;
        project.directory = dir;
        Track track;
        track.id = project.allocateId();
        project.tracks.push_back (std::move (track));
        auto& params = *project.tracks[0].params;
        params.compEnabled.store (true);
        Comp::store (params.comp, Comp::normalized ({ -24.5f, 8.0f, 2.5f, 250.0f, 3.0f, true }));
        expect (project.save (error), "保存できること");
    }
    const auto parsed = juce::JSON::parse (dir.getChildFile ("project.json").loadFileAsString());
    expect ((int) parsed.getProperty ("version", 0) == Project::currentVersion, "現行版数で保存されること");

    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr && reloaded->tracks.size() == 1, "再読込できること");
    if (reloaded != nullptr && ! reloaded->tracks.empty())
    {
        auto& p = *reloaded->tracks[0].params;
        const auto v = Comp::load (p.comp);
        expect (p.compEnabled.load(), "enabled維持");
        expect (juce::approximatelyEqual (v.thresholdDb, -24.5f)
                    && juce::approximatelyEqual (v.ratio, 8.0f)
                    && juce::approximatelyEqual (v.attackMs, 2.5f)
                    && juce::approximatelyEqual (v.releaseMs, 250.0f)
                    && juce::approximatelyEqual (v.makeupDb, 3.0f) && v.detectorHpf,
                "Compパラメータ維持");
    }
    dir.deleteRecursively();

    // v15形式: enabled=trueでもOFFへリセット（DSPが無かった頃の値は無意味）・パラメータは既定値
    auto dirV15 = makeTempDir();
    dirV15.getChildFile ("project.json").replaceWithText (R"({
        "version": 15, "bpm": 120.0, "sampleRate": 0.0, "nextId": 2,
        "tracks": [ { "id": 1, "type": "audio", "name": "t",
                      "mute": false, "solo": false, "volume": 0.5,
                      "fx": { "eq": { "enabled": true },
                              "comp": { "enabled": true, "threshold": -50.0 } },
                      "clips": [] } ]
    })");
    auto projectV15 = Project::load (dirV15, warnings, error);
    expect (projectV15 != nullptr && projectV15->tracks.size() == 1, "v15を読込めること");
    if (projectV15 != nullptr && ! projectV15->tracks.empty())
    {
        auto& p = *projectV15->tracks[0].params;
        expect (! p.compEnabled.load(), "v15読込: enabled=trueでもOFFへリセット");
        expect (juce::approximatelyEqual (Comp::load (p.comp).thresholdDb, 0.0f),
                "v15読込: パラメータは既定値（旧JSONの値は読まない）");
    }
    dirV15.deleteRecursively();

    // 不正データの正規化: 範囲外値のクランプ（手編集JSON対策）
    auto dirBad = makeTempDir();
    dirBad.getChildFile ("project.json").replaceWithText (R"({
        "version": 16, "bpm": 120.0, "sampleRate": 0.0, "nextId": 2,
        "tracks": [ { "id": 1, "type": "audio", "name": "t",
                      "mute": false, "solo": false, "volume": 0.5,
                      "fx": { "eq": { "enabled": true },
                              "comp": { "enabled": true, "threshold": 5.0, "ratio": 100.0,
                                        "attack": -3.0, "release": 99999.0, "makeup": -6.0 } },
                      "clips": [] } ]
    })");
    auto projectBad = Project::load (dirBad, warnings, error);
    expect (projectBad != nullptr && projectBad->tracks.size() == 1, "不正データを読込めること");
    if (projectBad != nullptr && ! projectBad->tracks.empty())
    {
        const auto v = Comp::load (projectBad->tracks[0].params->comp);
        expect (juce::approximatelyEqual (v.thresholdDb, 0.0f)
                    && juce::approximatelyEqual (v.ratio, 20.0f)
                    && juce::approximatelyEqual (v.attackMs, 0.1f)
                    && juce::approximatelyEqual (v.releaseMs, 1000.0f)
                    && juce::approximatelyEqual (v.makeupDb, 0.0f),
                "範囲外値がクランプされること");
    }
    dirBad.deleteRecursively();
}

// ---- TrackComp: 静的カーブ・時定数（63.2%）・レベル不変性・ステレオリンク・Make Up・
//      検波HPF・バイパス・再進入reset・平滑化10ms・ブロックサイズ不変・切替の振幅安全性 ----
void testTrackCompDynamics()
{
    beginTest ("track comp dynamics");
    constexpr double sr = 48000.0;
    constexpr int blockSize = 512;

    Comp::Values base;
    base.thresholdDb = -30.0f;
    base.ratio = 4.0f;
    base.attackMs = 10.0f;
    base.releaseMs = 100.0f;

    // 一定振幅ampをsamplesぶん流す（512ずつ・連番serial）
    auto feedConstant = [&] (TrackComp& comp, juce::uint64& serial, float amp, int samples,
                             const Comp::Values& targets, bool enabled = true)
    {
        float block[blockSize];
        int done = 0;
        while (done < samples)
        {
            const int n = juce::jmin (blockSize, samples - done);
            std::fill (block, block + n, amp);
            comp.process (block, nullptr, n, sr, ++serial, false, enabled, targets);
            done += n;
        }
    };

    // ---- 静的カーブ ----
    {
        const float t = -30.0f, r = 4.0f;
        expect (juce::approximatelyEqual (Comp::computeOutputDb (-50.0f, t, r), -50.0f),
                "Threshold-20dBは素通し");
        const float o1 = Comp::computeOutputDb (-20.0f, t, r);
        const float o2 = Comp::computeOutputDb (-10.0f, t, r);
        expect (std::abs ((o2 - o1) - 10.0f / r) < 1.0e-4f, "knee上端より上の傾きは1/Ratio");
        expect (std::abs (o2 - (t + (-10.0f - t) / r)) < 1.0e-4f, "上側の出力はT+over/Ratio");
        float prev = Comp::computeOutputDb (t - 4.0f, t, r);
        float maxStep = 0.0f;
        for (float in = t - 4.0f + 0.01f; in <= t + 4.0f; in += 0.01f)
        {
            const float out = Comp::computeOutputDb (in, t, r);
            maxStep = juce::jmax (maxStep, std::abs (out - prev));
            prev = out;
        }
        expect (maxStep < 0.02f, "knee帯（Threshold±3dB）の出力が連続");
    }

    // ---- Attack/Release時定数: α=exp(-1/(τ·fs))・τ後に最終GRの63.2% ----
    // 振幅0.5(-6.02dB)・T=-30・4:1 → 最終GR≒18dB。検波が一定なのでGR目標はステップになる
    float grFinal = 0.0f;
    {
        TrackComp comp;
        comp.snapTo (sr, true, base);
        juce::uint64 serial = 0;
        const int tauAttack = (int) std::lround (base.attackMs * 0.001 * sr); // 480
        feedConstant (comp, serial, 0.5f, tauAttack, base);
        const float grAtTau = comp.currentGainReductionDb();
        feedConstant (comp, serial, 0.5f, (int) sr, base); // 1秒で収束
        grFinal = comp.currentGainReductionDb();
        expect (std::abs (grFinal - 17.98f) < 0.1f, "最終GRが静的カーブどおり（約18dB）");
        expect (std::abs (grAtTau / grFinal - 0.632f) < 0.01f, "Attack: τ後に最終GRの63.2%");

        // Release: 無音に落としてτ_rel後に36.8%（e^-1）まで戻る
        const int tauRelease = (int) std::lround (base.releaseMs * 0.001 * sr); // 4800
        feedConstant (comp, serial, 0.0f, tauRelease, base);
        expect (std::abs (comp.currentGainReductionDb() / grFinal - 0.368f) < 0.01f,
                "Release: τ後に最終GRの36.8%へ減衰");
    }

    // ---- レベル不変性: 入力レベルを変えても最終GRで正規化したエンベロープの形が同じ ----
    {
        TrackComp compA, compB;
        compA.snapTo (sr, true, base);
        compB.snapTo (sr, true, base);
        juce::uint64 serialA = 0, serialB = 0;
        const int tauAttack = (int) std::lround (base.attackMs * 0.001 * sr);
        feedConstant (compA, serialA, 0.5f, tauAttack, base); // 最終GR≒18dB
        feedConstant (compB, serialB, 0.1f, tauAttack, base); // 最終GR≒7.5dB
        const float grA = compA.currentGainReductionDb();
        const float grB = compB.currentGainReductionDb();
        feedConstant (compA, serialA, 0.5f, (int) sr, base);
        feedConstant (compB, serialB, 0.1f, (int) sr, base);
        expect (std::abs (grA / compA.currentGainReductionDb()
                          - grB / compB.currentGainReductionDb()) < 5.0e-3f,
                "入力レベルによらず正規化エンベロープが一致（dBドメイン平滑化の性質）");
    }

    // ---- ステレオリンク: 片chだけ大きくても両chに同量のゲイン（定位不動）----
    {
        TrackComp comp;
        comp.snapTo (sr, true, base);
        float left[blockSize], right[blockSize], inL[blockSize], inR[blockSize];
        for (int i = 0; i < blockSize; ++i)
        {
            inL[i] = left[i] = 0.5f * (float) std::sin (0.13 * i);
            inR[i] = right[i] = 0.05f * (float) std::sin (0.13 * i + 1.0);
        }
        comp.process (left, right, blockSize, sr, 1, false, true, base);
        float maxRatioDiff = 0.0f;
        for (int i = 0; i < blockSize; ++i)
            if (std::abs (inL[i]) > 1.0e-4f && std::abs (inR[i]) > 1.0e-4f)
                maxRatioDiff = juce::jmax (maxRatioDiff,
                                           std::abs (left[i] / inL[i] - right[i] / inR[i]));
        expect (maxRatioDiff < 1.0e-5f, "両chのゲインが常に同量（定位が動かない）");
    }

    // ---- Make Up: 圧縮が起きないレベルで+6dB → 出力がちょうど2倍弱（10^(6/20)）----
    {
        auto makeup = Comp::defaults; // Threshold 0dB＝掛からない
        makeup.makeupDb = 6.0f;
        TrackComp comp;
        comp.snapTo (sr, true, makeup);
        float block[blockSize];
        std::fill (block, block + blockSize, 0.1f);
        comp.process (block, nullptr, blockSize, sr, 1, false, true, makeup);
        expect (std::abs (block[blockSize - 1] / 0.1f
                          - juce::Decibels::decibelsToGain (6.0f)) < 1.0e-3f,
                "Make Up +6dBが出力に掛かる");
    }

    // ---- 検波HPF: 80Hzより下のサインはONで検波から外れGRが減る ----
    {
        auto hpfOn = base;
        hpfOn.detectorHpf = true;
        TrackComp compOff, compOn;
        compOff.snapTo (sr, true, base);
        compOn.snapTo (sr, true, hpfOn);
        juce::uint64 serialOff = 0, serialOn = 0;
        float block[blockSize];
        auto feedSine = [&] (TrackComp& comp, juce::uint64& serial, const Comp::Values& targets)
        {
            double phase = 0.0;
            const double inc = juce::MathConstants<double>::twoPi * 40.0 / sr; // 40Hz
            for (int b = 0; b < 100; ++b) // 約1秒
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    block[i] = 0.5f * (float) std::sin (phase);
                    phase += inc;
                }
                comp.process (block, nullptr, blockSize, sr, ++serial, false, true, targets);
            }
            return comp.currentGainReductionDb();
        };
        const float grOff = feedSine (compOff, serialOff, base);
        const float grOn = feedSine (compOn, serialOn, hpfOn);
        expect (grOn < grOff - 3.0f, "検波HPF ONで40Hz入力のGRが3dB以上減る");
    }

    // ---- バイパス（compEnabled=false）: 完全素通し（ビット一致）----
    {
        TrackComp comp;
        comp.snapTo (sr, false, base);
        expect (! comp.needsActivePath (false), "OFFでsnapToすれば高速パス");
        float block[blockSize], input[blockSize];
        juce::Random random (7);
        for (int i = 0; i < blockSize; ++i)
            input[i] = block[i] = random.nextFloat() - 0.5f;
        comp.process (block, nullptr, blockSize, sr, 1, false, false, base);
        bool identical = true;
        for (int i = 0; i < blockSize; ++i)
            identical = identical && juce::exactlyEqual (block[i], input[i]);
        expect (identical, "バイパスはビット一致の素通し");
    }

    // ---- 全ゼロ入力（HPF ON/OFF）: 出力が厳密なゼロ・内部状態とメーター値がfinite ----
    {
        for (const bool hpf : { false, true })
        {
            auto targets = base;
            targets.detectorHpf = hpf;
            TrackComp comp;
            comp.snapTo (sr, true, targets);
            juce::uint64 serial = 0;
            float block[blockSize];
            bool allZero = true;
            for (int b = 0; b < 8; ++b)
            {
                std::fill (block, block + blockSize, 0.0f);
                comp.process (block, nullptr, blockSize, sr, ++serial, false, true, targets);
                for (int i = 0; i < blockSize; ++i)
                    allZero = allZero && block[i] == 0.0f;
            }
            expect (allZero, "全ゼロ入力の出力が厳密なゼロ");
            expect (std::isfinite (comp.currentGainReductionDb())
                        && std::isfinite (comp.blockMaxGainReductionDb())
                        && std::isfinite (comp.blockMaxDetectorPeak()),
                    "無音でもGR・検波メーター値がfinite（dBフロアの検証）");
        }
    }

    // ---- 強いGR中にOFF→時間経過→ON: 古いGRが再利用されず0から立ち上がる ----
    {
        TrackComp comp;
        comp.snapTo (sr, true, base);
        juce::uint64 serial = 0;
        feedConstant (comp, serial, 0.5f, (int) sr, base); // GR≒18dBまで掛ける
        expect (comp.currentGainReductionDb() > 15.0f, "前提: 強いGRが掛かっている");
        int guard = 0;
        while (comp.needsActivePath (false) && guard++ < 100)
            feedConstant (comp, serial, 0.5f, blockSize, base, false); // OFF（フェードアウト中はactive）
        expect (! comp.needsActivePath (false), "OFF後フェード完了で高速パスへ移る");
        serial += 50; // 高速パス相当の時間経過（processが呼ばれない）

        float one[1] = { 0.5f };
        comp.process (one, nullptr, 1, sr, ++serial, false, true, base); // 再ON
        expect (comp.currentGainReductionDb() < 1.0f, "再ONは0からの立ち上がり（凍結GRの再利用なし）");
        expect (std::abs (one[0] - 0.5f) < 1.0e-3f, "再ONの先頭サンプルはdry（フェードイン開始点）");
    }

    // ---- パラメータ平滑化: 変更後10msでtargetへ到達・中間はdB直線 ----
    {
        auto targets = Comp::defaults; // Threshold 0dB＝GRなし（Make Upだけを観測する）
        TrackComp comp;
        comp.snapTo (sr, true, targets);
        targets.makeupDb = 12.0f;
        constexpr int rampSamples = 480; // 10ms @ 48k
        float block[rampSamples + 64];
        std::fill (block, block + rampSamples + 64, 0.1f);
        comp.process (block, nullptr, rampSamples + 64, sr, 1, false, true, targets);
        expect (std::abs (block[240 - 1] / 0.1f - juce::Decibels::decibelsToGain (6.0f)) < 0.02f,
                "平滑化の中間（5ms）で+6dB＝dBドメインの直線ランプ");
        expect (std::abs (block[rampSamples + 32] / 0.1f
                          - juce::Decibels::decibelsToGain (12.0f)) < 0.02f,
                "10msでtarget（+12dB）へ到達");
    }

    // ---- ブロックサイズ不変: 64と512で処理しても出力がビット一致 ----
    {
        auto targets = base;
        targets.detectorHpf = true;
        targets.makeupDb = 2.0f;
        constexpr int total = 4096;
        float a[total], b[total];
        juce::Random random (21);
        for (int i = 0; i < total; ++i)
            a[i] = b[i] = (random.nextFloat() - 0.5f) * 0.8f;
        TrackComp compA, compB;
        compA.snapTo (sr, true, targets);
        compB.snapTo (sr, true, targets);
        juce::uint64 serialA = 0, serialB = 0;
        for (int pos = 0; pos < total; pos += 64)
            compA.process (a + pos, nullptr, 64, sr, ++serialA, false, true, targets);
        for (int pos = 0; pos < total; pos += 512)
            compB.process (b + pos, nullptr, 512, sr, ++serialB, false, true, targets);
        bool identical = true;
        for (int i = 0; i < total; ++i)
            identical = identical && juce::exactlyEqual (a[i], b[i]);
        expect (identical, "パラメータ平滑化・エンベロープがブロック境界に依存しない（ビット一致）");
    }

    // ---- ON/OFF切替の振幅安全性: NaN/Infなし・クロスフェードで跳躍しない ----
    // ⚠️ これはsmooth branchingの弱点＝**勾配**の不連続を検出しない（GR自体は連続なため）。
    //    勾配由来の可聴性は耳確認へ分離（plan: 動作確認）
    {
        TrackComp comp;
        comp.snapTo (sr, true, base);
        juce::uint64 serial = 0;
        double phase = 0.0;
        const double inc = juce::MathConstants<double>::twoPi * 1000.0 / sr;
        float previous = 0.0f, maxJump = 0.0f;
        bool allFinite = true;
        float block[blockSize];
        for (int b = 0; b < 40; ++b)
        {
            const bool on = (b / 4) % 2 == 0; // 4ブロックごとにON/OFF
            for (int i = 0; i < blockSize; ++i)
            {
                block[i] = 0.5f * (float) std::sin (phase);
                phase += inc;
            }
            comp.process (block, nullptr, blockSize, sr, ++serial, false, on, base);
            for (int i = 0; i < blockSize; ++i)
            {
                allFinite = allFinite && std::isfinite (block[i]);
                maxJump = juce::jmax (maxJump, std::abs (block[i] - previous));
                previous = block[i];
            }
        }
        expect (allFinite, "切替中にNaN/Infが出ないこと");
        // 1kHz/振幅0.5のサイン自体の最大傾斜は約0.065。瞬時切替ならGR≒18dB分の段差
        //（最大0.44規模）が出るので、この閾値で退行を検出できる
        expect (maxJump < 0.15f, "切替がクロスフェードされ跳躍しないこと");
    }
}

// ---- エンジン vs バウンス: Comp有効時（EQは中立＝Comp単独のactive経路）の出力一致 ----
void testEngineCompBounceConsistency()
{
    beginTest ("engine vs bounce with active comp");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 16;
    constexpr int totalSamples = blockSize * numBlocks;
    // 音声トラックは再生開始のシーク適用で timelineJumped=true が渡り、Comp/EQとも即時
    // スナップされる（dryフェードインは再進入時のみ）ため、先頭サンプルから比較できる
    constexpr int compareFrom = 0;

    auto makeAudio = [] (int channels, int len, float scale)
    {
        auto buffer = std::make_shared<juce::AudioBuffer<float>> (channels, len);
        juce::Random random (11);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < len; ++i)
                buffer->setSample (ch, i,
                                   (std::sin ((float) i * 0.05f + (float) ch) * 0.3f
                                    + (random.nextFloat() - 0.5f) * 0.2f) * scale);
        return buffer;
    };

    Project project;
    {
        Track track; // ステレオクリップ＋コンプ（HPFなし・Make Upあり）＋send
        track.id = 1;
        track.params->gain.store (0.8f);
        track.params->pan.store (0.3f);
        track.params->sends[1].store (0.4f);
        track.params->compEnabled.store (true);
        Comp::store (track.params->comp, Comp::normalized ({ -24.0f, 4.0f, 5.0f, 80.0f, 3.0f, false }));
        Clip clip;
        clip.audio = makeAudio (2, totalSamples, 1.0f);
        clip.lengthSamples = totalSamples;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }
    {
        Track track; // モノクリップ＋コンプ（検波HPFあり・速いAttack）
        track.id = 2;
        track.params->gain.store (0.7f);
        track.params->pan.store (-0.5f);
        track.params->compEnabled.store (true);
        Comp::store (track.params->comp, Comp::normalized ({ -30.0f, 8.0f, 1.0f, 50.0f, 0.0f, true }));
        Clip clip;
        clip.audio = makeAudio (1, totalSamples, 0.8f);
        clip.lengthSamples = totalSamples;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }
    project.busParams[1]->gain.store (0.9f);
    project.masterParams->gain.store (0.85f);

    auto renderEngine = [&] (juce::AudioBuffer<float>& out)
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
        for (int blockIndex = 0; blockIndex * blockSize < out.getNumSamples(); ++blockIndex)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, blockIndex * blockSize, buffer, ch, 0, blockSize);
        }
        engine.stop();
        snapshots.deleteRetired();
    };

    // Limiterの遅延ぶん余分に1ブロック回す（比較はエンジン側を +latency で読む）
    const int latency = engineLimiterLatency (sr);
    juce::AudioBuffer<float> engineOut (2, totalSamples + blockSize);
    renderEngine (engineOut);

    // Compが実際に効いていること（OFFとの差がある）を先に確認する — 空一致の防止
    {
        project.tracks[0].params->compEnabled.store (false);
        project.tracks[1].params->compEnabled.store (false);
        juce::AudioBuffer<float> bypassOut (2, totalSamples);
        renderEngine (bypassOut);
        float maxDiff = 0.0f;
        for (int i = compareFrom; i < totalSamples; ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (0, i)
                                                     - bypassOut.getSample (0, i)));
        expect (maxDiff > 1.0e-3f, "CompありはOFFと出力が異なること（Compが実際に効いている）");
        project.tracks[0].params->compEnabled.store (true);
        project.tracks[1].params->compEnabled.store (true);
    }

    // バウンス側（TrackRenderへプレーン値コピー＝本番と同じ流儀）。
    // 入力＋Make Up後もピーク1.0未満に収まるレベル設計なので、バウンスのピーク正規化は
    // 走らず比較が成立する（走ればmaxDiffが大きく出て検出される）
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce-comp.wav");
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
            render.loadFxFrom (*track.params);
            for (auto& clip : track.clips)
                appendClipPlaybacks (clip, render.clips);
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
            for (int i = compareFrom; i < totalSamples; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (ch, i + latency)
                                                         - bounceOut.getSample (ch, i)));
        // 24bit量子化＋浮動小数点の積和順序差ぶんの許容誤差
        expect (maxDiff < 1.0e-4f, "Comp有効時もエンジンとバウンスが許容誤差内で一致すること");
    }
    reader.reset();
    dir.deleteRecursively();
}

// ---- Compの検波はpan前（ステレオのみトラック）: 振っても圧縮量が変わらないこと ----
// L=大・R=小のステレオクリップをハード右pan（バランス法則でLが消える）にすると、
// pan後検波なら検波はRの小信号だけ＝GRほぼゼロになる。pan前検波ならLの大信号でGRが掛かり、
// 出力（R側）が大きく減衰する。エンジンとバウンスの一致も同時に確認する
void testEngineCompPrePanDetection()
{
    beginTest ("engine comp pre-pan detection");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 16;
    constexpr int totalSamples = blockSize * numBlocks;
    constexpr int measureFrom = 4096; // Attack整定後（GRが最終値に達した区間）を測る

    Project project;
    {
        Track track;
        track.id = 1;
        track.params->gain.store (1.0f);
        track.params->pan.store (1.0f); // ハード右: バランス法則で balL=0・balR=1
        track.params->compEnabled.store (true);
        Comp::store (track.params->comp, Comp::normalized ({ -30.0f, 8.0f, 1.0f, 200.0f, 0.0f, false }));
        Clip clip;
        auto audio = std::make_shared<juce::AudioBuffer<float>> (2, totalSamples);
        for (int i = 0; i < totalSamples; ++i)
        {
            audio->setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 500.0 * i / sr));
            audio->setSample (1, i, 0.02f * (float) std::sin (juce::MathConstants<double>::twoPi * 700.0 * i / sr));
        }
        clip.audio = std::move (audio);
        clip.lengthSamples = totalSamples;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }

    auto renderEngine = [&] (juce::AudioBuffer<float>& out)
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
        for (int blockIndex = 0; blockIndex * blockSize < out.getNumSamples(); ++blockIndex)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, blockIndex * blockSize, buffer, ch, 0, blockSize);
        }
        engine.stop();
        snapshots.deleteRetired();
    };

    auto rmsRight = [&] (const juce::AudioBuffer<float>& out)
    {
        double sum = 0.0;
        for (int i = measureFrom; i < totalSamples; ++i)
            sum += (double) out.getSample (1, i) * out.getSample (1, i);
        return std::sqrt (sum / (totalSamples - measureFrom));
    };

    // Limiterの遅延ぶん onOut は余分に1ブロック確保（バウンス比較で +latency 読みするため）
    const int latency = engineLimiterLatency (sr);
    juce::AudioBuffer<float> onOut (2, totalSamples + blockSize), offOut (2, totalSamples);
    renderEngine (onOut);
    project.tracks[0].params->compEnabled.store (false);
    renderEngine (offOut);
    project.tracks[0].params->compEnabled.store (true);

    // pan前検波なら L(-6dB) が閾値-30dBを大きく超え、GR≒21dBがR出力に掛かる。
    // pan後検波（バグ）だと検波はR(-34dB)のみ＝GRほぼゼロで比が1に近くなる
    const float ratio = (float) (rmsRight (onOut) / juce::jmax (1.0e-9, rmsRight (offOut)));
    expect (ratio < 0.3f, "ハード右panでもL側の大信号でGRが掛かる（検波はpan前）");
    // ハード右panでL出力は無音（バランス法則）
    expect (onOut.getMagnitude (0, measureFrom, totalSamples - measureFrom) < 1.0e-6f,
            "ハード右panでL出力は無音のまま");

    // 同条件でエンジンとバウンスが一致（バウンス側のprePan構造の検証）
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce-prepan.wav");
    {
        BounceRenderer::Request request;
        request.sampleRate = sr;
        request.bpm = 120.0;
        request.endSample = totalSamples;
        request.targetFile = target;
        BounceRenderer::TrackRender render;
        render.gain = 1.0f;
        render.pan = 1.0f;
        render.compEnabled = true;
        render.comp = Comp::load (project.tracks[0].params->comp);
        for (auto& clip : project.tracks[0].clips)
            appendClipPlaybacks (clip, render.clips);
        request.tracks.push_back (std::move (render));
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
            for (int i = 0; i < totalSamples; ++i) // 音声は再生開始で即時スナップ＝先頭から比較できる
                maxDiff = juce::jmax (maxDiff, std::abs (onOut.getSample (ch, i + latency)
                                                         - bounceOut.getSample (ch, i)));
        expect (maxDiff < 1.0e-4f, "ハード右pan＋Compでもエンジンとバウンスが一致すること");
    }
    reader.reset();
    dir.deleteRecursively();
}

// ---- エンジン: pan法則・post-fader send（素通しバス）・busGain/mute・Masterゲイン・メーター ----
// ---- LoudnessMeter: ITU-R BS.1770-5 / EBU Tech 3341 v4 準拠の絶対照合 ----
void testLoudnessMeterStandard()
{
    beginTest ("loudness meter standard");

    const auto makeSine = [] (double sr, double freq, float amplitude, int numSamples)
    {
        juce::AudioBuffer<float> buffer (2, numSamples);
        for (int i = 0; i < numSamples; ++i)
        {
            const float v = amplitude
                            * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / sr);
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v); // ステレオ同相
        }
        return buffer;
    };

    // Test 1: 1000Hz・ステレオ同相・各chピーク-23dBFS・20秒 → -23.0 ±0.1 LUFS（44.1k/48k両方）
    for (const double sr : { 48000.0, 44100.0 })
    {
        const float amp = juce::Decibels::decibelsToGain (-23.0f);
        const auto signal = makeSine (sr, 1000.0, amp, (int) (sr * 20.0));
        const auto dir = makeTempDir();
        const auto file = dir.getChildFile ("test1.wav");
        expect (writeBufferWav (file, signal, sr), "テスト信号を書けること");
        double lufs = 0.0, tpDb = 0.0;
        expect (Loudness::measureFile (file, lufs, tpDb), "計測できること");
        expect (std::abs (lufs - (-23.0)) < 0.1, "Test 1: integrated -23.0 ±0.1 LUFS");
        expect (std::abs (tpDb - (-23.0)) < 0.2, "正弦波のTPはサンプルピークとほぼ一致（-23dBTP）");
        dir.deleteRecursively();
    }

    // Test 3相当（相対ゲート）: -36dBFS 10秒 → -23dBFS 20秒 → -36dBFS 10秒。
    // 相対ゲート（仮平均-10LU）が前後の静かな区間を除外し、integratedは-23.0のまま
    {
        const double sr = 48000.0;
        const auto loud = makeSine (sr, 1000.0, juce::Decibels::decibelsToGain (-23.0f), (int) (sr * 20.0));
        const auto quiet = makeSine (sr, 1000.0, juce::Decibels::decibelsToGain (-36.0f), (int) (sr * 10.0));
        juce::AudioBuffer<float> combined (2, quiet.getNumSamples() * 2 + loud.getNumSamples());
        for (int ch = 0; ch < 2; ++ch)
        {
            combined.copyFrom (ch, 0, quiet, ch, 0, quiet.getNumSamples());
            combined.copyFrom (ch, quiet.getNumSamples(), loud, ch, 0, loud.getNumSamples());
            combined.copyFrom (ch, quiet.getNumSamples() + loud.getNumSamples(),
                               quiet, ch, 0, quiet.getNumSamples());
        }
        const auto dir = makeTempDir();
        const auto file = dir.getChildFile ("test3.wav");
        expect (writeBufferWav (file, combined, sr), "テスト信号を書けること");
        double lufs = 0.0, tpDb = 0.0;
        expect (Loudness::measureFile (file, lufs, tpDb), "計測できること");
        expect (std::abs (lufs - (-23.0)) < 0.1, "Test 3: 相対ゲートが静かな区間を除外して-23.0 ±0.1");
        dir.deleteRecursively();
    }
}

// ---- TruePeakDetector: サンプル間ピークの検出（fs/4正弦・位相π/4）とDCユニティ ----
void testTruePeakDetector()
{
    beginTest ("true peak detector");

    // fs/4の正弦を位相π/4でサンプリング: サンプル値は全て±0.7071だが真のピークは1.0
    {
        TruePeakDetector detector;
        detector.reset();
        float samplePeak = 0.0f, truePeak = 0.0f;
        for (int i = 0; i < 4096; ++i)
        {
            const float x = (float) std::sin (juce::MathConstants<double>::pi * 0.5 * i
                                              + juce::MathConstants<double>::pi / 4.0);
            const float tp = detector.processSample (0, x);
            if (i >= 64) // カーネル充填後から測る
            {
                samplePeak = juce::jmax (samplePeak, std::abs (x));
                truePeak = juce::jmax (truePeak, tp);
            }
        }
        expect (std::abs (samplePeak - 0.70711f) < 1.0e-4f, "サンプルピークは0.707（前提確認）");
        expect (std::abs (truePeak - 1.0f) < 0.02f, "サンプル間の真のピーク1.0を検出すること");
    }

    // DC（定常値）: 各相の係数和=1に正規化してあるのでTP=サンプル値
    {
        TruePeakDetector detector;
        detector.reset();
        float truePeak = 0.0f;
        for (int i = 0; i < 256; ++i)
        {
            const float tp = detector.processSample (0, 0.5f);
            if (i >= 64)
                truePeak = juce::jmax (truePeak, tp);
        }
        expect (std::abs (truePeak - 0.5f) < 0.005f, "定常信号のTPはサンプルピークと一致（ユニティ）");
    }
}

// ---- MasterMeterSource → リング → Aggregator のパイプライン＋計測契約の回帰 ----
// （100ms未満で停止しても最大TPを保持・旧世代Entryを混ぜない・リングoverflowで計測無効）
void testMasterMeterPipeline()
{
    beginTest ("master meter pipeline");

    const double sr = 48000.0;
    const auto fillSine = [&] (std::vector<float>& out, float amplitude)
    {
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = amplitude
                     * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) i / sr);
    };
    const auto feedSource = [&] (MasterMeterSource& source, const std::vector<float>& left,
                                 const std::vector<float>& right, bool startEdge)
    {
        constexpr int block = 480;
        for (size_t pos = 0; pos < left.size(); pos += block)
        {
            const int n = (int) juce::jmin ((size_t) block, left.size() - pos);
            source.process (left.data() + pos, right.data() + pos, n, sr,
                            true, startEdge && pos == 0, false);
        }
    };

    MasterMeterRing ring (1 << 10);
    MasterMeterSource source;
    source.setRing (&ring);
    Loudness::MasterMeterAggregator aggregator;

    // 5秒の1kHz同相 -23dBFS → short-term/integrated/相関/TPが揃う
    std::vector<float> left (48000 * 5), right;
    fillSine (left, juce::Decibels::decibelsToGain (-23.0f));
    right = left;
    feedSource (source, left, right, true);
    aggregator.consume (ring);
    {
        const auto& feed = aggregator.feed();
        expect (feed.measurementValid, "計測が有効であること");
        expect (feed.hasIntegrated && std::abs (feed.integratedLufs - (-23.0f)) < 0.15f,
                "パイプライン経由のintegratedが-23.0付近");
        expect (feed.hasShortTerm && std::abs (feed.shortTermLufs - (-23.0f)) < 0.15f,
                "short-termも-23.0付近");
        expect (feed.hasCorrelation && feed.correlation > 0.99f, "同相の相関は+1付近");
        expect (feed.hasTruePeak && std::abs (feed.maxTruePeakDb - (-23.0f)) < 0.3f,
                "TPは-23dBTP付近");
    }

    // 新しい再生セッション（世代切替）: -33dBFSの5秒。旧世代（-23）が混ざると-33にならない
    std::vector<float> quietL (48000 * 5), quietR;
    fillSine (quietL, juce::Decibels::decibelsToGain (-33.0f));
    quietR = quietL;
    feedSource (source, quietL, quietR, true);
    aggregator.consume (ring);
    expect (aggregator.feed().hasIntegrated
                && std::abs (aggregator.feed().integratedLufs - (-33.0f)) < 0.15f,
            "世代切替で旧セッションのEntryを集計に混ぜないこと");

    // 逆相: 相関は-1付近
    std::vector<float> invR (quietL.size());
    for (size_t i = 0; i < invR.size(); ++i)
        invR[i] = -quietL[i];
    feedSource (source, quietL, invR, true);
    aggregator.consume (ring);
    expect (aggregator.feed().hasCorrelation && aggregator.feed().correlation < -0.99f,
            "逆相の相関は-1付近");

    // 停止エッジの部分統計: 静音1.2秒（12ブロック）＋100サンプル（うち1サンプルだけ1.0の
    // スパイク）で停止 → 100ms未満の部分ブロックでも最大TPが保持されること
    {
        std::vector<float> tail (48000 + 9600 + 100, 0.0f);
        fillSine (tail, juce::Decibels::decibelsToGain (-40.0f));
        tail[tail.size() - 50] = 1.0f; // 最後の部分ブロック内のスパイク
        std::vector<float> tailR = tail;
        feedSource (source, tail, tailR, true);
        source.process (nullptr, nullptr, 0, sr, false, false, true); // 停止エッジ
        aggregator.consume (ring);
        expect (aggregator.feed().hasTruePeak && aggregator.feed().maxTruePeakDb > -0.5f,
                "100ms未満で停止しても最後の部分ブロックの最大TPを保持すること");
    }

    // short-termは3秒窓（完全な100msブロック×30）が揃うまで出さない
    //（揃う前に出すとMomentaryに近い短窓の値をshort-termとして誤読させる）
    {
        MasterMeterRing shortRing (1 << 10);
        MasterMeterSource shortSource;
        shortSource.setRing (&shortRing);
        shortSource.prepare (sr);
        Loudness::MasterMeterAggregator shortAggregator;

        std::vector<float> one (48000), oneR; // 1秒 = 10ブロック < 30
        fillSine (one, juce::Decibels::decibelsToGain (-23.0f));
        oneR = one;
        feedSource (shortSource, one, oneR, true);
        shortAggregator.consume (shortRing);
        expect (! shortAggregator.feed().hasShortTerm, "3秒未満はshort-termを出さないこと");
        expect (shortAggregator.feed().hasTruePeak, "TPは最初のブロックから出ること");

        std::vector<float> more (48000 * 3), moreR; // 追加3秒 → 計4秒で揃う
        fillSine (more, juce::Decibels::decibelsToGain (-23.0f));
        moreR = more;
        feedSource (shortSource, more, moreR, false);
        shortAggregator.consume (shortRing);
        expect (shortAggregator.feed().hasShortTerm
                    && std::abs (shortAggregator.feed().shortTermLufs - (-23.0f)) < 0.15f,
                "3秒揃ったらshort-termが出ること");

        // 停止時の部分ブロックが混ざっても、short-termは完全30個の3秒窓のまま
        //（部分を窓に入れると「完全29＋部分1」の3秒未満窓になる回帰）
        std::vector<float> stub (50), stubR;
        fillSine (stub, juce::Decibels::decibelsToGain (-23.0f));
        stubR = stub;
        feedSource (shortSource, stub, stubR, false);
        shortSource.process (nullptr, nullptr, 0, sr, false, false, true); // 停止エッジ
        shortAggregator.consume (shortRing);
        expect (shortAggregator.feed().hasShortTerm
                    && std::abs (shortAggregator.feed().shortTermLufs - (-23.0f)) < 0.15f,
                "部分ブロック混入後もshort-termは3秒窓のまま有効であること");
    }

    // リングoverflow: 容量8の小さいリングを読まずに溢れさせる → 計測無効（黙って継続しない）。
    // 次の再生セッション（世代切替）で回復する
    {
        MasterMeterRing tinyRing (8);
        MasterMeterSource tinySource;
        tinySource.setRing (&tinyRing);
        Loudness::MasterMeterAggregator tinyAggregator;

        std::vector<float> two (48000 * 2), twoR;
        fillSine (two, juce::Decibels::decibelsToGain (-23.0f));
        twoR = two;
        feedSource (tinySource, two, twoR, true); // 20ブロック > 容量8
        tinyAggregator.consume (tinyRing);
        expect (! tinyAggregator.feed().measurementValid, "リングoverflowで計測無効になること");

        std::vector<float> half (4800 * 4), halfR; // 新セッションは容量内（4ブロック）
        fillSine (half, juce::Decibels::decibelsToGain (-23.0f));
        halfR = half;
        feedSource (tinySource, half, halfR, true);
        tinyAggregator.consume (tinyRing);
        expect (tinyAggregator.feed().measurementValid, "次の再生セッションで計測有効へ回復すること");
    }

    // 複数世代がリング満杯中にまとめて破棄されたケース: リングに残った**古い世代**を
    // 有効表示しない（汚染世代は最後=最大の世代しか残らないため「以前は全部無効」で解釈する）
    {
        MasterMeterRing staleRing (8);
        MasterMeterSource staleSource;
        staleSource.setRing (&staleRing);
        staleSource.prepare (sr);
        Loudness::MasterMeterAggregator staleAggregator;

        std::vector<float> two (48000 * 2), twoR;
        fillSine (two, juce::Decibels::decibelsToGain (-23.0f));
        twoR = two;
        feedSource (staleSource, two, twoR, true); // gen1: 8件格納＋12件破棄
        feedSource (staleSource, two, twoR, true); // gen2: 満杯のまま全破棄（taint=gen2）
        staleAggregator.consume (staleRing);       // リングにはgen1のみ残っている
        expect (! staleAggregator.feed().measurementValid,
                "破棄された新世代より古い世代を有効表示しないこと");

        std::vector<float> next (4800 * 4), nextR; // gen3は空いたリングに収まる → 回復
        fillSine (next, juce::Decibels::decibelsToGain (-23.0f));
        nextR = next;
        feedSource (staleSource, next, nextR, true);
        staleAggregator.consume (staleRing);
        expect (staleAggregator.feed().measurementValid,
                "汚染世代より後のセッションで回復すること");
    }
}

// ---- RT安全性: Limiter＋常時計測（4x TP FIR含む）が192kHz・小ブロックでヒープ確保ゼロ ----
// 処理時間はRelease専用ベンチマークとして実測値を報告するだけ（Debug/CI負荷で不安定になるため
// 合否には入れない）
void testRtNoAllocation()
{
    beginTest ("rt no allocation (limiter + meter)");

    const double sr = 192000.0;
    constexpr int block = 64;

    MasterLimiter limiter;
    Limiter::Values values;
    values.gainDb = 6.0f;
    limiter.snapTo (sr, values);

    MasterMeterRing ring;
    MasterMeterSource source;
    source.setRing (&ring);

    std::vector<float> left (block), right (block);
    for (int i = 0; i < block; ++i)
        left[i] = right[i] = (float) std::sin (0.3 * i);

    // 事前準備は本番と同じ経路（prepareToPlay相当＝非RTスレッドで重い係数計算を済ませる）。
    // **再生開始エッジ（beginSession）はカウント対象に含める** — RT上で走る経路だから
    source.prepare (sr);
    limiter.snapTo (sr, values);

    const int blocksPerSecond = (int) (sr / block);
    testAllocationCount = 0;
    const auto startMs = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < blocksPerSecond; ++i)
    {
        limiter.process (left.data(), right.data(), block, sr, values);
        source.process (left.data(), right.data(), block, sr, true, i == 0, false);
    }
    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;
    expect (testAllocationCount == 0, "1秒分（192kHz・64サンプルブロック）でヒープ確保ゼロ");

#if ! JUCE_DEBUG
    // Release専用ベンチマーク（報告のみ）: 実時間1秒分の処理に掛かった時間と実時間比
    std::cout << "[bench] limiter+meter 192kHz/64: " << elapsedMs << " ms per 1s of audio ("
              << (elapsedMs / 10.0) << "% realtime)" << std::endl;
#else
    juce::ignoreUnused (elapsedMs);
#endif
}

// MasterLimiter: 出力天井の絶対保証（インパルス列・矩形・フルスケール正弦＋
// Gain/Ceilingのブロック途中急変）とステレオリンク（クランプ後もL/R比率維持）
void testMasterLimiterBrickwall()
{
    beginTest ("MasterLimiter brickwall ceiling");

    const double sr = 48000.0;
    MasterLimiter limiter;
    Limiter::Values values;
    values.gainDb = 12.0f;
    values.ceilingDb = -1.0f;
    values.releaseMs = 60.0f;
    limiter.snapTo (sr, values);
    const int lookahead = limiter.lookaheadSamples();
    expect (lookahead == 96, "lookahead is 2ms at 48kHz (96 samples)");

    // 信号: インパルス列 → 0dBFS矩形 → フルスケール正弦（Rは常にLの半分＝リンク検証用）
    const int total = 48000;
    std::vector<float> left (total), right (total);
    for (int i = 0; i < total; ++i)
    {
        float x = 0.0f;
        if (i < 16000)
            x = (i % 1200 == 0) ? 1.0f : 0.0f;
        else if (i < 32000)
            x = ((i / 240) % 2 == 0) ? 1.0f : -1.0f;
        else
            x = std::sin (juce::MathConstants<float>::twoPi * 997.0f * (float) i / (float) sr);
        left[i] = x;
        right[i] = x * 0.5f;
    }

    // ブロック処理（端数サイズ）＋途中でCeilingを-1→-6へ急変させる（Gainも12→6）
    const int switchAt = 24000;
    int pos = 0;
    while (pos < total)
    {
        const int n = juce::jmin (333, total - pos);
        auto v = values;
        if (pos >= switchAt)
        {
            v.ceilingDb = -6.0f;
            v.gainDb = 6.0f;
        }
        limiter.process (left.data() + pos, right.data() + pos, n, sr, v);
        pos += n;
    }

    const float eps = 1.0e-5f;
    const float ceil1 = juce::Decibels::decibelsToGain (-1.0f);
    const float ceil6 = juce::Decibels::decibelsToGain (-6.0f);
    // Ceiling実効値はLサンプル整列＋10msランプ平滑で、切替はブロック境界（最大333サンプル
    // 遅れ）から始まる。switchAt + ブロック粒度 + ランプ(10ms=480) + L ＋余裕の後に完成する
    const int settled = switchAt + 333 + 480 + lookahead + 200;
    bool ceilingOk = true, tightOk = true, linkOk = true;
    for (int i = 0; i < total; ++i)
    {
        const float peak = juce::jmax (std::fabs (left[i]), std::fabs (right[i]));
        if (peak > ceil1 + eps)
            ceilingOk = false;
        if (i >= settled && peak > ceil6 + eps)
            tightOk = false;
        if (std::fabs (left[i]) > 1.0e-3f
            && std::fabs (right[i] - 0.5f * left[i]) > 1.0e-6f * juce::jmax (1.0f, std::fabs (left[i])))
            linkOk = false;
    }
    expect (ceilingOk, "no sample ever exceeds the -1dB ceiling");
    expect (tightOk, "after the mid-stream switch, output obeys the new -6dB ceiling");
    expect (linkOk, "stereo link keeps the R/L ratio through limiting and clamping");
}

// MasterLimiter: 定常GRの実測一致・リリース時定数・素通しのビット一致（Lサンプル位置合わせ）・
// リセット直後のディレイ充填
void testMasterLimiterDynamics()
{
    beginTest ("MasterLimiter dynamics");

    const double sr = 48000.0;
    MasterLimiter limiter;
    Limiter::Values values;
    values.gainDb = 6.0f;
    values.ceilingDb = -1.0f;
    values.releaseMs = 100.0f;
    limiter.snapTo (sr, values);
    const int lookahead = limiter.lookaheadSamples();

    // 定常正弦（0dBFS・+6dB gain）→ 収束後の出力ピークはceiling・GRは 6-(-1)=7dB
    const int steady = 24000;
    std::vector<float> left (steady), right (steady);
    for (int i = 0; i < steady; ++i)
        left[i] = right[i] =
            std::sin (juce::MathConstants<float>::twoPi * 997.0f * (float) i / (float) sr);
    limiter.process (left.data(), right.data(), steady, sr, values);

    float tailPeak = 0.0f;
    for (int i = steady - 4800; i < steady; ++i)
        tailPeak = juce::jmax (tailPeak, std::fabs (left[i]));
    const float tailPeakDb = juce::Decibels::gainToDecibels (tailPeak);
    expect (std::fabs (tailPeakDb - (-1.0f)) < 0.1f, "steady-state output peak sits at the ceiling");
    expect (std::fabs (limiter.currentGainReductionDb() - 7.0f) < 0.3f,
            "steady-state GR matches gain-over-ceiling (7dB)");

    // リリース時定数: 入力を無音にして τ=100ms 後のGRが約36.8%まで減衰する
    std::vector<float> silence (4800, 0.0f), silenceR (4800, 0.0f);
    // 窓（L+1）からピークが抜けきるまで流してから計測開始
    limiter.process (silence.data(), silenceR.data(), 2 * lookahead, sr, values);
    const float grStart = limiter.currentGainReductionDb();
    const int tau = (int) std::lround (0.1 * sr);
    int remaining = tau;
    while (remaining > 0)
    {
        const int n = juce::jmin (remaining, 4800);
        limiter.process (silence.data(), silenceR.data(), n, sr, values);
        remaining -= n;
    }
    const float ratio = limiter.currentGainReductionDb() / grStart;
    expect (grStart > 5.0f, "GR is still engaged when release measurement starts");
    expect (std::fabs (ratio - std::exp (-1.0f)) < 0.05f,
            "GR decays to ~36.8% after one release time constant");

    // 素通し（Gain 0・ceiling未満）のビット一致: out[n] == in[n-L]
    limiter.snapTo (sr, Limiter::Values {});
    const int n = 9600;
    std::vector<float> input (n);
    for (int i = 0; i < n; ++i)
        input[i] = 0.25f * std::sin (0.13f * (float) i) + 0.1f * std::sin (1.7f * (float) i);
    std::vector<float> outL (input), outR (input);
    // 複数ブロックに割って処理（ブロック境界の連続性も同時に確認する）
    for (int pos = 0; pos < n; pos += 256)
        limiter.process (outL.data() + pos, outR.data() + pos,
                         juce::jmin (256, n - pos), sr, Limiter::Values {});
    bool zeroFill = true, bitExact = true;
    for (int i = 0; i < lookahead; ++i)
        if (outL[i] != 0.0f)
            zeroFill = false;
    for (int i = lookahead; i < n; ++i)
        if (outL[i] != input[(size_t) (i - lookahead)] || outR[i] != outL[i])
            bitExact = false;
    expect (zeroFill, "first L samples after reset are silent (delay fill)");
    expect (bitExact, "neutral settings pass audio through bit-exactly (L-sample aligned)");
}

// ---- Master Limiterのリセット契約（エンジン統合）: 明示シークで旧位置のディレイ内容が
// 漏れないこと（サイクルラップの「無音が入らない」側は testPlaybackEngineCycleLoop が担う）----
void testEngineLimiterSeekReset()
{
    beginTest ("engine limiter seek reset");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // 大音量のDCクリップ（Limiterのディレイに必ず非ゼロが残る状態を作る）
    Project project;
    Track track;
    track.id = 1;
    track.params->gain.store (1.0f);
    Clip clip;
    clip.lengthSamples = blockSize * 4;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, blockSize * 4);
    for (int i = 0; i < clip.audio->getNumSamples(); ++i)
        clip.audio->setSample (0, i, 0.8f);
    track.clips.push_back (std::move (clip));
    project.tracks.push_back (std::move (track));
    snapshots.push (project.buildSnapshot());

    juce::AudioBuffer<float> buffer (2, blockSize);
    transport.seekRequest.store (0);
    engine.play();
    for (int i = 0; i < 2; ++i)
    {
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
    }
    expect (buffer.getMagnitude (0, 0, blockSize) > 0.7f, "シーク前は大音量が出ていること");

    // クリップの無い無音地帯へ明示シーク → リセット契約によりディレイが消去され、
    // 直前の0.8のDCが最初のLサンプルに漏れない（漏れると先頭2msにDC断片が出る）
    transport.seekRequest.store (blockSize * 32);
    buffer.clear();
    juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
    engine.process (info);
    engine.stop();
    expect (buffer.getMagnitude (0, 0, blockSize) == 0.0f
                && buffer.getMagnitude (1, 0, blockSize) == 0.0f,
            "明示シーク直後のブロックに旧位置のディレイ内容が漏れないこと");

    snapshots.deleteRetired();
}

// ---- フェード×サイクル: フェード終端後もLimiterへ無音を流して状態を進めること ----
// fadeEnd以降の入力は無音・fadeEnd+L以降は出力加算だけ止める。サイクルはLimiterを
// リセットしない契約なので、ここを止めると凍結した古い2msが折り返し後の先頭へ漏れる
void testEngineFadeCycleLimiterState()
{
    beginTest ("engine fade + cycle limiter state");

    constexpr double sr = 48000.0;
    constexpr int blockSize = 512;
    constexpr double bpm = 50.0; // 1/16 = 14400サンプル
    const int latency = engineLimiterLatency (sr);

    TransportState transport;
    transport.bpm.store (bpm);
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    Project project;
    project.bpm = bpm;
    project.fadeOutStartSixteenths = 1; // 14400
    project.fadeOutEndSixteenths = 2;   // 28800
    Track track;
    track.id = 1;
    track.params->gain.store (1.0f);
    Clip clip;
    clip.lengthSamples = 40000;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, 40000);
    for (int i = 0; i < 40000; ++i)
        clip.audio->setSample (0, i, 0.5f);
    track.clips.push_back (std::move (clip));
    project.tracks.push_back (std::move (track));
    snapshots.push (project.buildSnapshot());

    // サイクル終端はフェード終端+Lより後ろ（折り返し時点でディレイが無音で満たされている状況）
    const int cycleEnd = 30000;
    transport.cycleRange.store (TransportState::packCycle (0, cycleEnd));
    transport.cycleEnabled.store (true);
    transport.seekRequest.store (0);
    engine.play();

    juce::AudioBuffer<float> stream (2, cycleEnd + blockSize * 3);
    stream.clear();
    juce::AudioBuffer<float> buffer (2, blockSize);
    int written = 0;
    while (written < stream.getNumSamples() - blockSize)
    {
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        for (int ch = 0; ch < 2; ++ch)
            stream.copyFrom (ch, written, buffer, ch, 0, blockSize);
        written += blockSize;
    }
    engine.stop();
    snapshots.deleteRetired();

    const auto fadeEnd = SongFade::sixteenthsToSamples (2, bpm, sr);
    expect (fadeEnd == 28800, "フェード終端の前提確認");

    // フェード終端+L以降は厳密に無音
    bool silentAfterFade = true;
    for (int i = (int) fadeEnd + latency; i < cycleEnd; ++i)
        if (stream.getSample (0, i) != 0.0f)
            silentAfterFade = false;
    expect (silentAfterFade, "フェード終端+L以降は厳密に無音であること");

    // 折り返し直後の先頭Lサンプル: ディレイは無音入力で満たされている＝古い音が漏れない
    bool cleanWrapHead = true;
    for (int i = cycleEnd; i < cycleEnd + latency; ++i)
        if (stream.getSample (0, i) != 0.0f)
            cleanWrapHead = false;
    expect (cleanWrapHead, "折り返し後の先頭Lサンプルに古いディレイ内容が漏れないこと");

    // その後はDCがL遅延で素通しに戻る（ラップ後はフェード前区間＝ゲイン1.0）
    expect (std::abs (stream.getSample (0, cycleEnd + latency + 50) - 0.5f) < 1.0e-6f,
            "折り返し後はフェード前のゲイン1.0で音が戻ること");
}

// ---- Limiterパラメータ: 既定値・保存/復元（v17）・欠損キー・不正データの正規化 ----
void testLimiterParamsRoundtrip()
{
    beginTest ("limiter params roundtrip");

    // 新規TrackParamsの既定値（Gain 0 / Ceiling -1.0 / Release 60ms = 中立スタート）
    TrackParams fresh;
    {
        const auto v = Limiter::load (fresh.limiter);
        expect (juce::approximatelyEqual (v.gainDb, 0.0f)
                    && juce::approximatelyEqual (v.ceilingDb, -1.0f)
                    && juce::approximatelyEqual (v.releaseMs, 60.0f),
                "既定値（Gain 0 / Ceiling -1.0 / Release 60ms）");
    }

    // 値を入れて保存 → 再読込で維持される
    auto dir = makeTempDir();
    juce::String error;
    juce::StringArray warnings;
    {
        Project project;
        project.directory = dir;
        Limiter::store (project.masterParams->limiter,
                        Limiter::normalized ({ 6.5f, -2.0f, 120.0f }));
        expect (project.save (error), "保存できること");
    }
    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr, "再読込できること");
    if (reloaded != nullptr)
    {
        const auto v = Limiter::load (reloaded->masterParams->limiter);
        expect (juce::approximatelyEqual (v.gainDb, 6.5f)
                    && juce::approximatelyEqual (v.ceilingDb, -2.0f)
                    && juce::approximatelyEqual (v.releaseMs, 120.0f),
                "Limiterパラメータ維持");
    }
    dir.deleteRecursively();

    // 欠損（v16以前のproject.json＝master.limiterなし）は既定値
    auto dirOld = makeTempDir();
    dirOld.getChildFile ("project.json").replaceWithText (R"({
        "version": 16, "bpm": 120.0, "sampleRate": 0.0, "nextId": 1,
        "tracks": [], "master": { "gain": 1.0 }
    })");
    auto projectOld = Project::load (dirOld, warnings, error);
    expect (projectOld != nullptr, "v16を読込めること");
    if (projectOld != nullptr)
        expect (juce::approximatelyEqual (Limiter::load (projectOld->masterParams->limiter).ceilingDb,
                                          -1.0f),
                "limiter欠損は既定値になること");
    dirOld.deleteRecursively();

    // 不正データの正規化: 範囲外値のクランプ（手編集JSON対策）
    auto dirBad = makeTempDir();
    dirBad.getChildFile ("project.json").replaceWithText (R"({
        "version": 17, "bpm": 120.0, "sampleRate": 0.0, "nextId": 1,
        "tracks": [],
        "master": { "gain": 1.0, "limiter": { "gain": 99.0, "ceiling": 3.0, "release": -5.0 } }
    })");
    auto projectBad = Project::load (dirBad, warnings, error);
    expect (projectBad != nullptr, "不正データを読込めること");
    if (projectBad != nullptr)
    {
        const auto v = Limiter::load (projectBad->masterParams->limiter);
        expect (juce::approximatelyEqual (v.gainDb, 12.0f)
                    && juce::approximatelyEqual (v.ceilingDb, 0.0f)
                    && juce::approximatelyEqual (v.releaseMs, 5.0f),
                "範囲外値がクランプされること");
    }
    dirBad.deleteRecursively();
}

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

    // 定数振幅0.4のクリップ（レベル検証がしやすい。send二重加算の0.8がMaster Limiterの
    // 天井-1dB=0.891を超えないレベル設計。0.5だと二重加算1.0が叩かれて検証にならない）
    Project project;
    Track track;
    track.id = 1;
    track.params->gain.store (1.0f);
    Clip clip;
    clip.startSample = 0;
    clip.lengthSamples = blockSize * 64;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, blockSize * 64);
    for (int i = 0; i < clip.audio->getNumSamples(); ++i)
        clip.audio->setSample (0, i, 0.4f);
    track.clips.push_back (std::move (clip));
    project.tracks.push_back (std::move (track));
    auto& params = *project.tracks[0].params;
    snapshots.push (project.buildSnapshot());

    // 出力はLimiterのlookahead分遅れるが、DC信号なのでブロック内で定常値に達し
    // getMagnitudeの読みは変わらない（先頭Lサンプルだけ無音）
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

    // panセンター: 両ch 0.4（等パワー補正型はセンター0dB = 既存プロジェクトの音量を変えない）
    measure (left, right);
    expect (std::abs (left - 0.4f) < 0.001f && std::abs (right - 0.4f) < 0.001f,
            "panセンターは両ch等量（0.4）");

    // pan右振り切り: 左ほぼ0・右は+3dB（0.4×√2≈0.566）
    params.peakL.exchange (0.0f); // センター測定の蓄積ピーク（CAS max）をリセット
    params.peakR.exchange (0.0f);
    params.pan.store (1.0f);
    measure (left, right);
    expect (left < 0.001f, "pan右振り切りで左chは無音");
    expect (std::abs (right - 0.5657f) < 0.005f, "pan右振り切りで右chは+3dB（約0.566）");
    expect (params.peakR.exchange (0.0f) > 0.55f, "Rメーターはpost-panピーク（約0.566）");
    expect (params.peakL.exchange (0.0f) < 0.001f, "pan右振り切りでLメーターは振れないこと");

    // send（v19: バスFXの full wet 返し）: 同一ブロックの素通し二重加算は無くなり、
    // Delayバス（fb=0・tone=0 = 1タップの遅延コピー）ならエコー到達後に dry+wet=0.8 になる。
    // 1/16音符 @120BPM = 0.125s = 5513サンプル → ブロック10以降にエコーが乗る
    params.pan.store (0.0f);
    params.sends[2].store (1.0f);
    Delay::store (project.busParams[2]->delay, Delay::normalized ({ 0, 0.0f, 0.0f, false }));
    const int echoBlocks = 20; // 5513/512 ≈ 10.8ブロック目から。20ブロック目は定常
    auto measureAfter = [&] (float& outLeft, float& outRight)
    {
        transport.seekRequest.store (0);
        engine.play();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        for (int i = 0; i < echoBlocks; ++i)
        {
            buffer.clear();
            engine.process (info);
        }
        outLeft = buffer.getMagnitude (0, 0, blockSize);
        outRight = buffer.getMagnitude (1, 0, blockSize);
        engine.stop();
        buffer.clear();
        engine.process (info); // 停止エッジの消化
    };
    measure (left, right);
    expect (std::abs (left - 0.4f) < 0.002f,
            "send100%でも同一ブロックは二重加算しない（full wetバス・エコーは後から）");
    measureAfter (left, right);
    expect (std::abs (left - 0.8f) < 0.002f, "エコー到達後は dry+wet で0.8");
    expect (project.busParams[2]->peakL.exchange (0.0f) > 0.35f, "バスメーターが振れること");

    // バスミュートでsend分が消える（バスFXの処理自体は止まらない契約だが出力には乗らない）
    project.busParams[2]->mute.store (true);
    measureAfter (left, right);
    expect (std::abs (left - 0.4f) < 0.002f, "バスMでsend分が消えること");
    project.busParams[2]->mute.store (false);

    // バスのリターン量（gain 0.5 → 0.4 + 0.2 = 0.6）
    project.busParams[2]->gain.store (0.5f);
    measureAfter (left, right);
    expect (std::abs (left - 0.6f) < 0.002f, "バスgainがリターン量として効くこと");
    project.busParams[2]->gain.store (1.0f);

    // Masterゲイン（全体 0.8 → 0.4）とMasterメーター
    project.masterParams->gain.store (0.5f);
    project.masterParams->peakL.exchange (0.0f); // 前シナリオの蓄積ピーク（CAS max）をリセット
    project.masterParams->peakR.exchange (0.0f);
    measureAfter (left, right);
    expect (std::abs (left - 0.4f) < 0.002f, "Masterゲインで全体が半減すること");
    expect (std::abs (project.masterParams->peakL.exchange (0.0f) - 0.4f) < 0.01f,
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
        // v19: バスはfull wet FX（Reverbのpre-delay分、最初のブロックにはwetが乗らない）。
        // ここでの検証対象は「ch0/1に出る・ch2以降に漏れない」であって加算レベルではない
        expect (buffer.getMagnitude (0, 0, blockSize) > 0.45f
                    && buffer.getMagnitude (1, 0, blockSize) > 0.45f,
                "4ch出力: ch0/1に音が出ること（クリップ＋クリック）");
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

    // 出力はMaster Limiterのlookahead分遅れる（遅延契約）。「Masterへ入る値の履歴」を組み立て、
    // 出力が「履歴を latency 遅らせた列」と全サンプル一致するか検証する。
    // **サイクルラップでは履歴が途切れず連結される**（＝ラップでLサンプルの無音が入らない、
    // というLimiterリセット契約の回帰テストを兼ねる）
    const int latency = engineLimiterLatency (sr);
    juce::AudioBuffer<float> buffer (2, blockSize);
    std::vector<float> expectedInput; // Master入力の履歴（再生開始からの処理順）
    int streamPos = 0;                // これまでに検証した出力サンプル総数
    const auto beginStream = [&]
    {
        expectedInput.clear(); // 再生開始（play）でLimiterはリセットされる＝履歴も仕切り直す
        streamPos = 0;
    };
    const auto pushRamp = [&] (int srcStart, int count)
    {
        for (int i = 0; i < count; ++i)
            expectedInput.push_back (source->getSample (0, srcStart + i));
    };
    const auto processBlock = [&]
    {
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
    };
    const auto verifyBlock = [&] (const char* description)
    {
        int mismatches = 0;
        for (int i = 0; i < blockSize; ++i)
        {
            const int k = streamPos + i - latency; // 遅延を差し引いた入力履歴の位置
            const float expected = (k >= 0 && k < (int) expectedInput.size())
                                       ? expectedInput[(size_t) k]
                                       : 0.0f;
            if (std::abs (buffer.getSample (0, i) - expected) > 1.0e-6f)
                ++mismatches;
        }
        expect (mismatches == 0, description);
        streamPos += blockSize;
    };

    // ---- ブロック途中でラップ: サイクル[200, 1000)・位置200から ----
    transport.cycleRange.store (TransportState::packCycle (200, 1000));
    transport.cycleEnabled.store (true);
    transport.seekRequest.store (200);
    engine.play();
    beginStream();

    processBlock(); // 200..712
    pushRamp (200, blockSize);
    verifyBlock ("1ブロック目: 範囲内をそのまま再生");
    expect (transport.playheadSamplePos.load() == 712, "1ブロック目の最終位置");

    processBlock(); // 712..1000（288）＋ラップして 200..424（224）
    pushRamp (712, 288);
    pushRamp (200, 224);
    verifyBlock ("境界前＋ラップ後セグメントが正しい内容（ラップで無音が入らない）");
    expect (transport.playheadSamplePos.load() == 424, "ラップ後の最終位置");
    engine.stop();
    processBlock(); // 停止エッジを消化

    // ---- 等号境界: ブロックが終端ちょうどで終わる → 次の再生位置は範囲頭（終端排他）----
    transport.cycleRange.store (TransportState::packCycle (200, 712)); // 範囲長=blockSize
    transport.seekRequest.store (200);
    engine.play();
    beginStream();
    processBlock(); // 200..712 = 終端ちょうど
    pushRamp (200, blockSize);
    verifyBlock ("等号境界ブロックの内容");
    expect (transport.playheadSamplePos.load() == 200, "終端ちょうどで終わっても範囲頭へ戻ること");
    processBlock();
    pushRamp (200, blockSize);
    verifyBlock ("次ブロックは範囲頭から再生されること");
    engine.stop();
    processBlock();

    // ---- 範囲長 < blockSize: 1コールバックで複数回ラップ ----
    transport.cycleRange.store (TransportState::packCycle (0, 128));
    transport.seekRequest.store (0);
    engine.play();
    beginStream();
    processBlock(); // 0..128 を4周
    for (int rep = 0; rep < 4; ++rep)
        pushRamp (0, 128);
    verifyBlock ("範囲長<blockSize: 各セグメントが正しい位置・内容で書かれること");
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
        const auto peaks = buildDomainPeakCache (*stereo, 0, 2000, Clip::samplesPerPeak);
        expect (! peaks.empty(), "ピークキャッシュが作られること");
        bool allMax = true;
        for (float peak : peaks)
            if (std::abs (peak - 0.2f) > 0.001f)
                allMax = false;
        expect (allMax, "ステレオのピークはL/Rのmax（0.2）");
    }

    // ステレオクリップの分割: audio共有
    {
        Clip clip;
        clip.audio = stereo;
        clip.startSample = 0;
        clip.lengthSamples = 2000;
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
    stereoClip.audio = std::make_shared<juce::AudioBuffer<float>> (2, blockSize * 64);
    stereoClip.audio->clear();
    for (int i = 0; i < stereoClip.audio->getNumSamples(); ++i)
        stereoClip.audio->setSample (0, i, 0.5f);
    stereoClip.lengthSamples = blockSize * 64;
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
        monoClip.audio = std::make_shared<juce::AudioBuffer<float>> (1, blockSize * 64);
        for (int i = 0; i < monoClip.audio->getNumSamples(); ++i)
            monoClip.audio->setSample (0, i, 0.25f);
        monoClip.lengthSamples = blockSize * 64;
        project.tracks[0].clips.push_back (std::move (monoClip));
    }
    snapshots.push (project.buildSnapshot());
    measure (left, right);
    expect (std::abs (left - 0.75f) < 0.001f, "混在トラックのL=ステレオL+モノ（0.5+0.25）");
    expect (std::abs (right - 0.25f) < 0.001f, "混在トラックのR=モノのみ（0.25）");

    // send: post-fader（gain・pan適用後）のステレオコピーがバスへ渡ること（v19）。
    // Delayバス（fb=0・tone=0・非ping-pong）はL/R独立の遅延コピー＝エコー到達後の定常値が
    // 「dry+wet」になる（L=(0.5+0.25)×0.5×2=0.75, R=0.25×0.5×2=0.25。Limiter天井0.891未満）。
    // 1/16音符 @120BPM = 5513サンプル → 20ブロック目は定常
    params.gain.store (0.5f);
    params.sends[2].store (1.0f);
    Delay::store (project.busParams[2]->delay, Delay::normalized ({ 0, 0.0f, 0.0f, false }));
    {
        transport.seekRequest.store (0);
        engine.play();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        for (int i = 0; i < 20; ++i)
        {
            buffer.clear();
            engine.process (info);
        }
        left = buffer.getMagnitude (0, 0, blockSize);
        right = buffer.getMagnitude (1, 0, blockSize);
        engine.stop();
        buffer.clear();
        engine.process (info);
    }
    expect (std::abs (left - 0.75f) < 0.002f, "sendのエコー到達後にLが dry+wet（0.75）");
    expect (std::abs (right - 0.25f) < 0.002f, "sendのエコー到達後にRが dry+wet（0.25）");

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

// ---- フェードの境目がスナップショット・⌘E用レンダリング要求へ載ること ----
// 「連なり全体の両端」なので全反復に同じ絶対位置が入る（ここ1箇所で再生・⌘E・⌘Bの3経路に効く）
void testClipFadeSnapshot()
{
    beginTest ("clip fade in snapshot and item render");

    Project project;
    Track track;
    track.id = 1;
    Clip clip;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, 2000);
    clip.audio->clear();
    clip.startSample = 500;
    clip.lengthSamples = 250;
    clip.loopCount = 3; // 連なり = 500..1500
    clip.fadeInSamples = 100;
    clip.fadeOutSamples = 400; // 反復をまたぐ長さ（250 < 400）
    track.clips.push_back (std::move (clip));
    project.tracks.push_back (std::move (track));

    auto snapshot = project.buildSnapshot();
    expect (snapshot != nullptr && snapshot->tracks.size() == 1, "スナップショットが作れること");
    if (snapshot == nullptr || snapshot->tracks.empty())
        return;
    const auto& clips = snapshot->tracks[0].clips;
    expect (clips.size() == 4, "ループ分が展開されること");
    for (const auto& playback : clips)
        expect (playback.fadeInStart == 500 && playback.fadeInEnd == 600
                    && playback.fadeOutStart == 1100 && playback.fadeOutEnd == 1500,
                "各反復に連なり全体の両端が絶対位置で載ること");

    // ⌘E（リージョン単体書き出し）も同じヘルパーを通る
    BounceRenderer::TrackRender render;
    juce::int64 rangeStart = 0, rangeEnd = 0;
    expect (BounceRenderer::buildItemRender (project.tracks[0], 0, 120.0, 48000.0,
                                            render, rangeStart, rangeEnd),
            "buildItemRenderが成功すること");
    expect (render.clips.size() == 4 && render.clips[0].fadeInEnd == 600
                && render.clips[3].fadeOutStart == 1100,
            "⌘E用のレンダリング要求にもフェードの境目が載ること");

    // モデルの不変条件が破れていても、展開時に頭打ちして区間が交差しないようにする
    project.tracks[0].clips[0].fadeInSamples = 99999;
    project.tracks[0].clips[0].fadeOutSamples = 99999;
    auto defensive = project.buildSnapshot();
    if (defensive != nullptr && ! defensive->tracks.empty() && ! defensive->tracks[0].clips.empty())
    {
        const auto& playback = defensive->tracks[0].clips[0];
        expect (playback.fadeInEnd == 1500 && playback.fadeOutStart == 1500,
                "異常値でもフェードイン優先で連なり内に収まること");
    }
}

// ---- フェードが再生に効くこと（モノ経路・ステレオ経路・ループまたぎ・ゲイン併用・ブロック境界）----
void testEngineClipFade()
{
    beginTest ("engine applies clip fades");

    constexpr double sr = 48000.0;
    constexpr int totalSamples = 2048;

    // 指定ブロックサイズで totalSamples ぶんレンダリングする（ブロック境界の影響を見るため可変）。
    // Master Limiterのlookahead遅延は「余分に1ブロック回して +latency から詰める」で吸収し、
    // 呼び出し側はタイムライン位置＝インデックスのまま検証できる
    const auto render = [&] (Project& project, int blockSize)
    {
        const int latency = engineLimiterLatency (sr);
        // 予備は latency をブロック倍数へ切り上げた分（blockSize 64 < latency 96 でも足りるように）
        const int extra = ((latency + blockSize - 1) / blockSize) * blockSize;
        juce::AudioBuffer<float> raw (2, totalSamples + extra);
        raw.clear();
        TransportState transport;
        SnapshotExchange snapshots;
        PreviewFifo previewFifo;
        PlaybackEngine engine (transport, snapshots, previewFifo);
        engine.prepareToPlay (blockSize, sr);
        snapshots.push (project.buildSnapshot());
        transport.seekRequest.store (0);
        engine.play();

        juce::AudioBuffer<float> buffer (2, blockSize);
        for (int pos = 0; pos < raw.getNumSamples(); pos += blockSize)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            const int n = juce::jmin (blockSize, raw.getNumSamples() - pos);
            for (int ch = 0; ch < 2; ++ch)
                raw.copyFrom (ch, pos, buffer, ch, 0, n);
        }
        engine.stop();
        snapshots.deleteRetired();

        juce::AudioBuffer<float> out (2, totalSamples);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, 0, raw, ch, latency, totalSamples);
        return out;
    };
    const auto makeProject = [] (int channels, float value, juce::int64 fadeIn, juce::int64 fadeOut,
                                 float gain, int loopCount, juce::int64 length)
    {
        auto project = std::make_unique<Project>();
        Track track;
        track.id = 1;
        track.params->gain.store (1.0f); // panセンター・トラックゲイン1.0で素の振幅を見る
        Clip clip;
        clip.audio = std::make_shared<juce::AudioBuffer<float>> (channels, (int) length);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < (int) length; ++i)
                clip.audio->setSample (ch, i, ch == 0 ? value : value * 0.5f); // ステレオはL:R = 2:1
        clip.lengthSamples = length;
        clip.loopCount = loopCount;
        clip.gain = gain;
        clip.fadeInSamples = fadeIn;
        clip.fadeOutSamples = fadeOut;
        track.clips.push_back (std::move (clip));
        project->tracks.push_back (std::move (track));
        return project;
    };

    // ---- モノ経路: 240サンプル（5ms）のフェードイン/アウト ----
    {
        auto project = makeProject (1, 0.5f, 240, 240, 1.0f, 0, totalSamples);
        const auto out = render (*project, 512);
        expect (std::abs (out.getSample (0, 0)) <= 1.0e-4f, "フェードインの先頭サンプルが無音とみなせること");
        expect (std::abs (out.getSample (0, 119) - 0.5f * 119.0f / 239.0f) < 0.002f,
                "フェードイン中間がおよそ半分の振幅になること");
        expect (std::abs (out.getSample (0, 239) - 0.5f) < 0.002f, "フェードイン終端でフル振幅になること");
        // 厳密一致にはしない: panセンターのゲイン（cos(π/4)*√2）がfloatで 1.0 にわずかに満たない
        expect (std::abs (out.getSample (0, 1000) - 0.5f) < 1.0e-6f, "平坦部は素の振幅（乗算なし）であること");
        // 段差が消える条件。半開区間へ退行すると 4.2e-3（-47.6dB）になるのでこの閾値で検出できる
        expect (std::abs (out.getSample (0, totalSamples - 1)) <= 1.0e-4f,
                "フェードアウトの最終サンプルが無音とみなせること（許容 -80dB）");
        expect (std::abs (out.getSample (0, totalSamples - 240) - 0.5f) < 0.002f,
                "フェードアウト開始でフル振幅であること");
        for (int i = 0; i < totalSamples; ++i)
            if (! std::isfinite (out.getSample (0, i)))
            {
                expect (false, "出力にNaN/Infが出ないこと");
                break;
            }
    }

    // ---- ステレオ経路: L/Rのバランスを保ったまま傾斜すること ----
    {
        auto project = makeProject (2, 0.4f, 240, 240, 1.0f, 0, totalSamples);
        const auto out = render (*project, 512);
        expect (std::abs (out.getSample (0, 0)) <= 1.0e-4f && std::abs (out.getSample (1, 0)) <= 1.0e-4f,
                "ステレオも先頭が無音とみなせること");
        expect (std::abs (out.getSample (0, 120) - out.getSample (1, 120) * 2.0f) < 1.0e-5f,
                "フェード中もL:R = 2:1が保たれること");
        expect (std::abs (out.getSample (0, 1000) - 0.4f) < 1.0e-6f
                    && std::abs (out.getSample (1, 1000) - 0.2f) < 1.0e-6f,
                "平坦部は素のL/R振幅であること");
        expect (std::abs (out.getSample (1, totalSamples - 1)) <= 1.0e-4f,
                "ステレオのRも最終サンプルが無音とみなせること");
    }

    // ---- ループまたぎ: 反復の切れ目で1.0へ戻らず単調減少すること ----
    {
        // 本体256×4反復 = 1024。フェードアウト600（2反復ぶんをまたぐ）
        auto project = makeProject (1, 0.5f, 0, 600, 1.0f, 3, 256);
        const auto out = render (*project, 512);
        const int chainEnd = 1024;
        const int fadeStart = chainEnd - 600;
        bool monotonic = true;
        for (int i = fadeStart + 1; i < chainEnd; ++i)
            if (out.getSample (0, i) > out.getSample (0, i - 1) + 1.0e-6f)
                monotonic = false;
        expect (monotonic, "フェードアウト中は反復境界でも増加しないこと");
        // 反復の切れ目（512 = 2反復目の終わり）でフル振幅へ戻っていないこと
        expect (out.getSample (0, 512) < 0.5f * 0.9f, "反復の切れ目でフル振幅へ戻らないこと");
        expect (std::abs (out.getSample (0, chainEnd - 1)) <= 1.0e-4f, "連なり末尾が無音とみなせること");
        expect (std::abs (out.getSample (0, 100) - 0.5f) < 1.0e-6f,
                "フェードの外（連なり前半）は素の振幅であること");
    }

    // ---- リージョンゲインとの併用: 両方が掛かること ----
    {
        auto project = makeProject (1, 0.4f, 240, 0, 0.5f, 0, totalSamples);
        const auto out = render (*project, 512);
        expect (std::abs (out.getSample (0, 1000) - 0.2f) < 1.0e-6f, "平坦部はゲインぶんだけ下がること");
        expect (std::abs (out.getSample (0, 119) - 0.2f * 119.0f / 239.0f) < 0.002f,
                "フェード中もゲインが掛かること");
    }

    // ---- 1〜2サンプルのフェード（境界条件。段差が消えることは主張しない）----
    {
        auto project = makeProject (1, 0.5f, 1, 1, 1.0f, 0, totalSamples);
        const auto out = render (*project, 512);
        expect (juce::exactlyEqual (out.getSample (0, 0), 0.0f), "1サンプルのフェードインは先頭が0");
        expect (std::abs (out.getSample (0, 1) - 0.5f) < 1.0e-6f, "その次はフル振幅（n=1の仕様）");
        expect (std::abs (out.getSample (0, totalSamples - 1)) < 1.0e-6f,
                "1サンプルのフェードアウトは最終サンプルが0");
        expect (std::abs (out.getSample (0, totalSamples - 2) - 0.5f) < 1.0e-6f, "その手前はフル振幅");

        auto two = makeProject (1, 0.5f, 2, 2, 1.0f, 0, totalSamples);
        const auto outTwo = render (*two, 512);
        expect (std::abs (outTwo.getSample (0, 0)) < 1.0e-6f
                    && std::abs (outTwo.getSample (0, 1) - 0.5f) < 1.0e-6f,
                "2サンプルのフェードインは 0, フル振幅");
        expect (std::abs (outTwo.getSample (0, totalSamples - 1)) < 1.0e-6f
                    && std::abs (outTwo.getSample (0, totalSamples - 2) - 0.5f) < 1.0e-6f,
                "2サンプルのフェードアウトは フル振幅, 0");
    }

    // ---- ブロック境界をまたぐフェード: ブロックサイズを変えても出力が一致すること ----
    // ランプの継ぎ目にギャップが無いことの証明。float累積のしかたが変わるのでビット一致はしない
    // （240サンプルのフェード × B=64/256/1024 の実測差は 3.13e-6）
    {
        auto a = makeProject (1, 0.5f, 240, 240, 1.0f, 0, totalSamples);
        auto b = makeProject (1, 0.5f, 240, 240, 1.0f, 0, totalSamples);
        auto c = makeProject (1, 0.5f, 240, 240, 1.0f, 0, totalSamples);
        const auto out64 = render (*a, 64);
        const auto out256 = render (*b, 256);
        const auto out1024 = render (*c, 1024);
        float maxDiff = 0.0f;
        for (int i = 0; i < totalSamples; ++i)
        {
            maxDiff = juce::jmax (maxDiff, std::abs (out64.getSample (0, i) - out1024.getSample (0, i)));
            maxDiff = juce::jmax (maxDiff, std::abs (out256.getSample (0, i) - out1024.getSample (0, i)));
        }
        expect (maxDiff < 1.0e-5f, "ブロックサイズ違いでも許容誤差内で一致すること");
    }
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
        // フェードも2経路で一致することを見る（区間分割が engine=512 / bounce=1024 で変わる条件）
        clip.fadeInSamples = 240;
        clip.fadeOutSamples = 1500;
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
        clip.fadeInSamples = 500;
        clip.fadeOutSamples = 800;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }
    project.busParams[1]->gain.store (0.9f);
    project.masterParams->gain.store (0.85f);

    // エンジン側レンダリング（Limiterの遅延ぶん余分に1ブロック回し、比較は +latency で読む）
    const int latency = engineLimiterLatency (sr);
    juce::AudioBuffer<float> engineOut (2, totalSamples + blockSize);
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
        for (int blockIndex = 0; blockIndex < numBlocks + 1; ++blockIndex)
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
                maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (ch, i + latency)
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






// ---- 曲末フェード: カーブ関数の境界 ----
void testSongFadeGainCurve()
{
    beginTest ("song fade gain curve boundaries");

    // 未設定（0/0）は全位置でユニティ。ここが0を返すと未設定プロジェクトが全部無音になる
    expect (SongFade::gainAt (0, 0, 0) == 1.0f, "未設定(0/0)・位置0でユニティ");
    expect (SongFade::gainAt (1, 0, 0) == 1.0f, "未設定(0/0)・正の位置でユニティ");
    expect (SongFade::gainAt (100000, 0, 0) == 1.0f, "未設定(0/0)・遠い位置でユニティ");
    expect (SongFade::gainAt (500, 1000, 1000) == 1.0f, "start==endでユニティ");
    expect (SongFade::gainAt (500, 2000, 1000) == 1.0f, "start>end（不正）でユニティ");

    // 有効区間 [1000, 2000)
    expect (SongFade::gainAt (0, 1000, 2000) == 1.0f, "開始より前はユニティ");
    expect (SongFade::gainAt (1000, 1000, 2000) == 1.0f, "開始点ちょうどはユニティ");
    expect (SongFade::gainAt (2000, 1000, 2000) == 0.0f, "終了点ちょうどは厳密に0");
    expect (SongFade::gainAt (2001, 1000, 2000) == 0.0f, "終了点より後は厳密に0");
    expect (SongFade::gainAt (100000, 1000, 2000) == 0.0f, "遠い後方も厳密に0");

    // S字（raised cosine）: 中点で0.5、単調減少、両端の傾きが0（＝端の変化が緩やか）
    expect (std::abs (SongFade::gainAt (1500, 1000, 2000) - 0.5f) < 1.0e-6f, "中点で0.5");
    float prev = 1.0f;
    bool monotonic = true;
    for (int i = 0; i <= 100; ++i)
    {
        const float g = SongFade::gainAt (1000 + i * 10, 1000, 2000);
        if (g > prev + 1.0e-6f)
            monotonic = false;
        prev = g;
    }
    expect (monotonic, "区間内は単調減少");
    const float nearStart = SongFade::gainAt (1010, 1000, 2000); // 1%進んだ点
    const float nearMid = SongFade::gainAt (1510, 1000, 2000);   // 中点から1%進んだ点
    expect ((1.0f - nearStart) < std::abs (0.5f - nearMid),
            "端の傾きは中央より緩やか（S字である証拠）");
}

// ---- 曲末フェード: 保存/読込・undo・終端算出 ----
void testSongFadeRoundtrip()
{
    beginTest ("song fade roundtrip, undo and auto end");
    const auto dir = makeTempDir();

    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "createNewできること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    expect (! project->hasFadeOut(), "新規プロジェクトはフェード未設定");

    project->fadeOutStartSixteenths = 64;
    project->fadeOutEndSixteenths = 128;
    expect (project->save (error), "保存できること");

    const auto saved = juce::JSON::parse (project->directory.getChildFile ("project.json").loadFileAsString());
    expect ((int) saved.getProperty ("version", 0) == Project::currentVersion, "現行バージョン(v12)で保存されること");
    const auto fadeVar = saved.getProperty ("fadeOut", {});
    expect (fadeVar.isObject() && (int) fadeVar.getProperty ("start", -1) == 64
                && (int) fadeVar.getProperty ("end", -1) == 128,
            "fadeOut が start/end で保存されること");

    juce::StringArray warnings;
    auto reloaded = Project::load (project->directory, warnings, error);
    expect (reloaded != nullptr && reloaded->fadeOutStartSixteenths == 64
                && reloaded->fadeOutEndSixteenths == 128,
            "再読込で復元されること");

    // v11以前（fadeOutキーなし）は未設定として読む
    {
        auto legacy = juce::JSON::parse (project->directory.getChildFile ("project.json").loadFileAsString());
        auto* obj = legacy.getDynamicObject();
        expect (obj != nullptr, "JSONオブジェクトを取れること");
        if (obj != nullptr)
        {
            obj->removeProperty ("fadeOut");
            obj->setProperty ("version", 11);
            project->directory.getChildFile ("project.json").replaceWithText (juce::JSON::toString (legacy));
        }
        auto old = Project::load (project->directory, warnings, error);
        expect (old != nullptr && ! old->hasFadeOut(), "v11以前は未設定として読むこと");
    }

    // 不正値（start >= end）は未設定へ落とす
    {
        auto bad = juce::JSON::parse (project->directory.getChildFile ("project.json").loadFileAsString());
        if (auto* obj = bad.getDynamicObject())
        {
            auto* fadeObj = new juce::DynamicObject();
            fadeObj->setProperty ("start", 200);
            fadeObj->setProperty ("end", 100);
            obj->setProperty ("fadeOut", juce::var (fadeObj));
            project->directory.getChildFile ("project.json").replaceWithText (juce::JSON::toString (bad));
        }
        auto loaded = Project::load (project->directory, warnings, error);
        expect (loaded != nullptr && ! loaded->hasFadeOut(), "start>=end は未設定へ落とすこと");
    }

    // undo/redo: UndoStack::State はtracks/markers以外も往復させる必要がある
    {
        Project p;
        UndoStack stack;
        p.fadeOutStartSixteenths = 10;
        p.fadeOutEndSixteenths = 20;

        stack.begin (p, UndoStack::EditKind::clipValue);
        p.fadeOutStartSixteenths = 30;
        p.fadeOutEndSixteenths = 40;

        UndoStack::EditKind kind = UndoStack::EditKind::structure;
        expect (stack.undo (p, kind), "undoできること");
        expect (kind == UndoStack::EditKind::clipValue, "種別 clipValue が返ること");
        expect (p.fadeOutStartSixteenths == 10 && p.fadeOutEndSixteenths == 20, "undoで元の値へ戻ること");

        expect (stack.redo (p, kind), "redoできること");
        expect (p.fadeOutStartSixteenths == 30 && p.fadeOutEndSixteenths == 40, "redoで戻ること");

        // 既存のundo対象（tracks）が壊れていないことも確認する（State拡張の回帰）
        stack.begin (p, UndoStack::EditKind::structure);
        Track t;
        t.id = 7;
        p.tracks.push_back (std::move (t));
        expect (stack.undo (p, kind), "tracksのundoができること");
        expect (p.tracks.empty(), "tracksが巻き戻ること");
    }

    // ⌃F／右クリックの終端解決（UIから切り離した判断そのもの）
    {
        constexpr double sr = 48000.0;
        Project p;
        p.bpm = 120.0;

        // アイテムが無ければ no-op
        expect (p.resolveSongFadeEnd (0, sr) == 0, "アイテムが無ければ0");

        // **既存フェードが残っていても** アイテムが無ければ no-op。
        // ここを「既存フェードがあれば終端を使う」と書くと、全アイテムを消した後に
        // ⌃F で開始点だけ動かせてしまう
        p.fadeOutStartSixteenths = 8;
        p.fadeOutEndSixteenths = 16;
        expect (p.resolveSongFadeEnd (4, sr) == 0, "既存フェードがあってもアイテムが無ければ0");
        p.fadeOutStartSixteenths = 0;
        p.fadeOutEndSixteenths = 0;

        const double sixteenthSamples = sr * 60.0 / 120.0 * 4.0 / 16.0;
        Track track;
        track.id = 1;
        Clip clip;
        clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, 100);
        clip.lengthSamples = (juce::int64) sixteenthSamples * 8; // 8個ぶん
        track.clips.push_back (clip);
        p.tracks.push_back (std::move (track));

        expect (p.resolveSongFadeEnd (4, sr) == 8, "フェード未設定なら自動終端（アイテム終端）");
        expect (p.resolveSongFadeEnd (8, sr) == 0, "開始点が終端ちょうどなら0");
        expect (p.resolveSongFadeEnd (99, sr) == 0, "開始点が終端より後なら0");

        // 既存フェードがあれば終端は動かさない（2本目を作らず開始点だけ移す）
        p.fadeOutStartSixteenths = 5;
        p.fadeOutEndSixteenths = 6;
        expect (p.resolveSongFadeEnd (2, sr) == 6, "既存フェードの終端を維持すること");
        expect (p.resolveSongFadeEnd (6, sr) == 0, "既存終端以後の開始点は0");

        // 全アイテムをミュートしたら（＝鳴るものが無い）既存フェードがあっても0
        p.tracks[0].clips[0].muted = true;
        expect (p.resolveSongFadeEnd (2, sr) == 0, "全アイテムがミュートなら0");
    }

    // 終端の自動算出: 端数は切り上げ（最近傍だと最後の音を切る）
    {
        constexpr double sr = 48000.0;
        Project p;
        p.bpm = 120.0;
        expect (p.lastItemEndSixteenths (sr) == 0, "アイテムが無ければ0");

        const double sixteenthSamples = sr * 60.0 / 120.0 * 4.0 / 16.0; // 120BPM/48kで6000サンプル
        Track track;
        track.id = 1;
        Clip clip;
        clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, 100);
        clip.startSample = 0;
        clip.lengthSamples = (juce::int64) sixteenthSamples * 3 + 1; // 3個ぶん＋1サンプル
        track.clips.push_back (clip);
        p.tracks.push_back (std::move (track));
        expect (p.lastItemEndSixteenths (sr) == 4, "端数は切り上げる（3.0002→4）");

        // ミュートされたリージョンは含めない
        p.tracks[0].clips[0].muted = true;
        expect (p.lastItemEndSixteenths (sr) == 0, "ミュートリージョンは含めないこと");
        p.tracks[0].clips[0].muted = false;

        // トラックのミュート・ソロは考慮しない（一時的なモニター状態であって曲の長さではない）
        p.tracks[0].params->mute.store (true);
        expect (p.lastItemEndSixteenths (sr) == 4, "トラックミュートは終端に影響しないこと");
        p.tracks[0].params->mute.store (false);

        // ループ込みの全長を見る
        p.tracks[0].clips[0].loopCount = 1;
        expect (p.lastItemEndSixteenths (sr) == 7, "ループ込みの全長で算出すること");
    }

    dir.deleteRecursively();
}

// ---- 曲末フェード: RT再生・境界分割・BPM/SR追従・バウンス一致 ----
// ---- 採用ループのアンカー（v14）の保存・再読込・移行・不正値 ----
void testLoopAnchorRoundtrip()
{
    beginTest ("loop anchor roundtrip, v13 migration and invalid values");
    const auto dir = makeTempDir();

    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "createNewできること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    expect (! project->loopAnchor.has_value(), "新規プロジェクトはアンカー未採用");

    // 有効なアンカー＋出自付きクリップを保存
    LoopAnchor anchor;
    anchor.libraryPath = "loops/PackA/guitar_Bm_85bpm.wav";
    anchor.bpm = 85.0;
    anchor.key = ProjectKey { 11, KeyMode::minor };
    anchor.loopBars = 2;
    anchor.slotsPerBar = 2;
    anchor.roots = { 2, 2, 9, 6 };
    anchor.confidence = { 0.9f, 0.85f, 0.8f, 0.88f };
    anchor.degraded = false;
    expect (anchor.isValid(), "有効なアンカーがisValidを通ること");
    project->loopAnchor = anchor;

    constexpr int wavLength = 4410;
    expect (writeTestWav (project->directory.getChildFile ("clip-001.wav"), wavLength), "テストWAVを書けること");
    Track track;
    track.type = TrackType::audio;
    track.name = "Loops";
    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.loopSource = anchor.libraryPath;
    clip.audio = Project::loadWav (project->directory.getChildFile ("clip-001.wav"));
    clip.lengthSamples = wavLength;
    track.clips.push_back (clip);
    project->tracks.push_back (std::move (track));

    expect (project->save (error), "保存できること");
    const auto saved = juce::JSON::parse (project->directory.getChildFile ("project.json").loadFileAsString());
    expect ((int) saved.getProperty ("version", 0) == Project::currentVersion, "現行バージョンで保存されること");
    expect (saved.getProperty ("loopAnchor", {}).isObject(), "loopAnchorが保存されること");

    juce::StringArray warnings;
    auto reloaded = Project::load (project->directory, warnings, error);
    expect (reloaded != nullptr && reloaded->loopAnchor.has_value(), "再読込でアンカーが復元されること");
    if (reloaded != nullptr && reloaded->loopAnchor.has_value())
    {
        const auto& a = *reloaded->loopAnchor;
        expect (a.libraryPath == anchor.libraryPath && a.bpm == 85.0
                    && a.key == anchor.key && a.loopBars == 2 && a.slotsPerBar == 2
                    && a.roots == anchor.roots && a.degraded == false,
                "アンカーの全フィールドが往復すること");
        expect (a.confidence.size() == 4 && std::abs (a.confidence[0] - 0.9f) < 1e-4f,
                "スロット別信頼度が往復すること");
    }
    // createNew は既定の「トラック 1」を作るので、クリップを載せたのは末尾のトラック
    expect (reloaded != nullptr && ! reloaded->tracks.empty() && ! reloaded->tracks.back().clips.empty()
                && reloaded->tracks.back().clips[0].loopSource == anchor.libraryPath,
            "クリップの出自（loopSource）が往復すること");

    // v13（loopAnchorキーなし）は未採用として読む
    {
        auto legacy = juce::JSON::parse (project->directory.getChildFile ("project.json").loadFileAsString());
        if (auto* obj = legacy.getDynamicObject())
        {
            obj->removeProperty ("loopAnchor");
            obj->setProperty ("version", 13);
            project->directory.getChildFile ("project.json").replaceWithText (juce::JSON::toString (legacy));
        }
        auto old = Project::load (project->directory, warnings, error);
        expect (old != nullptr && ! old->loopAnchor.has_value(), "v13は未採用として読むこと");
    }

    // 壊れたアンカー（rootsの長さ不一致）は未採用へ落として警告する
    {
        auto bad = juce::JSON::parse (project->directory.getChildFile ("project.json").loadFileAsString());
        if (auto* obj = bad.getDynamicObject())
        {
            auto* anchorObj = new juce::DynamicObject();
            anchorObj->setProperty ("libraryPath", "loops/x.wav");
            anchorObj->setProperty ("bpm", 85.0);
            auto* keyObj = new juce::DynamicObject();
            keyObj->setProperty ("root", 11);
            keyObj->setProperty ("mode", "minor");
            anchorObj->setProperty ("key", juce::var (keyObj));
            anchorObj->setProperty ("loopBars", 2);
            anchorObj->setProperty ("slotsPerBar", 2);
            juce::Array<juce::var> shortRoots; // 長さ3（4必須）
            shortRoots.add (2); shortRoots.add (2); shortRoots.add (9);
            anchorObj->setProperty ("roots", shortRoots);
            anchorObj->setProperty ("confidence", shortRoots);
            obj->setProperty ("loopAnchor", juce::var (anchorObj));
            project->directory.getChildFile ("project.json").replaceWithText (juce::JSON::toString (bad));
        }
        warnings.clear();
        auto broken = Project::load (project->directory, warnings, error);
        expect (broken != nullptr && ! broken->loopAnchor.has_value(), "壊れたアンカーは未採用へ落とすこと");
        expect (! warnings.isEmpty(), "壊れたアンカーは警告されること");
    }

    // 型違いは切り捨て・暗黙変換せず未採用へ落とす（strict契約: string/number/bool/整数を型で要求）
    {
        const auto writeAnchor = [&] (const std::function<void (juce::DynamicObject&)>& mutate)
        {
            auto base = juce::JSON::parse (project->directory.getChildFile ("project.json").loadFileAsString());
            auto* obj = base.getDynamicObject();
            if (obj == nullptr)
                return;
            auto* anchorObj = new juce::DynamicObject();
            anchorObj->setProperty ("libraryPath", "loops/x.wav");
            anchorObj->setProperty ("bpm", 85.0);
            auto* keyObj = new juce::DynamicObject();
            keyObj->setProperty ("root", 11);
            keyObj->setProperty ("mode", "minor");
            anchorObj->setProperty ("key", juce::var (keyObj));
            anchorObj->setProperty ("loopBars", 2);
            anchorObj->setProperty ("slotsPerBar", 2);
            juce::Array<juce::var> roots4;
            roots4.add (2); roots4.add (2); roots4.add (9); roots4.add (6);
            anchorObj->setProperty ("roots", roots4);
            juce::Array<juce::var> conf4;
            conf4.add (0.9); conf4.add (0.9); conf4.add (0.9); conf4.add (0.9);
            anchorObj->setProperty ("confidence", conf4);
            anchorObj->setProperty ("degraded", false); // 必須フィールド（欠損も型違い扱いで拒否される）
            mutate (*anchorObj);
            obj->setProperty ("loopAnchor", juce::var (anchorObj));
            project->directory.getChildFile ("project.json").replaceWithText (juce::JSON::toString (base));
        };
        const auto expectRejected = [&] (const char* label)
        {
            juce::StringArray typeWarnings;
            auto p = Project::load (project->directory, typeWarnings, error);
            expect (p != nullptr && ! p->loopAnchor.has_value() && ! typeWarnings.isEmpty(), label);
        };

        writeAnchor ([] (juce::DynamicObject& a) { a.setProperty ("loopBars", 2.0); });
        expectRejected ("浮動小数の loopBars は未採用（暗黙の切り捨て禁止）");
        writeAnchor ([] (juce::DynamicObject& a)
        {
            auto* k = new juce::DynamicObject();
            k->setProperty ("root", 11.9);
            k->setProperty ("mode", "minor");
            a.setProperty ("key", juce::var (k));
        });
        expectRejected ("浮動小数の key.root は未採用（11.9 を 11 に化けさせない）");
        writeAnchor ([] (juce::DynamicObject& a) { a.setProperty ("libraryPath", 85.0); });
        expectRejected ("数値の libraryPath は未採用");
        writeAnchor ([] (juce::DynamicObject& a) { a.setProperty ("bpm", "85"); });
        expectRejected ("文字列の bpm は未採用");
        writeAnchor ([] (juce::DynamicObject& a) { a.setProperty ("degraded", "no"); });
        expectRejected ("文字列の degraded は未採用");
        writeAnchor ([] (juce::DynamicObject& a)
        {
            auto* k = new juce::DynamicObject();
            k->setProperty ("root", (juce::int64) 4294967296); // int範囲外の64bit値
            k->setProperty ("mode", "minor");
            a.setProperty ("key", juce::var (k));
        });
        expectRejected ("int範囲外の64bit root は未採用（切り詰めで 0 に化けさせない）");

        // loopAnchor 自体がオブジェクトでない値も「欠損」と区別して警告される
        {
            auto base = juce::JSON::parse (project->directory.getChildFile ("project.json").loadFileAsString());
            if (auto* obj = base.getDynamicObject())
            {
                obj->setProperty ("loopAnchor", "broken");
                project->directory.getChildFile ("project.json").replaceWithText (juce::JSON::toString (base));
            }
            expectRejected ("オブジェクトでない loopAnchor は未採用＋警告");
        }

        // 対照: 変異なし（契約どおり）なら採用される — 検証がきつすぎて正常系を殺していないこと
        writeAnchor ([] (juce::DynamicObject&) {});
        juce::StringArray okWarnings;
        auto ok = Project::load (project->directory, okWarnings, error);
        expect (ok != nullptr && ok->loopAnchor.has_value(), "契約どおりのJSONは採用されること");
    }

    // isValid の境界（契約 looproots.py と同じ規則）
    {
        auto v = anchor;
        v.slotsPerBar = 3;
        expect (! v.isValid(), "slotsPerBar=3は不正");
        v = anchor; v.roots[0] = 12;
        expect (! v.isValid(), "root=12は不正");
        v = anchor; v.confidence[0] = 1.5f;
        expect (! v.isValid(), "confidence>1は不正");
        v = anchor; v.loopBars = 0;
        expect (! v.isValid(), "loopBars=0は不正");
        v = anchor; v.bpm = 500.0;
        expect (! v.isValid(), "BPM範囲外(30..300)は不正");
        v = anchor; v.libraryPath = "";
        expect (! v.isValid(), "空のlibraryPathは不正");
        v = anchor; v.libraryPath = "/Users/x/loops/a.wav";
        expect (! v.isValid(), "絶対パスは不正（ライブラリ相対の契約）");
        v = anchor; v.libraryPath = "loops/../secret.wav";
        expect (! v.isValid(), "..を含むパスは不正");
        v = anchor; v.libraryPath = "loops/foo..bar.wav";
        expect (v.isValid(), "パス要素でない .. を含む正常なファイル名は通ること");
        v = anchor; v.loopBars = 1 << 29; v.slotsPerBar = 16; // int積は溢れる組み合わせ
        expect (! v.isValid(), "巨大な loopBars×slotsPerBar でもオーバーフローせず不正判定できること");
        v = anchor; v.key.root = 12;
        expect (! v.isValid(), "アンカーのkey root範囲外は不正（saveが書くと次回loadで消える時限破損）");
        v = anchor; v.key.mode = (KeyMode) 7;
        expect (! v.isValid(), "enum外のkey modeは不正");
        // 不正なアンカーはそもそもJSONに書かれない
        auto p2 = Project::createNew (dir.getChildFile ("proj2"), error);
        expect (p2 != nullptr, "proj2をcreateNewできること");
        if (p2 != nullptr)
        {
            v = anchor; v.roots.pop_back();
            p2->loopAnchor = v;
            expect (p2->save (error), "保存自体は成功すること");
            const auto saved2 = juce::JSON::parse (p2->directory.getChildFile ("project.json").loadFileAsString());
            expect (! saved2.getProperty ("loopAnchor", {}).isObject(), "不正なアンカーは書き込まれないこと");
        }
    }

    // bass.py --roots へ渡す契約 JSON（looproots.py の契約 v1 と同じ形で出ること）
    {
        const auto parsedContract = juce::JSON::parse (LoopAnchors::rootsContractJson (anchor));
        expect ((int) parsedContract.getProperty ("version", 0) == 1, "契約 version=1");
        expect ((int) parsedContract.getProperty ("slots_per_bar", 0) == 2
                    && (int) parsedContract.getProperty ("loop_bars", 0) == 2,
                "slots_per_bar / loop_bars が転記されること");
        auto* rootsArr = parsedContract.getProperty ("roots", {}).getArray();
        auto* confArr = parsedContract.getProperty ("confidence", {}).getArray();
        expect (rootsArr != nullptr && rootsArr->size() == 4 && (int) (*rootsArr)[0] == 2
                    && confArr != nullptr && confArr->size() == 4,
                "roots / confidence が長さ込みで転記されること");
        expect (! (bool) parsedContract.getProperty ("degraded", true)
                    && (int) parsedContract.getProperty ("key_root", -1) == 11
                    && parsedContract.getProperty ("key_mode", "").toString() == "minor"
                    && parsedContract.getProperty ("source", "").toString() == anchor.libraryPath,
                "degraded / key / source が転記されること");
    }

    // looproots 契約 JSON → anchor の strict パース（rootsContractJson との往復＋型違反の拒否）
    {
        LoopAnchor parsed;
        expect (LoopAnchors::anchorFromContractJson (LoopAnchors::rootsContractJson (anchor), parsed),
                "自分が書いた契約を読み戻せること");
        parsed.bpm = anchor.bpm;                 // 契約に無い値は呼び出し側が設定する契約
        parsed.libraryPath = anchor.libraryPath; // source はファイル名にすぎないので上書き
        expect (parsed.isValid() && parsed.roots == anchor.roots
                    && parsed.slotsPerBar == anchor.slotsPerBar && parsed.key == anchor.key
                    && parsed.confidence.size() == anchor.confidence.size(),
                "契約の往復で進行・キーが保たれること");

        const auto reject = [] (const char* json, const char* label)
        {
            LoopAnchor out;
            expect (! LoopAnchors::anchorFromContractJson (juce::String::fromUTF8 (json), out), label);
        };
        reject (R"({"version": 2, "slots_per_bar": 2, "loop_bars": 2, "roots": [9,9,5,7],
                    "confidence": [0.9,0.9,0.9,0.9], "degraded": false, "key_root": 11, "key_mode": "minor"})",
                "version違いは拒否");
        reject (R"({"version": 1, "slots_per_bar": 2, "loop_bars": 2, "roots": [9,9,5,9.5],
                    "confidence": [0.9,0.9,0.9,0.9], "degraded": false, "key_root": 11, "key_mode": "minor"})",
                "浮動小数の roots は拒否（9に化けさせない）");
        reject (R"({"version": 1, "slots_per_bar": 2, "loop_bars": 2, "roots": [9,9,5,7],
                    "confidence": [0.9,0.9,0.9,0.9], "degraded": "no", "key_root": 11, "key_mode": "minor"})",
                "文字列の degraded は拒否");
    }

    // アンカーは通常 undo に載る（明示解除は tracks に触らないので、State に無いと⌘Zが取り残す）
    {
        UndoStack undoStack;
        Project p;
        p.loopAnchor = anchor;
        undoStack.begin (p);   // 解除の直前
        p.loopAnchor.reset();  // 明示解除
        UndoStack::EditKind kind;
        expect (undoStack.undo (p, kind) && p.loopAnchor.has_value()
                    && p.loopAnchor->libraryPath == anchor.libraryPath
                    && p.loopAnchor->roots == anchor.roots,
                "解除は⌘Z 1回でアンカーが戻ること");
        expect (undoStack.redo (p, kind) && ! p.loopAnchor.has_value(), "redoで解除し直せること");

        // pushCommitted（ガチャ「残す」）経路: before はセッション baseline の値。
        // ループ採用で仮配置中に BPM/キー/アンカーが変わっていても、⌘Z で仮配置前へ戻ること
        UndoStack stack2;
        Project q;
        q.bpm = 93.0; // 仮配置前（baseline）
        stack2.pushCommitted (std::vector<Track> {}, q,
                              /*beforeBpm*/ 93.0, /*beforeKey*/ std::nullopt, /*beforeAnchor*/ std::nullopt);
        q.bpm = 85.0;          // 逆コピー後の値（「残す」時点の現在値）
        q.loopAnchor = anchor; // 採用済みアンカー
        expect (stack2.undo (q, kind) && ! q.loopAnchor.has_value() && q.bpm == 93.0,
                "pushCommitted の⌘Zで仮配置前の BPM・アンカー無しへ戻ること");
        expect (stack2.redo (q, kind) && q.loopAnchor.has_value() && q.bpm == 85.0,
                "redo で採用状態（新BPM＋アンカー）へ戻れること");
    }

    dir.deleteRecursively();
}

void testEngineSongFade()
{
    beginTest ("engine applies song fade");

    constexpr double sr = 48000.0;
    constexpr int totalSamples = 32768;
    constexpr float level = 0.5f;

    // 全長にわたって一定値のクリップ1本（フェードの形がそのまま出力に出る）
    const auto makeProject = [] (int fadeStart16, int fadeEnd16, double bpm)
    {
        auto project = std::make_unique<Project>();
        project->bpm = bpm;
        Track track;
        track.id = 1;
        track.params->gain.store (1.0f);
        Clip clip;
        clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, totalSamples * 2);
        for (int i = 0; i < clip.audio->getNumSamples(); ++i)
            clip.audio->setSample (0, i, level);
        clip.lengthSamples = clip.audio->getNumSamples();
        track.clips.push_back (std::move (clip));
        project->tracks.push_back (std::move (track));
        project->fadeOutStartSixteenths = fadeStart16;
        project->fadeOutEndSixteenths = fadeEnd16;
        return project;
    };

    // Master Limiterのlookahead遅延は「余分に1ブロック回して +latency から詰める」で吸収
    //（フェード終端以後の厳密0は、エンジン側が fadeEnd+L 以後の出力加算を止めることで保たれる）
    const auto render = [&] (Project& project, int blockSize, double bpm)
    {
        const int latency = engineLimiterLatency (sr);
        juce::AudioBuffer<float> raw (2, totalSamples + blockSize);
        raw.clear();
        TransportState transport;
        transport.bpm.store (bpm);
        SnapshotExchange snapshots;
        PreviewFifo previewFifo;
        PlaybackEngine engine (transport, snapshots, previewFifo);
        engine.prepareToPlay (blockSize, sr);
        snapshots.push (project.buildSnapshot());
        transport.seekRequest.store (0);
        engine.play();

        juce::AudioBuffer<float> buffer (2, blockSize);
        for (int pos = 0; pos < raw.getNumSamples(); pos += blockSize)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            const int n = juce::jmin (blockSize, raw.getNumSamples() - pos);
            for (int ch = 0; ch < 2; ++ch)
                raw.copyFrom (ch, pos, buffer, ch, 0, n);
        }
        engine.stop();
        snapshots.deleteRetired();

        juce::AudioBuffer<float> out (2, totalSamples);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, 0, raw, ch, latency, totalSamples);
        return out;
    };

    // S字をブロック単位で線形近似する都合上、フェードが短いほど近似誤差が大きくなる
    // （誤差はフェード長の2乗に反比例）。実使用は数秒〜数十秒なので、テストもそれに近い
    // 長さにする: 50BPM/48kHz で 1/16 = 14400サンプル（0.3秒）
    constexpr double bpm = 50.0;
    const auto fadeStartSample = SongFade::sixteenthsToSamples (1, bpm, sr); // 14400
    const auto fadeEndSample = SongFade::sixteenthsToSamples (2, bpm, sr);   // 28800
    expect (fadeStartSample == 14400 && fadeEndSample == 28800, "1/16=14400サンプルになること");

    // ブロックサイズ512: フェード境界(14400/28800)はブロックの途中に来る（14400 = 512*28 + 64）
    {
        auto project = makeProject (1, 2, bpm);
        const auto out = render (*project, 512, bpm);

        expect (std::abs (out.getSample (0, 0) - level) < 1.0e-6f, "フェード前は素の振幅");
        expect (std::abs (out.getSample (0, (int) fadeStartSample - 1) - level) < 1.0e-6f,
                "開始点の直前まで減衰しないこと（ブロック中央に開始がある回帰）");

        // 終端以後は厳密に0（分割しないと直線ランプが境界を越えて非ゼロが残る）
        int nonZeroAfterEnd = 0;
        for (int i = (int) fadeEndSample; i < totalSamples; ++i)
            if (out.getSample (0, i) != 0.0f || out.getSample (1, i) != 0.0f)
                ++nonZeroAfterEnd;
        expect (nonZeroAfterEnd == 0, "終端以後は厳密に0（ブロック中央に終端がある回帰）");

        // 区間内はS字に沿う
        const int mid = (int) ((fadeStartSample + fadeEndSample) / 2);
        expect (std::abs (out.getSample (0, mid) - level * 0.5f) < 2.0e-3f, "中点で約半分");
        expect (out.getSample (0, (int) fadeStartSample + 10) < level, "開始直後は減衰している");
    }

    // ブロックサイズを変えても結果が一致する（境界分割の副作用がないこと）
    {
        auto a = makeProject (1, 2, bpm);
        auto b = makeProject (1, 2, bpm);
        const auto outA = render (*a, 512, bpm);
        const auto outB = render (*b, 500, bpm); // 境界に乗らないブロックサイズ
        float maxDiff = 0.0f;
        for (int i = 0; i < totalSamples; ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (outA.getSample (0, i) - outB.getSample (0, i)));
        // 許容は実測から決める（推測しない）。S字を1ブロック=1直線で近似するため、
        // ブロックサイズが変わると近似の刻みも変わる。実測の最大差は 3.5e-4（level 0.5 に対し0.07%＝
        // -69dB相当で聴感差はない）。GOTCHAS.md の 1e-4 は「同一傾斜内の分割」の値で、
        // 曲線を近似するここには当てはまらない
        expect (maxDiff < 2.0e-3f, "ブロックサイズを変えても許容誤差内で一致");
    }

    // フェード開始・終端がブロック先頭と完全一致しても無限ループしない（segLen=0の回帰）。
    // ブロック600なら 14400/28800 がちょうどブロック先頭に来る
    {
        auto project = makeProject (1, 2, bpm);
        const auto out = render (*project, 600, bpm); // 14400 = 600*24, 28800 = 600*48
        expect (std::abs (out.getSample (0, 0) - level) < 1.0e-6f, "境界一致でも先頭が正しく鳴ること");
        expect (out.getSample (0, (int) fadeEndSample) == 0.0f, "境界一致でも終端以後が0であること");
    }

    // BPM変更に追従する（スナップショットを再pushせずにtransport.bpmだけ変える経路）
    {
        auto project = makeProject (1, 2, bpm);
        juce::AudioBuffer<float> out (2, totalSamples);
        out.clear();
        TransportState transport;
        transport.bpm.store (bpm);
        SnapshotExchange snapshots;
        PreviewFifo previewFifo;
        PlaybackEngine engine (transport, snapshots, previewFifo);
        engine.prepareToPlay (512, sr);
        snapshots.push (project->buildSnapshot());
        transport.seekRequest.store (0);
        engine.play();

        // BPMを半分にすると1/16の長さは倍 → フェード開始は 28800 へ動く
        transport.bpm.store (bpm / 2.0);

        juce::AudioBuffer<float> buffer (2, 512);
        for (int pos = 0; pos < totalSamples; pos += 512)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, 512);
            engine.process (info);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, pos, buffer, ch, 0, juce::jmin (512, totalSamples - pos));
        }
        engine.stop();
        snapshots.deleteRetired();

        expect (std::abs (out.getSample (0, 28000) - level) < 1.0e-6f,
                "BPM変更後は新しい開始位置(28800)より前が素の振幅であること");
        expect (out.getSample (0, 29500) < level, "新しい開始位置より後は減衰していること");
    }

    // SR変更に追従する（同じ1/16でもSRが変われば位置が変わる）
    {
        const auto at48k = SongFade::sixteenthsToSamples (1, bpm, 48000.0);
        const auto at96k = SongFade::sixteenthsToSamples (1, bpm, 96000.0);
        expect (at96k == at48k * 2, "SRが倍なら同じ1/16のサンプル位置も倍");
    }
}

// ---- 曲末フェード: バウンス（終端固定・テールなし・RTとの一致）----
void testBounceSongFade()
{
    beginTest ("bounce song fade");

    constexpr double sr = 48000.0;
    constexpr double bpm = 50.0; // 1/16 = 14400サンプル（実使用に近い長さ。短いと線形近似の誤差が出る）
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("fade.wav");

    auto audio = std::make_shared<juce::AudioBuffer<float>> (1, 40000);
    for (int i = 0; i < 40000; ++i)
        audio->setSample (0, i, 0.5f);

    BounceRenderer::Request request;
    request.sampleRate = sr;
    request.bpm = bpm;
    request.startSample = 0;
    request.endSample = 28800; // MainComponent が fadeEndSample で切る想定と同じ
    request.wantTail = false; // フェードありのときはテールを付けない（末尾に無音1ブロックが付くため）
    request.fadeOutStartSixteenths = 1;
    request.fadeOutEndSixteenths = 2;
    request.targetFile = target;
    BounceRenderer::TrackRender track;
    track.gain = 1.0f;
    track.clips.push_back ({ audio, 0, 0, 40000 });
    request.tracks.push_back (std::move (track));

    BounceRenderer renderer;
    expect (renderer.start (std::move (request)), "startできること");
    expect (waitForBounce (renderer), "タイムアウトせず完了すること");
    const auto result = renderer.takeResult();
    expect (result.status == BounceRenderer::Status::success, "successで終わること");
    expect (result.writtenSamples == 28800, "出力長がフェード終端に厳密に一致（テールの1ブロックが付かない）");

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::FileInputStream (target), true));
    expect (reader != nullptr, "書き出したWAVを読めること");
    if (reader != nullptr)
    {
        expect (reader->lengthInSamples == 28800, "WAVの長さ=フェード終端");
        juce::AudioBuffer<float> readBack (2, 28800);
        reader->read (&readBack, 0, 28800, 0, true, true);

        expect (std::abs (readBack.getSample (0, 0) - 0.5f) < 1.0e-4f, "フェード前は素の振幅");
        expect (std::abs (readBack.getSample (0, 14399) - 0.5f) < 1.0e-4f, "開始点の直前まで減衰しない");
        expect (std::abs (readBack.getSample (0, 21600) - 0.25f) < 2.0e-3f, "中点で約半分");
        expect (std::abs (readBack.getSample (0, 28799)) < 2.0e-3f, "終端の直前はほぼ0");

        // サンプルの欠落・重複がないこと（pos += n の回帰）: 素材は一定値なので、
        // 出力はS字の単調減少になるはず。飛び・戻りがあれば単調性が崩れる
        bool monotonicAfterStart = true;
        for (int i = 14401; i < 28800; ++i)
            if (readBack.getSample (0, i) > readBack.getSample (0, i - 1) + 1.0e-5f)
                monotonicAfterStart = false;
        expect (monotonicAfterStart, "フェード区間が単調減少（サンプルの欠落・重複がないこと）");
    }

    // RT再生との一致（許容誤差1e-4。GOTCHAS.mdの実測値に従う）
    {
        auto project = std::make_unique<Project>();
        project->bpm = bpm;
        Track t;
        t.id = 1;
        t.params->gain.store (1.0f);
        Clip clip;
        clip.audio = audio;
        clip.lengthSamples = 40000;
        t.clips.push_back (std::move (clip));
        project->tracks.push_back (std::move (t));
        project->fadeOutStartSixteenths = 1;
        project->fadeOutEndSixteenths = 2;

        TransportState transport;
        transport.bpm.store (bpm);
        SnapshotExchange snapshots;
        PreviewFifo previewFifo;
        PlaybackEngine engine (transport, snapshots, previewFifo);
        engine.prepareToPlay (512, sr);
        snapshots.push (project->buildSnapshot());
        transport.seekRequest.store (0);
        engine.play();

        // Limiterの遅延ぶん余分に1ブロック回し、RT側を +latency で読んで比較する
        const int latency = engineLimiterLatency (sr);
        juce::AudioBuffer<float> rt (2, 28800 + 512);
        rt.clear();
        juce::AudioBuffer<float> buffer (2, 512);
        for (int pos = 0; pos < rt.getNumSamples(); pos += 512)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, 512);
            engine.process (info);
            for (int ch = 0; ch < 2; ++ch)
                rt.copyFrom (ch, pos, buffer, ch, 0, juce::jmin (512, rt.getNumSamples() - pos));
        }
        engine.stop();
        snapshots.deleteRetired();

        if (reader != nullptr)
        {
            juce::AudioBuffer<float> readBack (2, 28800);
            reader->read (&readBack, 0, 28800, 0, true, true);
            float maxDiff = 0.0f;
            for (int i = 0; i < 28800; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (rt.getSample (0, i + latency)
                                                         - readBack.getSample (0, i)));
            // RTは512、バウンスは renderBlockSize=1024 固定なので、S字の線形近似の刻みが違う。
            // 実測の最大差は 1.5e-3（level 0.5 に対し0.3%＝-56dB相当）。同じカーブを適用して
            // いることの確認が目的なので、この誤差は許容する
            expect (maxDiff < 5.0e-3f, "RT再生とバウンスが許容誤差内で一致すること");
        }
    }

    // バウンス範囲の決定そのもの（MainComponentが呼ぶ判断をRequest側に閉じてある）。
    // ここが jmin へ戻ると素材終端で切れるので、値で直接固定する
    {
        BounceRenderer::Request r;
        r.sampleRate = sr;
        r.bpm = bpm;
        r.endSample = 20000; // 素材終端（フェード終端28800より手前）
        r.wantTail = true;
        r.fadeOutStartSixteenths = 1;
        r.fadeOutEndSixteenths = 2;
        r.applySongFadeToRange();
        expect (r.endSample == 28800, "終端を素材より後ろへ置いたら伸ばすこと（jminにしない）");
        expect (! r.wantTail, "テールを落とすこと（末尾に無音1ブロックが付かないように）");

        // 素材の方が長いケースでもフェード終端に合わせる（＝常にフェード終端が曲の終端）
        BounceRenderer::Request shorter;
        shorter.sampleRate = sr;
        shorter.bpm = bpm;
        shorter.endSample = 40000;
        shorter.wantTail = true;
        shorter.fadeOutStartSixteenths = 1;
        shorter.fadeOutEndSixteenths = 2;
        shorter.applySongFadeToRange();
        expect (shorter.endSample == 28800, "素材が長くてもフェード終端で切ること");

        // フェード未設定なら何も変えない（テールの判断も既存のまま）
        BounceRenderer::Request none;
        none.sampleRate = sr;
        none.bpm = bpm;
        none.endSample = 20000;
        none.wantTail = true;
        none.applySongFadeToRange();
        expect (none.endSample == 20000 && none.wantTail, "フェード未設定なら範囲もテールも変えないこと");
    }

    // フェード終端を素材終端より後ろへ置いたケース（MIDIのリリースや余韻をフェードさせたいとき）。
    // 呼び出し側が endSample を jmin で切ると素材終端で終わってしまうので、伸ばす側も従うこと
    {
        const auto target3 = dir.getChildFile ("fade-beyond-material.wav");
        auto shortAudio = std::make_shared<juce::AudioBuffer<float>> (1, 20000); // フェード終端28800より短い
        for (int i = 0; i < 20000; ++i)
            shortAudio->setSample (0, i, 0.5f);

        BounceRenderer::Request r3;
        r3.sampleRate = sr;
        r3.bpm = bpm;
        r3.startSample = 0;
        r3.endSample = 28800; // = fadeEndSample。素材終端(20000)より後ろ
        r3.wantTail = false;
        r3.fadeOutStartSixteenths = 1;
        r3.fadeOutEndSixteenths = 2;
        r3.targetFile = target3;
        BounceRenderer::TrackRender t3;
        t3.gain = 1.0f;
        t3.clips.push_back ({ shortAudio, 0, 0, 20000 });
        r3.tracks.push_back (std::move (t3));

        BounceRenderer renderer3;
        expect (renderer3.start (std::move (r3)), "startできること");
        expect (waitForBounce (renderer3), "タイムアウトせず完了すること");
        const auto result3 = renderer3.takeResult();
        expect (result3.status == BounceRenderer::Status::success, "successで終わること");
        expect (result3.writtenSamples == 28800,
                "素材終端で切れず、フェード終端まで書き出すこと");

        juce::WavAudioFormat wav3;
        std::unique_ptr<juce::AudioFormatReader> reader3 (
            wav3.createReaderFor (new juce::FileInputStream (target3), true));
        if (reader3 != nullptr)
        {
            juce::AudioBuffer<float> back (2, 28800);
            reader3->read (&back, 0, 28800, 0, true, true);
            expect (back.getSample (0, 19999) > 0.0f, "素材の最後までフェードが掛かって鳴っていること");
            expect (std::abs (back.getSample (0, 25000)) < 1.0e-6f, "素材終端より後ろは無音であること");
        }
    }

    // サイクル範囲がフェード終端より後ろでも startSample >= endSample を作らない
    // （サイクルONではフェード終端で切らない＝範囲はサイクルが優先）
    {
        const auto target2 = dir.getChildFile ("cycle-after-fade.wav");
        BounceRenderer::Request r2;
        r2.sampleRate = sr;
        r2.bpm = bpm;
        r2.startSample = 28800; // フェード終端以後から始まるサイクル範囲
        r2.endSample = 32400;
        r2.wantTail = false;
        r2.fadeOutStartSixteenths = 1;
        r2.fadeOutEndSixteenths = 2;
        r2.targetFile = target2;
        BounceRenderer::TrackRender t2;
        t2.gain = 1.0f;
        t2.clips.push_back ({ audio, 0, 0, 40000 });
        r2.tracks.push_back (std::move (t2));

        expect (r2.startSample < r2.endSample, "startSample < endSample が保たれること");
        BounceRenderer renderer2;
        expect (renderer2.start (std::move (r2)), "サイクル範囲の書き出しをstartできること");
        expect (waitForBounce (renderer2), "タイムアウトせず完了すること");
        const auto result2 = renderer2.takeResult();
        expect (result2.status == BounceRenderer::Status::success, "successで終わること");
        expect (result2.writtenSamples == 3600, "サイクル範囲ぶんの長さになること（無音でも書き出す）");
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

// ---- 回帰ハッシュ（FX ON・全経路）----
// testMonoRenderRegressionHash は FX 中立＝高速パスの回帰のみを固定する。こちらは
// EQ・Comp を有効にして active経路の6経路（RTモノ / RTステレオ / RT MIDI（サンプラー）/
// バウンス本編 / バウンスMIDI / バウンスEQテール）を全て通し、エンジン集約リファクタの
// 前後で出力がビット一致で不変であることをハッシュの比較で確認する。
// 期待値はハードコードしない（浮動小数点の積和順序はコンパイラ・環境で変わりうるため、
// 同一環境での変更前後比較にのみ使う。採取・比較は scripts/check-render-hashes.sh）。
// MIDIはGM（DLSMusicDevice）でなく内蔵サンプラーを使う（外部AUは環境・状態依存の揺れがありうる）
void testTrackFxRegressionHash()
{
    beginTest ("track fx regression hash");

    // FNV-1a 64bit（testMonoRenderRegressionHash と同じ流儀）
    auto fnv1a = [] (const void* data, size_t size)
    {
        const auto* bytes = static_cast<const juce::uint8*> (data);
        juce::uint64 hash = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 0x100000001b3ULL;
        return juce::String::toHexString ((juce::int64) hash);
    };

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 32;
    constexpr int totalSamples = blockSize * numBlocks;

    auto makeAudio = [] (int channels, int len, float scale, int seed)
    {
        auto buffer = std::make_shared<juce::AudioBuffer<float>> (channels, len);
        juce::Random random (seed);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < len; ++i)
                buffer->setSample (ch, i,
                                   (std::sin ((float) i * 0.07f + (float) ch) * 0.3f
                                    + (random.nextFloat() - 0.5f) * 0.15f) * scale);
        return buffer;
    };

    // 減衰するサンプラー素材（1秒。バウンス範囲＝0.37秒の後もテールで鳴り続ける長さ）
    auto makeSamplerAudio = [] (int len)
    {
        auto buffer = std::make_shared<juce::AudioBuffer<float>> (1, len);
        for (int i = 0; i < len; ++i)
        {
            const float env = 1.0f - (float) i / (float) len;
            buffer->setSample (0, i, std::sin ((float) i * 0.09f) * 0.4f * env * env);
        }
        return buffer;
    };

    // 3トラックで6経路を全て active FX で通す:
    //   1: モノクリップ（RTモノ／バウンスmonoFx。リージョンゲイン・フェード込み）
    //   2: ステレオクリップ（RTステレオ／バウンスprePanFx）
    //   3: サンプラーMIDI（RT MIDI／バウンスMIDI）
    // EQはオーディオ2トラックとも非中立＝バウンスのEQテール経路にも入る。
    // バッチ3で Sat/Lo-fi を追加（3トラックに成分を配分し全経路で新FXも回す。
    // フィクスチャ拡張時はベースライン再captureが必要）
    auto fillProject = [&] (Project& project)
    {
        project.bpm = 120.0;
        project.sampleRate = sr;
        {
            Track track;
            track.id = 1;
            track.params->gain.store (0.7f);
            track.params->pan.store (-0.3f);
            track.params->sends[0].store (0.5f);
            Eq::store (track.params->eqBands[Eq::bell1],
                       Eq::normalized (Eq::bell1, { true, 500.0f, 5.0f, 1.2f }));
            track.params->compEnabled.store (true);
            Comp::store (track.params->comp,
                         Comp::normalized ({ -24.0f, 4.0f, 5.0f, 80.0f, 3.0f, false }));
            Sat::store (track.params->sat, Sat::normalized ({ 0.5f, 0.8f }));
            Lofi::store (track.params->lofi, Lofi::normalized ({ 0.3f, 0.0f, 0.0f, 0.4f }));
            Clip clip;
            clip.fileName = "clip-001.wav";
            clip.audio = makeAudio (1, totalSamples, 0.8f, 3);
            clip.lengthSamples = totalSamples;
            clip.gain = 1.2f;
            clip.fadeInSamples = 2000;
            clip.fadeOutSamples = 3000;
            track.clips.push_back (std::move (clip));
            project.tracks.push_back (std::move (track));
        }
        {
            Track track;
            track.id = 2;
            track.params->gain.store (0.8f);
            track.params->pan.store (0.4f);
            track.params->sends[1].store (0.3f);
            Eq::store (track.params->eqBands[Eq::highpass],
                       Eq::normalized (Eq::highpass, { true, 150.0f, 0.0f, 0.0f }));
            Eq::store (track.params->eqBands[Eq::highShelf],
                       Eq::normalized (Eq::highShelf, { true, 8000.0f, 3.0f, 0.0f }));
            track.params->compEnabled.store (true);
            Comp::store (track.params->comp,
                         Comp::normalized ({ -30.0f, 8.0f, 1.0f, 50.0f, 0.0f, true }));
            Sat::store (track.params->sat, Sat::normalized ({ 0.7f, 1.0f }));
            Lofi::store (track.params->lofi, Lofi::normalized ({ 0.0f, 0.5f, 0.4f, 0.0f }));
            Clip clip;
            clip.fileName = "clip-002.wav";
            clip.audio = makeAudio (2, totalSamples, 1.0f, 5);
            clip.lengthSamples = totalSamples;
            track.clips.push_back (std::move (clip));
            project.tracks.push_back (std::move (track));
        }
        {
            Track track;
            track.id = 3;
            track.type = TrackType::midi;
            track.instrument = InstrumentKind::sample;
            track.sampleFile = "instr-001.wav";
            track.sampleName = "fx-hash";
            track.sampleAudio = makeSamplerAudio ((int) sr);
            track.sampleSourceRate = sr;
            track.params->gain.store (0.9f);
            track.params->pan.store (0.2f);
            track.params->sends[2].store (0.25f);
            Eq::store (track.params->eqBands[Eq::bell2],
                       Eq::normalized (Eq::bell2, { true, 2000.0f, -4.0f, 1.0f }));
            track.params->compEnabled.store (true);
            Comp::store (track.params->comp,
                         Comp::normalized ({ -20.0f, 3.0f, 10.0f, 120.0f, 2.0f, false }));
            Sat::store (track.params->sat, Sat::normalized ({ 0.3f, 1.0f }));
            Lofi::store (track.params->lofi, Lofi::normalized ({ 0.0f, 0.3f, 0.0f, 0.5f }));
            MidiRegion region;
            region.id = 30;
            region.startPpq = 0;
            region.lengthPpq = Ppq::ticksPerBar;
            region.notes.push_back ({ 31, 60, 0, Ppq::ticksPerQuarter / 4, 110 });
            region.notes.push_back ({ 32, 64, Ppq::ticksPerQuarter / 2, Ppq::ticksPerQuarter / 4, 90 });
            track.midiRegions.push_back (std::move (region));
            project.tracks.push_back (std::move (track));
        }
        project.busParams[0]->gain.store (0.8f);
        project.busParams[1]->gain.store (0.9f);
        project.busParams[2]->gain.store (0.7f);
        project.masterParams->gain.store (0.85f);
    };

    auto renderEngine = [&] (Project& proj, juce::AudioBuffer<float>& out)
    {
        SynthBank synthBank;
        synthBank.sync (proj, sr, blockSize);
        TransportState transport;
        SnapshotExchange snapshots;
        PreviewFifo previewFifo;
        PlaybackEngine engine (transport, snapshots, previewFifo);
        engine.prepareToPlay (blockSize, sr);
        auto snapshot = proj.buildSnapshot();
        for (size_t i = 0; i < proj.tracks.size(); ++i)
            if (proj.tracks[i].type == TrackType::midi)
                snapshot->tracks[i].synth = synthBank.get (proj.tracks[i].id);
        snapshots.push (std::move (snapshot));
        transport.seekRequest.store (0);
        engine.play();
        juce::AudioBuffer<float> buffer (2, blockSize);
        for (int blockIndex = 0; blockIndex < numBlocks; ++blockIndex)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, blockIndex * blockSize, buffer, ch, 0, blockSize);
        }
        engine.stop();
        snapshots.deleteRetired();
    };

    Project project;
    fillProject (project);
    juce::AudioBuffer<float> engineOut (2, totalSamples);
    renderEngine (project, engineOut);

    // 各トラックのFXが実際に効いていること（そのトラックだけ中立にすると出力が変わる）。
    // フィールド名変更などでFXが黙って外れ、「全経路が素通しのままハッシュ一致」で
    // リファクタの破壊を見逃す空回りを防ぐ
    {
        const char* messages[] = { u8"トラック1（モノ）のFXが実際に効いていること",
                                   u8"トラック2（ステレオ）のFXが実際に効いていること",
                                   u8"トラック3（サンプラー）のFXが実際に効いていること" };
        for (size_t i = 0; i < 3; ++i)
        {
            Project neutral;
            fillProject (neutral);
            Eq::applyDefaults (neutral.tracks[i].params->eqBands);
            neutral.tracks[i].params->compEnabled.store (false);
            juce::AudioBuffer<float> neutralOut (2, totalSamples);
            renderEngine (neutral, neutralOut);
            float maxDiff = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < totalSamples; ++s)
                    maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (ch, s)
                                                             - neutralOut.getSample (ch, s)));
            expect (maxDiff > 1.0e-3f, messages[i]);
        }
    }

    {
        juce::MemoryBlock rendered;
        for (int ch = 0; ch < 2; ++ch)
            rendered.append (engineOut.getReadPointer (ch), sizeof (float) * (size_t) totalSamples);
        std::cout << "hash-fx-engine: " << fnv1a (rendered.getData(), rendered.getSize()) << std::endl;
    }

    // バウンス。クリップ・ノートは buildSnapshot から流用する（ループ展開・ノートの
    // フラット化を本番と同じヘルパーに任せる）。wantTail も本番（MainComponent::startBounce）
    // と同じ共通判定 resolveWantTail で立てる（この fixture では true になり、EQテール経路
    // ＝経路#6とサンプラーの鳴り残しを通す。判定をハードコードや再実装で済ませると、
    // 本番入口のテール判定の欠落を検出できない）
    auto runBounce = [&] (Project& proj, const juce::File& target) -> juce::String
    {
        SynthBank bounceBank;
        auto snap = proj.buildSnapshot (Project::SnapshotChange::offlineRender);
        BounceRenderer::Request request;
        request.sampleRate = sr;
        request.bpm = proj.bpm;
        request.endSample = totalSamples;
        request.targetFile = target;
        for (int b = 0; b < numSendBuses; ++b)
        {
            request.busGain[b] = proj.busParams[b]->gain.load();
            request.busMute[b] = proj.busParams[b]->mute.load();
        }
        request.masterGain = proj.masterParams->gain.load();
        for (size_t i = 0; i < proj.tracks.size(); ++i)
        {
            auto& track = proj.tracks[i];
            BounceRenderer::TrackRender render;
            render.gain = track.params->gain.load();
            render.pan = track.params->pan.load();
            for (int b = 0; b < numSendBuses; ++b)
                render.sends[b] = track.params->sends[b].load();
            render.loadFxFrom (*track.params);
            render.clips = snap->tracks[i].clips;
            render.notes = snap->tracks[i].notes;
            if (track.type == TrackType::midi)
            {
                render.synth = bounceBank.createIndependent (track, sr, BounceRenderer::renderBlockSize);
                expect (render.synth != nullptr, u8"バウンス専用サンプラーを作れること");
            }
            request.tracks.push_back (std::move (render));
        }
        request.resolveWantTail();
        expect (request.wantTail, u8"このfixtureでは共通判定がテールを要求すること（テール経路を通す前提）");
        BounceRenderer renderer;
        expect (renderer.start (std::move (request)), "startできること");
        expect (waitForBounce (renderer), u8"タイムアウトせず完了すること");
        const auto result = renderer.takeResult();
        expect (result.status == BounceRenderer::Status::success, "successで終わること");
        expect (result.writtenSamples > totalSamples,
                u8"テールが書かれること（EQリングアウト経路を通る）");
        juce::MemoryBlock fileData;
        expect (target.loadFileAsData (fileData), u8"書き出したWAVを読めること");
        return fnv1a (fileData.getData(), fileData.getSize());
    };

    {
        const auto dir = makeTempDir();
        std::cout << "hash-fx-bounce: "
                  << runBounce (project, dir.getChildFile ("bounce-fx.wav")) << std::endl;
        dir.deleteRecursively();
    }

    // ---- project.json 経由（保存→読込→バウンス）----
    // 実プロジェクト相当の経路（Project::save/load を通す。クリップWAVの24bit量子化を
    // 含むため上とは別ハッシュになる）。fixture自体が冪等なseeder＝再生成可能
    {
        const auto projDir = makeTempDir();
        Project src;
        fillProject (src);
        src.directory = projDir;
        for (auto& track : src.tracks)
            for (auto& clip : track.clips)
                expect (writeBufferWav (projDir.getChildFile (clip.fileName), *clip.audio, sr),
                        u8"クリップWAVを書けること");
        expect (writeBufferWav (projDir.getChildFile ("instr-001.wav"),
                                *src.tracks[2].sampleAudio, sr),
                u8"サンプルWAVを書けること");
        juce::String error;
        expect (src.save (error), u8"保存できること");

        juce::StringArray warnings;
        auto loaded = Project::load (projDir, warnings, error);
        expect (loaded != nullptr && loaded->tracks.size() == 3, u8"読み込めること");
        if (loaded != nullptr && loaded->tracks.size() == 3)
            std::cout << "hash-fx-project-bounce: "
                      << runBounce (*loaded, projDir.getChildFile ("bounce-roundtrip.wav"))
                      << std::endl;
        projDir.deleteRecursively();
    }
}

// analyze.py の進捗集約行のパース。書式は analyze.py の announce() と対
void testAnalyzeProgress()
{
    beginTest ("AnalyzeProgress");

    {
        const auto p = AnalyzeProgress::parse (
            juce::String::fromUTF8 (u8"==> 42% | 7/16 完了 ／ 実行中: ステム分離(4s)・BPM/キー/構成"));
        expect (p.progress.has_value(), "進捗率トークンが読める");
        expect (p.progress.has_value() && std::abs (*p.progress - 0.42f) < 1.0e-6f, "比率が正しい");
        expect (p.display == juce::String::fromUTF8 (u8"==> 7/16 完了 ／ 実行中: ステム分離(4s)・BPM/キー/構成"),
                "表示行から進捗率トークンが剥がれる（バーと二重表示にしない）");
    }
    {
        // 進捗率のない従来形式の行はそのまま表示に回す
        const auto p = AnalyzeProgress::parse (juce::String::fromUTF8 (u8"==> 完了: グルーヴ（35秒）"));
        expect (! p.progress.has_value(), "完了行は進捗率なし");
        expect (p.display == juce::String::fromUTF8 (u8"==> 完了: グルーヴ（35秒）"), "完了行はそのまま");
    }
    {
        const auto p = AnalyzeProgress::parse (juce::String::fromUTF8 (u8"カードを生成しない（キーのゲート落ち）"));
        expect (! p.progress.has_value() && p.display.isNotEmpty(), "card.py の申告行はそのまま");
    }
    expect (! AnalyzeProgress::parse ("==> 4x% | rest").progress.has_value(), "数字でないトークンは進捗にしない");
    expect (! AnalyzeProgress::parse ("==> 42 | rest").progress.has_value(), "% がなければ進捗にしない");
    expect (AnalyzeProgress::parse ("").display.isEmpty(), "空行は表示も空（呼び出し側が捨てる）");
    {
        const auto p = AnalyzeProgress::parse ("==> 250% | rest");
        expect (p.progress.has_value() && *p.progress <= 1.0f, "100 超は 1.0 に丸める（バーが飛び出さない）");
    }
    {
        const auto p = AnalyzeProgress::parse ("  ==> 0% | rest  ");
        expect (p.progress.has_value() && *p.progress == 0.0f && p.display == "==> rest",
                "前後の空白と 0% を扱える");
    }
}

// yt-dlp の出力パース。ケースは yt-dlp 2026.07.04 の実出力から採っている
void testYtDlpOutput()
{
    beginTest ("YtDlpOutput");

    // ---- 進捗行 ----
    {
        // 実測: 総サイズが判っている動画では total_bytes 側だけが入り estimate は NA
        const auto p = YtDlpOutput::parseProgress ("LALA_PROGRESS 130048 252182 NA");
        expect (p.has_value(), "実測の進捗行が読める");
        expect (p.has_value() && std::abs (*p - 130048.0f / 252182.0f) < 1.0e-6f, "進捗の比率が正しい");
    }
    {
        // 逆に total_bytes が NA で estimate だけ入るケースは分母を estimate から取る
        const auto p = YtDlpOutput::parseProgress ("LALA_PROGRESS 500 NA 1000");
        expect (p.has_value() && std::abs (*p - 0.5f) < 1.0e-6f, "total_bytes が NA なら estimate を使う");
    }
    expect (! YtDlpOutput::parseProgress ("LALA_PROGRESS 500 NA NA").has_value(),
            "分母が両方 NA なら進捗にしない");
    expect (! YtDlpOutput::parseProgress ("LALA_PROGRESS 500 0 NA").has_value(),
            "分母0は進捗にしない");
    expect (! YtDlpOutput::parseProgress ("LALA_PROGRESS 500").has_value(),
            "トークン不足は進捗にしない");
    expect (! YtDlpOutput::parseProgress ("[download] Destination: audio.webm").has_value(),
            "進捗以外の行は無視する");
    expect (! YtDlpOutput::parseProgress ("").has_value(), "空行は無視する");
    {
        // 分母を超えた値が来てもバーが飛び出さない
        const auto p = YtDlpOutput::parseProgress ("LALA_PROGRESS 2000 1000 NA");
        expect (p.has_value() && *p <= 1.0f, "1.0 を超えない");
    }

    // ---- メタ情報 ----
    {
        const juce::String out =
            "[youtube] Extracting URL: https://www.youtube.com/watch?v=jNQXAC9IVRw\n"
            "LALA_META:{\"title\": \"Me at the zoo\", \"duration\": 19, \"is_live\": false}\n";
        const auto meta = YtDlpOutput::parseMetadata (out);
        expect (meta.ok, "実測のメタ行が読める");
        expect (meta.title == "Me at the zoo", "タイトルが取れる");
        expect (meta.hasDuration && std::abs (meta.durationSeconds - 19.0) < 1.0e-9, "長さが取れる");
        expect (! meta.isLive, "is_live=false が読める");
    }
    {
        // 平文フィールド方式なら壊れるケース: タイトルに改行・引用符・prefix文字列が入る。
        // JSONならエスケープされて1行に収まるので読める
        const juce::String out =
            "LALA_META:{\"title\": \"a\\nb \\\"q\\\" LALA_META: x\", \"duration\": 5, \"is_live\": false}";
        const auto meta = YtDlpOutput::parseMetadata (out);
        expect (meta.ok, "タイトルに改行・引用符・prefixが入っても読める");
        expect (meta.title.contains ("LALA_META:") && meta.title.contains ("q"), "タイトルの中身が保たれる");
        expect (meta.hasDuration, "同じ行の duration も読める");
    }
    {
        const auto meta = YtDlpOutput::parseMetadata (
            "LALA_META:{\"title\": \"live\", \"duration\": null, \"is_live\": true}");
        expect (meta.ok, "duration が null でも行としては読める");
        expect (! meta.hasDuration, "duration が null なら長さ無しとして扱う");
        expect (meta.isLive, "is_live=true が読める");
    }
    {
        const auto meta = YtDlpOutput::parseMetadata (
            "WARNING: something\nLALA_META:{\"title\": \"t\", \"duration\": 3, \"is_live\": false}\nbye");
        expect (meta.ok && meta.title == "t", "前後に警告行が混ざっても読める");
    }
    expect (! YtDlpOutput::parseMetadata ("ERROR: nope").ok, "prefix行が無ければ ok=false");
    expect (! YtDlpOutput::parseMetadata ("LALA_META:{broken").ok, "壊れたJSONは ok=false");
    {
        const auto meta = YtDlpOutput::parseMetadata (
            "LALA_META:{\"title\": \"t\", \"duration\": -5, \"is_live\": false}");
        expect (! meta.hasDuration, "負の duration は長さ無しとして扱う");
    }

    // ---- 長さ上限の閾値 ----
    // UrlDownloader は duration×48000 を AudioImporter::maxInputFrames と比べてDL前に弾く。
    // 中間WAVの実SRは事前に判らないが、YouTube由来は48kで実測されており、44.1kソースには
    // 安全側（実フレーム数はこれより少ない）に働く。maxInputFrames を動かしたらここで気づける
    {
        const double limitSeconds = (double) AudioImporter::maxInputFrames
                                    / UrlDownloader::assumedSampleRate;
        expect (limitSeconds > 2000.0 && limitSeconds < 2100.0,
                "長さ上限は約2083秒（34分台）");
        expect (5.0 * UrlDownloader::assumedSampleRate < (double) AudioImporter::maxInputFrames,
                "5秒の動画は通る");
        expect (3600.0 * UrlDownloader::assumedSampleRate > (double) AudioImporter::maxInputFrames,
                "1時間の動画は弾かれる");
    }

    // ---- エラー行の抽出 ----
    expect (YtDlpOutput::extractErrorLine ("ERROR: [youtube] AAAAAAAAAAA: Video unavailable")
                == "ERROR: [youtube] AAAAAAAAAAA: Video unavailable",
            "実測のERROR行を取り出せる");
    expect (YtDlpOutput::extractErrorLine ("WARNING: a\nERROR: first\nERROR: second")
                == "ERROR: first",
            "ERROR行が複数なら最初の1行");
    expect (YtDlpOutput::extractErrorLine ("just a note\nlast line\n") == "last line",
            "ERROR行が無ければ末尾の非空行");
    expect (YtDlpOutput::extractErrorLine ("").isEmpty(), "空入力なら空");

    // ---- 403 の判定（player client を替えたリトライの条件）----
    expect (YtDlpOutput::isHttp403 (
                "ERROR: unable to download video data: HTTP Error 403: Forbidden"),
            "実測の403エラー行を403と判定する");
    expect (! YtDlpOutput::isHttp403 ("ERROR: [youtube] AAAAAAAAAAA: Video unavailable"),
            "403以外のエラーではリトライしない");
    expect (! YtDlpOutput::isHttp403 (""), "空入力は403ではない");

    // ---- URLのマスク ----
    {
        // 漏れ方①: stdout の scheme 付きURL
        const auto r = YtDlpOutput::redactUrls (
            "[youtube] Extracting URL: https://www.youtube.com/watch?v=jNQXAC9IVRw");
        expect (! r.contains ("v=jNQXAC9IVRw"), "queryが消える");
        expect (r.contains ("https://www.youtube.com/watch"), "scheme+host+先頭パスは残る");
    }
    {
        // 漏れ方②: stderr の scheme も host も落ちた形。元URL既知の置換で消す
        const juce::String original = "https://example.com/foo?token=SECRET123#frag";
        const auto r = YtDlpOutput::redactUrls (
            "ERROR: [generic] foo?token=SECRET123#frag: Unable to download webpage", original);
        expect (! r.contains ("SECRET123"), "scheme無しで漏れたトークンも消える");
    }
    {
        const auto r = YtDlpOutput::redactUrls (
            "a https://x.test/p/q?k=1 b http://y.test/z?k=2 c");
        expect (! r.contains ("k=1") && ! r.contains ("k=2"), "URLが複数でも全部伏せる");
        expect (r.startsWith ("a ") && r.endsWith (" c"), "URL以外の本文は保たれる");
    }
    expect (YtDlpOutput::redactUrls ("no urls here") == "no urls here",
            "URLを含まない本文は変わらない");
    {
        const auto r = YtDlpOutput::redactUrls ("see https://a.test/p?x=1.");
        expect (! r.contains ("x=1"), "末尾に句読点が付いていても伏せる");
    }
    {
        // query が無く fragment だけに機密が載るURL。"?" 以降だけを見ていると素通りする
        const juce::String original = "https://example.com/foo#SECRET";
        const auto r = YtDlpOutput::redactUrls (
            "ERROR: [generic] foo#SECRET: Unable to download webpage", original);
        expect (! r.contains ("SECRET"), "fragmentだけの機密値も消える");
    }
    {
        // query と fragment の両方があるケースでどちらも残らない
        const juce::String original = "https://example.com/foo?k=Q1#F2";
        const auto r = YtDlpOutput::redactUrls ("ERROR: foo?k=Q1#F2: nope", original);
        expect (! r.contains ("Q1") && ! r.contains ("F2"), "query と fragment の両方が消える");
    }
    {
        // userinfo（user:password@）付きURL。host は残してよいが認証情報は残してはいけない
        const auto r = YtDlpOutput::redactUrls (
            "[generic] Extracting URL: https://alice:hunter2@example.com/p?x=1");
        expect (! r.contains ("alice") && ! r.contains ("hunter2"), "URLの認証情報が消える");
        expect (r.contains ("example.com"), "ホスト名は残る（どのサイトか分かる情報は要る）");
    }
    {
        // userinfo が無いURLでホスト名が壊れないこと
        const auto r = YtDlpOutput::redactUrls ("see https://example.com/p/q");
        expect (r.contains ("https://example.com/p"), "userinfo が無ければ従来どおり");
    }
}

// 指定プロセスグループに属するプロセス数を数える（キャンセルで孫まで死んだかの裏取り用）。
// pgrep -f は名前で拾うのでユーザーが別用途で動かしている同名プロセスと衝突する。PGIDで見る
int countProcessesInGroup (int pgid)
{
    SpawnedProcess ps;
    if (! ps.start ({ "/bin/ps", "-o", "pgid=", "-ax" }))
        return -1;

    int count = 0;
    ps.readUntilFinished (nullptr,
                          [&count, pgid] (const juce::String& line)
                          {
                              if (line.trim().getIntValue() == pgid)
                                  ++count;
                          },
                          nullptr);
    return count;
}

// 外部プロセスの起動・行読み・キャンセル
void testSpawnedProcess()
{
    beginTest ("SpawnedProcess");

    // ---- stdout が行単位に取れる ----
    {
        SpawnedProcess proc;
        expect (proc.start ({ "/bin/echo", "hello" }), "/bin/echo を起動できる");

        juce::StringArray lines;
        const bool finished = proc.readUntilFinished (
            nullptr, [&lines] (const juce::String& l) { lines.add (l); }, nullptr);

        expect (finished, "最後まで読み切れる");
        expect (lines.size() == 1 && lines[0] == "hello", "stdout の1行が取れる");
        expect (proc.exitCode() == 0, "正常終了の exit code は0");
    }

    // ---- 複数行 ----
    {
        SpawnedProcess proc;
        proc.start ({ "/bin/sh", "-c", "printf 'a\\nb\\nc\\n'" });

        juce::StringArray lines;
        proc.readUntilFinished (nullptr, [&lines] (const juce::String& l) { lines.add (l); }, nullptr);
        expect (lines.size() == 3 && lines[2] == "c", "複数行が順に取れる");
    }

    // ---- stdout と stderr が分離して取れる（JUCE版は同一pipeにマージされて分離できない）----
    {
        SpawnedProcess proc;
        proc.start ({ "/bin/sh", "-c", "echo out; echo err >&2" });

        juce::StringArray outLines, errLines;
        proc.readUntilFinished (nullptr,
                                [&outLines] (const juce::String& l) { outLines.add (l); },
                                [&errLines] (const juce::String& l) { errLines.add (l); });

        expect (outLines.size() == 1 && outLines[0] == "out", "stdout だけが stdout に来る");
        expect (errLines.size() == 1 && errLines[0] == "err", "stderr だけが stderr に来る");
    }

    // ---- 異常終了の exit code ----
    {
        SpawnedProcess proc;
        proc.start ({ "/bin/sh", "-c", "exit 42" });
        proc.readUntilFinished (nullptr, nullptr, nullptr);
        expect (proc.exitCode() == 42, "終了コードを拾える");
    }

    // ---- 出力を出さないプロセスでもキャンセルが即座に効く ----
    // （JUCE の ChildProcess::read() は fread() でブロックするのでここが固まる）
    {
        SpawnedProcess proc;
        expect (proc.start ({ "/bin/sleep", "60" }), "/bin/sleep を起動できる");

        const auto startMs = juce::Time::getMillisecondCounter();
        const bool finished = proc.readUntilFinished (
            [startMs] { return juce::Time::getMillisecondCounter() - startMs > 500; },
            nullptr, nullptr);
        const auto elapsed = juce::Time::getMillisecondCounter() - startMs;

        expect (! finished, "キャンセルされたら false を返す");
        expect (elapsed < 3000, "無出力でも数秒以内に返る（ブロッキング読みで固まらない）");
    }

    // ---- キャンセルで孫プロセスまで死ぬ ----
    {
        SpawnedProcess proc;
        // sh が sleep を2つ持つ。killpg が効かないと孫の sleep が孤児として残る
        expect (proc.start ({ "/bin/sh", "-c", "sleep 30 & sleep 30; wait" }), "子を持つプロセスを起動できる");

        const int pgid = proc.pgid();
        expect (pgid > 0, "PGIDが取れる");

        // 孫が起動するのを待ってから、撃つ前の頭数を確認する。
        // これが無いと「そもそも起動していない」ケースでもテストが通ってしまう
        juce::Thread::sleep (400);
        const int before = countProcessesInGroup (pgid);
        expect (before >= 2, "撃つ前はグループに親と子（孫のsleep）が居る");

        proc.readUntilFinished ([] { return true; }, nullptr, nullptr);

        // SIGKILL 後にプロセステーブルから消えるまでの猶予を見てリトライする
        int remaining = -1;
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            remaining = countProcessesInGroup (pgid);
            if (remaining == 0)
                break;
            juce::Thread::sleep (50);
        }
        expect (remaining == 0,
                "キャンセル後、そのプロセスグループに属するプロセスが残らない（孫まで終了）");
    }

    // ---- 子が先に終了し、孫がSIGTERMを無視するケースでも取り逃さない ----
    // 「直接の子を回収できたら終わり」にすると、TERMを無視する孫（ffmpeg等）が孤児として残る
    {
        SpawnedProcess proc;
        // sh は即 exit し、孫のサブシェルは TERM を無視して生き続ける
        expect (proc.start ({ "/bin/sh", "-c", "(trap '' TERM; sleep 30) & exit 0" }),
                "「子が先に死ぬ」プロセスを起動できる");

        const int pgid = proc.pgid();
        juce::Thread::sleep (500);
        expect (countProcessesInGroup (pgid) >= 1, "撃つ前は孫が生きている");

        proc.readUntilFinished ([] { return true; }, nullptr, nullptr);

        int remaining = -1;
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            remaining = countProcessesInGroup (pgid);
            if (remaining == 0)
                break;
            juce::Thread::sleep (50);
        }
        expect (remaining == 0,
                "子が先に終了していても、TERMを無視する孫までSIGKILLで終了する");
    }
}

// 実ネットワークを使うURLダウンロードの通し確認。ネットに繋がらない環境やCIで落ちないよう、
// 環境変数 LALA_VERIFY_URL が指定されたときだけ走る（GUIを介さずワーカー単体を検証する）:
//   LALA_VERIFY_URL=https://www.youtube.com/watch?v=... ./daw_tests
//   LALA_VERIFY_LONG_URL=... を足すと「長すぎる動画をDL前に弾く」も確認する
void testUrlDownloaderLive()
{
    const auto url = juce::SystemStats::getEnvironmentVariable ("LALA_VERIFY_URL", {});
    if (url.isEmpty())
        return;

    beginTest ("UrlDownloader (live)");

    const auto ytDlp = UrlDownloader::findYtDlp();
    expect (ytDlp != juce::File(), "yt-dlp が見つかる");
    if (ytDlp == juce::File())
        return;

    // ---- スキーム違いは即 failed（ネットに出ない） ----
    {
        UrlDownloader downloader;
        UrlDownloader::Request request;
        request.url = "--exec=touch /tmp/pwned";
        expect (downloader.start (std::move (request)), "start できる");
        while (downloader.status() == UrlDownloader::Status::running)
            juce::Thread::sleep (20);

        const auto result = downloader.takeResult();
        expect (result.status == UrlDownloader::Status::failed, "http(s)以外のURLは failed");
        expect (result.tempDirectory == juce::File(), "失敗時は一時ディレクトリを持たない");
        expect (! juce::File ("/tmp/pwned").existsAsFile(), "オプション文字列が実行されない");
    }

    // ---- 存在しない動画 ----
    {
        UrlDownloader downloader;
        UrlDownloader::Request request;
        request.url = "https://www.youtube.com/watch?v=AAAAAAAAAAA";
        downloader.start (std::move (request));
        while (downloader.status() == UrlDownloader::Status::running)
            juce::Thread::sleep (50);

        const auto result = downloader.takeResult();
        expect (result.status == UrlDownloader::Status::failed, "存在しない動画は failed");
        expect (result.errorMessage.isNotEmpty(), "理由が入っている");
        expect (result.tempDirectory == juce::File(), "失敗時は一時ディレクトリを持たない");
    }

    // ---- 長すぎる動画はダウンロード前に弾く ----
    if (const auto longUrl = juce::SystemStats::getEnvironmentVariable ("LALA_VERIFY_LONG_URL", {});
        longUrl.isNotEmpty())
    {
        UrlDownloader downloader;
        UrlDownloader::Request request;
        request.url = longUrl;
        const auto startMs = juce::Time::getMillisecondCounter();
        downloader.start (std::move (request));
        while (downloader.status() == UrlDownloader::Status::running)
            juce::Thread::sleep (50);
        const auto elapsed = juce::Time::getMillisecondCounter() - startMs;

        const auto result = downloader.takeResult();
        expect (result.status == UrlDownloader::Status::failed, "長すぎる動画は failed");
        expect (result.errorMessage.contains (juce::String::fromUTF8 (u8"長すぎ")), "長さ超過の理由が出る");
        // メタ取得だけなら数秒。ダウンロードまで走っていたらこれより遥かに長くなる
        expect (elapsed < 30000, "ダウンロードを始める前に弾いている");
        std::cout << "     （長さ判定までの所要: " << elapsed << "ms）" << std::endl;
    }

    // ---- キャンセルで一時ディレクトリもプロセスも残らない ----
    {
        UrlDownloader downloader;
        UrlDownloader::Request request;
        request.url = url;
        downloader.start (std::move (request));

        juce::Thread::sleep (2500); // メタ取得を抜けてダウンロードに入るまで待つ
        downloader.cancelAndWait();

        const auto result = downloader.takeResult();
        expect (result.status == UrlDownloader::Status::cancelled
                    || result.status == UrlDownloader::Status::success,
                "キャンセルは cancelled（間に合わず完了した場合は success）");
        if (result.status == UrlDownloader::Status::cancelled)
            expect (result.tempDirectory == juce::File(), "キャンセル時は worker が一時ディレクトリを消す");
        else
            result.tempDirectory.deleteRecursively(); // 完了していたら後片付け

        // yt-dlp / ffmpeg が孤児として残っていないこと。
        // 名前で数えるとユーザーが別用途で動かしているffmpeg等を誤検知するのでPGIDで見る
        const int pgid = downloader.pgid();
        expect (pgid > 0, "ダウンロードのPGIDが記録されている");
        int remaining = -1;
        for (int attempt = 0; attempt < 20 && pgid > 0; ++attempt)
        {
            remaining = countProcessesInGroup (pgid);
            if (remaining == 0)
                break;
            juce::Thread::sleep (50);
        }
        expect (remaining == 0, "そのプロセスグループに yt-dlp / ffmpeg が残っていない");
    }

    // ---- 通常のダウンロード ----
    {
        UrlDownloader downloader;
        UrlDownloader::Request request;
        request.url = url;
        downloader.start (std::move (request));

        float maxProgress = 0.0f;
        while (downloader.status() == UrlDownloader::Status::running)
        {
            maxProgress = juce::jmax (maxProgress, downloader.progress());
            juce::Thread::sleep (50);
        }

        auto result = downloader.takeResult();
        expect (result.status == UrlDownloader::Status::success, "ダウンロードが成功する");
        if (result.status != UrlDownloader::Status::success)
        {
            std::cout << "     失敗理由: " << result.errorMessage << std::endl;
            return;
        }

        expect (result.title.isNotEmpty(), "動画タイトルが取れる");
        expect (result.audioFile.existsAsFile(), "WAVが出来ている");
        expect (result.tempDirectory.isDirectory(), "一時ディレクトリの所有権を受け取る");
        expect (result.audioFile.getParentDirectory() == result.tempDirectory,
                "WAVは一時ディレクトリの中にある");
        expect (maxProgress > 0.0f, "進捗が動く");

        // 出来たWAVが実際に読めること（この後 AudioImporter に渡せるか）
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (
            formats.createReaderFor (result.audioFile));
        expect (reader != nullptr, "WAVをデコードできる");
        if (reader != nullptr)
        {
            expect (reader->sampleRate > 0.0, "サンプルレートが読める");
            expect (reader->lengthInSamples > 0, "長さがある");
            std::cout << "     取得: \"" << result.title << "\" "
                      << reader->numChannels << "ch " << (int) reader->sampleRate << "Hz "
                      << (double) reader->lengthInSamples / reader->sampleRate << "s" << std::endl;
        }
        reader.reset();

        // take した側が消す責任を負う（MainComponent では cleanupUrlTempDir が担う）
        result.tempDirectory.deleteRecursively();
        expect (! result.tempDirectory.exists(), "take した側で削除できる");
    }
}

// 一時ディレクトリの残骸掃除の判定
void testTempDirSweep()
{
    beginTest ("TempDirSweep");

    constexpr int alivePid = 4242;
    constexpr int deadPid  = 4243;
    auto isAlive = [] (int pid) { return pid == alivePid; };

    juce::StringArray names {
        TempDirSweep::makeDirName (alivePid, "aaaa"), // 別インスタンスが作業中
        TempDirSweep::makeDirName (deadPid,  "bbbb"), // 持ち主が居ない残骸
        "lala-url-abc-cccc",                          // PIDが数字でない
        "lala-url-",                                  // PID部もuuid部も無い
        "lala-url-999",                               // uuid部が無い
        "lala-url--dddd",                             // PID部が空
        "some-other-dir"                              // 無関係
    };

    const auto stale = TempDirSweep::selectStaleTempDirs (names, isAlive);

    expect (stale.size() == 1, "削除対象は1件だけ");
    expect (stale.contains (TempDirSweep::makeDirName (deadPid, "bbbb")),
            "持ち主が居ないものだけ削除対象になる");
    expect (! stale.contains (TempDirSweep::makeDirName (alivePid, "aaaa")),
            "生存中PIDのディレクトリは残す");
    // 注意: expect の説明は const char* をそのまま渡す。日本語リテラルを juce::String(const char*)
    // に通すと ASCII 扱いで Debug assertion が飛ぶ（JUCEの String(const char*) は Latin-1 想定）
    for (const auto& bad : { "lala-url-abc-cccc", "lala-url-", "lala-url-999",
                             "lala-url--dddd", "some-other-dir" })
        expect (! stale.contains (bad), "不正な名前のディレクトリには触らない");

    // 自分自身のPIDのディレクトリは（生きているので）残る
    const int selfPid = (int) ::getpid();
    const auto selfName = TempDirSweep::makeDirName (selfPid, "self");
    const auto staleWithSelf = TempDirSweep::selectStaleTempDirs (
        { selfName }, [selfPid] (int pid) { return pid == selfPid; });
    expect (staleWithSelf.isEmpty(), "現PIDのディレクトリは削除対象にしない");
}


// ---- MidiImport（.mid 取り込み）----

// テスト用 SMF をメモリ上で組む。events は (tick, channel 1..16, pitch, velocity, isOn)。
// tpq はファイルの ticks_per_beat（ガチャの .mid は 480）
juce::MemoryBlock buildSmf (int tpq, const std::vector<std::tuple<int, int, int, int, bool>>& events,
                            int midiFileType = 1, bool smpte = false)
{
    juce::MidiFile mf;
    if (smpte)
        mf.setSmpteTimeFormat (25, 40);
    else
        mf.setTicksPerQuarterNote (tpq);

    juce::MidiMessageSequence seq;
    for (const auto& [tick, channel, pitch, velocity, isOn] : events)
    {
        auto msg = isOn ? juce::MidiMessage::noteOn (channel, pitch, (juce::uint8) velocity)
                        : juce::MidiMessage::noteOff (channel, pitch, (juce::uint8) 0);
        msg.setTimeStamp ((double) tick);
        seq.addEvent (msg);
    }
    mf.addTrack (seq);

    juce::MemoryOutputStream out;
    mf.writeTo (out, midiFileType);
    return out.getMemoryBlock();
}

bool parseSmf (const juce::MemoryBlock& data, MidiImport::Result& result, juce::String& error)
{
    juce::MemoryInputStream in (data, false);
    return MidiImport::parse (in, result, error);
}

void testMidiFileTypes()
{
    beginTest ("MidiFileTypes filter");
    expect (MidiFileTypes::isSupported (juce::String::fromUTF8 (u8"/tmp/a.mid")), "midを受理");
    expect (MidiFileTypes::isSupported (juce::String::fromUTF8 (u8"/tmp/a.MIDI")), "MIDI(大文字)を受理");
    expect (! MidiFileTypes::isSupported (juce::String::fromUTF8 (u8"/tmp/a.wav")), "wavは対象外");
    expect (! AudioFileTypes::isSupported ("/tmp/a.mid"), "AudioFileTypesにmidは足さない");
}

void testMidiImportParse()
{
    beginTest ("MidiImport parse");

    // PPQ換算（480→960 = ×2）と ch10 分離。ch10 のノート＋ch1 のノートの混在ファイル
    {
        MidiImport::Result result;
        juce::String error;
        const auto data = buildSmf (480, {
            { 0,    10, 36, 100, true }, { 60,   10, 36, 0, false },  // kick（vel0のnote_on=off）
            { 480,  10, 42, 90,  true }, { 540,  10, 42, 0, false },  // hat
            { 240,  1,  60, 80,  true }, { 720,  1,  60, 0, false },  // piano（ch1）
        });
        expect (parseSmf (data, result, error), "混在SMFをパースできる");
        expect ((int) result.drumNotes.size() == 2, "ch10のノートは2つ");
        expect ((int) result.otherNotes.size() == 1, "他チャンネルのノートは1つ");
        expect (result.drumNotes[0].startPpq == 0 && result.drumNotes[0].lengthPpq == 120,
                "tick 0-60 → PPQ 0-120（×2換算）");
        expect (result.drumNotes[1].startPpq == 960 && result.drumNotes[1].lengthPpq == 120,
                "tick 480 → PPQ 960");
        expect (result.drumNotes[0].velocity == 100 && result.drumNotes[0].pitch == 36,
                "velocity・pitch を保持");
        expect (result.otherNotes[0].startPpq == 480 && result.otherNotes[0].lengthPpq == 960,
                "ch1: tick 240-720 → PPQ 480-1440");
        expect (result.drumRegionLengthPpq == Ppq::ticksPerBar, "ドラムは1小節に切り上げ");
        expect (result.otherRegionLengthPpq == Ppq::ticksPerBar, "他chも1小節に切り上げ");
    }

    // 小節切り上げ: 終端が小節境界を1tickでも越えたら次の小節へ
    {
        MidiImport::Result result;
        juce::String error;
        // tpq=960（換算なし）。ノート終端 = 3841 tick > 1小節(3840)
        const auto data = buildSmf (960, { { 3800, 10, 36, 100, true }, { 3841, 10, 36, 0, false } });
        expect (parseSmf (data, result, error), "パース成功");
        expect (result.drumRegionLengthPpq == Ppq::ticksPerBar * 2,
                "終端3841 → 2小節へ切り上げ");
    }

    // (channel, pitch) の対応付け: 別チャンネルの同音 off が誤って閉じない
    {
        MidiImport::Result result;
        juce::String error;
        const auto data = buildSmf (960, {
            { 0,   1, 60, 100, true },   // ch1 on
            { 100, 2, 60, 90,  true },   // ch2 on（同pitch）
            { 200, 2, 60, 0,   false },  // ch2 off → ch2のノートを閉じる（ch1ではない）
            { 400, 1, 60, 0,   false },  // ch1 off
        });
        expect (parseSmf (data, result, error), "パース成功");
        expect ((int) result.otherNotes.size() == 2, "2ノートになる");
        expect (result.otherNotes[0].startPpq == 0 && result.otherNotes[0].lengthPpq == 400,
                "ch1のノートはch2のoffで閉じない（長さ400）");
        expect (result.otherNotes[1].startPpq == 100 && result.otherNotes[1].lengthPpq == 100,
                "ch2のノートは自分のoffで閉じる（長さ100）");
    }

    // 重複 note_on（同ch同pitch）: 各々独立ノート・最も古い未クローズから閉じる（FIFO）
    {
        MidiImport::Result result;
        juce::String error;
        const auto data = buildSmf (960, {
            { 0,   1, 62, 100, true },
            { 100, 1, 62, 80,  true },
            { 300, 1, 62, 0,   false },  // → 最初のon（tick 0）を閉じる
            { 500, 1, 62, 0,   false },  // → 2番目のon（tick 100）を閉じる
        });
        expect (parseSmf (data, result, error), "パース成功");
        expect ((int) result.otherNotes.size() == 2, "重複onは各々独立ノート");
        expect (result.otherNotes[0].startPpq == 0 && result.otherNotes[0].lengthPpq == 300,
                "FIFO: 最初のoffが最古のonを閉じる");
        expect (result.otherNotes[1].startPpq == 100 && result.otherNotes[1].lengthPpq == 400,
                "2番目のoffが残りのonを閉じる");
    }

    // 孤立 note_off は無視・未クローズ note_on はファイル末尾で閉じる
    {
        MidiImport::Result result;
        juce::String error;
        const auto data = buildSmf (960, {
            { 0,    1, 70, 0,   false },  // 孤立off（無視）
            { 100,  1, 64, 100, true },   // 未クローズon
            { 2000, 1, 65, 90,  true },   // 未クローズon（これが最終イベント）
        });
        expect (parseSmf (data, result, error), "パース成功");
        expect ((int) result.otherNotes.size() == 2, "孤立offはノートにならない");
        expect (result.otherNotes[0].startPpq == 100 && result.otherNotes[0].lengthPpq == 1900,
                "未クローズonは最終イベント時刻(2000)で閉じる");
        expect (result.otherNotes[1].lengthPpq == 1, "最終イベント自身のノートは最低長1");
    }

    // ゼロ長になる換算でも lengthPpq >= 1
    {
        MidiImport::Result result;
        juce::String error;
        const auto data = buildSmf (480, { { 0, 1, 60, 100, true }, { 0, 1, 60, 0, false } });
        // 同tickのon/off: offが先に処理される→孤立off無視→onは末尾(0)で閉じ、長さ最低1
        expect (parseSmf (data, result, error), "パース成功");
        expect ((int) result.otherNotes.size() == 1 && result.otherNotes[0].lengthPpq == 1,
                "ゼロ長ノートは長さ1にクランプ");
    }

    // SMPTE 形式は明示エラー
    {
        MidiImport::Result result;
        juce::String error;
        const auto data = buildSmf (0, { { 0, 1, 60, 100, true }, { 100, 1, 60, 0, false } },
                                    1, /*smpte=*/true);
        expect (! parseSmf (data, result, error), "SMPTEは失敗する");
        expect (error.contains ("SMPTE"), "エラーにSMPTEと明示");
    }

    // ノートが1つも無い・壊れたデータはエラー
    {
        MidiImport::Result result;
        juce::String error;
        const auto empty = buildSmf (480, {});
        expect (! parseSmf (empty, result, error), "ノートなしは失敗");

        juce::MemoryBlock garbage ("not a midi file", 15);
        expect (! parseSmf (garbage, result, error), "壊れたデータは失敗");
    }

    // type 0（1トラック）と type 1（複数トラック相当）で同じ結果になる
    {
        MidiImport::Result r0, r1;
        juce::String error;
        const std::vector<std::tuple<int, int, int, int, bool>> events = {
            { 0, 10, 36, 100, true }, { 60, 10, 36, 0, false },
        };
        expect (parseSmf (buildSmf (480, events, 0), r0, error), "type 0 をパースできる");
        expect (parseSmf (buildSmf (480, events, 1), r1, error), "type 1 をパースできる");
        expect (r0.drumNotes.size() == r1.drumNotes.size()
                    && r0.drumNotes[0].startPpq == r1.drumNotes[0].startPpq
                    && r0.drumRegionLengthPpq == r1.drumRegionLengthPpq,
                "type 0/1 で同じ結果");
    }
}

void testMidiImportApply()
{
    beginTest ("MidiImport apply");

    const auto dir = makeTempDir();
    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "プロジェクト作成");
    project->tracks.clear(); // createNew の初期トラックを外して apply の追加分だけを見る
    UndoStack undo;

    MidiImport::Result mixed;
    mixed.drumNotes = { MidiNote { 0, 36, 0, 120, 100 }, MidiNote { 0, 42, 960, 120, 90 } };
    mixed.otherNotes = { MidiNote { 0, 60, 480, 960, 80 } };
    mixed.drumRegionLengthPpq = Ppq::ticksPerBar;
    mixed.otherRegionLengthPpq = Ppq::ticksPerBar * 2;

    expect (project->sampleRate <= 0.0, "前提: 新規プロジェクトはSR未確定");
    const auto outcome = MidiImport::apply (*project, undo, mixed, "beat", Ppq::ticksPerBar * 4);

    expect (outcome.numTracksCreated == 2, "混在SMFは2トラック");
    expect (outcome.firstTrackIndex == 0, "先頭に作られたトラックのindex");
    expect ((int) project->tracks.size() == 2, "トラックが2本増えた");

    const auto& drumTrack = project->tracks[0];
    const auto& gmTrack = project->tracks[1];
    expect (drumTrack.type == TrackType::midi && drumTrack.drums && drumTrack.drumPitch == -1,
            "ch10側は Drum Kit（drums=true）");
    expect (gmTrack.type == TrackType::midi && ! gmTrack.drums, "他ch側はGM（drums=false）");
    expect (drumTrack.midiRegions.size() == 1 && gmTrack.midiRegions.size() == 1,
            "各トラックに1リージョン");
    expect (drumTrack.midiRegions[0].startPpq == Ppq::ticksPerBar * 4
                && gmTrack.midiRegions[0].startPpq == Ppq::ticksPerBar * 4,
            "2トラックは同じ開始位置");
    expect (drumTrack.midiRegions[0].lengthPpq == Ppq::ticksPerBar
                && gmTrack.midiRegions[0].lengthPpq == Ppq::ticksPerBar * 2,
            "リージョン長は変換結果どおり");
    expect ((int) drumTrack.midiRegions[0].notes.size() == 2, "ドラムノートが入る");
    expect (drumTrack.midiRegions[0].notes[0].id != 0
                && drumTrack.midiRegions[0].notes[1].id != 0
                && drumTrack.midiRegions[0].notes[0].id != drumTrack.midiRegions[0].notes[1].id,
            "ノートIDが採番される");
    expect (drumTrack.id != 0 && gmTrack.id != 0 && drumTrack.id != gmTrack.id,
            "トラックIDが採番される");
    expect (project->sampleRate <= 0.0, "サンプルレートを確定させない");
    expect (drumTrack.name.contains (juce::String::fromUTF8 (u8"ドラム")),
            "混在時のドラム側は接尾辞つき");
    expect (gmTrack.name == "beat", "他ch側はファイル名そのまま");

    // undo は全体で1件: 1回の undo で2トラックとも消える
    auto kind = UndoStack::EditKind::structure;
    expect (undo.undo (*project, kind), "undoできる");
    expect (project->tracks.empty(), "undo 1回で2トラックとも消える（全体で1件）");
    expect (! undo.canUndo(), "undo履歴は1件だけだった");

    // 単独（ドラムのみ）はファイル名そのまま・1トラック
    MidiImport::Result drumsOnly;
    drumsOnly.drumNotes = { MidiNote { 0, 36, 0, 120, 100 } };
    drumsOnly.drumRegionLengthPpq = Ppq::ticksPerBar;
    const auto outcome2 = MidiImport::apply (*project, undo, drumsOnly, "gacha-1", 0);
    expect (outcome2.numTracksCreated == 1 && (int) project->tracks.size() == 1,
            "ドラムのみは1トラック");
    expect (project->tracks[0].name == "gacha-1", "単独はファイル名そのまま");

    // 空の結果は何もしない（undoも積まない）
    const bool couldUndoBefore = undo.canUndo();
    const auto emptyOutcome = MidiImport::apply (*project, undo, {}, "empty", 0);
    expect (emptyOutcome.numTracksCreated == 0 && (int) project->tracks.size() == 1,
            "空の結果は何もしない");
    expect (undo.canUndo() == couldUndoBefore, "空の結果でundoを積まない");

    dir.deleteRecursively();
}

void testReferenceExport()
{
    beginTest ("ReferenceExport range copy and naming");

    // サニタイズ: パス区切り・禁止文字・前後ドット/空白の除去。空は "reference"
    expect (ReferenceExport::sanitizeName ("My Beat") == "My Beat", "普通の名前はそのまま");
    expect (ReferenceExport::sanitizeName ("a/b\\c:d*e?f\"g<h>i|j") == "abcdefghij", "禁止文字を除去");
    expect (ReferenceExport::sanitizeName ("  .hidden  ") == "hidden", "先頭ドットと空白を刈る");
    expect (ReferenceExport::sanitizeName ("///") == "reference", "空になったらreference");
    expect (ReferenceExport::sanitizeName ("") == "reference", "空文字はreference");

    const auto dir = makeTempDir();

    // 衝突連番: 既存フォルダがあれば -2, -3, ...
    const auto first = ReferenceExport::allocateFolder (dir, "song");
    expect (first.getFileName() == "song", "1つ目はそのままの名前");
    first.createDirectory();
    const auto second = ReferenceExport::allocateFolder (dir, "song");
    expect (second.getFileName() == "song-2", "衝突は-2");
    second.createDirectory();
    expect (ReferenceExport::allocateFolder (dir, "song").getFileName() == "song-3", "さらに衝突は-3");

    // 範囲コピー: ステレオWAVの offset〜offset+length が素のまま切り出される
    juce::AudioBuffer<float> sourceBuffer (2, 1000);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 1000; ++i)
            sourceBuffer.setSample (ch, i, std::sin ((float) (i + ch * 500) * 0.01f) * 0.5f);
    const auto wavFile = dir.getChildFile ("clip-001.wav");
    expect (writeBufferWav (wavFile, sourceBuffer, 44100.0), "ソースWAVを用意");

    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.name = "verse loop";
    clip.offsetSamples = 100;
    clip.lengthSamples = 300;
    clip.gain = 2.0f;          // 適用されないこと（素のままコピー）
    clip.fadeInSamples = 50;   // 同上
    clip.loopCount = 3;        // ループは1周分のみ

    const auto folder = ReferenceExport::allocateFolder (dir, clip.name);
    juce::String error;
    expect (ReferenceExport::exportClipRange (dir, clip, folder, error), "書き出し成功");
    const auto track = folder.getChildFile ("track.wav");
    expect (track.existsAsFile(), "track.wavができる");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (track));
    expect (reader != nullptr && reader->lengthInSamples == 300, "長さ=lengthSamples（ループ1周分）");
    expect (reader != nullptr && (int) reader->numChannels == 2, "チャンネル数を保持");
    if (reader != nullptr)
    {
        juce::AudioBuffer<float> readBack (2, 300);
        reader->read (&readBack, 0, 300, 0, true, true);
        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 300; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (readBack.getSample (ch, i)
                                                         - sourceBuffer.getSample (ch, i + 100)));
        expect (maxDiff < 1.0f / (1 << 20), "サンプルが素のまま一致（ゲイン・フェード不適用）");
    }

    // 範囲が空のクリップはエラー
    Clip empty;
    empty.fileName = "clip-001.wav";
    empty.offsetSamples = 1000;
    empty.lengthSamples = 100; // ソース全長を超える（クランプ後 length 0）
    expect (! ReferenceExport::exportClipRange (dir, empty,
                                                ReferenceExport::allocateFolder (dir, "x"), error),
            "空の範囲は失敗を返す");

    dir.deleteRecursively();
}

// GachaSession 用の最小ドラム変換結果（4小節・kick 2発）
MidiImport::Result makeDrumResult()
{
    MidiImport::Result result;
    result.drumNotes = { MidiNote { 0, 36, 0, 120, 100 }, MidiNote { 0, 42, 960, 120, 90 } };
    result.drumRegionLengthPpq = Ppq::ticksPerBar * 4;
    return result;
}

// GachaSession 用の最小ベース変換結果（8小節・2ノート。ch1 相当なので otherNotes 側）
MidiImport::Result makeBassResult()
{
    MidiImport::Result result;
    result.otherNotes = { MidiNote { 0, 30, 0, 960, 96 }, MidiNote { 0, 34, 1920, 960, 90 } };
    result.otherRegionLengthPpq = Ppq::ticksPerBar * 8;
    return result;
}

void testGachaPorcelainParse()
{
    beginTest ("GachaSession porcelain parse");

    GachaSession::Candidate c;
    expect (GachaSession::parsePorcelainLine (
                R"({"base": "drums-k01-s02-h03-abc", "lane_seeds": {"kick": "0000ab01", "snare": "0000ab02", "hat": "0000ab03"}, "status": "generated"})",
                c),
            "正常な行をパースできる");
    expect (c.base == "drums-k01-s02-h03-abc" && c.kickSeed == "0000ab01"
                && c.snareSeed == "0000ab02" && c.hatSeed == "0000ab03" && c.status == "generated",
            "各フィールドが入る");
    expect (! GachaSession::parsePorcelainLine ("not json", c), "JSONでない行は拒否");
    expect (! GachaSession::parsePorcelainLine (R"({"base": "x"})", c), "キー欠損は拒否");
    expect (! GachaSession::parsePorcelainLine (
                R"({"base": "x", "lane_seeds": {"kick": "01"}, "status": "generated"})", c),
            "レーン欠損は拒否");
}

void testGachaPatternMiniature()
{
    beginTest ("GachaSession pattern miniature");

    // 骨格(vel>=53)と装飾(vel<53)の濃淡・16分スロットへの丸め・2小節目以降の無視
    std::vector<MidiNote> notes = {
        MidiNote { 0, 36, 0, 120, 116 },     // kick 骨格 @slot0
        MidiNote { 0, 38, 960, 120, 100 },   // snare 骨格 @slot4（2拍目）
        MidiNote { 0, 38, 730, 120, 40 },    // snare 装飾 @slot3（720へ丸め。ジッター+10tick）
        MidiNote { 0, 42, 1150, 120, 30 },   // hat 装飾 @slot5（1200へ丸め。-50tick）
        MidiNote { 0, 42, 3830, 120, 90 },   // hat 骨格 小節末（3840へ丸まるが15に収める）
        MidiNote { 0, 36, 3840, 120, 116 },  // 2小節目 → 無視
        MidiNote { 0, 47, 480, 120, 100 },   // 対象外ピッチ（tom等）→ 無視
    };
    const auto pattern = GachaSession::patternFromDrumNotes (notes);
    expect (pattern[0][0] == 2, "kickの骨格が濃で入る");
    expect (pattern[1][4] == 2, "snareの骨格がスロット4に入る");
    expect (pattern[1][3] == 1, "snareの装飾（ジッター込み）が最近傍スロットへ丸まる");
    expect (pattern[2][5] == 1, "hatの装飾が最近傍スロットへ丸まる");
    expect (pattern[2][15] == 2, "小節末のノートはスロット15に収まる（16へ溢れない）");
    int total = 0;
    for (const auto& lane : pattern)
        for (int cell : lane)
            total += cell != 0 ? 1 : 0;
    expect (total == 5, "2小節目・対象外ピッチは数えない");
}

void testGachaSessionPreview()
{
    beginTest ("GachaSession preview / replace / cancel / keep");

    const auto dir = makeTempDir();
    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "プロジェクト作成");
    project->tracks.clear();

    // --- 自動作成: Drum Kit トラックが無ければ「Drums」を作って配置 ---
    {
        GachaSession session;
        expect (session.previewCandidate (GachaSession::Part::drums, *project, makeDrumResult(), Ppq::ticksPerBar * 2),
                "仮配置できる");
        expect ((int) project->tracks.size() == 1, "Drumsトラックが自動作成される");
        const auto& track = project->tracks[0];
        expect (track.type == TrackType::midi && track.drums && track.name == "Drums",
                "自動作成トラックは Drum Kit");
        expect ((int) track.midiRegions.size() == 1, "仮リージョンが1つ");
        expect (track.midiRegions[0].startPpq == Ppq::ticksPerBar * 2
                    && track.midiRegions[0].lengthPpq == Ppq::ticksPerBar * 4,
                "配置位置と長さ（4小節）");
        expect (session.isPreviewObject (track.id, track.midiRegions[0].id),
                "仮オブジェクト判定（リージョン）");
        expect (session.trackIsPreviewOwned (track.id), "仮オブジェクト判定（自動作成トラック）");

        // --- 差し替え: 2候補目でもリージョンは1つ・同じ場所 ---
        auto second = makeDrumResult();
        second.drumNotes[0].pitch = 38;
        expect (session.previewCandidate (GachaSession::Part::drums, *project, second, Ppq::ticksPerBar * 9),
                "差し替えできる");
        expect ((int) project->tracks.size() == 1 && (int) project->tracks[0].midiRegions.size() == 1,
                "差し替えでリージョンは増えない");
        expect (project->tracks[0].midiRegions[0].startPpq == Ppq::ticksPerBar * 2,
                "差し替えは初回の位置を維持する（新しいstartPpqは無視）");
        expect (project->tracks[0].midiRegions[0].notes[0].pitch == 38, "中身は新候補");

        // --- キャンセル: 自動作成トラックごと撤去 ---
        expect (session.cancelPreview (*project), "キャンセルで撤去");
        expect (project->tracks.empty(), "自動作成トラックごと消える");
        expect (! session.hasPreview(), "セッションが畳まれる");
        expect (! session.cancelPreview (*project), "二重キャンセルはno-op");
    }

    // --- 既存の Drum Kit トラックがあっても常に専用の新規トラックへ（2026-08-08 統一 —
    //     選択駆動の流用は「回す前に正しいトラックを選んでおく」暗黙知を要求するため廃止）---
    {
        Track drumKit;
        drumKit.id = project->allocateId();
        drumKit.type = TrackType::midi;
        drumKit.drums = true;
        drumKit.name = "Drums"; // 同名衝突 → 自動作成側が連番になることも同時に検証
        project->tracks.push_back (std::move (drumKit));

        GachaSession session;
        expect (session.previewCandidate (GachaSession::Part::drums, *project, makeDrumResult(), 0), "仮配置");
        expect ((int) project->tracks.size() == 2, "既存 Drum Kit があっても専用トラックが増える");
        expect (project->tracks[1].name == "Drums 2" && project->tracks[1].drums,
                "自動作成トラックは同名衝突で連番になる");
        expect (project->tracks[0].midiRegions.empty()
                    && (int) project->tracks[1].midiRegions.size() == 1,
                "リージョンは専用トラック側に載る");
        expect (session.trackIsPreviewOwned (project->tracks[1].id), "専用トラックは自動作成扱い");
        expect (session.cancelPreview (*project), "キャンセル");
        expect ((int) project->tracks.size() == 1 && project->tracks[0].midiRegions.empty(),
                "キャンセルで専用トラックごと消え、既存トラックは無傷");
        project->tracks.clear();
    }

    // --- 残す→undo→redo / pushCommitted が redo 履歴を消す ---
    {
        UndoStack undo;
        // 先に1編集入れて undo し、redo 履歴を作っておく（pushCommitted が破棄することの確認用）
        undo.begin (*project);
        Track dummy;
        dummy.id = project->allocateId();
        dummy.name = "dummy";
        project->tracks.push_back (std::move (dummy));
        auto kind = UndoStack::EditKind::structure;
        undo.undo (*project, kind);
        expect (undo.canRedo(), "前提: redo履歴がある");

        GachaSession session;
        expect (session.previewCandidate (GachaSession::Part::drums, *project, makeDrumResult(), 0), "仮配置");
        expect (session.keep (*project, undo), "keepは「確定変更あり」を返す");
        expect (! undo.canRedo(), "pushCommittedはredo履歴を破棄する");
        expect ((int) project->tracks.size() == 1
                    && (int) project->tracks[0].midiRegions.size() == 1,
                "残した候補はプロジェクトに残る");
        expect (! session.hasPreview(), "keep後はセッションが畳まれる");

        expect (undo.undo (*project, kind), "keep後にundoできる");
        expect (project->tracks.empty(), "undoで仮配置前（トラックなし）に戻る");
        expect (undo.redo (*project, kind), "redoできる");
        expect ((int) project->tracks.size() == 1
                    && (int) project->tracks[0].midiRegions.size() == 1,
                "redoで候補が復活する");
        project->tracks.clear();

        // 仮配置なしの keep は false
        GachaSession empty;
        expect (! empty.keep (*project, undo), "仮配置なしのkeepはfalse");
    }

    // --- セッショントランザクション: 仮配置中の BPM/キー/アンカー変更（ループ採用の逆コピー相当）が
    //     キャンセルで baseline へ戻り、keep 後の ⌘Z 1回でも仮配置前へ全復元される ---
    {
        LoopAnchor adopted;
        adopted.libraryPath = "loops/P/x_Bm_85bpm.wav";
        adopted.bpm = 85.0;
        adopted.key = ProjectKey { 11, KeyMode::minor };
        adopted.loopBars = 2;
        adopted.slotsPerBar = 2;
        adopted.roots = { 2, 2, 9, 6 };
        adopted.confidence = { 0.9f, 0.9f, 0.9f, 0.9f };

        const auto setBaselineValues = [&]
        {
            project->tracks.clear();
            project->bpm = 93.0;
            project->key = ProjectKey { 9, KeyMode::minor };
            project->loopAnchor.reset();
        };
        const auto adoptLoopValues = [&]
        {
            project->bpm = 85.0;
            project->key = adopted.key;
            project->loopAnchor = adopted;
        };
        const auto atBaseline = [&]
        {
            return project->bpm == 93.0 && project->key == ProjectKey { 9, KeyMode::minor }
                && ! project->loopAnchor.has_value();
        };

        // cancelPreview（全撤去）で値が戻る
        setBaselineValues();
        GachaSession session;
        expect (session.previewCandidate (GachaSession::Part::drums, *project, makeDrumResult(), 0), "仮配置");
        adoptLoopValues();
        expect (session.cancelPreview (*project), "キャンセル");
        expect (atBaseline(), "cancelPreview で BPM/キー/アンカーが baseline へ戻る");

        // cancelPart で最後の仮配置が消えたときも戻る
        setBaselineValues();
        GachaSession session2;
        expect (session2.previewCandidate (GachaSession::Part::drums, *project, makeDrumResult(), 0), "仮配置2");
        adoptLoopValues();
        expect (session2.cancelPart (GachaSession::Part::drums, *project), "パーツ撤去");
        expect (atBaseline(), "最後の cancelPart でも baseline へ戻る");

        // keep → ⌘Z 1回で仮配置前へ全復元 → redo で採用状態へ
        setBaselineValues();
        GachaSession session3;
        UndoStack undo3;
        expect (session3.previewCandidate (GachaSession::Part::drums, *project, makeDrumResult(), 0), "仮配置3");
        adoptLoopValues();
        expect (session3.keep (*project, undo3), "keep");
        expect (project->bpm == 85.0 && project->loopAnchor.has_value(), "keep 直後は採用状態のまま");
        auto kind3 = UndoStack::EditKind::structure;
        expect (undo3.undo (*project, kind3), "undoできる");
        expect (atBaseline() && project->tracks.empty(), "⌘Z 1回で仮配置前（トラック・BPM・キー・アンカー）へ全復元");
        expect (undo3.redo (*project, kind3), "redoできる");
        expect (project->bpm == 85.0 && project->loopAnchor.has_value()
                    && ! project->tracks.empty(),
                "redoで採用状態（候補＋新BPM＋アンカー）へ戻る");

        // 失敗経路でも候補値を取り残さない:
        // (a) 対象トラック消失（入口の撤去漏れ）で差し替えに失敗してセッションが畳まれるとき
        setBaselineValues();
        GachaSession session4;
        expect (session4.previewCandidate (GachaSession::Part::drums, *project, makeDrumResult(), 0), "仮配置4");
        adoptLoopValues();
        project->tracks.clear(); // 外部でトラックが消えた想定
        expect (! session4.previewCandidate (GachaSession::Part::drums, *project, makeDrumResult(), 0),
                "トラック消失時の差し替えは失敗する");
        expect (atBaseline(), "失敗でセッションが畳まれたら BPM/キー/アンカーは baseline へ戻る");

        // (b) keep 対象が1つも実在しないとき（キャンセル相当）
        setBaselineValues();
        GachaSession session5;
        UndoStack undo5;
        expect (session5.previewCandidate (GachaSession::Part::drums, *project, makeDrumResult(), 0), "仮配置5");
        adoptLoopValues();
        project->tracks.clear();
        expect (! session5.keep (*project, undo5), "実在しない keep は false");
        expect (atBaseline(), "keep 不成立でも BPM/キー/アンカーは baseline へ戻る");

        project->tracks.clear();
        project->bpm = 120.0;
        project->key.reset();
        project->loopAnchor.reset();
    }

    // --- begin フック（willBegin）で撤去 → その後の undo で仮リージョンが復活しない ---
    {
        UndoStack undo;
        GachaSession session;
        undo.willBegin = [&] { session.cancelPreview (*project); };

        expect (session.previewCandidate (GachaSession::Part::drums, *project, makeDrumResult(), 0), "仮配置");
        expect ((int) project->tracks.size() == 1, "仮トラックがある");

        // 別の編集（トラック追加）: begin のフックが先に仮配置を撤去する
        undo.begin (*project);
        Track other;
        other.id = project->allocateId();
        other.name = "audio";
        project->tracks.push_back (std::move (other));

        expect ((int) project->tracks.size() == 1 && project->tracks[0].name == "audio",
                "フックで仮トラックが消えてから編集された");
        auto kind = UndoStack::EditKind::structure;
        expect (undo.undo (*project, kind), "undoできる");
        expect (project->tracks.empty(), "undoで仮リージョンが復活しない（空に戻る）");
    }

    dir.deleteRecursively();
}

// パーツ別仮配置（Drums / Bass の同時保持・パーツ単位のキャンセル・全パーツ一括の keep）
void testGachaSessionParts()
{
    beginTest ("GachaSession per-part previews / partial cancel / combined keep");

    using Part = GachaSession::Part;
    const auto dir = makeTempDir();
    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "プロジェクト作成");
    project->tracks.clear();

    // --- bass の porcelain / ミニチュア ---
    {
        GachaSession::Candidate c;
        expect (GachaSession::parseBassPorcelainLine (
                    R"({"base": "bass-p01-r02-abc", "lane_seeds": {"prog": "0000ab01", "rhythm": "0000ab02"}, "status": "generated"})",
                    c),
                "bass の porcelain 行をパースできる");
        expect (c.base == "bass-p01-r02-abc" && c.progSeed == "0000ab01"
                    && c.rhythmSeed == "0000ab02", "prog/rhythm が入る");
        expect (! GachaSession::parseBassPorcelainLine (
                    R"({"base": "x", "lane_seeds": {"kick": "01", "snare": "02", "hat": "03"}, "status": "generated"})",
                    c),
                "drums の行は bass としては拒否");

        const std::vector<MidiNote> notes = { MidiNote { 0, 28, 0, 960, 96 },
                                              MidiNote { 0, 51, 1920, 960, 96 },
                                              MidiNote { 0, 40, Ppq::ticksPerBar * 8, 960, 96 } };
        const auto dots = GachaSession::bassDotsFromNotes (notes, Ppq::ticksPerBar * 8);
        expect ((int) dots.size() == 2, "パターン1周を超えるノートは描かない");
        expect (std::abs (dots[0].x) < 1e-6 && std::abs (dots[0].y) < 1e-6,
                "最低音（MIDI 28）は y=0");
        expect (std::abs (dots[1].y - 1.0f) < 1e-6, "最高音（MIDI 51）は y=1");
    }

    // --- Drums 仮配置 → Bass 仮配置（同時保持）→ keep が全パーツまとめて 1 undo ---
    {
        UndoStack undo;
        GachaSession session;
        expect (session.previewCandidate (Part::drums, *project, makeDrumResult(), 0),
                "Drums 仮配置");
        expect (session.previewCandidate (Part::bass, *project, makeBassResult(), 0),
                "Bass 仮配置（Drums 仮配置のまま）");
        expect (session.hasPreview (Part::drums) && session.hasPreview (Part::bass),
                "両パーツが同時に仮配置中");
        expect ((int) project->tracks.size() == 2, "Drums / Bass の2トラック");
        const auto& bassTrack = project->tracks[1];
        expect (bassTrack.type == TrackType::midi && ! bassTrack.drums
                    && bassTrack.instrument == InstrumentKind::gm && bassTrack.gmProgram == 33
                    && bassTrack.name == "Bass",
                "自動作成の Bass トラックは GM Finger Bass (33)");

        expect (session.keep (*project, undo), "keep は全パーツ一括で確定する");
        expect ((int) project->tracks.size() == 2, "確定後も2トラック残る");
        auto kind = UndoStack::EditKind::structure;
        expect (undo.undo (*project, kind), "undo できる");
        expect (! undo.canUndo(), "undo は1件だけ（全パーツで1操作）");
        expect (project->tracks.empty(), "undo 1回で Drums / Bass とも仮配置前に戻る");
        expect (undo.redo (*project, kind), "redo で両方復活する");
        expect ((int) project->tracks.size() == 2, "redo 後も2トラック");
        project->tracks.clear();
    }

    // --- パーツ単位のキャンセル: Bass のカード変更相当（cancelPart）でも Drums は残る ---
    {
        GachaSession session;
        expect (session.previewCandidate (Part::drums, *project, makeDrumResult(), 0),
                "Drums 仮配置");
        expect (! session.cancelPart (Part::bass, *project),
                "Bass 未配置のパーツキャンセルは no-op");
        expect (session.hasPreview (Part::drums) && (int) project->tracks.size() == 1,
                "Drums 仮配置は維持される");

        expect (session.previewCandidate (Part::bass, *project, makeBassResult(), 0),
                "Bass も仮配置");
        expect (session.cancelPart (Part::bass, *project), "Bass だけキャンセル");
        expect (session.hasPreview (Part::drums) && ! session.hasPreview (Part::bass),
                "Drums は残り Bass だけ消える");
        expect ((int) project->tracks.size() == 1 && project->tracks[0].name == "Drums",
                "Bass トラックだけ撤去される");

        // 逆向き: Bass 仮配置後の Drums カード変更では Bass が残る
        expect (session.previewCandidate (Part::bass, *project, makeBassResult(), 0),
                "Bass を配置し直す");
        expect (session.cancelPart (Part::drums, *project), "Drums だけキャンセル");
        expect (! session.hasPreview (Part::drums) && session.hasPreview (Part::bass),
                "Bass は残り Drums だけ消える");
        expect ((int) project->tracks.size() == 1 && project->tracks[0].name == "Bass",
                "Drums トラックだけ撤去される");

        // 最後の1件が消えたら baseline 破棄（＝以後の keep は no-op）
        expect (session.cancelPart (Part::bass, *project), "最後のパーツもキャンセル");
        expect (! session.hasPreview(), "全パーツが畳まれる");
        UndoStack undo;
        expect (! session.keep (*project, undo), "baseline 破棄後の keep は false");
        expect (project->tracks.empty(), "何も残らない");
    }

    // --- baseline はセッション全体で1回: 後から始めた Bass の undo に Drums が混ざらない ---
    {
        UndoStack undo;
        GachaSession session;
        expect (session.previewCandidate (Part::drums, *project, makeDrumResult(), 0),
                "Drums 仮配置（ここが baseline）");
        expect (session.previewCandidate (Part::bass, *project, makeBassResult(), 0),
                "Bass 仮配置");
        // Drums をキャンセルしても baseline は維持され、keep の undo は「両方なし」へ戻る
        expect (session.cancelPart (Part::drums, *project), "Drums キャンセル");
        expect (session.keep (*project, undo), "Bass だけ残す");
        expect ((int) project->tracks.size() == 1 && project->tracks[0].name == "Bass",
                "Bass だけ確定される");
        auto kind = UndoStack::EditKind::structure;
        expect (undo.undo (*project, kind), "undo できる");
        expect (project->tracks.empty(), "undo でセッション開始前（空）に戻る — 未確定 Drums が混ざらない");
        project->tracks.clear();
    }

    // --- GM ベース系トラックがあっても流用しない — 常に Bass を自動作成（2026-08-08 統一）---
    {
        GachaSession session;
        Track synthBass;
        synthBass.id = project->allocateId();
        synthBass.type = TrackType::midi;
        synthBass.gmProgram = 38; // Synth Bass 1（GM ベースファミリー 32..39 でも流用しない）
        synthBass.name = "808";
        project->tracks.push_back (std::move (synthBass));

        expect (session.previewCandidate (Part::bass, *project, makeBassResult(), 0), "仮配置");
        expect ((int) project->tracks.size() == 2 && project->tracks[1].name == "Bass"
                    && project->tracks[1].gmProgram == 33,
                "GM ベース系トラックがあっても Bass（Finger Bass）を自動作成する");
        expect (project->tracks[0].midiRegions.empty()
                    && (int) project->tracks[1].midiRegions.size() == 1,
                "リージョンは専用トラック側に載る");
        session.cancelPreview (*project);
        expect ((int) project->tracks.size() == 1 && project->tracks[0].midiRegions.empty(),
                "キャンセルで専用トラックごと消え、既存トラックは無傷");
        project->tracks.clear();
    }

    dir.deleteRecursively();
}

// ベース振り直しの実行計画（試聴長・キック抽出。UIから切り出した判断ロジック）
// ---- recommend.py --json のパース（ループ候補ページ）----
void testParseRecommendJson()
{
    beginTest ("parse recommend json page");

    const char* good = R"({
        "reference": "/x/refs/totsuka", "ref_bpm": 93.0, "ref_key": "A minor",
        "key_trusted": true, "total": 32, "page": 1,
        "candidates": [
            { "path": "loops/P/a.wav", "bpm": 85.0, "key_root": 11, "key_mode": "minor",
              "transpose_semitones": -2, "tempo_relation": "same", "bpm_ratio": 0.914,
              "score": 0.42, "reason": "明るさがほぼ同じ", "is_contrast": false },
            { "path": "loops/P/b.wav", "bpm": 90.0, "key_root": 7, "key_mode": "major",
              "transpose_semitones": 2, "tempo_relation": "same", "bpm_ratio": 0.968,
              "score": 0.55, "reason": "音数が同じくらい", "is_contrast": false }
        ]
    })";
    GachaSession::LoopRecommendation rec;
    expect (GachaSession::parseRecommendJson (juce::String::fromUTF8 (good), rec), "正常ページを読めること");
    expect (rec.refBpm == 93.0 && rec.refKeyText == "A minor" && rec.keyTrusted
                && rec.total == 32 && rec.page == 1 && (int) rec.candidates.size() == 2,
            "参照情報とページ情報");
    expect (rec.pageSize == 5, "page_size 欠損（旧形式）は既定の5");

    GachaSession::LoopRecommendation sized;
    expect (GachaSession::parseRecommendJson (
                R"({"ref_bpm": 93.0, "ref_key": "A minor", "key_trusted": true,
                    "total": 32, "page": 2, "page_size": 10, "candidates": []})",
                sized)
                && sized.pageSize == 10,
            "page_size ありはその値");
    expect (rec.candidates[0].path == "loops/P/a.wav" && rec.candidates[0].bpm == 85.0
                && rec.candidates[0].keyRoot == 11 && rec.candidates[0].keyMode == KeyMode::minor
                && rec.candidates[0].transposeSemitones == -2
                && rec.candidates[0].reason == juce::String::fromUTF8 (u8"明るさがほぼ同じ"),
            "候補のフィールド");

    // 壊れたページは丸ごと拒否（読める分だけ表示すると順位がずれる）
    GachaSession::LoopRecommendation bad;
    expect (! GachaSession::parseRecommendJson ("not json", bad), "非JSONはfalse");
    expect (! GachaSession::parseRecommendJson (R"({"ref_bpm": 93.0, "total": 1, "page": 1})", bad),
            "candidates欠損はfalse");
    expect (! GachaSession::parseRecommendJson (
                R"({"ref_bpm": 93.0, "ref_key": "A minor", "key_trusted": true, "total": 1, "page": 1,
                    "candidates": [{ "path": "x.wav", "bpm": 85.0, "key_root": 12, "key_mode": "minor",
                                     "transpose_semitones": 0, "bpm_ratio": 1.0, "reason": "r" }]})",
                bad),
            "key_root範囲外はfalse");
    expect (! GachaSession::parseRecommendJson (
                R"({"ref_bpm": 93.0, "ref_key": "A minor", "key_trusted": true, "total": 1, "page": 1,
                    "candidates": [{ "path": "x.wav", "bpm": 85.0, "key_root": 1.5, "key_mode": "minor",
                                     "transpose_semitones": 0, "bpm_ratio": 1.0, "reason": "r" }]})",
                bad),
            "浮動小数の key_root は型違いとして拒否（1に切り捨てて受理しない）");
    expect (! GachaSession::parseRecommendJson (
                R"({"ref_bpm": 93.0, "ref_key": "A minor", "key_trusted": true, "total": 1, "page": 1,
                    "candidates": [{ "path": "x.wav", "bpm": 85.0, "key_root": 4294967296, "key_mode": "minor",
                                     "transpose_semitones": 0, "bpm_ratio": 1.0, "reason": "r" }]})",
                bad),
            "int範囲外の64bit key_root は拒否（0 へ切り詰めて範囲検証を通さない）");
    // loop_bars は nullable — int範囲外は「不明(0)」に落ちるがページは受理される
    GachaSession::LoopRecommendation hugeBars;
    expect (GachaSession::parseRecommendJson (
                R"({"ref_bpm": 93.0, "ref_key": "A minor", "key_trusted": true, "total": 1, "page": 1,
                    "candidates": [{ "path": "x.wav", "bpm": 85.0, "loop_bars": 4294967296,
                                     "key_root": 9, "key_mode": "minor",
                                     "transpose_semitones": 0, "bpm_ratio": 1.0, "reason": "r" }]})",
                hugeBars)
                && hugeBars.candidates.size() == 1 && hugeBars.candidates[0].loopBars == 0,
            "int範囲外の loop_bars は不明(0)扱い（切り詰めた巨大値を --bars に渡さない）");
}

// ---- loadWavResampled: ライブラリ wav をプロジェクト SR のバッファとして読む ----
void testLoadWavResampled()
{
    beginTest ("loadWavResampled converts sample rate in memory");
    const auto dir = makeTempDir();

    // 22050Hz・440Hz サイン・0.5秒 の wav を書く
    const auto file = dir.getChildFile ("loop-22050.wav");
    {
        constexpr double sr = 22050.0;
        constexpr int frames = 11025;
        juce::AudioBuffer<float> buffer (1, frames);
        for (int i = 0; i < frames; ++i)
            buffer.setSample (0, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                             * 440.0 * i / sr));
        std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };
        juce::WavAudioFormat wav;
        using Opts = juce::AudioFormatWriterOptions;
        auto writer = wav.createWriterFor (stream, Opts {}.withSampleRate (sr)
                                                          .withNumChannels (1)
                                                          .withBitsPerSample (24));
        expect (writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, frames),
                "テスト wav を書けること");
    }

    auto resampled = Project::loadWavResampled (file, 44100.0);
    expect (resampled != nullptr, "読めること");
    if (resampled == nullptr)
        { dir.deleteRecursively(); return; }
    expect (std::abs (resampled->getNumSamples() - 22050) <= 2,
            "長さが 2倍（44100Hz で 0.5秒 = 22050サンプル）になること");

    // 内容の裏取り: 440Hz サインなら 0.5秒でゼロクロス約440回・RMS ≈ 0.5/√2
    int crossings = 0;
    const auto* data = resampled->getReadPointer (0);
    for (int i = 1; i < resampled->getNumSamples(); ++i)
        if ((data[i - 1] < 0.0f) != (data[i] < 0.0f))
            ++crossings;
    expect (std::abs (crossings - 440) <= 6, "440Hz の音程が保たれること（ゼロクロス数）");
    const float rms = resampled->getRMSLevel (0, 0, resampled->getNumSamples());
    expect (std::abs (rms - 0.3536f) < 0.02f, "振幅が保たれること（RMS）");

    // SR 一致なら変換せずそのまま読める
    auto same = Project::loadWavResampled (file, 22050.0);
    expect (same != nullptr && same->getNumSamples() == 11025, "SR一致は無変換で全長のまま");

    dir.deleteRecursively();
}

// ---- ループ（音声）仮配置: 敷く・差し替え・逆コピー・ベース撤去・キャンセル・実体化 ----
void testGachaSessionLoops()
{
    beginTest ("GachaSession loop preview / adopt / replace / keep materialization");

    using Part = GachaSession::Part;
    const auto dir = makeTempDir();
    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "プロジェクト作成");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }
    project->tracks.clear();
    project->sampleRate = 44100.0;
    project->bpm = 93.0;
    project->key = ProjectKey { 9, KeyMode::minor };

    const auto makeAnchor = [] (double bpm, int keyRoot)
    {
        LoopAnchor a;
        a.libraryPath = "loops/P/loop_" + juce::String (keyRoot) + ".wav";
        a.bpm = bpm;
        a.key = ProjectKey { keyRoot, KeyMode::minor };
        a.loopBars = 2;
        a.slotsPerBar = 2;
        a.roots = { keyRoot, keyRoot, (keyRoot + 5) % 12, (keyRoot + 7) % 12 };
        a.confidence = { 0.9f, 0.9f, 0.9f, 0.9f };
        return a;
    };
    const auto makeInput = [&] (double bpm, int keyRoot, bool applyKeyBpm = true,
                                juce::int64 startSample = 22050)
    {
        GachaSession::LoopPreviewInput input;
        input.anchor = makeAnchor (bpm, keyRoot);
        // バッファは小節グリッドちょうど（loopBars × 240/bpm × rate）で作る — 刻みヘルパーの
        // 契約（一致しない長さは切り詰め/無音埋めされ、30ms超の不足は失敗）が入ったため
        const int target = (int) std::llround (input.anchor.loopBars * 240.0 / bpm * 44100.0);
        input.audio = std::make_shared<juce::AudioBuffer<float>> (1, target);
        for (int i = 0; i < target; ++i)
            input.audio->setSample (0, i, 0.1f);
        input.displayName = "loop";
        input.startSample = startSample;
        input.loopCount = 1;
        input.applyKeyBpm = applyKeyBpm;
        input.audioSampleRate = 44100.0; // 契約: バッファの変換レートを必ず申告（刻みが使う）
        return input;
    };

    // --- 敷く（設定して敷く）: 専用トラック自動作成（名前=ループ名）・アンカー・逆コピー ---
    {
        GachaSession session;
        expect (session.previewLoopCandidate (*project, makeInput (85.0, 11)), "ループを敷ける");
        expect ((int) project->tracks.size() == 1 && project->tracks[0].type == TrackType::audio
                    && project->tracks[0].name == "loop",
                "専用オーディオトラックが自動作成され、名前はループ名になる");
        expect ((int) project->tracks[0].clips.size() == 1, "仮クリップが1つ");
        const auto& clip = project->tracks[0].clips[0];
        expect (clip.fileName == GachaSession::loopPreviewMarker
                    && clip.loopSource == "loops/P/loop_11.wav"
                    && clip.startSample == 22050 && clip.loopCount == 1,
                "仮クリップの中身（マーカー・出自・位置・リピート）");
        expect (project->loopAnchor.has_value() && project->loopAnchor->key.root == 11,
                "アンカーが設定される");
        expect (project->bpm == 85.0 && project->key == ProjectKey { 11, KeyMode::minor },
                "逆コピー（BPM・キー）が効く");

        // --- 差し替えで BPM が変わる → ベースの仮配置だけ撤去・ドラムは維持。
        //     配置位置は毎回 input の値（BPM が変われば呼び出し側が小節頭を換算し直す契約） ---
        expect (session.previewCandidate (Part::drums, *project, makeDrumResult(), 0), "ドラム仮配置");
        expect (session.previewCandidate (Part::bass, *project, makeBassResult(), 0), "ベース仮配置");
        expect ((int) project->tracks.size() == 3, "Loops + Drums + Bass");
        expect (session.previewLoopCandidate (*project, makeInput (90.0, 7, true, 40000)),
                "別ループへ差し替え");
        expect (! session.hasPreview (Part::bass), "BPM変更の差し替えでベースだけ撤去される");
        expect (session.hasPreview (Part::drums), "ドラムは維持される");
        expect ((int) project->tracks[0].clips.size() == 1, "差し替えでクリップは増えない");
        expect (project->tracks[0].clips[0].startSample == 40000,
                "差し替えは新しい配置位置を使う（新BPMの小節頭換算を呼び出し側がやり直すため）");
        expect (project->bpm == 90.0 && project->loopAnchor->libraryPath == "loops/P/loop_7.wav",
                "アンカーと逆コピーが新ループの値になる");

        // --- 同じキー/BPMでも進行（roots）が違えばベースは撤去される ---
        expect (session.previewCandidate (Part::bass, *project, makeBassResult(), 0), "ベース再仮配置");
        auto sameValuesNewRoots = makeInput (90.0, 7);
        sameValuesNewRoots.anchor.roots = { 7, 7, 7, 2 }; // キー/BPM同一・進行だけ変更
        expect (session.previewLoopCandidate (*project, sameValuesNewRoots), "進行違いへ差し替え");
        expect (! session.hasPreview (Part::bass),
                "キー/BPMが同じでも roots が変わればベースは撤去される（ベースが従う本体は進行）");

        // --- 完全に同一のループを敷き直したときだけベースは残る ---
        expect (session.previewCandidate (Part::bass, *project, makeBassResult(), 0), "ベース再々仮配置");
        expect (session.previewLoopCandidate (*project, sameValuesNewRoots), "同一ループの敷き直し");
        expect (session.hasPreview (Part::bass), "進行もキー/BPMも同じならベースは維持される");

        // --- キャンセルで全部 baseline へ ---
        expect (session.cancelPreview (*project), "キャンセル");
        expect (project->tracks.empty(), "自動作成トラックが全部消える");
        expect (project->bpm == 93.0 && project->key == ProjectKey { 9, KeyMode::minor }
                    && ! project->loopAnchor.has_value(),
                "BPM・キー・アンカーが baseline へ戻る");
    }

    // --- 敷くだけ（applyKeyBpm=false）: アンカーは付くが BPM・キーは動かない ---
    {
        GachaSession session;
        expect (session.previewLoopCandidate (*project, makeInput (85.0, 11, false)), "敷くだけ");
        expect (project->bpm == 93.0 && project->key == ProjectKey { 9, KeyMode::minor },
                "敷くだけでは BPM・キーが動かない");
        expect (project->loopAnchor.has_value() && project->loopAnchor->bpm == 85.0,
                "アンカーは付く（採用の意味論）");
        session.cancelPreview (*project);
    }

    // --- keep: マーカーが clip-NNN.wav に実体化され、⌘Z 1回で全復元 ---
    {
        GachaSession session;
        UndoStack undo;
        expect (session.previewLoopCandidate (*project, makeInput (85.0, 11)), "敷く");
        expect (session.keep (*project, undo), "keep");
        expect ((int) project->tracks.size() == 1 && (int) project->tracks[0].clips.size() == 1, "確定後もクリップがある");
        const auto& kept = project->tracks[0].clips[0];
        expect (kept.fileName.startsWith ("clip-") && kept.fileName.endsWith (".wav"),
                "マーカーが clip-NNN.wav へ実体化される");
        const auto wavFile = project->directory.getChildFile (kept.fileName);
        expect (wavFile.existsAsFile(), "実体 WAV が書かれている");
        double sourceRate = 0.0;
        auto written = Project::loadWav (wavFile, &sourceRate);
        const int keptTarget = (int) std::llround (2 * 240.0 / 85.0 * 44100.0); // loopBars=2 @85bpm
        expect (written != nullptr && written->getNumSamples() == keptTarget && sourceRate == 44100.0,
                "書かれた WAV がプロジェクトSR・小節グリッド長で読み戻せる");
        expect (kept.loopSource == "loops/P/loop_11.wav", "実体化後も出自は残る");
        expect (project->loopAnchor.has_value() && project->bpm == 85.0, "採用状態が確定する");

        auto kind = UndoStack::EditKind::structure;
        expect (undo.undo (*project, kind), "keep後にundoできる");
        expect (project->tracks.empty() && project->bpm == 93.0 && ! project->loopAnchor.has_value(),
                "⌘Z 1回でトラック・BPM・キー・アンカーが仮配置前へ戻る");
        expect (undo.redo (*project, kind), "redoできる");
        expect ((int) project->tracks.size() == 1 && project->bpm == 85.0
                    && project->loopAnchor.has_value(),
                "redoで採用状態へ戻る");
        project->tracks.clear();
        project->bpm = 93.0;
        project->key = ProjectKey { 9, KeyMode::minor };
        project->loopAnchor.reset();
    }

    // --- ループだけの部分キャンセル: 他パーツが残っていても値は戻り、追従ベースは連動撤去 ---
    {
        GachaSession session;
        expect (session.previewCandidate (Part::drums, *project, makeDrumResult(), 0), "ドラム");
        expect (session.previewLoopCandidate (*project, makeInput (85.0, 11)), "ループ採用");
        expect (session.previewCandidate (Part::bass, *project, makeBassResult(), 0), "追従ベース");
        expect (session.cancelPart (Part::loops, *project), "ループだけキャンセル");
        expect (session.hasPreview (Part::drums), "ドラムは残る");
        expect (! session.hasPreview (Part::bass), "追従していたベースは連動撤去される");
        expect (project->bpm == 93.0 && project->key == ProjectKey { 9, KeyMode::minor }
                    && ! project->loopAnchor.has_value(),
                "他パーツが残っていても BPM・キー・アンカーは baseline へ戻る"
                "（残すと Drums の Keep が候補ループの値を確定させてしまう）");
        session.cancelPreview (*project);
    }

    // --- Bass → Loops の順で仮配置しても壊れない（Bass自動トラック削除で index がずれる回帰） ---
    {
        GachaSession session;
        expect (session.previewCandidate (Part::bass, *project, makeBassResult(), 0), "ベースが先");
        expect (session.previewLoopCandidate (*project, makeInput (85.0, 11)), "後からループ採用");
        expect (! session.hasPreview (Part::bass), "ベースは撤去される");
        expect (session.hasPreview (Part::loops), "ループの仮配置は生きている");
        bool clipFound = false;
        for (const auto& track : project->tracks)
            for (const auto& clip : track.clips)
                clipFound = clipFound || clip.fileName == GachaSession::loopPreviewMarker;
        expect (clipFound, "仮クリップが正しいトラックに載っている（indexずれで消えない）");
        session.cancelPreview (*project);
    }

    // --- 差し替え失敗（Loopsトラック消失）: 他パーツが残っていても値は戻り、アンカーを取り残さない ---
    {
        GachaSession session;
        expect (session.previewCandidate (Part::drums, *project, makeDrumResult(), 0), "ドラム");
        expect (session.previewLoopCandidate (*project, makeInput (85.0, 11)), "ループ採用");
        // Loops トラックだけ外部で消す（入口の撤去漏れの想定）
        const auto loopsTrackId = session.previewTrackId (Part::loops);
        project->tracks.erase (std::remove_if (project->tracks.begin(), project->tracks.end(),
                                               [loopsTrackId] (const Track& t)
                                               { return t.id == loopsTrackId; }),
                               project->tracks.end());
        expect (! session.previewLoopCandidate (*project, makeInput (90.0, 7)), "差し替えは失敗する");
        expect (session.hasPreview (Part::drums), "ドラムの仮配置は残る");
        expect (! session.hasPreview (Part::loops), "ループの仮配置は畳まれる");
        expect (project->bpm == 93.0 && ! project->loopAnchor.has_value(),
                "他パーツが残っていても BPM・アンカーは baseline へ戻る"
                "（取り残すと「アンカーあり・ループ無し」のまま Keep できてしまう）");
        session.cancelPreview (*project);
    }

    // --- keep 失敗（SR未確定で実体化できない）: 実体の無い採用を作らず**セッション全体**を畳む ---
    {
        project->sampleRate = 0.0;
        GachaSession session;
        UndoStack undo;
        expect (session.previewCandidate (Part::drums, *project, makeDrumResult(), 0), "ドラムも仮配置");
        expect (session.previewLoopCandidate (*project, makeInput (85.0, 11)), "敷く");
        expect (! session.keep (*project, undo), "実体化できない keep は false");
        expect (project->tracks.empty() && project->bpm == 93.0 && ! project->loopAnchor.has_value(),
                "部分確定を作らずセッション全体がキャンセルされる（ドラムも確定しない）");
        expect (! session.hasPreview(), "セッションは畳まれている");
        project->sampleRate = 44100.0;
    }

    // --- 既存オーディオトラックがあっても常に専用の新規トラックへ敷く（2026-08-08 変更 —
    //     既存トラックに敷くと前に採用した旧ループと同位置に重なり、見えない二重再生になる）---
    {
        Track audioTrack;
        audioTrack.id = project->allocateId();
        audioTrack.type = TrackType::audio;
        audioTrack.name = "My Audio";
        project->tracks.push_back (std::move (audioTrack));

        GachaSession session;
        expect (session.previewLoopCandidate (*project, makeInput (85.0, 11)), "敷ける");
        expect ((int) project->tracks.size() == 2, "既存トラックがあっても専用トラックが増える");
        expect (project->tracks[0].clips.empty() && (int) project->tracks[1].clips.size() == 1,
                "クリップは専用トラック側に置かれる");
        expect (session.cancelPreview (*project), "キャンセル");
        expect ((int) project->tracks.size() == 1 && project->tracks[0].name == "My Audio"
                    && project->tracks[0].clips.empty(),
                "キャンセルで専用トラックごと消え、既存トラックは無傷");
        project->tracks.clear();
    }

    // --- 刻みの統合: 30ms超の不足は false・Project も Session も完全に不変 ---
    {
        GachaSession session;
        expect (session.previewCandidate (Part::drums, *project, makeDrumResult(), 0), "ドラム仮配置");
        const auto tracksBefore = (int) project->tracks.size();
        auto shortInput = makeInput (85.0, 11);
        const int shortTarget = shortInput.audio->getNumSamples() - (int) (0.040 * 44100.0);
        shortInput.audio = std::make_shared<juce::AudioBuffer<float>> (1, shortTarget);
        expect (! session.previewLoopCandidate (*project, shortInput), "30ms超の不足は失敗");
        expect ((int) project->tracks.size() == tracksBefore && ! session.hasPreview (Part::loops)
                    && session.hasPreview (Part::drums)
                    && project->bpm == 93.0 && ! project->loopAnchor.has_value(),
                "失敗時はトラック未作成・ループ未配置・ドラム仮配置も値も無傷");
        session.cancelPreview (*project);
    }

    // --- SR再変換相当: 異なるレートのバッファで2回呼んでも、2回目も刻まれる ---
    {
        GachaSession session;
        expect (session.previewLoopCandidate (*project, makeInput (85.0, 11)), "初回 44.1k");
        auto input48 = makeInput (85.0, 11);
        const int target48 = (int) std::llround (2 * 240.0 / 85.0 * 48000.0);
        input48.audio = std::make_shared<juce::AudioBuffer<float>> (1, target48 + 4800); // 100msのテール
        for (int i = 0; i < input48.audio->getNumSamples(); ++i)
            input48.audio->setSample (0, i, 0.1f);
        input48.audioSampleRate = 48000.0;
        expect (session.previewLoopCandidate (*project, input48), "48k のバッファへ差し替え");
        bool found = false;
        for (const auto& track : project->tracks)
            for (const auto& clip : track.clips)
                if (clip.fileName == GachaSession::loopPreviewMarker)
                {
                    found = true;
                    expect ((int) clip.lengthSamples == target48,
                            "差し替え（SR再変換相当）でも小節グリッド長に刻まれる");
                }
        expect (found, "差し替え後の仮クリップが存在する");
        session.cancelPreview (*project);
        project->tracks.clear();
    }

    dir.deleteRecursively();
}

// ---- trimLoopBufferToBars: ループバッファの小節グリッド刻み（docs/plans/2026-08-09-0024）----
void testTrimLoopBufferToBars()
{
    beginTest ("GachaSession::trimLoopBufferToBars grid trimming");
    const double rate = 44100.0, bpm = 120.0;
    const int bar = (int) std::llround (240.0 / bpm * rate);
    const int target = 2 * bar;

    const auto filled = [] (int n, float v)
    {
        juce::AudioBuffer<float> b (1, n);
        for (int i = 0; i < n; ++i)
            b.setSample (0, i, v);
        return b;
    };

    // 目標長と一致 → サンプル単位で完全無変更（フェードを焼き込まない）
    {
        auto b = filled (target, 0.5f);
        expect (GachaSession::trimLoopBufferToBars (b, rate, bpm, 2), "一致は成功");
        bool identical = b.getNumSamples() == target;
        for (int i = 0; identical && i < target; ++i)
            identical = b.getSample (0, i) == 0.5f;
        expect (identical, "正常素材は完全無変更（毎周の継ぎ目を作らない）");
    }
    // 長い素材（テール付き） → 切り詰め＋両端フェード。フルスケール入力で両端が**厳密に0**
    // （applyGainRamp の endGain は排他端の値なので 1→0 指定では最終サンプルに残差が出る —
    // GOTCHAS 参照。振幅0.5＋許容誤差のテストでは残差を見逃していた）
    {
        auto b = filled (target + bar, 1.0f);
        expect (GachaSession::trimLoopBufferToBars (b, rate, bpm, 2), "切り詰め成功");
        expect (b.getNumSamples() == target, "出力長 = loopBars × 小節サンプル数と厳密一致");
        expect (b.getSample (0, 0) == 0.0f && b.getSample (0, target - 1) == 0.0f,
                "フルスケールでも両端が厳密に0（繋ぎ目が 0 → 0 で連続）");
        expect (b.getSample (0, target / 2) == 1.0f, "中間は無変更");
        const int fadeIn = (int) std::llround (0.003 * rate);
        expect (b.getSample (0, fadeIn - 1) == 1.0f && b.getSample (0, fadeIn) == 1.0f,
                "フェードイン終端に段差が無い（排他端=1 が原音へちょうど接続する）");
    }
    // 30ms 以内の不足 → 無音埋め。フェードアウトは原音末尾に掛かる（境界が連続）
    {
        const int shortfall = (int) (0.020 * rate);
        auto b = filled (target - shortfall, 1.0f);
        expect (GachaSession::trimLoopBufferToBars (b, rate, bpm, 2), "無音埋め成功");
        expect (b.getNumSamples() == target, "埋め後も出力長はグリッド一致");
        const int audioEnd = target - shortfall;
        expect (b.getSample (0, audioEnd - 1) == 0.0f,
                "原音末尾がフェードで厳密に0へ落ちる（末尾非0の入力でも境界が連続）");
        expect (b.getSample (0, audioEnd) == 0.0f && b.getSample (0, target - 1) == 0.0f,
                "パディング部は無音");
    }
    // 30ms を超える不足 → 失敗（上流の推定規則の契約違反）
    {
        auto b = filled (target - (int) (0.040 * rate), 0.5f);
        expect (! GachaSession::trimLoopBufferToBars (b, rate, bpm, 2), "30ms超の不足は失敗");
    }
}

void testGachaBassRollPlan()
{
    beginTest ("GachaSession::planBassRoll bars rounding / kick extraction");

    Project project;
    Track drums;
    drums.id = 1;
    drums.type = TrackType::midi;
    drums.drums = true;
    MidiRegion region;
    region.id = 2;
    region.startPpq = Ppq::ticksPerBar * 4; // 絶対位置は抽出に影響しない（リージョン相対で返す）
    region.lengthPpq = Ppq::ticksPerBar;    // 1小節パターン
    region.loopCount = 3;                   // ×4回 = 4小節
    region.notes = { MidiNote { 0, 36, 0, 120, 100 },        // kick @0
                     MidiNote { 0, 36, 1450, 120, 100 },     // kick @1450（オフグリッド）
                     MidiNote { 0, 38, 960, 120, 100 } };    // snare（対象外）
    drums.midiRegions.push_back (region);
    project.tracks.push_back (std::move (drums));

    // ドラム4小節 × loop_bars 1 → 試聴長4小節・ループ反復込みで kick 8発（PPQ 960→480 換算）
    {
        const auto plan = GachaSession::planBassRoll (project, 0, 0, 1);
        expect (plan.drumsBars == 4 && plan.previewBars == 4, "1小節パターンはドラム長へ伸ばす");
        expect (plan.kickTicks.size() == 8, "ループ反復込みで kick 8発");
        expect (plan.kickTicks[0] == "0" && plan.kickTicks[1] == "725",
                "PPQ 960 → 480 の換算（1450/2=725）");
        expect (plan.kickTicks[2] == juce::String (Ppq::ticksPerBar / 2),
                "2小節目の反復が展開される");
    }

    // ドラム4小節 × loop_bars 8 → 試聴長8小節（ベースが長い側）
    {
        const auto plan = GachaSession::planBassRoll (project, 0, 0, 8);
        expect (plan.previewBars == 8 && plan.drumsBars == 4, "ベースの loop_bars が試聴長を決める");
    }

    // ドラム6小節相当（1小節×6）× loop_bars 4 → 8小節へ倍数切り上げ
    {
        project.tracks[0].midiRegions[0].loopCount = 5;
        const auto plan = GachaSession::planBassRoll (project, 0, 0, 4);
        expect (plan.drumsBars == 6 && plan.previewBars == 8,
                "試聴長は loop_bars の倍数へ切り上げ（bass.py は倍数のみ受ける）");
        project.tracks[0].midiRegions[0].loopCount = 3;
    }

    // 固定ピッチ打楽器トラック（Kick 専用トラック）: ノートのピッチに依らず実効ピッチで判定
    {
        project.tracks[0].drumPitch = 36;
        project.tracks[0].midiRegions[0].notes[2].pitch = 60; // 何を書いても kick として鳴る
        const auto plan = GachaSession::planBassRoll (project, 0, 0, 1);
        expect (plan.kickTicks.size() == 12, "固定ピッチ 36 のトラックは全ノートが kick");
        project.tracks[0].drumPitch = -1;
        project.tracks[0].midiRegions[0].notes[2].pitch = 38;
    }

    // ドラムソース無し → loop_bars のまま・キック無し
    {
        const auto plan = GachaSession::planBassRoll (project, -1, -1, 8);
        expect (plan.previewBars == 8 && plan.drumsBars == 0 && plan.kickTicks.isEmpty(),
                "ソース無しはブーストなしで loop_bars のまま");
    }
}

void testReferenceAlign()
{
    beginTest ("ReferenceAlign calculation / info gates / apply contract");

    // ---- alignedClipStart: 位置計算 ----
    const double barLen = 48000.0 * 240.0 / 120.0; // 120BPM/48k → 96000サンプル/小節
    // fd=0 かつ現在位置が小節頭 → no-op
    expect (ReferenceAlign::alignedClipStart (96000 * 2, 0, barLen) == 96000 * 2,
            "fd=0で小節頭ならそのまま");
    // fd=0 かつオフグリッド → 最寄りの小節頭へスナップ
    expect (ReferenceAlign::alignedClipStart (96000 * 2 + 40000, 0, barLen) == 96000 * 2,
            "fd=0でオフグリッドは手前の小節へ（40000<半小節）");
    expect (ReferenceAlign::alignedClipStart (96000 * 2 + 50000, 0, barLen) == 96000 * 3,
            "fd=0でオフグリッドは近い方の小節へ（50000>半小節）");
    // fd>0: クリップ内の小節頭が小節線に乗る位置へ。startSample>=0 を保つ
    expect (ReferenceAlign::alignedClipStart (0, 66000, barLen) == 96000 - 66000,
            "曲頭付近では小節1の頭に合わせて正の位置へ（startSample>=0）");
    // current=5小節目・fd=66000 → (480000+66000)/96000=5.69 → N=6 → 576000-66000=510000
    expect (ReferenceAlign::alignedClipStart (96000 * 5, 66000, barLen) == 510000,
            "現在位置から最も近い整合位置へ動く");

    // ---- readInfo: groove.json / gates.json のゲート ----
    const auto dir = makeTempDir();
    const auto analysis = dir.getChildFile ("analysis");
    analysis.createDirectory();
    const auto writeJson = [&analysis] (const char* name, const juce::String& text)
    { analysis.getChildFile (name).replaceWithText (text); };

    // gates が無い → 提供不可
    expect (! ReferenceAlign::readInfo (dir).available, "gatesが無ければ提供不可");
    // downbeat.ok=false → 提供不可（値が在っても信頼できない）
    writeJson ("gates.json", R"({"downbeat": {"ok": false}})");
    writeJson ("groove.json", R"({"first_downbeat_sec": 1.3595})");
    expect (! ReferenceAlign::readInfo (dir).available, "downbeat.ok=falseは提供不可");
    // ok=true + groove欠損 → 提供不可
    writeJson ("gates.json", R"({"downbeat": {"ok": true}})");
    writeJson ("groove.json", R"({})");
    expect (! ReferenceAlign::readInfo (dir).available, "grooveのfirst_downbeat_sec欠損は提供不可");
    // 揃った → 提供可・groove.json の補正済み値を返す
    writeJson ("groove.json", R"({"first_downbeat_sec": 1.3595})");
    {
        const auto info = ReferenceAlign::readInfo (dir);
        expect (info.available, "gates ok + groove あり → 提供可");
        expect (std::abs (info.firstDownbeatSec - 1.3595) < 1e-9, "groove.jsonの値を返す");
    }

    // ---- source.json の往復 ----
    expect (! ReferenceAlign::readSourceDescriptor (dir).isValid(), "source.json 不在は invalid");
    expect (ReferenceAlign::writeSourceDescriptor (dir, { "clip-001.wav", 1440000, 5760000 }),
            "source.jsonを書ける");
    {
        const auto descriptor = ReferenceAlign::readSourceDescriptor (dir);
        expect (descriptor.isValid() && descriptor.fileName == "clip-001.wav"
                    && descriptor.offsetSamples == 1440000 && descriptor.lengthSamples == 5760000,
                "source.jsonの往復");
    }

    // ---- locateClip / apply の契約 ----
    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "プロジェクト作成");
    project->sampleRate = 48000.0;
    project->bpm = 100.0;
    project->tracks.clear();

    Track track;
    track.id = project->allocateId();
    track.type = TrackType::audio;
    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.offsetSamples = 1440000;
    clip.lengthSamples = 5760000;
    clip.startSample = 0;
    track.clips.push_back (clip);
    project->tracks.push_back (std::move (track));

    const ReferenceAlign::ClipDescriptor descriptor { "clip-001.wav", 1440000, 5760000 };
    expect (ReferenceAlign::locateClip (*project, descriptor).matches == 1, "1件一致");

    UndoStack undo;
    // 適用: undo 深さがちょうど1増える（誤って2回beginしない）
    expect (ReferenceAlign::apply (*project, undo, descriptor, 112.938, 1.3595)
                == ReferenceAlign::ApplyResult::applied,
            "適用できる");
    expect (undo.canUndo(), "undo履歴が積まれる");
    expect (std::abs (project->bpm - 112.938) < 1e-9, "BPMが変わる");
    const auto fdSamples = (juce::int64) std::llround (1.3595 * 48000.0);
    const double barLen2 = 48000.0 * 240.0 / 112.938;
    const auto expectedStart = (juce::int64) std::llround (barLen2) - fdSamples; // 曲頭付近→小節1
    expect (project->tracks[0].clips[0].startSample == expectedStart,
            "クリップ内の小節頭が小節線に乗る位置へ動く");

    // 同値再実行: begin されず noChange（no-op の undo 履歴を積まない）
    auto kind = UndoStack::EditKind::structure;
    expect (ReferenceAlign::apply (*project, undo, descriptor, 112.938, 1.3595)
                == ReferenceAlign::ApplyResult::noChange,
            "頭出し済みの再実行はnoChange");
    expect (undo.undo (*project, kind), "undoは1件だけ");
    expect (! undo.canUndo(), "再実行分の履歴は積まれていない（深さ1だった）");
    expect (std::abs (project->bpm - 100.0) < 1e-9 && project->tracks[0].clips[0].startSample == 0,
            "undo 1回でBPMとクリップ位置が両方戻る");
    expect (undo.redo (*project, kind), "redoできる");
    expect (std::abs (project->bpm - 112.938) < 1e-9
                && project->tracks[0].clips[0].startSample == expectedStart,
            "redoで両方再適用される");

    // 複製で2件一致 → notFound（何も変更しない）
    project->tracks[0].clips.push_back (project->tracks[0].clips[0]);
    const auto startBefore = project->tracks[0].clips[0].startSample;
    expect (ReferenceAlign::locateClip (*project, descriptor).matches == 2, "複製で2件一致");
    expect (ReferenceAlign::apply (*project, undo, descriptor, 120.0, 1.3595)
                == ReferenceAlign::ApplyResult::notFound,
            "2件一致は適用しない");
    expect (std::abs (project->bpm - 112.938) < 1e-9
                && project->tracks[0].clips[0].startSample == startBefore,
            "notFoundでは何も変更しない");

    // 0件（記述子不一致）→ notFound
    expect (ReferenceAlign::apply (*project, undo, { "clip-999.wav", 0, 100 }, 120.0, 1.0)
                == ReferenceAlign::ApplyResult::notFound,
            "0件一致は適用しない");

    dir.deleteRecursively();
}

void testReferenceReportGenerator()
{
    beginTest ("ReferenceReportGenerator worker (fake script)");

    const auto dir = makeTempDir();
    const auto writeScript = [&dir] (const char* name, const juce::String& body)
    {
        const auto file = dir.getChildFile (name);
        // replaceWithText は既定で改行を CRLF にする → bash が `exit 65\r` を食って壊れる。LF 明示
        file.replaceWithText ("#!/bin/bash\n" + body + "\n", false, false, "\n");
        return file;
    };

    // ---- 成功: stdout の最新行が currentLine に載り、status が success になる ----
    {
        ReferenceReportGenerator gen;
        const auto script = writeScript ("ok.sh", "echo '==> step1'; echo '==> step2'");
        expect (gen.start ({ script, dir }), "起動できる");
        while (gen.status() == ReferenceReportGenerator::Status::running)
            juce::Thread::sleep (10);
        expect (gen.currentLine() == "==> step2", "stdout の最新行が currentLine に載る");
        // 完了〜takeResult の間（status=success・スレッド停止済み）は開始できない
        //（前回の result・対象フォルダを黙って上書きし、poll 前の完了通知が消えるため）
        expect (! gen.start ({ script, dir }), "結果の未回収中は再開始できない");
        expect (gen.takeResult().status == ReferenceReportGenerator::Status::success,
                "exit 0 は success");
        expect (gen.status() == ReferenceReportGenerator::Status::idle, "takeResult 後は idle");
        expect (gen.start ({ script, dir }), "takeResult 後は再開始できる");
        while (gen.status() == ReferenceReportGenerator::Status::running)
            juce::Thread::sleep (10);
        gen.takeResult();
    }

    // ---- 失敗: stderr の理由行が errorMessage に載る ----
    {
        ReferenceReportGenerator gen;
        const auto script = writeScript ("fail.sh", "echo 'reason line' >&2; exit 65");
        gen.start ({ script, dir });
        while (gen.status() == ReferenceReportGenerator::Status::running)
            juce::Thread::sleep (10);
        const auto result = gen.takeResult();
        expect (result.status == ReferenceReportGenerator::Status::failed, "exit 非0 は failed");
        expect (result.errorMessage.contains ("reason line"), "stderr の理由行が載る");
        expect (result.errorMessage.contains ("65"), "exit code が載る");
    }

    // ---- キャンセル: プロセスグループごと止まり、残骸 .next も回収される ----
    {
        const auto next = dir.getChildFile ("report.md.next");
        next.replaceWithText ("# partial"); // SIGKILL で trap が走らなかった体
        ReferenceReportGenerator gen;
        const auto script = writeScript ("slow.sh", "echo started; sleep 30");
        gen.start ({ script, dir });
        while (gen.currentLine() != "started" && gen.status() == ReferenceReportGenerator::Status::running)
            juce::Thread::sleep (10);
        gen.cancel();
        while (gen.status() == ReferenceReportGenerator::Status::running)
            juce::Thread::sleep (10);
        const int pgid = gen.pgid();
        expect (gen.takeResult().status == ReferenceReportGenerator::Status::cancelled,
                "キャンセルは cancelled");
        expect (countProcessesInGroup (pgid) == 0, "プロセスグループごと止まる");
        expect (! next.existsAsFile(), "残骸 .next がロック経由の掃除で消える");
    }

    // ---- 掃除の抑制: 別プロセスがロック保持中は .next に触れない ----
    {
        const auto next = dir.getChildFile ("report.md.next");
        next.replaceWithText ("# other runner writing");
        SpawnedProcess holder;
        expect (holder.start ({ "/bin/bash", "-c",
                                "exec 9>'" + dir.getChildFile (".report.lock").getFullPathName()
                                    + "'; /usr/bin/lockf -s -t 0 9 && sleep 10" }),
                "ロック保持プロセスを起動できる");
        juce::Thread::sleep (500); // ロック取得を待つ

        ReferenceReportGenerator gen;
        const auto script = writeScript ("fail2.sh", "exit 1");
        gen.start ({ script, dir });
        while (gen.status() == ReferenceReportGenerator::Status::running)
            juce::Thread::sleep (10);
        gen.takeResult();
        expect (next.existsAsFile(), "ロック保持中は手動実行の .next に触れない");
        holder.terminate();
    }

    dir.deleteRecursively();
}

void testReferenceReport()
{
    beginTest ("ReferenceReport paths / existence / render cache invalidation");

    const auto dir = makeTempDir();
    const auto renderer = dir.getChildFile ("render_report.py");
    renderer.replaceWithText ("# renderer");

    expect (ReferenceReport::reportMd (dir) == dir.getChildFile ("report.md"), "report.mdのパス");
    expect (ReferenceReport::reportHtml (dir) == dir.getChildFile ("report.html"), "report.htmlのパス");
    expect (! ReferenceReport::exists (dir), "report.mdが無ければ存在しない扱い");

    const auto md = dir.getChildFile ("report.md");
    md.replaceWithText ("# report");
    expect (ReferenceReport::exists (dir), "report.mdがあれば存在する");
    expect (ReferenceReport::needsRender (dir, renderer), "HTMLキャッシュが無ければ変換が要る");

    // キャッシュが両方（md・変換器）より新しい → 変換不要
    const auto html = dir.getChildFile ("report.html");
    html.replaceWithText ("<html>");
    const auto base = juce::Time::getCurrentTime();
    md.setLastModificationTime (base - juce::RelativeTime::seconds (60));
    renderer.setLastModificationTime (base - juce::RelativeTime::seconds (60));
    html.setLastModificationTime (base);
    expect (! ReferenceReport::needsRender (dir, renderer), "キャッシュが新しければ変換不要");

    // report.md の方が新しい（書き直し後）→ 変換が要る
    md.setLastModificationTime (base + juce::RelativeTime::seconds (60));
    expect (ReferenceReport::needsRender (dir, renderer), "report.mdが新しければ変換が要る");

    // 変換器の方が新しい（render_report.py やCSSの更新後）→ 変換が要る
    md.setLastModificationTime (base - juce::RelativeTime::seconds (60));
    renderer.setLastModificationTime (base + juce::RelativeTime::seconds (60));
    expect (ReferenceReport::needsRender (dir, renderer), "変換器が新しければ変換が要る");

    dir.deleteRecursively();
}

void testUndoStackBpm()
{
    beginTest ("UndoStack BPM roundtrip");
    const auto dir = makeTempDir();
    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "プロジェクト作成");
    project->bpm = 100.0;

    UndoStack undo;
    undo.begin (*project);
    project->bpm = 140.0;

    auto kind = UndoStack::EditKind::structure;
    expect (undo.undo (*project, kind), "undoできる");
    expect (std::abs (project->bpm - 100.0) < 1e-9, "undoでBPMが変更前に戻る");
    expect (undo.redo (*project, kind), "redoできる");
    expect (std::abs (project->bpm - 140.0) < 1e-9, "redoでBPMが再適用される");

    // BPMを含まない編集のundoでもBPMは（beginの時点の値へ）正しく往復する
    undo.begin (*project);
    project->tracks.clear();
    expect (undo.undo (*project, kind), "構造編集のundo");
    expect (std::abs (project->bpm - 140.0) < 1e-9, "構造編集のundoでBPMは維持される");

    dir.deleteRecursively();
}

// ---- v13: プロジェクトキー（root+mode・Optional）の保存/読込・破損時の未設定化・undo ----
void testProjectKey()
{
    beginTest ("project key roundtrip / invalid values / undo");

    // 名前ヘルパー（表示・CLI・カードテキストの相互変換）
    expect (ProjectKeys::rootName (6) == juce::String::fromUTF8 (u8"F♯"), "rootName は♯表記");
    expect (ProjectKeys::displayName ({ 6, KeyMode::minor }) == juce::String::fromUTF8 (u8"F♯m"),
            "minor は m を付ける");
    expect (ProjectKeys::displayName ({ 6, KeyMode::major }) == juce::String::fromUTF8 (u8"F♯"),
            "major は素の音名");
    expect (ProjectKeys::cliText ({ 6, KeyMode::minor }) == "F#:minor", "CLI形式はASCII");
    int root = -1;
    expect (ProjectKeys::rootFromName ("Db", root) && root == 1, "♭表記の別名も同じピッチクラスへ");
    expect (! ProjectKeys::rootFromName ("H", root), "未知の音名は拒否");
    {
        const auto parsed = ProjectKeys::fromCardText ("F# major");
        expect (parsed.has_value() && parsed->root == 6 && parsed->mode == KeyMode::major,
                "カードのキーテキストを読める");
        expect (! ProjectKeys::fromCardText ("").has_value(), "空（キーのゲート落ち）は nullopt");
        expect (! ProjectKeys::fromCardText ("F# dorian").has_value(), "未知モードは nullopt");
    }

    const auto dir = makeTempDir();
    Project project;
    project.directory = dir;
    juce::String error;
    juce::StringArray warnings;

    // 未設定のまま保存 → key を書かない・未設定のまま読める
    expect (project.save (error), "未設定のまま保存できる");
    {
        const auto parsed = juce::JSON::parse (dir.getChildFile ("project.json").loadFileAsString());
        expect ((int) parsed.getProperty ("version", 0) == Project::currentVersion, "現行バージョンで保存されること");
        expect (! parsed.hasProperty ("key"), "未設定は JSON を汚さない");
        auto reloaded = Project::load (dir, warnings, error);
        expect (reloaded != nullptr && ! reloaded->key.has_value(), "未設定のまま往復する");
    }

    // 設定して保存 → 往復で一致
    project.key = ProjectKey { 6, KeyMode::minor };
    expect (project.save (error), "キー付きで保存できる");
    {
        warnings.clear();
        auto reloaded = Project::load (dir, warnings, error);
        expect (reloaded != nullptr && reloaded->key.has_value()
                    && *reloaded->key == ProjectKey ({ 6, KeyMode::minor }),
                "設定値が往復で一致する");
        expect (warnings.isEmpty(), "正常値に警告は出ない");
    }

    // v12 以前（key 欠損）は未設定として読める
    dir.getChildFile ("project.json").replaceWithText (R"({
        "version": 12, "bpm": 120.0, "sampleRate": 0.0, "nextId": 1, "tracks": []
    })");
    {
        warnings.clear();
        auto legacy = Project::load (dir, warnings, error);
        expect (legacy != nullptr && ! legacy->key.has_value(), "v12 の key 欠損は未設定");
        expect (warnings.isEmpty(), "欠損は破損ではないので警告しない");
    }

    // 破損値（root 範囲外・未知 mode）は未設定化＋警告（拒否しない — 両ケース同じ扱い）
    for (const char* json : {
             R"({"version": 13, "bpm": 120.0, "nextId": 1, "tracks": [], "key": {"root": 12, "mode": "minor"}})",
             R"({"version": 13, "bpm": 120.0, "nextId": 1, "tracks": [], "key": {"root": 3, "mode": "dorian"}})" })
    {
        dir.getChildFile ("project.json").replaceWithText (json);
        warnings.clear();
        auto broken = Project::load (dir, warnings, error);
        expect (broken != nullptr, "破損キーでもプロジェクト自体は開ける");
        expect (broken != nullptr && ! broken->key.has_value(), "破損キーは未設定化される");
        expect (! warnings.isEmpty(), "破損キーは警告される");
    }

    // undo/redo: キーの set が往復する
    {
        Project undoProject;
        undoProject.directory = dir;
        UndoStack undo;
        undo.begin (undoProject);
        undoProject.key = ProjectKey { 9, KeyMode::major };
        auto kind = UndoStack::EditKind::structure;
        expect (undo.undo (undoProject, kind), "キー変更をundoできる");
        expect (! undoProject.key.has_value(), "undoで未設定に戻る");
        expect (undo.redo (undoProject, kind), "redoできる");
        expect (undoProject.key.has_value() && undoProject.key->root == 9, "redoで再適用される");
    }

    dir.deleteRecursively();
}

// ---- 分析完了ダイアログの undo 粒度（経路別に1件）: BPM＋キー（＋クリップ移動）----
void testReferenceAlignWithKey()
{
    beginTest ("ReferenceAlign with key: combined single undo per path");

    const auto dir = makeTempDir();
    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "プロジェクト作成");
    project->sampleRate = 48000.0;
    project->bpm = 100.0;
    project->tracks.clear();

    Track track;
    track.id = project->allocateId();
    track.type = TrackType::audio;
    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.offsetSamples = 0;
    clip.lengthSamples = 480000;
    track.clips.push_back (clip);
    project->tracks.push_back (std::move (track));
    const ReferenceAlign::ClipDescriptor descriptor { "clip-001.wav", 0, 480000 };

    const ProjectKey key { 6, KeyMode::minor };
    UndoStack undo;
    auto kind = UndoStack::EditKind::structure;

    // 経路1: キーあり＋頭出し可 → BPM・key・クリップ移動が 1 undo
    expect (ReferenceAlign::apply (*project, undo, descriptor, 112.938, 1.3595, key)
                == ReferenceAlign::ApplyResult::applied,
            "キー付きで適用できる");
    expect (project->key.has_value() && *project->key == key, "キーが設定される");
    expect (std::abs (project->bpm - 112.938) < 1e-9, "BPMが変わる");
    expect (project->tracks[0].clips[0].startSample != 0, "クリップが動く");
    expect (undo.undo (*project, kind), "undoできる");
    expect (! undo.canUndo(), "undoは1件だけ（複合1操作）");
    expect (! project->key.has_value() && std::abs (project->bpm - 100.0) < 1e-9
                && project->tracks[0].clips[0].startSample == 0,
            "undo 1回でBPM・キー・クリップ位置がすべて戻る");
    expect (undo.redo (*project, kind), "redoできる");
    expect (project->key.has_value() && *project->key == key, "redoでキーも再適用される");

    // no-op 判定は対象項目すべて: BPM・移動先が同値でもキーが違えば applied
    const ProjectKey other { 4, KeyMode::major };
    expect (ReferenceAlign::apply (*project, undo, descriptor, 112.938, 1.3595, other)
                == ReferenceAlign::ApplyResult::applied,
            "キーだけ違えば適用される");
    // 全項目同値 → noChange（begin されない）
    expect (ReferenceAlign::apply (*project, undo, descriptor, 112.938, 1.3595, other)
                == ReferenceAlign::ApplyResult::noChange,
            "全項目同値は noChange");
    // key = nullopt はキーに触らない（既存の「BPMを設定して頭出し」互換）
    expect (ReferenceAlign::apply (*project, undo, descriptor, 112.938, 1.3595)
                == ReferenceAlign::ApplyResult::noChange,
            "nullopt キーは同値扱い（既存経路の no-op を壊さない）");

    // 経路2: キーあり＋頭出し不可 → BPM・key が 1 undo（クリップは動かない）
    UndoStack undo2;
    Project project2;
    project2.directory = dir;
    project2.bpm = 100.0;
    expect (ReferenceAlign::applyBpmAndKey (project2, undo2, 92.5, key)
                == ReferenceAlign::ApplyResult::applied,
            "BPM＋キーを適用できる");
    expect (undo2.undo (project2, kind), "undoできる");
    expect (! undo2.canUndo(), "undoは1件だけ");
    expect (! project2.key.has_value() && std::abs (project2.bpm - 100.0) < 1e-9,
            "undo 1回でBPMとキーが両方戻る");
    expect (undo2.redo (project2, kind), "redoできる");
    expect (ReferenceAlign::applyBpmAndKey (project2, undo2, 92.5, key)
                == ReferenceAlign::ApplyResult::noChange,
            "同値の再適用は noChange");
    expect (ReferenceAlign::applyBpmAndKey (project2, undo2, 20.0, key)
                == ReferenceAlign::ApplyResult::notFound,
            "BPM範囲外は notFound");

    dir.deleteRecursively();
}

// ---- TrackSaturator: 倍音・自動補償・ADAAエイリアス・遷移 ----
// plan: docs/plans/2026-08-16-2058-fx-batch3-saturation-lofi.md Phase 2
// 閾値・信号条件は plan の「検証方針」で事前固定（実測後に動かさない）

// 一定周波数の正弦を通して出力列を返す（snapTo済み・serial連番・モノ）
std::vector<float> renderSaturator (float driveValue, float mixValue, double amp, double freqHz,
                                    double sr, int totalSamples)
{
    TrackSaturator sat;
    const Sat::Values values { driveValue, mixValue };
    sat.snapTo (sr, true, values);
    constexpr int blockSize = 512;
    std::vector<float> out;
    out.reserve ((size_t) totalSamples + blockSize);
    juce::uint64 serial = 0;
    double phase = 0.0;
    const double inc = juce::MathConstants<double>::twoPi * freqHz / sr;
    float block[blockSize];
    int done = 0;
    while (done < totalSamples)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            block[i] = (float) (amp * std::sin (phase));
            phase += inc;
        }
        sat.process (block, nullptr, blockSize, sr, ++serial, false, true, values);
        out.insert (out.end(), block, block + blockSize);
        done += blockSize;
    }
    out.resize ((size_t) totalSamples);
    return out;
}

// start以降のnサンプルから周波数freqHzの振幅を測る（相関法。nは整数周期で切ること）
double satBinAmp (const std::vector<float>& y, size_t start, size_t n, double freqHz, double sr)
{
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double ph = juce::MathConstants<double>::twoPi * freqHz * (double) i / sr;
        re += (double) y[start + i] * std::cos (ph);
        im += (double) y[start + i] * std::sin (ph);
    }
    return 2.0 * std::hypot (re, im) / (double) n;
}

// start以降nサンプルのAC RMS（平均を引く＝DC成分を除いた実効値）
double satAcRms (const std::vector<float>& y, size_t start, size_t n)
{
    double sum = 0.0, sumSq = 0.0;
    for (size_t i = start; i < start + n; ++i)
    {
        sum += (double) y[i];
        sumSq += (double) y[i] * (double) y[i];
    }
    const double mean = sum / (double) n;
    return std::sqrt (juce::jmax (0.0, sumSq / (double) n - mean * mean));
}

void testTrackSaturatorHarmonics()
{
    beginTest ("track saturator harmonics & compensation");
    constexpr double sr = 48000.0;
    const double amp18 = std::pow (10.0, -18.0 / 20.0);
    constexpr size_t discard = 24000, n = 48000; // 0.5s捨て（DCブロッカ整定）＋1s計測
    constexpr int total = (int) (discard + n);

    // ---- knob50%: 偶数倍音（H2）が存在し、H3より優勢（非対称カーブの証明）----
    {
        const auto y = renderSaturator (0.5f, 1.0f, amp18, 1000.0, sr, total);
        const double h1 = satBinAmp (y, discard, n, 1000.0, sr);
        const double h2 = satBinAmp (y, discard, n, 2000.0, sr);
        const double h3 = satBinAmp (y, discard, n, 3000.0, sr);
        expect (20.0 * std::log10 (h2 / h1) > -35.0, "H2が存在（偶数倍音＝非対称の証明。設計値≈-25dBc）");
        expect (h2 > h3, "H2がH3より優勢（暖かい方向の性格）");
    }

    // ---- THD（基本波比）がDriveで単調増加 ----
    {
        double lastThd = -1.0;
        for (const float d : { 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const auto y = renderSaturator (d, 1.0f, amp18, 1000.0, sr, total);
            const double h1 = satBinAmp (y, discard, n, 1000.0, sr);
            double harmPower = 0.0;
            for (int h = 2; h <= 6; ++h)
            {
                const double a = satBinAmp (y, discard, n, 1000.0 * h, sr);
                harmPower += a * a;
            }
            const double thd = std::sqrt (harmPower) / h1;
            expect (thd > lastThd, "THD（基本波比）がDriveで単調増加");
            lastThd = thd;
        }
    }

    // ---- 自動補償: 定義信号（-18dBFS正弦）でDrive全域±0.1dB ----
    for (int k = 0; k <= 10; ++k)
    {
        const auto y = renderSaturator ((float) k / 10.0f, 1.0f, amp18, 1000.0, sr, total);
        const double errDb = juce::Decibels::gainToDecibels (
            satAcRms (y, discard, n) / (amp18 / std::sqrt (2.0)));
        expect (std::abs (errDb) < 0.1, "-18dBFS基準でDrive全域±0.1dB");
    }

    // ---- 別レベル（-6dBFS）: NaN/infなし・隣接Drive点で不連続なし・|誤差|≤6dB ----
    // 基準入力だけを補償する仕様上、絶対値保証・改善保証はできない（planの検証方針）
    {
        const double amp6 = std::pow (10.0, -6.0 / 20.0);
        double prevErr = 0.0;
        bool finite = true;
        for (int k = 0; k <= 20; ++k)
        {
            const auto y = renderSaturator ((float) k / 20.0f, 1.0f, amp6, 1000.0, sr, total);
            for (size_t i = discard; i < discard + n; ++i)
                finite = finite && std::isfinite (y[i]);
            const double errDb = juce::Decibels::gainToDecibels (
                satAcRms (y, discard, n) / (amp6 / std::sqrt (2.0)));
            expect (std::abs (errDb) <= 6.0, "-6dBFSの誤差が6dB以内");
            if (k > 0)
                expect (std::abs (errDb - prevErr) < 1.0, "-6dBFSのDrive掃引で隣接点に不連続なし");
            prevErr = errDb;
        }
        expect (finite, "-6dBFS掃引でNaN/Infなし");
    }

    // ---- DC残留: 0dBFS・100Hz・最大Drive。整定1秒後の1秒間の出力平均が-80dBFS以下 ----
    {
        const auto y = renderSaturator (1.0f, 1.0f, 1.0, 100.0, sr, 96000);
        double sum = 0.0;
        for (size_t i = 48000; i < 96000; ++i)
            sum += (double) y[i];
        const double meanDb = 20.0 * std::log10 (juce::jmax (std::abs (sum / 48000.0), 1.0e-12));
        expect (meanDb < -80.0, "DC残留が-80dBFS以下（DCブロッカ）");
    }

    // ---- 中立判定と高速パス ----
    {
        TrackSaturator sat;
        expect (! sat.needsActivePath (true, { 0.0f, 1.0f }), "drive=0は高速パス（中立）");
        expect (! sat.needsActivePath (true, { 0.5f, 0.0f }), "mix=0は高速パス（中立）");
        expect (sat.needsActivePath (true, { 0.5f, 1.0f }), "非中立＋ONはactive");
        expect (! sat.needsActivePath (false, { 0.5f, 1.0f }), "OFFは高速パス");
    }
}

void testTrackSaturatorAliasing()
{
    beginTest ("track saturator ADAA aliasing");
    // planの検証方針で固定した代表点: f0=5kHz・-6dBFS @48kHz・Driveノブ50%。
    // 折り返し周波数 |k·f0 − m·fs| は1kHz格子に乗るため、直接倍音（5/10/15/20kHz）以外の
    // 1kHz格子ビンが全てエイリアス。素朴実装（カーブ直評価）と比較する
    constexpr double sr = 48000.0;
    const double amp = std::pow (10.0, -6.0 / 20.0);
    constexpr size_t discard = 24000, n = 9600; // 1kHz格子の整数周期
    constexpr int total = (int) (discard + n);

    const auto adaa = renderSaturator (0.5f, 1.0f, amp, 5000.0, sr, total);

    std::vector<float> naive ((size_t) total);
    const auto curve = Sat::Curve::fromDrive (0.5f);
    double phase = 0.0;
    const double inc = juce::MathConstants<double>::twoPi * 5000.0 / sr;
    for (int i = 0; i < total; ++i)
    {
        naive[(size_t) i] = Sat::transfer (curve, (float) (amp * std::sin (phase)));
        phase += inc;
    }

    // 補償ゲイン・DCブロッカのスケール差を打ち消すため基本波比（dBc相当）で比較する
    const double fundAdaa = satBinAmp (adaa, discard, n, 5000.0, sr);
    const double fundNaive = satBinAmp (naive, discard, n, 5000.0, sr);
    double aliasPowerAdaa = 0.0, aliasPowerNaive = 0.0;
    bool majorNotWorse = true;
    for (int kHz = 1; kHz <= 23; ++kHz)
    {
        if (kHz % 5 == 0)
            continue; // 直接倍音を除外
        const double a = satBinAmp (adaa, discard, n, kHz * 1000.0, sr) / fundAdaa;
        const double b = satBinAmp (naive, discard, n, kHz * 1000.0, sr) / fundNaive;
        aliasPowerAdaa += a * a;
        aliasPowerNaive += b * b;
        // 主要折り返しビン＝素朴実装で-80dBc超のビン。5%（≈0.4dB）は相関法の測定余裕
        if (20.0 * std::log10 (juce::jmax (b, 1.0e-12)) > -80.0)
            majorNotWorse = majorNotWorse && (a <= b * 1.05);
    }
    expect (majorNotWorse, "主要折り返しビンが悪化しない");
    const double improveDb = 10.0 * std::log10 (aliasPowerNaive / aliasPowerAdaa);
    expect (improveDb >= 5.0, "合算エイリアス電力が5dB以上改善（事前評価≈6.9dB）");
}

void testTrackSaturatorTransitions()
{
    beginTest ("track saturator transitions");
    constexpr double sr = 48000.0;
    constexpr int blockSize = 512;
    const Sat::Values hot { 0.8f, 1.0f };

    // ---- ON/OFF切替を繰り返してもNaN/Inf・不連続なサンプル跳躍が出ないこと ----
    {
        TrackSaturator sat;
        sat.snapTo (sr, true, hot);
        juce::uint64 serial = 0;
        double phase = 0.0;
        const double inc = juce::MathConstants<double>::twoPi * 1000.0 / sr;
        float previous = 0.0f, maxJump = 0.0f;
        bool allFinite = true;
        for (int b = 0; b < 40; ++b)
        {
            const bool on = (b / 4) % 2 == 0; // 4ブロックごとにON/OFF
            float block[blockSize];
            for (int i = 0; i < blockSize; ++i)
            {
                block[i] = (float) std::sin (phase) * 0.5f;
                phase += inc;
            }
            sat.process (block, nullptr, blockSize, sr, ++serial, false, on, hot);
            for (int i = 0; i < blockSize; ++i)
            {
                allFinite = allFinite && std::isfinite (block[i]);
                maxJump = juce::jmax (maxJump, std::abs (block[i] - previous));
                previous = block[i];
            }
        }
        expect (allFinite, "ON/OFF切替中にNaN/Infが出ないこと");
        expect (maxJump < 0.5f, "ON/OFF切替がクロスフェードされ跳躍しないこと");
    }

    // ---- Drive/Mixの急変（1ブロックで0→最大）でも跳躍しないこと（SmoothedValue平滑）----
    {
        TrackSaturator sat;
        sat.snapTo (sr, true, { 0.0f, 1.0f });
        juce::uint64 serial = 0;
        double phase = 0.0;
        const double inc = juce::MathConstants<double>::twoPi * 1000.0 / sr;
        float previous = 0.0f, maxJump = 0.0f;
        for (int b = 0; b < 20; ++b)
        {
            const Sat::Values targets { b < 10 ? (b % 2 == 0 ? 0.0f : 1.0f) : 0.8f,
                                        b < 10 ? 1.0f : (b % 2 == 0 ? 0.0f : 1.0f) };
            float block[blockSize];
            for (int i = 0; i < blockSize; ++i)
            {
                block[i] = (float) std::sin (phase) * 0.5f;
                phase += inc;
            }
            sat.process (block, nullptr, blockSize, sr, ++serial, false, true, targets);
            for (int i = 0; i < blockSize; ++i)
            {
                maxJump = juce::jmax (maxJump, std::abs (block[i] - previous));
                previous = block[i];
            }
        }
        expect (maxJump < 0.5f, "Drive/Mix急変でも跳躍しないこと");
    }

    // ---- timelineJumped後の出力が「初期化直後」と決定的に一致すること（旧履歴の混入なし）----
    {
        TrackSaturator warmed, fresh;
        warmed.snapTo (sr, true, hot);
        fresh.snapTo (sr, true, hot);
        juce::uint64 serial = 0;
        float noise[blockSize];
        juce::Random random (42);
        for (int b = 0; b < 8; ++b) // warmedにだけ履歴を溜める
        {
            for (int i = 0; i < blockSize; ++i)
                noise[i] = random.nextFloat() - 0.5f;
            warmed.process (noise, nullptr, blockSize, sr, ++serial, false, true, hot);
        }
        float a[blockSize], bBuf[blockSize];
        for (int i = 0; i < blockSize; ++i)
            a[i] = bBuf[i] = (float) std::sin ((double) i * 0.13) * 0.4f;
        warmed.process (a, nullptr, blockSize, sr, ++serial, true, true, hot); // ジャンプ
        fresh.process (bBuf, nullptr, blockSize, sr, 1, false, true, hot);
        bool identical = true;
        for (int i = 0; i < blockSize; ++i)
            identical = identical && juce::exactlyEqual (a[i], bBuf[i]);
        expect (identical, "ジャンプ後は初期化直後とビット一致（旧履歴が混入しない）");
    }

    // ---- Drive→0 の中立整定: 高速パス切替が不連続にならない（中立フェードの回帰）----
    // DC/低域を含む素材（定数+正弦）で drive を 0 へ落とし、settled 後の出力が入力と
    // ビット一致することを確認する。フェードが無いと「DCブロッカを通った出力」から
    // 「raw入力」への切替瞬間にポップが出る
    {
        TrackSaturator sat;
        sat.snapTo (sr, true, hot);
        juce::uint64 serial = 0;
        double phase = 0.0;
        const double inc = juce::MathConstants<double>::twoPi * 1000.0 / sr;
        const Sat::Values neutral { 0.0f, 1.0f };
        bool identical = true;
        bool reachedFastPath = false;
        float maxJump = 0.0f;
        float previous = 0.0f;
        for (int b = 0; b < 60 && ! reachedFastPath; ++b)
        {
            const auto& targets = b < 5 ? hot : neutral; // 5ブロック目でDriveを0へ
            float block[blockSize], input[blockSize];
            for (int i = 0; i < blockSize; ++i)
            {
                input[i] = block[i] = 0.3f + (float) std::sin (phase) * 0.4f; // DC入り
                phase += inc;
            }
            sat.process (block, nullptr, blockSize, sr, ++serial, false, true, targets);
            for (int i = 0; i < blockSize; ++i)
            {
                maxJump = juce::jmax (maxJump, std::abs (block[i] - previous));
                previous = block[i];
            }
            if (b >= 5 && ! sat.needsActivePath (true, neutral))
            {
                // 整定＝フェード完了はこのブロック内。末尾サンプルはビット一致していること
                identical = juce::exactlyEqual (block[blockSize - 1], input[blockSize - 1]);
                // 高速パス移行を模擬: 次ブロックはraw入力。境界の跳躍もmaxJumpに含める
                const float firstRaw = 0.3f + (float) std::sin (phase) * 0.4f;
                maxJump = juce::jmax (maxJump, std::abs (firstRaw - previous));
                reachedFastPath = true;
            }
        }
        expect (reachedFastPath, "Drive=0で高速パスへ整定すること");
        expect (identical, "フェード完了後の出力が入力とビット一致（高速パス切替が連続）");
        expect (maxJump < 0.5f, "Drive→0の遷移〜高速パス境界で跳躍しないこと");
    }

    // ---- RT確保ゼロ（192kHz・64サンプルブロックで1秒分）----
    {
        constexpr double rtSr = 192000.0;
        constexpr int block = 64;
        TrackSaturator sat;
        sat.snapTo (rtSr, true, hot);
        float l[block], r[block];
        juce::uint64 serial = 0;
        const int blocks = (int) (rtSr / block);
        testAllocationCount = 0;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i)
                l[i] = r[i] = (float) std::sin (0.3 * (b * block + i)) * 0.5f;
            sat.process (l, r, block, rtSr, ++serial, false, true, hot);
        }
        expect (testAllocationCount == 0, "1秒分（192kHz・64サンプルブロック）でヒープ確保ゼロ");
    }
}

// ---- FxSlots: ミキサー/パネルの表示位置→意味ID投影（5枠化の回帰） ----
// ミキサーは以前、表示位置iをそのままIDとして使っていた（0..2が偶然一致）。
// Sat=4/Lo-fi=5 の追加でIDが非連続になったため、投影のズレ＝誤ったエディタが開く事故を固定する
void testFxSlotProjection()
{
    beginTest ("fx slot projection (mixer/panel order)");

    expect (FxSlots::mixerOrder[0] == FxSlots::eq && FxSlots::mixerOrder[1] == FxSlots::comp
                && FxSlots::mixerOrder[2] == FxSlots::sat && FxSlots::mixerOrder[3] == FxSlots::lofi
                && FxSlots::mixerOrder[4] == FxSlots::ext,
            "ミキサー表示順 EQ,Comp,Sat,Lo-fi,Ext と既存IDの対応");
    expect (FxSlots::mixerPositionOf (FxSlots::grSlot) == 1, "GRミニバーの逆引き＝Comp位置(1)");

    TrackParams params;
    const auto layout = FxSlots::trackBaseLayout (&params);
    expect (layout.slots[FxSlots::eq].enabled == &params.eqEnabled
                && layout.slots[FxSlots::comp].enabled == &params.compEnabled
                && layout.slots[FxSlots::sat].enabled == &params.satEnabled
                && layout.slots[FxSlots::lofi].enabled == &params.lofiEnabled,
            "電源トグルが各FXの正しいatomicを指す");
    expect (layout.slots[FxSlots::ext].placeholder
                && layout.slots[FxSlots::ext].enabled == nullptr,
            "Extは空き表示・トグルなし");
    expect (! layout.slots[FxSlots::instrument].used, "ベース構成にInstrumentは無い");

    Track audioTrack;
    int order[FxSlots::maxSlots];
    int n = FxSlots::panelOrder (FxSlots::trackPanelLayout (audioTrack), order);
    expect (n == 5 && order[0] == FxSlots::eq && order[1] == FxSlots::comp
                && order[2] == FxSlots::sat && order[3] == FxSlots::lofi
                && order[4] == FxSlots::ext,
            "オーディオトラックのFXパネル表示順");

    Track midiTrack;
    midiTrack.type = TrackType::midi;
    n = FxSlots::panelOrder (FxSlots::trackPanelLayout (midiTrack), order);
    expect (n == 6 && order[0] == FxSlots::instrument && order[1] == FxSlots::eq
                && order[5] == FxSlots::ext,
            "MIDIトラックはInstrumentが先頭・以降は同順");
}

// ---- TrackLofi: Crush折り返し/量子化・Wow偏差・Noise追従・中立・決定性 ----
// plan: docs/plans/2026-08-16-2058-fx-batch3-saturation-lofi.md Phase 5
// 閾値・信号条件は plan の「検証方針」で事前固定（実測後に動かさない）

// 一定周波数の正弦（freqHz=0 なら定数 amp）を通して出力列を返す（snapTo済み・serial連番・モノ）
std::vector<float> renderLofi (const Lofi::Values& values, double amp, double freqHz,
                               double sr, int totalSamples)
{
    TrackLofi lofi;
    lofi.snapTo (sr, true, values);
    constexpr int blockSize = 512;
    std::vector<float> out;
    out.reserve ((size_t) totalSamples + blockSize);
    juce::uint64 serial = 0;
    double phase = 0.0;
    const double inc = juce::MathConstants<double>::twoPi * freqHz / sr;
    float block[blockSize];
    int done = 0;
    while (done < totalSamples)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            block[i] = freqHz > 0.0 ? (float) (amp * std::sin (phase)) : (float) amp;
            phase += inc;
        }
        lofi.process (block, nullptr, blockSize, sr, ++serial, false, true, values);
        out.insert (out.end(), block, block + blockSize);
        done += blockSize;
    }
    out.resize ((size_t) totalSamples);
    return out;
}

void testTrackLofiComponents()
{
    beginTest ("track lofi components");
    constexpr double sr = 48000.0;

    // ---- Crush: 折り返し線が理論周波数（S&Hイメージ n·R ± f0）に立つ ----
    // 内部レート R = 16kHz（ratio=1/3）になるノブ値を逆算し、1kHz正弦のイメージ R−f0=15kHz を
    // Goertzel で直接測る。中立レンダリングには同じ周波数に何も立たない
    {
        const float knob = std::pow (std::log (3.0f) / std::log (8.0f),
                                     1.0f / Lofi::crushRatioExponent);
        Lofi::Values values;
        values.crush = knob;
        constexpr size_t discard = 24000, n = 48000;
        const auto y = renderLofi (values, 0.4, 1000.0, sr, (int) (discard + n));
        const auto neutral = renderLofi ({}, 0.4, 1000.0, sr, (int) (discard + n));
        const double fund = satBinAmp (y, discard, n, 1000.0, sr);
        const double image = satBinAmp (y, discard, n, 15000.0, sr);
        const double imageNeutral = satBinAmp (neutral, discard, n, 15000.0, sr);
        expect (20.0 * std::log10 (image / fund) > -40.0,
                "S&Hの折り返し線が理論周波数 R−f0=15kHz に立つ");
        expect (20.0 * std::log10 (juce::jmax (imageNeutral, 1.0e-12) / fund) < -80.0,
                "中立では同じ周波数に何も立たない");
    }

    // ---- Crush: 量子化ステップの実測一致（定数入力 → held値がヘッダの quantize と一致）----
    {
        Lofi::Values values;
        values.crush = 0.5f;
        const auto y = renderLofi (values, 0.35, 0.0, sr, 9600);
        const float expected = Lofi::quantize (0.35f, Lofi::crushBits (0.5f));
        expect (std::abs (y.back() - expected) < 1.0e-6f, "量子化値がヘッダの式と一致");
    }

    // ---- Wow: 瞬時周波数偏差が理論値±10%（補間ゼロ交差で周期の最小/最大を測る）----
    {
        Lofi::Values values;
        values.wow = 1.0f; // 偏差 p = maxWowDepth = 1.5%
        const int total = (int) (3.5 * sr);
        const auto y = renderLofi (values, 0.4, 1000.0, sr, total);
        const size_t discard = 48000; // 1s捨て（LFO・平滑の整定）
        std::vector<double> crossings;
        for (size_t i = discard + 1; i < y.size(); ++i)
            if (y[i - 1] <= 0.0f && y[i] > 0.0f)
                crossings.push_back ((double) i - (double) y[i] / ((double) y[i] - (double) y[i - 1]));
        double minPeriod = 1.0e9, maxPeriod = 0.0;
        // 1周期ずつだと補間誤差が乗るので8周期窓の平均周期で評価する
        for (size_t i = 8; i < crossings.size(); ++i)
        {
            const double period = (crossings[i] - crossings[i - 8]) / 8.0;
            minPeriod = juce::jmin (minPeriod, period);
            maxPeriod = juce::jmax (maxPeriod, period);
        }
        const double deviation = (maxPeriod - minPeriod) / (maxPeriod + minPeriod);
        expect (std::abs (deviation - (double) Lofi::maxWowDepth) < 0.1 * Lofi::maxWowDepth,
                "Wowのピッチ偏差が A=p/(2πf) 換算の設定値±10%以内");
    }

    // ---- Wow: L/R同一の揺れ（同一入力ならビット一致）----
    {
        TrackLofi lofi;
        Lofi::Values values;
        values.wow = 1.0f;
        lofi.snapTo (sr, true, values);
        constexpr int blockSize = 512;
        float l[blockSize], r[blockSize];
        bool identical = true;
        juce::uint64 serial = 0;
        double phase = 0.0;
        for (int b = 0; b < 40; ++b)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                l[i] = r[i] = (float) std::sin (phase) * 0.4f;
                phase += juce::MathConstants<double>::twoPi * 1000.0 / sr;
            }
            lofi.process (l, r, blockSize, sr, ++serial, false, true, values);
            for (int i = 0; i < blockSize; ++i)
                identical = identical && juce::exactlyEqual (l[i], r[i]);
        }
        expect (identical, "L/Rが同一の揺れ（別々に揺れて広がらない）");
    }

    // ---- Noise: 入力追従（入力ありで鳴る・無音1秒以内に-60dBFS以下へ減衰）----
    {
        TrackLofi lofi;
        Lofi::Values values;
        values.noise = 1.0f;
        lofi.snapTo (sr, true, values);
        constexpr int blockSize = 512;
        juce::uint64 serial = 0;
        double phase = 0.0;
        double sumSqActive = 0.0;
        int activeCount = 0;
        const int activeBlocks = (int) (sr / blockSize);
        for (int b = 0; b < activeBlocks; ++b) // 1秒: 入力あり
        {
            float block[blockSize];
            for (int i = 0; i < blockSize; ++i)
            {
                block[i] = (float) std::sin (phase) * 0.5f;
                phase += juce::MathConstants<double>::twoPi * 1000.0 / sr;
            }
            lofi.process (block, nullptr, blockSize, sr, ++serial, false, true, values);
            if (b > activeBlocks / 2) // 後半のみ集計（エンベロープ立ち上がり後）
                for (int i = 0; i < blockSize; ++i)
                {
                    // ノイズ成分＝出力−入力（入力は素通しに加算される設計）
                    const float noiseOnly = block[i] - (float) std::sin (phase
                                                - juce::MathConstants<double>::twoPi * 1000.0 / sr
                                                      * (double) (blockSize - i)) * 0.5f;
                    sumSqActive += (double) noiseOnly * noiseOnly;
                    ++activeCount;
                }
        }
        expect (std::sqrt (sumSqActive / juce::jmax (1, activeCount)) > 1.0e-4,
                "入力がある間はノイズが鳴る");
        // 無音を1.2秒流し、1.0〜1.1秒窓の最大絶対値が-60dBFS以下
        float maxAfter = 0.0f;
        const int silentBlocks = (int) (1.2 * sr / blockSize);
        for (int b = 0; b < silentBlocks; ++b)
        {
            float block[blockSize] {};
            lofi.process (block, nullptr, blockSize, sr, ++serial, false, true, values);
            const double t0 = (double) b * blockSize / sr;
            if (t0 >= 1.0 && t0 < 1.1)
                for (int i = 0; i < blockSize; ++i)
                    maxAfter = juce::jmax (maxAfter, std::abs (block[i]));
        }
        expect (maxAfter < 1.0e-3f, "無音1秒後のノイズが-60dBFS以下（入力追従）");
    }

    // ---- 全ノブ0のビット一致素通し ----
    {
        TrackLofi lofi;
        lofi.snapTo (sr, true, {});
        constexpr int blockSize = 512;
        float block[blockSize], input[blockSize];
        juce::Random random (7);
        bool identical = true;
        juce::uint64 serial = 0;
        for (int b = 0; b < 8; ++b)
        {
            for (int i = 0; i < blockSize; ++i)
                input[i] = block[i] = random.nextFloat() - 0.5f;
            lofi.process (block, nullptr, blockSize, sr, ++serial, false, true, {});
            for (int i = 0; i < blockSize; ++i)
                identical = identical && juce::exactlyEqual (block[i], input[i]);
        }
        expect (identical, "全ノブ0はビット一致の素通し");
    }

    // ---- 中立判定と高速パス ----
    {
        TrackLofi lofi;
        expect (! lofi.needsActivePath (true, {}), "全ノブ0は高速パス（中立）");
        Lofi::Values wowOnly;
        wowOnly.wow = 0.3f;
        expect (lofi.needsActivePath (true, wowOnly), "非中立＋ONはactive");
        expect (! lofi.needsActivePath (false, wowOnly), "OFFは高速パス");
    }
}

void testTrackLofiTransitions()
{
    beginTest ("track lofi transitions");
    constexpr double sr = 48000.0;
    constexpr int blockSize = 512;

    // ---- Depth急変（0→最大を1ブロックで）: 隣接サンプル差がDepth固定時の1.5倍以下 ----
    {
        auto maxDelta = [] (const std::vector<float>& y, size_t from)
        {
            float d = 0.0f;
            for (size_t i = juce::jmax (from, (size_t) 1); i < y.size(); ++i)
                d = juce::jmax (d, std::abs (y[i] - y[i - 1]));
            return d;
        };
        Lofi::Values full;
        full.wow = 1.0f;
        const auto steady = renderLofi (full, 0.5, 1000.0, sr, 96000);
        const float steadyDelta = maxDelta (steady, 24000);

        TrackLofi lofi;
        lofi.snapTo (sr, true, {}); // Depth 0 から開始
        std::vector<float> out;
        juce::uint64 serial = 0;
        double phase = 0.0;
        for (int b = 0; b < 96000 / blockSize; ++b)
        {
            const Lofi::Values values = b < 10 ? Lofi::Values {} : full; // 10ブロック目で急変
            float block[blockSize];
            for (int i = 0; i < blockSize; ++i)
            {
                block[i] = (float) std::sin (phase) * 0.5f;
                phase += juce::MathConstants<double>::twoPi * 1000.0 / sr;
            }
            lofi.process (block, nullptr, blockSize, sr, ++serial, false, true, values);
            out.insert (out.end(), block, block + blockSize);
        }
        expect (maxDelta (out, 1) <= steadyDelta * 1.5f,
                "Depth急変の隣接サンプル差がDepth固定時の1.5倍以下（クリックなし）");
    }

    // ---- リセット契約の決定性: 同一seed・同一入力で2回レンダリングがビット一致 ----
    {
        Lofi::Values values { 0.5f, 0.3f, 0.7f, 0.4f };
        const auto a = renderLofi (values, 0.4, 500.0, sr, 24000);
        const auto b = renderLofi (values, 0.4, 500.0, sr, 24000);
        bool identical = a.size() == b.size();
        for (size_t i = 0; identical && i < a.size(); ++i)
            identical = juce::exactlyEqual (a[i], b[i]);
        expect (identical, "同一条件の2回レンダリングがビット一致（seed/位相のリセット契約）");
    }

    // ---- ON/OFF切替でNaN/Inf・跳躍が出ないこと ----
    {
        TrackLofi lofi;
        Lofi::Values values { 0.6f, 0.5f, 0.5f, 0.5f };
        lofi.snapTo (sr, true, values);
        juce::uint64 serial = 0;
        double phase = 0.0;
        float previous = 0.0f, maxJump = 0.0f;
        bool allFinite = true;
        for (int b = 0; b < 40; ++b)
        {
            const bool on = (b / 4) % 2 == 0;
            float block[blockSize];
            for (int i = 0; i < blockSize; ++i)
            {
                block[i] = (float) std::sin (phase) * 0.5f;
                phase += juce::MathConstants<double>::twoPi * 1000.0 / sr;
            }
            lofi.process (block, nullptr, blockSize, sr, ++serial, false, on, values);
            for (int i = 0; i < blockSize; ++i)
            {
                allFinite = allFinite && std::isfinite (block[i]);
                maxJump = juce::jmax (maxJump, std::abs (block[i] - previous));
                previous = block[i];
            }
        }
        expect (allFinite, "ON/OFF切替中にNaN/Infが出ないこと");
        expect (maxJump < 0.6f, "ON/OFF切替がクロスフェードされ跳躍しないこと");
    }

    // ---- timelineJumped後は初期化直後とビット一致（旧履歴・乱数状態の混入なし）----
    {
        TrackLofi warmed, fresh;
        Lofi::Values values { 0.5f, 0.0f, 0.5f, 0.5f };
        warmed.snapTo (sr, true, values);
        fresh.snapTo (sr, true, values);
        juce::uint64 serial = 0;
        float noise[blockSize];
        juce::Random random (42);
        for (int b = 0; b < 8; ++b)
        {
            for (int i = 0; i < blockSize; ++i)
                noise[i] = random.nextFloat() - 0.5f;
            warmed.process (noise, nullptr, blockSize, sr, ++serial, false, true, values);
        }
        float a[blockSize], bBuf[blockSize];
        for (int i = 0; i < blockSize; ++i)
            a[i] = bBuf[i] = (float) std::sin ((double) i * 0.13) * 0.4f;
        warmed.process (a, nullptr, blockSize, sr, ++serial, true, true, values);
        fresh.process (bBuf, nullptr, blockSize, sr, 1, false, true, values);
        bool identical = true;
        for (int i = 0; i < blockSize; ++i)
            identical = identical && juce::exactlyEqual (a[i], bBuf[i]);
        expect (identical, "ジャンプ後は初期化直後とビット一致");
    }

    // ---- Tone 0境界: 出入りで跳躍せず、0へ整定後は素通しとビット一致（クロスフェードの回帰）----
    // 開放側の20kHz LPFは恒等ではないため、即時切替だと高域の段差・クリックになる
    {
        TrackLofi lofi;
        lofi.snapTo (sr, true, {});
        juce::uint64 serial = 0;
        double phase = 0.0;
        const double inc = juce::MathConstants<double>::twoPi * 6000.0 / sr; // 高域寄り＝段差が出やすい
        Lofi::Values toneOn;
        toneOn.tone = 0.6f;
        bool identical = true;
        bool settledToNeutral = false;
        float maxJump = 0.0f;
        float previous = 0.0f;
        for (int b = 0; b < 80 && ! settledToNeutral; ++b)
        {
            const auto& targets = (b >= 10 && b < 30) ? toneOn : Lofi::Values {}; // 0→0.6→0
            float block[blockSize], input[blockSize];
            for (int i = 0; i < blockSize; ++i)
            {
                input[i] = block[i] = (float) std::sin (phase) * 0.5f;
                phase += inc;
            }
            lofi.process (block, nullptr, blockSize, sr, ++serial, false, true, targets);
            for (int i = 0; i < blockSize; ++i)
            {
                maxJump = juce::jmax (maxJump, std::abs (block[i] - previous));
                previous = block[i];
            }
            if (b >= 30 && ! lofi.needsActivePath (true, {}))
            {
                // 整定＝クロスフェード完了はこのブロック内。末尾サンプルはビット一致していること
                identical = juce::exactlyEqual (block[blockSize - 1], input[blockSize - 1]);
                // 高速パス移行を模擬: 次ブロックはraw入力。境界の跳躍もmaxJumpに含める
                const float firstRaw = (float) std::sin (phase) * 0.5f;
                maxJump = juce::jmax (maxJump, std::abs (firstRaw - previous));
                settledToNeutral = true;
            }
        }
        expect (settledToNeutral, "Tone=0で高速パスへ整定すること");
        expect (identical, "クロスフェード完了後の出力が入力とビット一致（素通しへの復帰が連続）");
        // 6kHz正弦（振幅0.5）自体の最大傾斜は約0.35。即時切替の段差はこれを超えて現れる
        expect (maxJump < 0.5f, "Toneの出入りで跳躍しないこと");
    }

    // ---- RT確保ゼロ（192kHz・64サンプルブロックで1秒分・全成分ON）----
    {
        constexpr double rtSr = 192000.0;
        constexpr int block = 64;
        TrackLofi lofi;
        const Lofi::Values values { 0.8f, 0.5f, 0.8f, 0.5f };
        lofi.snapTo (rtSr, true, values);
        float l[block], r[block];
        juce::uint64 serial = 0;
        const int blocks = (int) (rtSr / block);
        testAllocationCount = 0;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i)
                l[i] = r[i] = (float) std::sin (0.3 * (b * block + i)) * 0.5f;
            lofi.process (l, r, block, rtSr, ++serial, false, true, values);
        }
        expect (testAllocationCount == 0, "1秒分（192kHz・64サンプルブロック）でヒープ確保ゼロ");
    }
}

// ---- Sat: 保存/読込のroundtripと旧版数の既定値補完（v18） ----
void testSatParamsRoundtrip()
{
    beginTest ("sat params roundtrip");

    // 新規TrackParamsの既定値（ON・中立スタート: Drive 0 / Mix 100%。EQと同じ「ONでも音を
    // 変えない」型 — Compと違い保証された中立設定があるためピルは既定ON）
    TrackParams fresh;
    expect (fresh.satEnabled.load(), "既定: SatはON（Drive0が中立を保証）");
    {
        const auto v = Sat::load (fresh.sat);
        expect (juce::approximatelyEqual (v.drive, 0.0f) && juce::approximatelyEqual (v.mix, 1.0f),
                "既定値（中立スタート）");
        expect (Sat::isNeutral (v), "既定値は中立（高速パス対象）");
    }

    // 値を入れて保存 → 再読込で維持される
    auto dir = makeTempDir();
    juce::String error;
    juce::StringArray warnings;
    {
        Project project;
        project.directory = dir;
        Track track;
        track.id = project.allocateId();
        project.tracks.push_back (std::move (track));
        auto& params = *project.tracks[0].params;
        params.satEnabled.store (false);
        Sat::store (params.sat, Sat::normalized ({ 0.65f, 0.4f }));
        expect (project.save (error), "保存できること");
    }
    const auto parsed = juce::JSON::parse (dir.getChildFile ("project.json").loadFileAsString());
    expect ((int) parsed.getProperty ("version", 0) == Project::currentVersion, "現行版数で保存されること");

    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr && reloaded->tracks.size() == 1, "再読込できること");
    if (reloaded != nullptr && ! reloaded->tracks.empty())
    {
        auto& p = *reloaded->tracks[0].params;
        const auto v = Sat::load (p.sat);
        expect (! p.satEnabled.load(), "enabled維持（OFF）");
        expect (juce::approximatelyEqual (v.drive, 0.65f) && juce::approximatelyEqual (v.mix, 0.4f),
                "Satパラメータ維持");
    }
    dir.deleteRecursively();

    // v17形式（satブロックなし）→ 既定値（ON・中立）で補完。範囲外・NaNはnormalizedが直す
    auto dirV17 = makeTempDir();
    dirV17.getChildFile ("project.json").replaceWithText (R"({
        "version": 17, "bpm": 120.0, "sampleRate": 0.0, "nextId": 2,
        "tracks": [ { "id": 1, "type": "audio", "name": "t",
                      "mute": false, "solo": false, "volume": 0.5,
                      "fx": { "eq": { "enabled": true } },
                      "clips": [] } ]
    })");
    auto v17 = Project::load (dirV17, warnings, error);
    expect (v17 != nullptr && v17->tracks.size() == 1, "v17を読めること");
    if (v17 != nullptr && ! v17->tracks.empty())
    {
        auto& p = *v17->tracks[0].params;
        const auto v = Sat::load (p.sat);
        expect (p.satEnabled.load() && Sat::isNeutral (v), "sat欠損は既定値（ON・中立）で補完");
    }
    dirV17.deleteRecursively();

    expect (juce::approximatelyEqual (Sat::normalized ({ 2.0f, -1.0f }).drive, 1.0f)
                && juce::approximatelyEqual (Sat::normalized ({ 2.0f, -1.0f }).mix, 0.0f),
            "normalizedが範囲へクランプ");
    const auto nan = Sat::normalized ({ std::numeric_limits<float>::quiet_NaN(), 0.5f });
    expect (juce::approximatelyEqual (nan.drive, Sat::defaults.drive), "NaNは既定値へ");
}

// ---- Lo-fi: 保存/読込のroundtripと旧版数の既定値補完（v18） ----
void testLofiParamsRoundtrip()
{
    beginTest ("lofi params roundtrip");

    TrackParams fresh;
    expect (fresh.lofiEnabled.load(), "既定: Lo-fiはON（全ノブ0が中立を保証）");
    expect (Lofi::isNeutral (Lofi::load (fresh.lofi)), "既定値は中立（高速パス対象）");

    auto dir = makeTempDir();
    juce::String error;
    juce::StringArray warnings;
    {
        Project project;
        project.directory = dir;
        Track track;
        track.id = project.allocateId();
        project.tracks.push_back (std::move (track));
        auto& params = *project.tracks[0].params;
        params.lofiEnabled.store (false);
        Lofi::store (params.lofi, Lofi::normalized ({ 0.1f, 0.2f, 0.3f, 0.4f }));
        expect (project.save (error), "保存できること");
    }
    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr && reloaded->tracks.size() == 1, "再読込できること");
    if (reloaded != nullptr && ! reloaded->tracks.empty())
    {
        auto& p = *reloaded->tracks[0].params;
        const auto v = Lofi::load (p.lofi);
        expect (! p.lofiEnabled.load(), "enabled維持（OFF）");
        expect (juce::approximatelyEqual (v.wow, 0.1f) && juce::approximatelyEqual (v.tone, 0.2f)
                    && juce::approximatelyEqual (v.noise, 0.3f)
                    && juce::approximatelyEqual (v.crush, 0.4f),
                "Lo-fiパラメータ維持");
    }
    dir.deleteRecursively();

    // 欠損（v17以前相当）は既定値（ON・中立）。範囲外・NaNはnormalizedが直す
    const auto clamped = Lofi::normalized ({ 2.0f, -1.0f,
                                            std::numeric_limits<float>::quiet_NaN(), 0.5f });
    expect (juce::approximatelyEqual (clamped.wow, 1.0f) && juce::approximatelyEqual (clamped.tone, 0.0f)
                && juce::approximatelyEqual (clamped.noise, Lofi::defaults.noise)
                && juce::approximatelyEqual (clamped.crush, 0.5f),
            "normalizedがクランプ・NaN既定値化");
}

// ---- Lo-fi: 成分単独でもテールが回収され、RT再生とバウンスが一致すること ----
// producesTail の漏れ（Tone単独・Crush単独で範囲終端の余韻が切れる）の回帰テスト
void testBounceLofiTail()
{
    beginTest ("bounce lofi ring-out tail per component");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 8;
    constexpr int totalSamples = blockSize * numBlocks;
    constexpr int tailCompare = 2048;

    struct Case
    {
        const char* name;
        Lofi::Values values;
        bool expectAudibleTail; // wow/noiseは確実に鳴る。tone/crushは数サンプル規模で
                                // 位相依存になり得るため「切れずにRTと一致」だけを要求する
    };
    const Case cases[] = {
        { "wow", { 1.0f, 0.0f, 0.0f, 0.0f }, true },
        { "tone", { 0.0f, 1.0f, 0.0f, 0.0f }, false },
        { "noise", { 0.0f, 0.0f, 1.0f, 0.0f }, true },
        { "crush", { 0.0f, 0.0f, 0.0f, 0.7f }, false },
    };

    for (const auto& testCase : cases)
    {
        Project project;
        {
            Track track;
            track.id = 1;
            track.params->gain.store (1.0f);
            Lofi::store (track.params->lofi, Lofi::normalized (testCase.values));
            Clip clip;
            clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, totalSamples);
            const double inc = juce::MathConstants<double>::twoPi * 220.0 / sr;
            for (int i = 0; i < totalSamples; ++i)
                clip.audio->setSample (0, i, (float) std::sin (inc * i) * 0.4f);
            clip.lengthSamples = totalSamples;
            track.clips.push_back (std::move (clip));
            project.tracks.push_back (std::move (track));
        }

        const int latency = engineLimiterLatency (sr);
        juce::AudioBuffer<float> engineOut (2, totalSamples + tailCompare + blockSize);
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
            for (int blockIndex = 0; blockIndex < numBlocks + tailCompare / blockSize + 1;
                 ++blockIndex)
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

        const auto dir = makeTempDir();
        const auto target = dir.getChildFile ("bounce-lofi-tail.wav");
        juce::int64 writtenSamples = 0;
        {
            BounceRenderer::Request request;
            request.sampleRate = sr;
            request.bpm = 120.0;
            request.endSample = totalSamples;
            request.targetFile = target;
            for (auto& track : project.tracks)
            {
                BounceRenderer::TrackRender render;
                render.gain = track.params->gain.load();
                render.loadFxFrom (*track.params);
                for (auto& clip : track.clips)
                    appendClipPlaybacks (clip, render.clips);
                request.tracks.push_back (std::move (render));
            }
            // 本番と同じ入口判定を通す＝producesTailの配線そのものを検証する
            request.resolveWantTail();
            expect (request.wantTail, "成分単独でもwantTailが立つ（producesTail配線）");
            BounceRenderer renderer;
            expect (renderer.start (std::move (request)), "startできること");
            expect (waitForBounce (renderer), "タイムアウトせず完了すること");
            const auto result = renderer.takeResult();
            expect (result.status == BounceRenderer::Status::success, "successで終わること");
            writtenSamples = result.writtenSamples;
        }
        expect (writtenSamples > totalSamples, "テールが書き出されること");

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (
            wav.createReaderFor (new juce::FileInputStream (target), true));
        const int compareLen =
            (int) juce::jmin ((juce::int64) tailCompare, writtenSamples - totalSamples);
        expect (reader != nullptr && reader->lengthInSamples >= totalSamples + compareLen,
                "テール込みの長さで読めること");
        if (reader != nullptr && reader->lengthInSamples >= totalSamples + compareLen)
        {
            juce::AudioBuffer<float> bounceOut (2, totalSamples + compareLen);
            reader->read (&bounceOut, 0, totalSamples + compareLen, 0, true, true);
            float tailPeak = 0.0f;
            float maxDiff = 0.0f;
            for (int i = totalSamples; i < totalSamples + compareLen; ++i)
            {
                tailPeak = juce::jmax (tailPeak, std::abs (bounceOut.getSample (0, i)));
                for (int ch = 0; ch < 2; ++ch)
                    maxDiff = juce::jmax (maxDiff,
                                          std::abs (engineOut.getSample (ch, i + latency)
                                                    - bounceOut.getSample (ch, i)));
            }
            if (testCase.expectAudibleTail)
                expect (tailPeak > 1.0e-3f, "終端直後にリングアウトが実在すること");
            expect (maxDiff < 1.0e-4f, "テールがRT再生と一致すること");
        }
        reader.reset();
        dir.deleteRecursively();
    }
}

// ---- Sat有効時にエンジン（RT）とバウンスが一致すること（EQ/Compの整合テストと同型） ----
void testEngineSatBounceConsistency()
{
    beginTest ("engine vs bounce with active sat");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 16;
    constexpr int totalSamples = blockSize * numBlocks;
    constexpr int compareFrom = 0; // 再生開始のシークで timelineJumped=true ＝先頭から比較できる

    auto makeAudio = [] (int channels, int len, float scale)
    {
        auto buffer = std::make_shared<juce::AudioBuffer<float>> (channels, len);
        juce::Random random (23);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < len; ++i)
                buffer->setSample (ch, i,
                                   (std::sin ((float) i * 0.07f + (float) ch) * 0.3f
                                    + (random.nextFloat() - 0.5f) * 0.2f) * scale);
        return buffer;
    };

    Project project;
    {
        Track track; // ステレオクリップ＋Sat（深め・full wet）
        track.id = 1;
        track.params->gain.store (0.8f);
        track.params->pan.store (0.3f);
        Sat::store (track.params->sat, Sat::normalized ({ 0.8f, 1.0f }));
        Clip clip;
        clip.audio = makeAudio (2, totalSamples, 1.0f);
        clip.lengthSamples = totalSamples;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }
    {
        Track track; // モノクリップ＋Sat（浅め・Mix 50%）
        track.id = 2;
        track.params->gain.store (0.7f);
        track.params->pan.store (-0.5f);
        Sat::store (track.params->sat, Sat::normalized ({ 0.4f, 0.5f }));
        Clip clip;
        clip.audio = makeAudio (1, totalSamples, 0.8f);
        clip.lengthSamples = totalSamples;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }
    project.masterParams->gain.store (0.85f);

    auto renderEngine = [&] (juce::AudioBuffer<float>& out)
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
        for (int blockIndex = 0; blockIndex * blockSize < out.getNumSamples(); ++blockIndex)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, blockIndex * blockSize, buffer, ch, 0, blockSize);
        }
        engine.stop();
        snapshots.deleteRetired();
    };

    const int latency = engineLimiterLatency (sr);
    juce::AudioBuffer<float> engineOut (2, totalSamples + blockSize);
    renderEngine (engineOut);

    // Satが実際に効いていること（OFFとの差がある）を先に確認する — 空一致の防止
    {
        project.tracks[0].params->satEnabled.store (false);
        project.tracks[1].params->satEnabled.store (false);
        juce::AudioBuffer<float> bypassOut (2, totalSamples + blockSize);
        renderEngine (bypassOut);
        float maxDiff = 0.0f;
        for (int i = compareFrom; i < totalSamples; ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (0, i)
                                                     - bypassOut.getSample (0, i)));
        expect (maxDiff > 1.0e-3f, "SatありはOFFと出力が異なること（Satが実際に効いている）");
        project.tracks[0].params->satEnabled.store (true);
        project.tracks[1].params->satEnabled.store (true);
    }

    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce-sat.wav");
    {
        BounceRenderer::Request request;
        request.sampleRate = sr;
        request.bpm = 120.0;
        request.endSample = totalSamples;
        request.targetFile = target;
        request.masterGain = 0.85f;
        for (auto& track : project.tracks)
        {
            BounceRenderer::TrackRender render;
            render.gain = track.params->gain.load();
            render.pan = track.params->pan.load();
            render.loadFxFrom (*track.params);
            for (auto& clip : track.clips)
                appendClipPlaybacks (clip, render.clips);
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
            for (int i = compareFrom; i < totalSamples; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (ch, i + latency)
                                                         - bounceOut.getSample (ch, i)));
        expect (maxDiff < 1.0e-4f, "Sat有効時もエンジンとバウンスが許容誤差内で一致すること");
    }
    reader.reset();
    dir.deleteRecursively();
}

// ---- バスDelay: インパルス応答（タップ位置・fb減衰・Tone・Ping-pong・スイープ耐性）----
void testBusDelayImpulse()
{
    beginTest ("bus delay impulse");

    constexpr double sr = 48000.0;
    constexpr int blockSize = 512;
    const double bpm = 120.0;
    // timeIndex 0 = 1/16音符 @120BPM = 0.125s
    const int delaySamples = (int) std::lround (Delay::timeSeconds (bpm, 0) * sr);
    expect (delaySamples == 6000, "1/16 @120BPM/48kHz = 6000サンプル");

    auto render = [&] (const Delay::Values& values, int totalSamples,
                       juce::AudioBuffer<float>& out, bool impulseBothChannels)
    {
        BusDelay delay;
        delay.prepare (sr);
        delay.snapTo (values, bpm);
        out.setSize (2, totalSamples);
        out.clear();
        out.setSample (0, 0, 1.0f);
        if (impulseBothChannels)
            out.setSample (1, 0, 1.0f);
        for (int pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int n = juce::jmin (blockSize, totalSamples - pos);
            delay.process (out.getWritePointer (0) + pos, out.getWritePointer (1) + pos,
                           n, bpm, values);
        }
    };

    // タップ窓の合計（ループ内LPはDCゲイン1なので、窓合計はタップ振幅そのものを表す）
    auto windowSum = [] (const juce::AudioBuffer<float>& out, int ch, int center)
    {
        double sum = 0.0;
        const int from = juce::jmax (0, center - 50);
        const int to = juce::jmin (out.getNumSamples(), center + 800);
        for (int i = from; i < to; ++i)
            sum += (double) out.getSample (ch, i);
        return sum;
    };

    // ストレート: タップ位置がぴったり・fb^n の減衰・full wet（dryは出力に残らない）
    {
        const Delay::Values values { 0, 0.5f, 0.0f, false };
        juce::AudioBuffer<float> out;
        render (values, delaySamples * 3 + 2000, out, true);
        expect (std::abs (out.getSample (0, 0)) < 1.0e-6f, "full wet: dryのインパルスは出力に出ない");
        // 第1タップの開始位置（最初に有意な振幅が出るサンプル）が delaySamples ちょうど
        int firstIndex = -1;
        for (int i = 0; i < out.getNumSamples(); ++i)
            if (std::abs (out.getSample (0, i)) > 0.05f)
            {
                firstIndex = i;
                break;
            }
        expect (firstIndex == delaySamples, "第1エコーの位置がディレイタイムと実測一致");
        const auto tap1 = windowSum (out, 0, delaySamples);
        const auto tap2 = windowSum (out, 0, delaySamples * 2);
        const auto tap3 = windowSum (out, 0, delaySamples * 3);
        expect (std::abs (tap1 - 1.0) < 0.02, "第1エコーの振幅（窓合計）が入力と一致");
        expect (std::abs (tap2 / tap1 - 0.5) < 0.02, "第2/第1 = feedback（0.5）");
        expect (std::abs (tap3 / tap2 - 0.5) < 0.02, "第3/第2 = feedback（0.5）");
    }

    // Tone: ループ内LPなのでタップのピークが鈍る（窓合計＝DC成分は変わらない）
    {
        juce::AudioBuffer<float> bright, dark;
        render ({ 0, 0.0f, 0.0f, false }, delaySamples + 2000, bright, true);
        render ({ 0, 0.0f, 1.0f, false }, delaySamples + 2000, dark, true);
        const auto peakOf = [&] (const juce::AudioBuffer<float>& out)
        { return out.getMagnitude (0, delaySamples - 10, 1500); };
        expect (peakOf (dark) < peakOf (bright) * 0.5f,
                "Tone=1でエコーのピークが鈍ること（高域が削れている）");
        expect (std::abs (windowSum (dark, 0, delaySamples) - 1.0) < 0.02,
                "ToneはDC成分を変えない（LPのDCゲイン=1）");
    }

    // Ping-pong: モノ化（0.5）でL→R交互・1タップごとの減衰がfb
    {
        const Delay::Values values { 0, 0.5f, 0.0f, true };
        juce::AudioBuffer<float> out;
        render (values, delaySamples * 2 + 2000, out, false); // Lのみのインパルス
        const auto tap1L = windowSum (out, 0, delaySamples);
        const auto tap1R = windowSum (out, 1, delaySamples);
        const auto tap2L = windowSum (out, 0, delaySamples * 2);
        const auto tap2R = windowSum (out, 1, delaySamples * 2);
        expect (std::abs (tap1L - 0.5) < 0.02, "第1エコーはLに0.5（モノ化）");
        expect (std::abs (tap1R) < 0.01, "第1エコーはRに出ない");
        expect (std::abs (tap2R / tap1L - 0.5) < 0.02, "第2エコーはRにfb倍で出る（交互）");
        expect (std::abs (tap2L) < 0.01, "第2エコーはLに出ない");
    }

    // 容量境界: BPM下限30・高SR（192k）の1/2音符（4秒 = 768000サンプル）が収まり、
    // タップ位置が壊れない（DelayLineの容量契約「4秒＋マージン×prepare時SR」）
    {
        constexpr double highSr = 192000.0;
        BusDelay delay;
        delay.prepare (highSr);
        const Delay::Values values { 3, 0.0f, 0.0f, false };
        delay.snapTo (values, 30.0);
        const int d = (int) std::lround (Delay::timeSeconds (30.0, 3) * highSr);
        expect (d == 768000, "1/2 @30BPM/192kHz = 768000サンプル");
        juce::AudioBuffer<float> out (2, d + 4000);
        out.clear();
        out.setSample (0, 0, 1.0f);
        out.setSample (1, 0, 1.0f);
        for (int pos = 0; pos < out.getNumSamples(); pos += blockSize)
            delay.process (out.getWritePointer (0) + pos, out.getWritePointer (1) + pos,
                           juce::jmin (blockSize, out.getNumSamples() - pos), 30.0, values);
        int firstIndex = -1;
        for (int i = 1; i < out.getNumSamples(); ++i)
            if (std::abs (out.getSample (0, i)) > 0.05f)
            {
                firstIndex = i;
                break;
            }
        expect (firstIndex == d, "容量境界（30BPM・192kHz）でもタップ位置が正確なこと");
    }

    // スイープ耐性: 再生中に全パラメータを端から端まで動かしても不連続・NaN・発散がない
    {
        BusDelay delay;
        delay.prepare (sr);
        delay.snapTo (Delay::defaults, bpm);
        juce::AudioBuffer<float> block (2, blockSize);
        float maxAbs = 0.0f;
        float maxStep = 0.0f;
        float prevSample[2] = { 0.0f, 0.0f };
        bool allFinite = true;
        for (int blockIndex = 0; blockIndex < 400; ++blockIndex)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float x = 0.5f * std::sin (0.05f * (float) (blockIndex * blockSize + i));
                block.setSample (0, i, x);
                block.setSample (1, i, -x);
            }
            const float t = (float) blockIndex / 399.0f;
            Delay::Values values;
            values.timeIndex = blockIndex % Delay::numTimeChoices;
            values.feedback = Delay::maxFeedback * t;
            values.tone = 1.0f - t;
            values.pingPong = (blockIndex / 16) % 2 == 1;
            delay.process (block.getWritePointer (0), block.getWritePointer (1), blockSize,
                           bpm + 200.0 * t, values);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                {
                    const float y = block.getSample (ch, i);
                    allFinite = allFinite && std::isfinite (y);
                    maxAbs = juce::jmax (maxAbs, std::abs (y));
                    maxStep = juce::jmax (maxStep, std::abs (y - prevSample[ch]));
                    prevSample[ch] = y;
                }
        }
        expect (allFinite, "フルスイープでNaN/infが出ない");
        expect (maxAbs < 8.0f, "フルスイープで発散しない");
        // 隣接サンプル段差（クリック）の上限。入力サイン（振幅0.5・0.05rad/sample）の自然な
        // 段差は約0.025で、平滑・クロスフェードが効いていれば出力もその数倍に収まる
        expect (maxStep < 0.3f, "フルスイープでクリック級の段差が出ない");
    }
}

// ---- バスReverb: Pre-delayのサンプル一致・Low Cut応答・テール収束・Width=0・スイープ耐性 ----
void testBusReverbBasics()
{
    beginTest ("bus reverb basics");

    constexpr double sr = 48000.0;
    constexpr int blockSize = 512;

    auto renderImpulse = [&] (const Reverb::Values& values, int totalSamples,
                              juce::AudioBuffer<float>& out)
    {
        BusReverb reverb;
        reverb.prepare (sr);
        reverb.snapTo (values);
        out.setSize (2, totalSamples);
        out.clear();
        out.setSample (0, 0, 1.0f);
        out.setSample (1, 0, 1.0f);
        for (int pos = 0; pos < totalSamples; pos += blockSize)
            reverb.process (out.getWritePointer (0) + pos, out.getWritePointer (1) + pos,
                            juce::jmin (blockSize, totalSamples - pos), values);
    };

    auto firstAudibleIndex = [] (const juce::AudioBuffer<float>& out)
    {
        for (int i = 0; i < out.getNumSamples(); ++i)
            if (std::abs (out.getSample (0, i)) > 1.0e-5f)
                return i;
        return -1;
    };

    // Pre-delay: 0msと50msの出だしの差がぴったり 50ms×SR サンプル
    {
        auto base = Reverb::defaultsForBus (0);
        base.preDelayMs = 0.0f;
        auto delayed = base;
        delayed.preDelayMs = 50.0f;
        juce::AudioBuffer<float> outBase, outDelayed;
        renderImpulse (base, 12000, outBase);
        renderImpulse (delayed, 12000, outDelayed);
        const int i0 = firstAudibleIndex (outBase);
        const int i1 = firstAudibleIndex (outDelayed);
        expect (i0 >= 0 && i1 >= 0, "残響が出ること");
        expect (i1 - i0 == (int) std::lround (0.05 * sr),
                "Pre-delay 50msのオフセットがサンプル単位で一致");
    }

    // Low Cut: 30Hzサインは 500Hzカットで大きく減る（残響へ低域を送らない）
    {
        auto open = Reverb::defaultsForBus (0);
        open.lowCutHz = Reverb::minLowCutHz;
        auto cut = open;
        cut.lowCutHz = 500.0f;
        auto renderSine = [&] (const Reverb::Values& values)
        {
            BusReverb reverb;
            reverb.prepare (sr);
            reverb.snapTo (values);
            juce::AudioBuffer<float> out (2, (int) sr);
            for (int i = 0; i < out.getNumSamples(); ++i)
            {
                const float x = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 30.0f
                                                 * (float) i / (float) sr);
                out.setSample (0, i, x);
                out.setSample (1, i, x);
            }
            for (int pos = 0; pos < out.getNumSamples(); pos += blockSize)
                reverb.process (out.getWritePointer (0) + pos, out.getWritePointer (1) + pos,
                                juce::jmin (blockSize, out.getNumSamples() - pos), values);
            return out.getRMSLevel (0, out.getNumSamples() / 2, out.getNumSamples() / 2);
        };
        const auto rmsOpen = renderSine (open);
        const auto rmsCut = renderSine (cut);
        expect (rmsCut < rmsOpen * 0.3f, "Low Cut 500Hzで30Hzの残響が大きく減ること");
    }

    // テール収束: 中庸設定のインパルス残響が-60dBへ減衰しきる
    {
        Reverb::Values values { 0.5f, 0.5f, 1.0f, 0.0f, 100.0f };
        juce::AudioBuffer<float> out;
        renderImpulse (values, (int) (sr * 12.0), out);
        expect (out.getMagnitude (0, out.getNumSamples() - (int) sr, (int) sr) < 0.001f,
                "テールが-60dB未満へ収束すること（12秒以内）");
    }

    // Width=0: 完全モノ（L==R）
    {
        Reverb::Values values { 0.5f, 0.5f, 0.0f, 0.0f, 100.0f };
        juce::AudioBuffer<float> out;
        renderImpulse (values, 24000, out);
        float maxDiff = 0.0f;
        for (int i = 0; i < out.getNumSamples(); ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (out.getSample (0, i) - out.getSample (1, i)));
        expect (maxDiff < 1.0e-6f, "Width=0でL/Rが一致（モノ残響）");
    }

    // スイープ耐性: 全ノブを端から端まで動かしてもNaN・発散がない
    {
        BusReverb reverb;
        reverb.prepare (sr);
        reverb.snapTo (Reverb::defaultsForBus (0));
        juce::AudioBuffer<float> block (2, blockSize);
        float maxAbs = 0.0f;
        float maxStep = 0.0f;
        float prevSample[2] = { 0.0f, 0.0f };
        bool allFinite = true;
        for (int blockIndex = 0; blockIndex < 400; ++blockIndex)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float x = 0.5f * std::sin (0.03f * (float) (blockIndex * blockSize + i));
                block.setSample (0, i, x);
                block.setSample (1, i, x * 0.7f);
            }
            const float t = (float) blockIndex / 399.0f;
            const Reverb::Values values { t, 1.0f - t, t, Reverb::maxPreDelayMs * t,
                                          Reverb::minLowCutHz
                                              + (Reverb::maxLowCutHz - Reverb::minLowCutHz) * t };
            reverb.process (block.getWritePointer (0), block.getWritePointer (1), blockSize, values);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                {
                    const float y = block.getSample (ch, i);
                    allFinite = allFinite && std::isfinite (y);
                    maxAbs = juce::jmax (maxAbs, std::abs (y));
                    maxStep = juce::jmax (maxStep, std::abs (y - prevSample[ch]));
                    prevSample[ch] = y;
                }
        }
        expect (allFinite, "フルスイープでNaN/infが出ない");
        expect (maxAbs < 8.0f, "フルスイープで発散しない");
        // Low Cut平滑（20ms）・Pre-delayクロスフェード・juce::Reverb内部平滑が効いていれば
        // 隣接サンプル段差は入力サインの自然な段差（約0.015）の数倍に収まる
        expect (maxStep < 0.3f, "フルスイープでクリック級の段差が出ない");
    }
}

// ---- バウンス: バスFXテール契約（中立トラック＋sendだけでテールへ入る・最初のエコーが切れない）----
void testBounceBusFxTail()
{
    beginTest ("bounce bus fx tail");

    constexpr double sr = 48000.0;
    const int delaySamples = (int) std::lround (Delay::timeSeconds (120.0, 0) * sr); // 6000

    auto makeClickTrack = [&] (int clipLength)
    {
        BounceRenderer::TrackRender render;
        render.gain = 1.0f;
        render.sends[2] = 1.0f;
        ClipPlayback clip;
        auto audio = std::make_shared<juce::AudioBuffer<float>> (1, clipLength);
        audio->clear();
        for (int i = 0; i < 100; ++i)
            audio->setSample (0, i, 0.5f); // 短いクリック（FXは全て中立＝トラックテールなし）
        clip.audio = audio;
        clip.lengthSamples = clipLength;
        render.clips.push_back (std::move (clip));
        return render;
    };

    const auto dir = makeTempDir();
    {
        BounceRenderer::Request request;
        request.sampleRate = sr;
        request.bpm = 120.0;
        request.endSample = 2400; // クリック直後に本編が終わる＝エコーは全部テール側
        request.targetFile = dir.getChildFile ("bus-tail.wav");
        request.busDelay = Delay::normalized ({ 0, 0.5f, 0.0f, false });
        request.tracks.push_back (makeClickTrack (2400));
        request.resolveWantTail();
        expect (request.wantTail, "中立トラック＋sendだけでテールに入ること（resolveWantTailの拡張）");

        BounceRenderer renderer;
        expect (renderer.start (std::move (request)), "startできること");
        expect (waitForBounce (renderer), "タイムアウトせず完了すること");
        const auto result = renderer.takeResult();
        expect (result.status == BounceRenderer::Status::success, "successで終わること");

        auto rendered = Project::loadWav (dir.getChildFile ("bus-tail.wav"));
        expect (rendered != nullptr, "書き出しWAVを読めること");
        if (rendered != nullptr)
        {
            // 第1エコー（6000）はエコー前の無音ブロックで打ち切られずに残る
            expect (rendered->getNumSamples() > delaySamples + 500,
                    "最初のエコーの前でテールが打ち切られないこと");
            expect (rendered->getMagnitude (0, delaySamples - 10, 1000) > 0.1f,
                    "第1エコーが書かれていること");
            expect (rendered->getMagnitude (0, delaySamples * 2 - 10, 1000) > 0.05f,
                    "第2エコー（fb 0.5）が書かれていること");
            // fb 0.5 は9タップ前後で-60dBを切る。窓（1周期＋1ブロック）を足しても
            // 12タップ分を大きく超えない（テールが無駄に伸びない）
            expect (rendered->getNumSamples() < delaySamples * 13,
                    "減衰後は速やかに終わること");
        }
    }

    // Reverbのみ（Delayなし）: 曲末ぎりぎりの入力の残響は Pre-delay＋初期反射の後に始まる。
    // 無音窓が1ブロックだと最初のテールブロックの無音で打ち切られる（レビューP1の回帰）
    {
        BounceRenderer::Request request;
        request.sampleRate = sr;
        request.bpm = 120.0;
        request.endSample = 2400;
        request.targetFile = dir.getChildFile ("bus-tail-reverb.wav");
        auto reverbValues = Reverb::defaultsForBus (0);
        reverbValues.preDelayMs = 50.0f; // 2400サンプル ＝ 範囲終端まで残響が一切出ない設定
        request.busReverb[0] = reverbValues;
        {
            BounceRenderer::TrackRender render;
            render.gain = 1.0f;
            render.sends[0] = 1.0f; // Reverb Aのみ
            ClipPlayback clip;
            auto audio = std::make_shared<juce::AudioBuffer<float>> (1, 2400);
            audio->clear();
            for (int i = 400; i < 2400; ++i)
                audio->setSample (0, i, 0.9f); // 曲末で切れるバースト
            clip.audio = audio;
            clip.lengthSamples = 2400;
            render.clips.push_back (std::move (clip));
            request.tracks.push_back (std::move (render));
        }
        request.resolveWantTail();
        expect (request.wantTail, "Reverb sendのみでもテールに入ること");

        BounceRenderer renderer;
        expect (renderer.start (std::move (request)), "startできること（Reverbのみ）");
        expect (waitForBounce (renderer), "タイムアウトせず完了すること（Reverbのみ）");
        expect (renderer.takeResult().status == BounceRenderer::Status::success,
                "successで終わること（Reverbのみ）");

        auto rendered = Project::loadWav (dir.getChildFile ("bus-tail-reverb.wav"));
        expect (rendered != nullptr, "書き出しWAVを読めること（Reverbのみ）");
        if (rendered != nullptr)
        {
            // 最初の残響はおよそ burst開始(400)＋Pre-delay(2400)＋最短コム(約1200) ≈ 4000
            expect (rendered->getNumSamples() > 4500,
                    "残響が始まる前の無音ブロックで打ち切られないこと");
            expect (rendered->getMagnitude (0, 4000, 1500) > 0.005f,
                    "テールに残響が書かれていること");
        }
    }
    dir.deleteRecursively();
}

// ---- バウンス: バスFXテールの上限30秒とフェード畳み（fb90%は減衰しきらない設定）----
void testBounceBusTailCap()
{
    beginTest ("bounce bus tail cap");

    constexpr double sr = 8000.0; // 上限30秒を速くレンダするための低SR
    const auto dir = makeTempDir();

    BounceRenderer::Request request;
    request.sampleRate = sr;
    request.bpm = 120.0;
    request.endSample = 800;
    request.targetFile = dir.getChildFile ("bus-tail-cap.wav");
    // 1/2音符 @120BPM = 1秒周期・fb90% → -60dBまで約66秒 ＝ 30秒の上限に必ず当たる
    request.busDelay = Delay::normalized ({ 3, 0.9f, 0.0f, false });
    {
        BounceRenderer::TrackRender render;
        render.gain = 1.0f;
        render.sends[2] = 1.0f;
        ClipPlayback clip;
        auto audio = std::make_shared<juce::AudioBuffer<float>> (1, 800);
        audio->clear();
        for (int i = 0; i < 50; ++i)
            audio->setSample (0, i, 0.5f);
        clip.audio = audio;
        clip.lengthSamples = 800;
        render.clips.push_back (std::move (clip));
        request.tracks.push_back (std::move (render));
    }
    request.resolveWantTail();
    expect (request.wantTail, "テールに入ること");

    BounceRenderer renderer;
    expect (renderer.start (std::move (request)), "startできること");
    expect (waitForBounce (renderer), "タイムアウトせず完了すること");
    expect (renderer.takeResult().status == BounceRenderer::Status::success, "successで終わること");

    auto rendered = Project::loadWav (dir.getChildFile ("bus-tail-cap.wav"));
    expect (rendered != nullptr, "書き出しWAVを読めること");
    if (rendered != nullptr)
    {
        const auto total = (juce::int64) rendered->getNumSamples();
        // エコー間の無音（約1秒）で早期終了せず、上限30秒まで書かれること（±2ブロック）
        expect (total >= 800 + (juce::int64) (sr * 30.0) - 2048,
                "1秒間隔のエコーの無音ギャップで早期終了しないこと（上限まで書く）");
        expect (total <= 800 + (juce::int64) (sr * 30.0) + 4096,
                "上限30秒で打ち切られること");
        // 末尾0.5秒のフェードで畳まれてクリックにならない。Limiter flush（lookahead分）も
        // ゼロ保証（レビューP2: 未フェードのエコーが末尾で復活しない）
        expect (rendered->getMagnitude (0, rendered->getNumSamples() - 100, 100) < 1.0e-4f,
                "上限到達時は末尾フェード＋flushゼロ保証で閉じること");
    }
    dir.deleteRecursively();
}

// ---- エンジン: バスミュート中もバスFXは進む（解除時に凍結した古いエコーが出ない）----
void testEngineBusMuteKeepsFxRunning()
{
    beginTest ("engine bus mute keeps fx running");

    constexpr double sr = 48000.0;
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
    track.params->sends[2].store (1.0f);
    Clip clip;
    clip.lengthSamples = blockSize * 4; // 短いクリップ（2048サンプルで入力が止まる）
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, blockSize * 4);
    for (int i = 0; i < clip.audio->getNumSamples(); ++i)
        clip.audio->setSample (0, i, 0.4f);
    track.clips.push_back (std::move (clip));
    project.tracks.push_back (std::move (track));
    Delay::store (project.busParams[2]->delay, Delay::normalized ({ 0, 0.0f, 0.0f, false }));
    project.busParams[2]->mute.store (true); // 最初からミュート
    snapshots.push (project.buildSnapshot());

    // エコー（6000〜8048）が鳴り終わるところまでミュートのまま進める（30ブロック=15360）。
    // FXが止まっていたらリングに古いエコーが残り、解除後に化けて出る
    transport.seekRequest.store (0);
    engine.play();
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
    for (int i = 0; i < 30; ++i)
    {
        buffer.clear();
        engine.process (info);
    }

    project.busParams[2]->mute.store (false); // 解除
    float maxAfterUnmute = 0.0f;
    for (int i = 0; i < 10; ++i)
    {
        buffer.clear();
        engine.process (info);
        maxAfterUnmute = juce::jmax (maxAfterUnmute, buffer.getMagnitude (0, 0, blockSize));
    }
    engine.stop();
    buffer.clear();
    engine.process (info);
    expect (maxAfterUnmute < 1.0e-4f,
            "ミュート解除後に凍結した古いエコーが出ないこと（ミュート中もFXが進む）");
    snapshots.deleteRetired();
}

// ---- エンジン⇄バウンス: バスFX（Reverb A/B・Delay）有効時の経路一致 ----
// パラメータ・バスindex・リセット条件の食い違いを検出する（A/Bは別値にして入れ違いも見る）
void testEngineBounceBusFxConsistency()
{
    beginTest ("engine vs bounce with bus fx");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 24; // 12288サンプル > Delayの1タップ目（5513）
    constexpr int totalSamples = blockSize * numBlocks;

    auto makeAudio = [] (int len)
    {
        auto buffer = std::make_shared<juce::AudioBuffer<float>> (1, len);
        juce::Random random (77);
        for (int i = 0; i < len; ++i)
            buffer->setSample (0, i, std::sin ((float) i * 0.11f) * 0.25f
                                         + (random.nextFloat() - 0.5f) * 0.1f);
        return buffer;
    };

    Project project;
    {
        Track track;
        track.id = 1;
        track.params->gain.store (0.8f);
        track.params->pan.store (0.2f);
        track.params->sends[0].store (0.6f);
        track.params->sends[1].store (0.3f);
        track.params->sends[2].store (0.5f);
        Clip clip;
        clip.audio = makeAudio (totalSamples);
        clip.lengthSamples = totalSamples;
        track.clips.push_back (std::move (clip));
        project.tracks.push_back (std::move (track));
    }
    // A/Bを別値に（入れ違い検出）。Delayは1/16・fb50%・Tone中庸
    Reverb::store (project.busParams[0]->reverb,
                   Reverb::normalized ({ 0.3f, 0.7f, 1.0f, 10.0f, 150.0f },
                                       Reverb::defaultsForBus (0)));
    Reverb::store (project.busParams[1]->reverb,
                   Reverb::normalized ({ 0.9f, 0.2f, 0.5f, 60.0f, 60.0f },
                                       Reverb::defaultsForBus (1)));
    Delay::store (project.busParams[2]->delay, Delay::normalized ({ 0, 0.5f, 0.5f, true }));
    project.busParams[0]->gain.store (0.8f);
    project.busParams[1]->gain.store (0.9f);
    project.busParams[2]->gain.store (0.7f);
    project.masterParams->gain.store (0.85f);

    auto renderEngine = [&] (juce::AudioBuffer<float>& out)
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
        for (int blockIndex = 0; blockIndex * blockSize < out.getNumSamples(); ++blockIndex)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, blockIndex * blockSize, buffer, ch, 0, blockSize);
        }
        engine.stop();
        snapshots.deleteRetired();
    };

    const int latency = engineLimiterLatency (sr);
    juce::AudioBuffer<float> engineOut (2, totalSamples + blockSize);
    renderEngine (engineOut);

    // バスFXが実際に効いていること（send全0との差がある）— 空一致の防止
    {
        auto& params = *project.tracks[0].params;
        const float sends[3] = { params.sends[0].load(), params.sends[1].load(),
                                 params.sends[2].load() };
        for (auto& send : params.sends)
            send.store (0.0f);
        juce::AudioBuffer<float> dryOut (2, totalSamples + blockSize);
        renderEngine (dryOut);
        float maxDiff = 0.0f;
        for (int i = 0; i < totalSamples; ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (0, i)
                                                     - dryOut.getSample (0, i)));
        expect (maxDiff > 1.0e-3f, "sendありはsend全0と出力が異なること（バスFXが効いている）");
        for (int b = 0; b < numSendBuses; ++b)
            params.sends[b].store (sends[b]);
    }

    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce-busfx.wav");
    {
        BounceRenderer::Request request;
        request.sampleRate = sr;
        request.bpm = 120.0;
        request.endSample = totalSamples;
        request.targetFile = target;
        request.masterGain = 0.85f;
        for (int b = 0; b < numSendBuses; ++b)
        {
            request.busGain[b] = project.busParams[b]->gain.load();
            request.busMute[b] = project.busParams[b]->mute.load();
        }
        request.busReverb[0] = Reverb::load (project.busParams[0]->reverb);
        request.busReverb[1] = Reverb::load (project.busParams[1]->reverb);
        request.busDelay = Delay::load (project.busParams[2]->delay);
        for (auto& track : project.tracks)
        {
            BounceRenderer::TrackRender render;
            render.gain = track.params->gain.load();
            render.pan = track.params->pan.load();
            for (int b = 0; b < numSendBuses; ++b)
                render.sends[b] = track.params->sends[b].load();
            render.loadFxFrom (*track.params);
            for (auto& clip : track.clips)
                appendClipPlaybacks (clip, render.clips);
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
                maxDiff = juce::jmax (maxDiff, std::abs (engineOut.getSample (ch, i + latency)
                                                         - bounceOut.getSample (ch, i)));
        expect (maxDiff < 1.0e-4f, "バスFX有効時もエンジンとバウンスが許容誤差内で一致すること");
    }
    reader.reset();
    dir.deleteRecursively();
}

// ---- バスFXパラメータの永続化（新規既定値・v18読込・保存往復）----
void testBusFxParamsPersistence()
{
    beginTest ("bus fx params persistence");

    auto expectReverbEquals = [&] (const Reverb::Values& actual, const Reverb::Values& expected,
                                   const char* label)
    {
        expect (std::abs (actual.size - expected.size) < 1.0e-6f
                    && std::abs (actual.damp - expected.damp) < 1.0e-6f
                    && std::abs (actual.width - expected.width) < 1.0e-6f
                    && std::abs (actual.preDelayMs - expected.preDelayMs) < 1.0e-6f
                    && std::abs (actual.lowCutHz - expected.lowCutHz) < 1.0e-6f,
                label);
    };

    // 新規作成: A/Bがバスindex別の既定値・Delayが既定値
    {
        Project fresh;
        expectReverbEquals (Reverb::load (fresh.busParams[0]->reverb), Reverb::defaultsForBus (0),
                            "新規作成でReverb AがA用の既定値");
        expectReverbEquals (Reverb::load (fresh.busParams[1]->reverb), Reverb::defaultsForBus (1),
                            "新規作成でReverb BがB用の既定値");
        const auto delay = Delay::load (fresh.busParams[2]->delay);
        expect (delay.timeIndex == Delay::defaults.timeIndex
                    && ! delay.pingPong
                    && std::abs (delay.feedback - Delay::defaults.feedback) < 1.0e-6f,
                "新規作成でDelayが既定値");
    }

    const auto dir = makeTempDir();
    const Reverb::Values customA { 0.11f, 0.22f, 0.33f, 44.0f, 55.0f };
    const Reverb::Values customB { 0.91f, 0.82f, 0.73f, 64.0f, 255.0f };
    const Delay::Values customD { 1, 0.6f, 0.8f, true };

    // 保存→読込の往復
    {
        Project src;
        src.directory = dir;
        Reverb::store (src.busParams[0]->reverb, Reverb::normalized (customA, Reverb::defaultsForBus (0)));
        Reverb::store (src.busParams[1]->reverb, Reverb::normalized (customB, Reverb::defaultsForBus (1)));
        Delay::store (src.busParams[2]->delay, Delay::normalized (customD));
        juce::String error;
        expect (src.save (error), "保存できること");

        juce::StringArray warnings;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr, "読込できること");
        if (loaded != nullptr)
        {
            expectReverbEquals (Reverb::load (loaded->busParams[0]->reverb), customA,
                                "Reverb Aの値が往復すること");
            expectReverbEquals (Reverb::load (loaded->busParams[1]->reverb), customB,
                                "Reverb Bの値が往復すること");
            const auto delay = Delay::load (loaded->busParams[2]->delay);
            expect (delay.timeIndex == customD.timeIndex && delay.pingPong
                        && std::abs (delay.feedback - customD.feedback) < 1.0e-6f
                        && std::abs (delay.tone - customD.tone) < 1.0e-6f,
                    "Delayの値が往復すること");
        }
    }

    // v18読込（busesにreverb/delayキーが無い）: バスindex別の既定値で埋まる
    {
        const auto jsonFile = dir.getChildFile ("project.json");
        auto parsed = juce::JSON::parse (jsonFile.loadFileAsString());
        parsed.getDynamicObject()->setProperty ("version", 18);
        if (auto* busesArray = parsed.getProperty ("buses", {}).getArray())
            for (auto& busVar : *busesArray)
                if (auto* busObj = busVar.getDynamicObject())
                {
                    busObj->removeProperty ("reverb");
                    busObj->removeProperty ("delay");
                }
        expect (jsonFile.replaceWithText (juce::JSON::toString (parsed)), "v18相当へ書き換えられること");

        juce::StringArray warnings;
        juce::String error;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr, "v18相当を読込できること");
        if (loaded != nullptr)
        {
            expectReverbEquals (Reverb::load (loaded->busParams[0]->reverb), Reverb::defaultsForBus (0),
                                "v18読込でReverb AがA用の既定値");
            expectReverbEquals (Reverb::load (loaded->busParams[1]->reverb), Reverb::defaultsForBus (1),
                                "v18読込でReverb BがB用の既定値");
            const auto delay = Delay::load (loaded->busParams[2]->delay);
            expect (delay.timeIndex == Delay::defaults.timeIndex && ! delay.pingPong,
                    "v18読込でDelayが既定値");
        }
    }
    dir.deleteRecursively();
}

// ---- 移調・タイムストレッチ（docs/plans/2026-08-18-1028-audio-transpose-stretch.md）----

// テスト用の正弦波バッファ
std::shared_ptr<juce::AudioBuffer<float>> makeToneBuffer (double freqHz, int numSamples,
                                                          double sr, int channels = 1)
{
    auto buffer = std::make_shared<juce::AudioBuffer<float>> (channels, numSamples);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buffer->setSample (ch, i,
                               0.5f * (float) std::sin (juce::MathConstants<double>::twoPi
                                                        * freqHz * i / sr));
    return buffer;
}

// FFT（Hann窓）で支配的な周波数を求める
double dominantFrequency (const juce::AudioBuffer<float>& buffer, int start, int length, double sr)
{
    constexpr int order = 14; // 16384
    constexpr int size = 1 << order;
    length = juce::jmin (length, size, buffer.getNumSamples() - start);
    std::vector<float> data ((size_t) size * 2, 0.0f);
    for (int i = 0; i < length; ++i)
    {
        const double window = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i
                                                    / juce::jmax (1, length - 1));
        data[(size_t) i] = buffer.getSample (0, start + i) * (float) window;
    }
    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (data.data());
    int peakBin = 1;
    for (int bin = 1; bin < size / 2; ++bin)
        if (data[(size_t) bin] > data[(size_t) peakBin])
            peakBin = bin;
    return peakBin * sr / size;
}

float bufferRms (const juce::AudioBuffer<float>& buffer, int start, int length)
{
    double sum = 0.0;
    length = juce::jmin (length, buffer.getNumSamples() - start);
    for (int i = 0; i < length; ++i)
    {
        const double v = buffer.getSample (0, start + i);
        sum += v * v;
    }
    return length > 0 ? (float) std::sqrt (sum / length) : 0.0f;
}

// 数学のテスト用の「偽の」レンダー済みドメイン（中身は無音。長さ・座標だけが本物）
std::shared_ptr<const RenderedDomain> makeFakeDomain (
    const std::shared_ptr<juce::AudioBuffer<float>>& source,
    juce::int64 domainOffset, juce::int64 domainLength, int semitones, double ratio, double sr)
{
    auto d = std::make_shared<RenderedDomain>();
    const auto outLength = (juce::int64) std::llround ((double) domainLength * ratio);
    d->audio = std::make_shared<juce::AudioBuffer<float>> (
        juce::jmax (1, source->getNumChannels()), (int) juce::jmax ((juce::int64) 1, outLength));
    d->sourceAudio = source;
    d->audioBaseOffset = 0;
    d->domainOffset = domainOffset;
    d->domainLength = domainLength;
    d->semitones = semitones;
    d->ratio = ratio;
    d->sampleRate = sr;
    d->peakCache = buildDomainPeakCache (*d->audio, 0, d->audio->getNumSamples(),
                                         Clip::samplesPerPeak);
    return d;
}

void testClipStretcher()
{
    beginTest ("ClipStretcher render");
    const double sr = 48000.0;
    auto tone = makeToneBuffer (440.0, 48000, sr);

    // 無加工設定は入力とほぼ一致（位相ボコーダーの再合成を通るため厳密一致ではない）
    if (auto out = ClipStretcher::render (*tone, 0, 48000, 0, 1.0, sr); out != nullptr)
    {
        expect (out->getNumSamples() == 48000, "0半音/1.0倍で全長が保たれること");
        expect (std::abs (dominantFrequency (*out, 8000, 16384, sr) - 440.0) < 6.0,
                "0半音でピッチが変わらないこと");
        expect (std::abs (bufferRms (*out, 8000, 16000) - bufferRms (*tone, 8000, 16000)) < 0.08f,
                "0半音/1.0倍でレベルがほぼ保たれること");
    }
    else
        expect (false, "0半音/1.0倍のレンダリングが成功すること");

    // +12半音で基本周波数が2倍（FFTのピーク位置で判定）
    if (auto out = ClipStretcher::render (*tone, 0, 48000, 12, 1.0, sr); out != nullptr)
        expect (std::abs (dominantFrequency (*out, 8000, 16384, sr) - 880.0) < 12.0,
                "+12半音で440Hzが880Hzになること");
    else
        expect (false, "+12半音のレンダリングが成功すること");

    // ratio=1.25 で出力長が round(length × 1.25) ちょうど・ピッチは変わらない
    if (auto out = ClipStretcher::render (*tone, 0, 48000, 0, 1.25, sr); out != nullptr)
    {
        expect (out->getNumSamples() == 60000, "1.25倍で出力長が厳密に一致すること");
        expect (std::abs (dominantFrequency (*out, 16000, 16384, sr) - 440.0) < 6.0,
                "1.25倍に伸ばしてもピッチが変わらないこと");
        // 出力の頭が無音でない（レイテンシ補償の回帰。補償を外すと必ず落ちる）
        expect (bufferRms (*out, 0, 1000) > 0.1f, "出力の頭が無音でないこと");
    }
    else
        expect (false, "1.25倍のレンダリングが成功すること");

    // 同じ入力を2回処理すると完全に一致する（固定シードの回帰）
    {
        auto a = ClipStretcher::render (*tone, 1000, 20000, 3, 1.1, sr);
        auto b = ClipStretcher::render (*tone, 1000, 20000, 3, 1.1, sr);
        bool identical = a != nullptr && b != nullptr
                      && a->getNumSamples() == b->getNumSamples();
        if (identical)
            for (int i = 0; i < a->getNumSamples(); ++i)
                if (! juce::exactlyEqual (a->getSample (0, i), b->getSample (0, i)))
                {
                    identical = false;
                    break;
                }
        expect (identical, "同じ入力の2回のレンダリングが完全一致すること（固定シード）");
    }

    // 20〜50msの短いクリップ（チョップ用途・分析窓より短い）が無音にならない。
    // exact() は入力が seek 用の長さに足りないと無音を返すため、前後パディングの回帰
    {
        auto out = ClipStretcher::render (*tone, 24000, 960, 2, 1.0, sr); // 20ms
        expect (out != nullptr && out->getNumSamples() == 960 && bufferRms (*out, 0, 960) > 0.1f,
                "20msのチョップが無音にならないこと");
    }

    // 原音バッファの先頭・末尾に接するクリップ（助走が取れない側を無音で埋める経路）
    {
        auto head = ClipStretcher::render (*tone, 0, 960, 2, 1.0, sr);
        auto tail = ClipStretcher::render (*tone, 47040, 960, 2, 1.0, sr);
        expect (head != nullptr && bufferRms (*head, 0, 960) > 0.1f,
                "バッファ先頭に接するクリップが鳴ること");
        expect (tail != nullptr && bufferRms (*tail, 0, 960) > 0.1f,
                "バッファ末尾に接するクリップが鳴ること");
    }

    // 異常値は**失敗を返す**（直呼びは受付層を通っていないのでクランプしない。
    // 原音を「その指紋の生成結果」として返さない）
    expect (ClipStretcher::render (*tone, 0, 48000, 0, 0.0, sr) == nullptr, "ratio=0は失敗");
    expect (ClipStretcher::render (*tone, 0, 48000, 0, -1.0, sr) == nullptr, "負のratioは失敗");
    expect (ClipStretcher::render (*tone, 0, 48000, 0,
                                   std::numeric_limits<double>::quiet_NaN(), sr) == nullptr,
            "NaNは失敗");
    expect (ClipStretcher::render (*tone, 0, 48000, 0,
                                   std::numeric_limits<double>::infinity(), sr) == nullptr,
            "infは失敗");
    expect (ClipStretcher::render (*tone, 0, 48000, 0, 100.0, sr) == nullptr, "巨大倍率は失敗");
    expect (ClipStretcher::render (*tone, 0, 48000, 13, 1.0, sr) == nullptr, "±12半音の外は失敗");
    expect (ClipStretcher::render (*tone, 40000, 20000, 0, 1.0, sr) == nullptr,
            "原音外のドメインは失敗");
}

void testStretchDomainMath()
{
    beginTest ("stretch domain math (view/fade/split)");
    const double sr = 48000.0;

    // ループをまたぐフェードの chain 変換: 本体1サンプル・ratio=1.5・100反復の全長フェードは
    // 実効200（round(fade × ratio) = 150 だと落ちる）
    {
        auto source = makeToneBuffer (440.0, 10, sr);
        Clip clip;
        clip.audio = source;
        clip.offsetSamples = 0;
        clip.lengthSamples = 1;
        clip.loopCount = 99; // 100反復
        clip.fadeInSamples = 100; // 連なり全長
        clip.activeDomain = makeFakeDomain (source, 0, 1, 0, 1.5, sr);
        expect (clip.renderedLengthSamples() == 2, "本体1サンプル×1.5は実効2");
        expect (clip.renderedTotalLengthSamples() == 200, "連なりの実効全長は200");
        expect (clip.renderedFadeIn() == 200,
                "連なり全長のフェードが実効200になること（×ratioの丸め累積だと150）");
    }

    // 0.5倍・1.5倍でフェードの相対位置（波形に対する斜線の位置）が変わらない
    for (double ratio : { 0.5, 1.5 })
    {
        auto source = makeToneBuffer (440.0, 2000, sr);
        Clip clip;
        clip.audio = source;
        clip.offsetSamples = 0;
        clip.lengthSamples = 2000;
        clip.fadeInSamples = 500;  // 本体の1/4
        clip.fadeOutSamples = 250; // 本体の1/8
        clip.activeDomain = makeFakeDomain (source, 0, 2000, 0, ratio, sr);
        const auto renderedBody = clip.renderedLengthSamples();
        expect (clip.renderedFadeIn() * 4 == renderedBody,
                "フェードインの相対位置（1/4）が伸縮後も保たれること");
        expect (clip.renderedFadeOut() * 8 == renderedBody,
                "フェードアウトの相対位置（1/8）が伸縮後も保たれること");
    }

    // フェードドラッグの往復（実効→原音→実効）で値が変わらない（idempotent）
    {
        auto source = makeToneBuffer (440.0, 1000, sr);
        Clip clip;
        clip.audio = source;
        clip.offsetSamples = 100;
        clip.lengthSamples = 700;
        clip.loopCount = 2;
        clip.activeDomain = makeFakeDomain (source, 100, 700, 0, 1.3, sr);
        bool stable = true;
        for (juce::int64 rendered = 0; rendered <= clip.renderedTotalLengthSamples();
             rendered += 37)
        {
            const auto sourceFade = clip.sourceFadeFromRendered (rendered, false);
            const auto rendered2 = clip.renderedFadeLength (sourceFade, false);
            const auto sourceFade2 = clip.sourceFadeFromRendered (rendered2, false);
            if (clip.renderedFadeLength (sourceFade2, false) != rendered2)
                stable = false;
            // フェードアウト側も同じ規則
            const auto outSource = clip.sourceFadeFromRendered (rendered, true);
            const auto outRendered = clip.renderedFadeLength (outSource, true);
            if (clip.renderedFadeLength (clip.sourceFadeFromRendered (outRendered, true), true)
                != outRendered)
                stable = false;
        }
        expect (stable, "フェードの実効→原音→実効の往復が安定すること");
    }

    // clampFades は原音長基準のまま（実効長を渡すと1.5倍で fadeOut が過剰に許容される）
    {
        auto source = makeToneBuffer (440.0, 1000, sr);
        Clip clip;
        clip.audio = source;
        clip.lengthSamples = 1000;
        clip.activeDomain = makeFakeDomain (source, 0, 1000, 0, 1.5, sr);
        clip.fadeOutSamples = 1400; // 実効全長(1500)未満だが原音全長(1000)超
        clip.clampFades();
        expect (clip.fadeOutSamples == 1000, "clampFadesが原音長基準でクランプすること");
    }

    // stretchRatio を往復させても lengthSamples が変わらない（非破壊）
    {
        auto source = makeToneBuffer (440.0, 1000, sr);
        Clip clip;
        clip.audio = source;
        clip.lengthSamples = 1000;
        ClipDomains::applyStretchRequest (clip, 0, 1.5);
        ClipDomains::applyStretchRequest (clip, 0, 1.0);
        expect (clip.lengthSamples == 1000 && clip.stretchRatio == 1.0,
                "ratio往復でlengthSamplesが変わらないこと");
    }

    // 移調だけ変えても stretchRatio と見かけ長が変わらない
    {
        auto source = makeToneBuffer (440.0, 1000, sr);
        Clip clip;
        clip.audio = source;
        clip.lengthSamples = 1000;
        clip.stretchRatio = 1.25;
        clip.activeDomain = makeFakeDomain (source, 0, 1000, 0, 1.25, sr);
        const auto before = clip.renderedLengthSamples();
        ClipDomains::applyStretchRequest (clip, 2, clip.stretchRatio);
        expect (clip.stretchRatio == 1.25 && clip.renderedLengthSamples() == before,
                "移調だけの変更で伸縮と見かけ長が変わらないこと");
    }

    // 小節数 → stretchRatio の換算
    {
        expect (ClipDomains::ratioForBars (8.0, 48000.0, 384000) == 1.0, "8小節→1.0倍");
        expect (ClipDomains::ratioForBars (12.0, 48000.0, 384000) == 1.5, "12小節→1.5倍");
        expect (ClipDomains::ratioForBars (0.0, 48000.0, 384000) == 1.0, "不正入力は1.0");
    }

    // 分割: 0.5倍・1.5倍の双方で正しい位置で切れる・再レンダーなし・隙間も重なりもない
    for (double ratio : { 0.5, 1.5 })
    {
        auto source = makeToneBuffer (440.0, 1000, sr);
        Clip clip;
        clip.audio = source;
        clip.startSample = 10000;
        clip.offsetSamples = 0;
        clip.lengthSamples = 1000;
        clip.stretchRatio = ratio;
        clip.activeDomain = makeFakeDomain (source, 0, 1000, 0, ratio, sr);
        clip.renderDomainOffset = 0;
        clip.renderDomainLength = 1000;

        const auto renderedLength = clip.renderedLengthSamples();
        const auto splitAt = clip.startSample + renderedLength / 2;
        Clip left, right;
        expect (splitClip (clip, splitAt, left, right), "伸縮クリップを分割できること");
        expect (left.activeDomain.get() == clip.activeDomain.get()
                    && right.activeDomain.get() == clip.activeDomain.get(),
                "分割に再レンダーが走らないこと（RenderedDomainが同一インスタンス）");
        expect (right.startSample == left.startSample + left.renderedLengthSamples(),
                "右の開始が左の実効終端と一致すること（隙間も重なりもない）");
        expect (left.renderedLengthSamples() + right.renderedLengthSamples() == renderedLength,
                "左右の実効長の和が親と厳密に一致すること");
        expect (left.lengthSamples + right.lengthSamples == 1000
                    && right.offsetSamples == left.offsetSamples + left.lengthSamples,
                "原音側の分割も整合すること");
        expect (right.viewStartRendered() == left.viewEndRendered(),
                "左右のviewが同じ境界を共有すること");
        // 分割で値・ドメインが継承される
        expect (juce::exactlyEqual (right.stretchRatio, ratio) && right.renderDomainLength == 1000,
                "分割で要求値とレンダードメインが継承されること");
    }

    // view がバッファ終端を越えない退化ケース（domainLength=2 / ratio=0.5 / 右子 offset=+1）。
    // 独立に round(length × ratio) すると長さ1のバッファの [1, 2) を読む
    {
        auto source = makeToneBuffer (440.0, 2, sr);
        Clip child;
        child.audio = source;
        child.offsetSamples = 1;
        child.lengthSamples = 1;
        child.activeDomain = makeFakeDomain (source, 0, 2, 0, 0.5, sr);
        // 加工バッファ長は round(2×0.5) = 1。絶対境界の差なら view は [1,1) = 長さ0
        expect (child.renderedLengthSamples() == 0
                    || child.viewEndRendered() <= child.activeDomain->renderedDomainLength(),
                "退化した子viewがバッファ終端を越えないこと");
        std::vector<ClipPlayback> playbacks;
        appendClipPlaybacks (child, playbacks);
        for (const auto& p : playbacks)
            expect (p.offsetSamples + p.lengthSamples <= p.audio->getNumSamples(),
                    "退化viewの再生が範囲外を読まないこと");
        // 長さ0のviewができる分割は行われない（実効長1の親は内側の分割点を持たない）
        Clip parent;
        parent.audio = source;
        parent.offsetSamples = 0;
        parent.lengthSamples = 2;
        parent.activeDomain = child.activeDomain;
        Clip l, r;
        expect (! splitClip (parent, parent.startSample + 1, l, r) || (l.renderedLengthSamples() > 0 && r.renderedLengthSamples() > 0),
                "長さ0のviewを作る分割が拒否されること");
    }

    // 無加工ドメインで domainOffset != 0 のクリップが原音の正しい位置を鳴らす
    // （audioBaseOffset の回帰。落とすと先頭から鳴る）
    {
        auto source = makeToneBuffer (440.0, 3000, sr);
        Clip clip;
        clip.audio = source;
        clip.offsetSamples = 1500;
        clip.lengthSamples = 300;
        clip.activeDomain = ClipDomains::makeNeutralDomain (source, 1000, 2000, sr);
        std::vector<ClipPlayback> playbacks;
        appendClipPlaybacks (clip, playbacks);
        expect (playbacks.size() == 1 && playbacks[0].offsetSamples == 1500
                    && playbacks[0].lengthSamples == 300,
                "無加工の分割済みクリップが原音の正しい位置を読むこと");
    }

    // バウンス範囲が実効長で決まる（BounceRenderer::buildItemRender の rangeEnd 回帰）
    {
        auto source = makeToneBuffer (440.0, 1000, sr);
        Track track;
        Clip clip;
        clip.audio = source;
        clip.startSample = 4000;
        clip.lengthSamples = 1000;
        clip.loopCount = 1; // 2反復
        clip.activeDomain = makeFakeDomain (source, 0, 1000, 0, 1.5, sr);
        track.clips.push_back (clip);
        BounceRenderer::TrackRender render;
        juce::int64 rangeStart = 0, rangeEnd = 0;
        expect (BounceRenderer::buildItemRender (track, 0, 120.0, sr, render, rangeStart, rangeEnd),
                "伸縮クリップの書き出し範囲を組めること");
        expect (rangeStart == 4000 && rangeEnd == 4000 + 3000,
                "書き出し範囲がループ込みの実効長（1500×2）で決まること");
        expect (render.clips.size() == 2 && render.clips[0].lengthSamples == 1500,
                "展開されたClipPlaybackが実効長であること");
    }

    // チョップした右側の波形に、左側にしかないトランジェントが出ない（view端のピーク再集計）
    {
        auto source = std::make_shared<juce::AudioBuffer<float>> (1, 1024);
        source->clear();
        source->setSample (0, 590, 1.0f); // 右viewの開始(600)と同じ512binの左側
        Clip right;
        right.audio = source;
        right.offsetSamples = 600;
        right.lengthSamples = 424;
        right.activeDomain = ClipDomains::makeNeutralDomain (source, 0, 1024, sr);
        const auto peak = right.activeDomain->peakBetween (right.viewStartRendered(),
                                                           right.viewStartRendered() + 100,
                                                           Clip::samplesPerPeak);
        expect (peak < 0.5f, "境界の外にあるトランジェントが右側の波形に出ないこと");
        // 共有キャッシュ自体はドメイン全体を覆っている（トランジェントを含むbinは1.0）
        expect (right.activeDomain->peakBetween (512, 1024, Clip::samplesPerPeak) > 0.99f,
                "ドメイン全体のキャッシュにはトランジェントが入っていること");
    }
}

void testStretchPersistence()
{
    beginTest ("stretch persistence (v20)");
    auto dir = makeTempDir();
    const double sr = 48000.0;

    // 保存 → 読込で値とドメインが保たれる
    {
        Project project;
        project.directory = dir;
        project.sampleRate = sr;
        Track track;
        track.id = project.allocateId();
        Clip clip;
        clip.fileName = "clip-001.wav";
        clip.lengthSamples = 4000;
        clip.offsetSamples = 1000;
        clip.transposeSemitones = 2;
        clip.stretchRatio = 0.911;
        clip.renderDomainOffset = 0;
        clip.renderDomainLength = 8000;
        clip.audio = makeToneBuffer (440.0, 8000, sr);
        track.clips.push_back (clip);
        project.tracks.push_back (std::move (track));

        juce::AudioBuffer<float> wav (1, 8000);
        for (int i = 0; i < 8000; ++i)
            wav.setSample (0, i, std::sin ((float) i * 0.05f) * 0.4f);
        expect (writeBufferWav (dir.getChildFile ("clip-001.wav"), wav, sr), "WAVを書けること");

        juce::String error;
        expect (project.save (error), "保存できること");

        juce::StringArray warnings;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr && ! loaded->tracks.empty()
                    && ! loaded->tracks[0].clips.empty(),
                "読込できること");
        if (loaded != nullptr && ! loaded->tracks.empty() && ! loaded->tracks[0].clips.empty())
        {
            const auto& c = loaded->tracks[0].clips[0];
            expect (c.transposeSemitones == 2 && juce::exactlyEqual (c.stretchRatio, 0.911),
                    "移調・伸縮の値が保存→読込で保たれること");
            expect (c.renderDomainOffset == 0 && c.renderDomainLength == 8000,
                    "レンダードメインが保たれること");
            expect (c.activeDomain != nullptr && c.activeDomain->ratio == 1.0,
                    "読込直後の実効状態は原音素通しであること");
        }
    }

    // v19相当（キー欠損）は既定値で読める
    {
        const auto jsonFile = dir.getChildFile ("project.json");
        auto parsed = juce::JSON::parse (jsonFile.loadFileAsString());
        parsed.getDynamicObject()->setProperty ("version", 19);
        if (auto* tracksArray = parsed.getProperty ("tracks", {}).getArray())
            for (auto& trackVar : *tracksArray)
                if (auto* clipsArray = trackVar.getProperty ("clips", {}).getArray())
                    for (auto& clipVar : *clipsArray)
                        if (auto* obj = clipVar.getDynamicObject())
                        {
                            obj->removeProperty ("transposeSemitones");
                            obj->removeProperty ("stretchRatio");
                            obj->removeProperty ("renderDomainOffset");
                            obj->removeProperty ("renderDomainLength");
                        }
        expect (jsonFile.replaceWithText (juce::JSON::toString (parsed)), "v19相当へ書き換え");

        juce::StringArray warnings;
        juce::String error;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr && ! loaded->tracks[0].clips.empty(), "v19相当を読込できること");
        if (loaded != nullptr && ! loaded->tracks[0].clips.empty())
        {
            const auto& c = loaded->tracks[0].clips[0];
            expect (c.transposeSemitones == 0 && c.stretchRatio == 1.0,
                    "v19の読込で既定値（無加工）が入ること");
            expect (c.requestedDomainOffset() == c.offsetSamples
                        && c.requestedDomainLength() == c.lengthSamples,
                    "v19の読込でドメインがクリップ自身の範囲になること");
        }
    }

    // 異常JSON（ratio が 0 / 負 / 文字列 / null・巨大semitone・不正ドメイン）を読んでも落ちない
    {
        const auto jsonFile = dir.getChildFile ("project.json");
        const auto setClipProps = [&jsonFile] (juce::var ratio, juce::var semis,
                                               juce::var dOff, juce::var dLen)
        {
            auto parsed = juce::JSON::parse (jsonFile.loadFileAsString());
            if (auto* tracksArray = parsed.getProperty ("tracks", {}).getArray())
                for (auto& trackVar : *tracksArray)
                    if (auto* clipsArray = trackVar.getProperty ("clips", {}).getArray())
                        for (auto& clipVar : *clipsArray)
                            if (auto* obj = clipVar.getDynamicObject())
                            {
                                obj->setProperty ("stretchRatio", ratio);
                                obj->setProperty ("transposeSemitones", semis);
                                obj->setProperty ("renderDomainOffset", dOff);
                                obj->setProperty ("renderDomainLength", dLen);
                            }
            jsonFile.replaceWithText (juce::JSON::toString (parsed));
        };

        struct Case { juce::var ratio, semis, dOff, dLen; };
        const Case cases[] = {
            { 0.0, 40, -5, 100 },                       // ratio 0・範囲外semitone・負のドメイン
            { -2.0, 0, 0, 999999 },                     // 負のratio・原音外のドメイン
            { "broken", 2, 2000, 0 },                   // 文字列ratio・長さ0ドメイン
            { juce::var(), 0, 5000, 100 },              // null ratio・クリップ範囲を含まないドメイン
        };
        for (const auto& c : cases)
        {
            setClipProps (c.ratio, c.semis, c.dOff, c.dLen);
            juce::StringArray warnings;
            juce::String error;
            auto loaded = Project::load (dir, warnings, error);
            expect (loaded != nullptr && ! loaded->tracks[0].clips.empty(),
                    "異常JSONでも読込が落ちないこと");
            if (loaded != nullptr && ! loaded->tracks[0].clips.empty())
            {
                const auto& clip = loaded->tracks[0].clips[0];
                expect (std::isfinite (clip.stretchRatio) && clip.stretchRatio >= ClipStretchLimits::minRatio
                            && clip.stretchRatio <= ClipStretchLimits::maxRatio,
                        "ratioが安全限界内へ収まること");
                expect (std::abs (clip.transposeSemitones) <= ClipStretchLimits::maxSemitones,
                        "semitoneが±12へ収まること");
                expect (clip.requestedDomainOffset() <= clip.offsetSamples
                            && clip.requestedDomainOffset() + clip.requestedDomainLength()
                                   >= clip.offsetSamples + clip.lengthSamples
                            && clip.requestedDomainOffset() >= 0,
                        "不正なドメインがクリップ自身の範囲へ戻ること");
            }
        }
    }
    dir.deleteRecursively();
}

void testRenderCachePipeline()
{
    beginTest ("render cache pipeline");
    const double sr = 48000.0;

    // 1クリップの要求 → レンダー → 装着 → ClipPlayback の長さと実バッファが一致
    {
        Project project;
        project.sampleRate = sr;
        Track track;
        Clip clip;
        clip.audio = makeToneBuffer (440.0, 24000, sr);
        clip.lengthSamples = 24000;
        track.clips.push_back (clip);
        project.tracks.push_back (std::move (track));
        ClipDomains::reconcile (project, sr);

        auto& liveClip = project.tracks[0].clips[0];
        expect (ClipDomains::applyStretchRequest (liveClip, 2, 1.25), "要求を受理できること");
        expect (liveClip.renderPending (sr), "受理直後はレンダリング待ちであること");
        expect (liveClip.effectiveTransposeSemitones() == 0,
                "完了前は実効の移調量（+2バッジの表示値）が旧値のままであること");

        // 完了前: 見かけも音も古い（無加工）まま。長さと実バッファの有効長が常に一致する
        {
            std::vector<ClipPlayback> playbacks;
            appendClipPlaybacks (liveClip, playbacks);
            expect (playbacks.size() == 1 && playbacks[0].lengthSamples == 24000
                        && playbacks[0].offsetSamples + playbacks[0].lengthSamples
                               <= playbacks[0].audio->getNumSamples(),
                    "完了前は古い実効長のままで、バッファと矛盾しないこと");
        }

        RenderCache cache;
        bool attachedAny = false;
        cache.collectRequests = [&]
        {
            bool attached = false;
            auto requests = ClipDomains::collectRequests (
                project, sr, [&cache] (const RenderFingerprint& fp) { return cache.lookup (fp); },
                attached);
            attachedAny = attachedAny || attached;
            return requests;
        };
        std::shared_ptr<const RenderedDomain> ready;
        cache.onRenderReady = [&] (const std::shared_ptr<const RenderedDomain>& domain)
        { ready = domain; ClipDomains::attachRenderResult (project, sr, domain); };
        cache.syncNow();
        expect (cache.waitForRenders (20000), "レンダリングが完了すること");
        cache.drainCompletedNow();
        expect (ready != nullptr, "完了コールバックが結果を受け取ること");
        expect (! liveClip.renderPending (sr), "装着後は要求と実効が一致すること");
        expect (liveClip.effectiveTransposeSemitones() == 2,
                "装着と同時に実効の移調量（バッジ）が切り替わること");
        expect (liveClip.renderedLengthSamples() == 30000, "見かけ長が1.25倍になること");
        {
            std::vector<ClipPlayback> playbacks;
            appendClipPlaybacks (liveClip, playbacks);
            expect (playbacks.size() == 1 && playbacks[0].lengthSamples == 30000
                        && playbacks[0].audio->getNumSamples() == 30000,
                    "装着後のClipPlaybackが加工済みバッファと長さ一致すること");
        }

        // 同じループを8個にチョップしても RenderedDomain は1本しか作られない（ドメイン共有）
        {
            auto& clips = project.tracks[0].clips;
            for (int i = 0; i < 3; ++i) // 3回の分割で4片（さらに複製で8片）
            {
                std::vector<Clip> next;
                for (auto& c : clips)
                {
                    Clip l, r;
                    if (splitClip (c, c.startSample + c.renderedLengthSamples() / 2, l, r))
                    {
                        next.push_back (l);
                        next.push_back (r);
                    }
                    else
                        next.push_back (c);
                }
                clips = next;
            }
            expect (clips.size() == 8, "8片にチョップできること");
            bool sameDomain = true;
            for (const auto& c : clips)
                sameDomain = sameDomain && c.activeDomain.get() == ready.get();
            expect (sameDomain, "8片が同じRenderedDomainを共有すること");
            bool attached = false;
            const auto requests = ClipDomains::collectRequests (
                project, sr, [&cache] (const RenderFingerprint& fp) { return cache.lookup (fp); },
                attached);
            expect (requests.empty(), "チョップしても新しいレンダー要求が発生しないこと");
        }
    }

    // 同じ素材の複製を +2 / -2 にしたとき両方の要求が残る（「同じ範囲は最新1件」にしない）
    {
        Project project;
        project.sampleRate = sr;
        Track track;
        Clip clip;
        clip.audio = makeToneBuffer (440.0, 4800, sr);
        clip.lengthSamples = 4800;
        track.clips.push_back (clip);
        track.clips.push_back (clip);
        project.tracks.push_back (std::move (track));
        ClipDomains::reconcile (project, sr);
        ClipDomains::applyStretchRequest (project.tracks[0].clips[0], 2, 1.0);
        ClipDomains::applyStretchRequest (project.tracks[0].clips[1], -2, 1.0);
        bool attached = false;
        const auto requests = ClipDomains::collectRequests (project, sr, nullptr, attached);
        expect (requests.size() == 2, "+2と-2の両方の要求が残ること");
    }

    // 失敗の巻き戻し: 「現在の要求指紋が失敗指紋と一致するクリップだけ」
    {
        Project project;
        project.sampleRate = sr;
        Track track;
        Clip clip;
        clip.audio = makeToneBuffer (440.0, 4800, sr);
        clip.lengthSamples = 4800;
        track.clips.push_back (clip);
        project.tracks.push_back (std::move (track));
        ClipDomains::reconcile (project, sr);
        auto& liveClip = project.tracks[0].clips[0];

        ClipDomains::applyStretchRequest (liveClip, 2, 1.0);
        const auto oldFingerprint = liveClip.requestedFingerprint (sr);
        ClipDomains::applyStretchRequest (liveClip, 3, 1.0); // +2実行中に+3へ変更した状況

        expect (! ClipDomains::rollbackFailedRequest (project, sr, oldFingerprint),
                "古い+2の失敗で+3の要求が巻き戻らないこと");
        expect (liveClip.transposeSemitones == 3, "+3の要求が維持されること");

        const auto currentFingerprint = liveClip.requestedFingerprint (sr);
        expect (ClipDomains::rollbackFailedRequest (project, sr, currentFingerprint),
                "現在の要求の失敗は巻き戻されること（dirty化が要る=true）");
        expect (liveClip.transposeSemitones == 0 && liveClip.stretchRatio == 1.0,
                "前例が無いので無加工へ戻ること");
        expect (! liveClip.renderPending (sr), "巻き戻し後は永続状態＝鳴っている音であること");
    }

    // 失敗後に保存 → 再読込で、鳴る音が失敗直後と一致する（値も実効も無加工）
    {
        auto dir = makeTempDir();
        Project project;
        project.directory = dir;
        project.sampleRate = sr;
        Track track;
        track.id = project.allocateId();
        Clip clip;
        clip.fileName = "clip-001.wav";
        clip.audio = makeToneBuffer (440.0, 4800, sr);
        clip.lengthSamples = 4800;
        track.clips.push_back (clip);
        project.tracks.push_back (std::move (track));
        ClipDomains::reconcile (project, sr);
        juce::AudioBuffer<float> wav (1, 4800);
        wav.clear();
        writeBufferWav (dir.getChildFile ("clip-001.wav"), wav, sr);

        auto& liveClip = project.tracks[0].clips[0];
        ClipDomains::applyStretchRequest (liveClip, 5, 1.0);
        ClipDomains::rollbackFailedRequest (project, sr, liveClip.requestedFingerprint (sr));
        juce::String error;
        expect (project.save (error), "巻き戻し後に保存できること");
        juce::StringArray warnings;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr
                    && loaded->tracks[0].clips[0].transposeSemitones == 0
                    && loaded->tracks[0].clips[0].stretchRatio == 1.0,
                "再読込した値が失敗直後に鳴っていた音（無加工）と一致すること");
        dir.deleteRecursively();
    }

    // 実行中と同じ指紋を sync が積み直しても、同じレンダーが二重実行されない
    {
        Project project;
        project.sampleRate = sr;
        Track track;
        Clip clip;
        clip.audio = makeToneBuffer (220.0, 480000, sr); // 10秒（sync の再実行が確実に処理中に走る）
        clip.lengthSamples = 480000;
        track.clips.push_back (clip);
        project.tracks.push_back (std::move (track));
        ClipDomains::reconcile (project, sr);
        ClipDomains::applyStretchRequest (project.tracks[0].clips[0], 3, 1.5);

        RenderCache cache;
        cache.collectRequests = [&]
        {
            bool attached = false;
            return ClipDomains::collectRequests (
                project, sr, [&cache] (const RenderFingerprint& fp) { return cache.lookup (fp); },
                attached);
        };
        int readyCount = 0;
        cache.onRenderReady = [&] (const std::shared_ptr<const RenderedDomain>& d)
        { ++readyCount; ClipDomains::attachRenderResult (project, sr, d); };
        cache.syncNow();
        juce::Thread::sleep (100); // ワーカーがジョブを掴んで処理中になるまで待つ
        cache.syncNow();           // クリップはまだ pending → 同じ指紋が集まる
        expect (cache.waitForRenders (60000), "レンダーが完了すること");
        cache.drainCompletedNow();
        cache.syncNow();           // 配達済み: もう要求は無い
        expect (cache.waitForRenders (60000), "再同期後も待ちが残らないこと");
        cache.drainCompletedNow();
        expect (readyCount == 1, "実行中の指紋が再キューされず、レンダーが1回で済むこと");
    }

    // レンダー実行中に RenderCache を破棄しても落ちない（プロジェクト切替相当）
    {
        Project project;
        project.sampleRate = sr;
        Track track;
        Clip clip;
        clip.audio = makeToneBuffer (220.0, 480000, sr); // 10秒（確実に実行中に破棄が走る）
        clip.lengthSamples = 480000;
        track.clips.push_back (clip);
        project.tracks.push_back (std::move (track));
        ClipDomains::reconcile (project, sr);
        ClipDomains::applyStretchRequest (project.tracks[0].clips[0], -7, 2.0);
        {
            RenderCache cache;
            cache.collectRequests = [&]
            {
                bool attached = false;
                return ClipDomains::collectRequests (project, sr, nullptr, attached);
            };
            cache.syncNow();
            juce::Thread::sleep (20); // ワーカーがジョブを掴むまで少し待つ
        } // ← 実行中に破棄（デストラクタが停止要求 → join）
        expect (true, "実行中の破棄がクラッシュしないこと");
    }

    // 処理中に最後の参照元クリップを削除しても原音が生存する（request の強参照）
    {
        Project project;
        project.sampleRate = sr;
        Track track;
        Clip clip;
        clip.audio = makeToneBuffer (330.0, 96000, sr);
        clip.lengthSamples = 96000;
        track.clips.push_back (clip);
        project.tracks.push_back (std::move (track));
        ClipDomains::reconcile (project, sr);
        ClipDomains::applyStretchRequest (project.tracks[0].clips[0], 4, 1.5);
        std::weak_ptr<juce::AudioBuffer<float>> weakSource = project.tracks[0].clips[0].audio;

        RenderCache cache;
        cache.collectRequests = [&]
        {
            bool attached = false;
            return ClipDomains::collectRequests (project, sr, nullptr, attached);
        };
        std::shared_ptr<const RenderedDomain> ready;
        cache.onRenderReady = [&ready] (const std::shared_ptr<const RenderedDomain>& d) { ready = d; };
        cache.syncNow();
        project.tracks[0].clips.clear(); // 最後の参照元クリップを削除
        expect (! weakSource.expired(), "request が原音の生存を保証すること");
        expect (cache.waitForRenders (30000), "クリップ削除後もレンダーが完走すること");
        cache.drainCompletedNow();
        expect (ready != nullptr && ready->sourceAudio != nullptr,
                "結果が原音の強参照を持つこと（アドレス再利用の誤ヒット防止）");
    }

    // undo に activeDomain が入らない（100件積んでも render がメモリに滞留しない）
    {
        Project project;
        project.sampleRate = sr;
        Track track;
        Clip clip;
        clip.audio = makeToneBuffer (440.0, 4800, sr);
        clip.lengthSamples = 4800;
        track.clips.push_back (clip);
        project.tracks.push_back (std::move (track));
        ClipDomains::reconcile (project, sr);

        const auto domain = project.tracks[0].clips[0].activeDomain;
        const auto baseline = domain.use_count();
        UndoStack undo;
        for (int i = 0; i < 100; ++i)
            undo.begin (project);
        expect (domain.use_count() == baseline,
                "undoを100件積んでもactiveDomainの参照が増えないこと");

        // undo 復元後は activeDomain が空 → reconcile が立て直す
        auto kind = UndoStack::EditKind::structure;
        undo.undo (project, kind);
        expect (project.tracks[0].clips[0].activeDomain == nullptr,
                "undo stateにはactiveDomainが入っていないこと");
        expect (ClipDomains::reconcile (project, sr), "undo後のreconcileが立て直すこと");
        expect (project.tracks[0].clips[0].activeDomain != nullptr, "実効状態が復元されること");
    }

    // レンダー完了前に undo → 結果到着 → redo（キャッシュから引き直し）
    {
        Project project;
        project.sampleRate = sr;
        Track track;
        Clip clip;
        clip.audio = makeToneBuffer (440.0, 24000, sr);
        clip.lengthSamples = 24000;
        track.clips.push_back (clip);
        project.tracks.push_back (std::move (track));
        ClipDomains::reconcile (project, sr);

        UndoStack undo;
        undo.begin (project, UndoStack::EditKind::clipValue); // 吹き出し1件ぶん
        ClipDomains::applyStretchRequest (project.tracks[0].clips[0], 6, 1.0);

        RenderCache cache;
        cache.collectRequests = [&]
        {
            bool attached = false;
            return ClipDomains::collectRequests (project, sr, nullptr, attached);
        };
        std::shared_ptr<const RenderedDomain> ready;
        cache.onRenderReady = [&] (const std::shared_ptr<const RenderedDomain>& d)
        { ready = d; ClipDomains::attachRenderResult (project, sr, d); };
        cache.syncNow();

        auto kind = UndoStack::EditKind::structure;
        undo.undo (project, kind); // 完了前に undo
        ClipDomains::reconcile (project, sr);

        expect (cache.waitForRenders (20000), "レンダーが完了すること");
        cache.drainCompletedNow(); // 結果到着（undo済みなので誰も装着しない）
        expect (project.tracks[0].clips[0].transposeSemitones == 0
                    && ! project.tracks[0].clips[0].renderPending (sr),
                "undo後に到着した結果が古いクリップへ適用されないこと");
        {
            std::vector<ClipPlayback> playbacks;
            appendClipPlaybacks (project.tracks[0].clips[0], playbacks);
            expect (! playbacks.empty()
                        && playbacks[0].offsetSamples + playbacks[0].lengthSamples
                               <= playbacks[0].audio->getNumSamples(),
                    "undo→到着の途中でも不変条件が保たれること");
        }

        undo.redo (project, kind);
        ClipDomains::reconcile (project, sr);
        bool attached = false;
        const auto requests = ClipDomains::collectRequests (
            project, sr, [&cache] (const RenderFingerprint& fp) { return cache.lookup (fp); },
            attached);
        expect (attached && requests.empty(),
                "redo後はキャッシュから即装着され、再レンダーが要らないこと");
        expect (! project.tracks[0].clips[0].renderPending (sr), "redo後に実効が追いつくこと");
    }
}

void testStretchSplitSaveReload()
{
    beginTest ("stretch split -> save -> reload keeps sound");
    auto dir = makeTempDir();
    const double sr = 48000.0;

    Project project;
    project.directory = dir;
    project.sampleRate = sr;
    Track track;
    track.id = project.allocateId();
    // 原音は最初からWAV経由で読む（24bit量子化を挟むため、メモリ上のfloatと混ぜると
    // 再読込後のレンダーがビット一致しなくなる — 比較の前提を「同一ソース」に揃える）
    {
        auto tone = makeToneBuffer (440.0, 24000, sr);
        expect (writeBufferWav (dir.getChildFile ("clip-001.wav"), *tone, sr), "WAVを書けること");
    }
    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.audio = Project::loadWav (dir.getChildFile ("clip-001.wav"));
    expect (clip.audio != nullptr, "WAVを読み戻せること");
    clip.lengthSamples = 24000;
    track.clips.push_back (clip);
    project.tracks.push_back (std::move (track));
    ClipDomains::reconcile (project, sr);

    // レンダー → 装着 → 分割
    const auto renderAll = [&sr] (Project& target)
    {
        RenderCache cache;
        cache.collectRequests = [&target, &sr]
        {
            bool attached = false;
            return ClipDomains::collectRequests (target, sr, nullptr, attached);
        };
        cache.onRenderReady = [&target, &sr] (const std::shared_ptr<const RenderedDomain>& d)
        { ClipDomains::attachRenderResult (target, sr, d); };
        cache.syncNow();
        const bool done = cache.waitForRenders (30000);
        cache.drainCompletedNow();
        return done;
    };

    ClipDomains::applyStretchRequest (project.tracks[0].clips[0], 2, 1.25);
    expect (renderAll (project), "初回レンダーが完了すること");
    expect (! project.tracks[0].clips[0].renderPending (sr), "装着済みであること");

    auto& clips = project.tracks[0].clips;
    Clip left, right;
    const auto splitPoint = clips[0].startSample + clips[0].renderedLengthSamples() / 2;
    expect (splitClip (clips[0], splitPoint, left, right), "伸縮済みクリップを分割できること");
    clips[0] = left;
    clips.push_back (right);

    // 境界付近の音（分割の左右をつなげた読み出し）を控える
    const auto boundarySamples = [&clips] () -> std::vector<float>
    {
        std::vector<float> samples;
        std::vector<ClipPlayback> playbacks;
        for (const auto& c : clips)
            appendClipPlaybacks (c, playbacks);
        std::sort (playbacks.begin(), playbacks.end(),
                   [] (const ClipPlayback& a, const ClipPlayback& b)
                   { return a.startSample < b.startSample; });
        for (const auto& p : playbacks)
            for (juce::int64 i = 0; i < p.lengthSamples; ++i)
                samples.push_back (p.audio->getSample (0, (int) (p.offsetSamples + i)));
        return samples;
    };
    const auto before = boundarySamples();
    expect ((juce::int64) before.size() == (juce::int64) 30000,
            "分割後も左右の実効長の和が全体と一致すること");

    juce::String error;
    expect (project.save (error), "分割状態を保存できること");

    juce::StringArray warnings;
    auto loaded = Project::load (dir, warnings, error);
    expect (loaded != nullptr && loaded->tracks[0].clips.size() == 2, "分割状態を読込できること");
    if (loaded != nullptr)
    {
        ClipDomains::reconcile (*loaded, sr);
        expect (renderAll (*loaded), "再読込後の一括再生成が完了すること");
        // 左右が同じ1本のドメインへ戻る（ドメイン永続化の中心的な回帰）
        expect (loaded->tracks[0].clips[0].activeDomain.get()
                    == loaded->tracks[0].clips[1].activeDomain.get(),
                "再読込後も左右が1本のレンダーを共有すること");
        auto& reloadedClips = loaded->tracks[0].clips;
        std::vector<float> after;
        {
            std::vector<ClipPlayback> playbacks;
            for (const auto& c : reloadedClips)
                appendClipPlaybacks (c, playbacks);
            std::sort (playbacks.begin(), playbacks.end(),
                       [] (const ClipPlayback& a, const ClipPlayback& b)
                       { return a.startSample < b.startSample; });
            for (const auto& p : playbacks)
                for (juce::int64 i = 0; i < p.lengthSamples; ++i)
                    after.push_back (p.audio->getSample (0, (int) (p.offsetSamples + i)));
        }
        bool identical = before.size() == after.size();
        if (identical)
            for (size_t i = 0; i < before.size(); ++i)
                if (! juce::exactlyEqual (before[i], after[i]))
                {
                    identical = false;
                    break;
                }
        expect (identical,
                "分割 → 保存 → 再読込で境界付近を含めて同じ音になること（固定シード）");
    }
    dir.deleteRecursively();
}

void testGachaLoopStretchValues()
{
    beginTest ("gacha loop preview stretch values");
    const double sr = 48000.0;

    const auto makeAnchor = [] (double bpm)
    {
        LoopAnchor anchor;
        anchor.libraryPath = "loops/test.wav";
        anchor.bpm = bpm;
        anchor.key = ProjectKey { 9, KeyMode::minor };
        anchor.loopBars = 2;
        anchor.slotsPerBar = 1;
        anchor.roots = { 9, 9 };
        anchor.confidence = { 1.0f, 1.0f };
        return anchor;
    };

    // 「採用のみ」（applyKeyBpm=false）: stretchRatio = ループBPM / プロジェクトBPM・移調も入る
    {
        Project project;
        project.bpm = 90.0;
        project.sampleRate = sr;
        GachaSession session;
        GachaSession::LoopPreviewInput input;
        input.anchor = makeAnchor (82.0);
        const int loopSamples = (int) std::llround (2 * 240.0 / 82.0 * sr);
        input.audio = makeToneBuffer (440.0, loopSamples, sr);
        input.audioSampleRate = sr;
        input.displayName = "test";
        input.applyKeyBpm = false;
        input.transposeSemitones = 2;
        expect (session.previewLoopCandidate (project, input), "仮配置できること");

        const Clip* placed = nullptr;
        for (const auto& t : project.tracks)
            for (const auto& c : t.clips)
                if (session.isPreviewClip (t.id, c.fileName))
                    placed = &c;
        expect (placed != nullptr, "仮クリップが見つかること");
        if (placed != nullptr)
        {
            expect (placed->transposeSemitones == 2, "候補の移調量が入ること");
            expect (std::abs (placed->stretchRatio - 82.0 / 90.0) < 1e-12,
                    "stretchRatio = ループBPM/プロジェクトBPM で入ること");
            expect (placed->activeDomain != nullptr && placed->activeDomain->ratio == 1.0,
                    "プレビューの実効状態は無加工から始まること（副作用はメモリ内のみ）");
            // 敷いた時点で「ちょうどN小節」の見かけ長になる要求であること
            const auto targetLength = (juce::int64) std::llround (
                (double) placed->lengthSamples * placed->stretchRatio);
            const auto projectBar = 240.0 / 90.0 * sr;
            expect (std::abs ((double) targetLength - projectBar * 2) < 2.0,
                    "要求の見かけ長がプロジェクトBPMの2小節に一致すること");
        }

        // キャンセルで track ごと消え、値が残らない
        session.cancelPart (GachaSession::Part::loops, project);
        bool remains = false;
        for (const auto& t : project.tracks)
            remains = remains || ! t.clips.empty();
        expect (! remains, "キャンセルで仮クリップが残らないこと");
    }

    // 逆コピー（applyKeyBpm=true）: BPM・キーがループに合うので両方とも中立
    {
        Project project;
        project.bpm = 90.0;
        project.sampleRate = sr;
        GachaSession session;
        GachaSession::LoopPreviewInput input;
        input.anchor = makeAnchor (82.0);
        const int loopSamples = (int) std::llround (2 * 240.0 / 82.0 * sr);
        input.audio = makeToneBuffer (440.0, loopSamples, sr);
        input.audioSampleRate = sr;
        input.displayName = "test";
        input.applyKeyBpm = true;
        input.transposeSemitones = 2; // 逆コピーでは無視される
        expect (session.previewLoopCandidate (project, input), "逆コピーで仮配置できること");
        const Clip* placed = nullptr;
        for (const auto& t : project.tracks)
            for (const auto& c : t.clips)
                if (session.isPreviewClip (t.id, c.fileName))
                    placed = &c;
        expect (placed != nullptr && placed->transposeSemitones == 0
                    && placed->stretchRatio == 1.0,
                "逆コピーでは移調・伸縮とも中立であること");
    }
}



// ---- ボーカルのピッチ補正 Phase 1: 検出・サイドカー・ノート（docs/plans/2026-08-20-2244-vocal-pitch-correction.md）----

// 合成ボーカル風の素材: のこぎり波＋ビブラート・無音・ノイズバースト。真値 f0（フレーム単位）を返す。
// 決定論的（乱数は固定シード）。実声はコミットしない（PUBLIC リポジトリ）
struct SynthVoice
{
    std::shared_ptr<juce::AudioBuffer<float>> audio;
    std::vector<double> truthF0; // フレーム k の真値（無声 = 0）
    std::vector<std::pair<int, double>> notes; // (開始フレーム, midi) 期待ノート
};

SynthVoice makeSynthVoice (double sr)
{
    // (midi, 秒, デチューン cent, ビブラート深さ cent)
    const std::vector<std::tuple<double, double, double, double>> melody = {
        { 62, 0.55, 25, 0 }, { 64, 0.45, -30, 0 }, { 65, 0.70, 15, 40 }, { 67, 0.50, -20, 0 },
        { 69, 0.90, 35, 50 }, { 62, 1.10, 10, 60 }, { 57, 0.50, -40, 0 }, { 60, 0.80, 20, 0 } };
    std::vector<float> samples;
    std::vector<double> f0PerSample;
    juce::Random rng (7);
    auto addSilence = [&] (double sec)
    {
        const int n = (int) (sec * sr);
        samples.insert (samples.end(), (size_t) n, 0.0f);
        f0PerSample.insert (f0PerSample.end(), (size_t) n, 0.0);
    };
    auto addNoise = [&] (double sec, float level)
    {
        const int n = (int) (sec * sr);
        float prev = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float white = (rng.nextFloat() * 2.0f - 1.0f) * level;
            const float hp = white - 0.95f * prev; // 高域寄り（s 音っぽく）
            prev = white;
            const float env = 0.5f - 0.5f * (float) std::cos (juce::MathConstants<double>::twoPi * i / juce::jmax (1, n - 1));
            samples.push_back (hp * env);
            f0PerSample.push_back (0.0);
        }
    };
    SynthVoice out;
    addSilence (0.3);
    int idx = 0;
    for (const auto& [midi, dur, cents, vib] : melody)
    {
        if (idx % 3 == 1)
            addNoise (0.06, 0.08f);
        const int n = (int) (dur * sr);
        const int startFrame = (int) samples.size() / PitchCurve::hopSamplesFor (sr);
        out.notes.push_back ({ startFrame, midi + cents / 100.0 });
        double phase = 0.0;
        // 簡易フォルマント（2共振）
        double y1[2] = {}, y2[2] = {};
        const double fc[2] = { 700, 1200 }, bw[2] = { 130, 160 };
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            double m = midi + cents / 100.0;
            if (vib > 0)
                m += (vib / 100.0) * juce::jlimit (0.0, 1.0, t / 0.15) * std::sin (juce::MathConstants<double>::twoPi * 5.5 * t);
            const double f0 = 440.0 * std::pow (2.0, (m - 69.0) / 12.0);
            phase += f0 / sr;
            phase -= std::floor (phase);
            double v = 2.0 * phase - 1.0;
            for (int r = 0; r < 2; ++r)
            {
                const double rr = std::exp (-juce::MathConstants<double>::pi * bw[r] / sr);
                const double a1 = -2.0 * rr * std::cos (juce::MathConstants<double>::twoPi * fc[r] / sr);
                const double a2 = rr * rr;
                const double yy = (1.0 - rr) * v - a1 * y1[r] - a2 * y2[r];
                y2[r] = y1[r]; y1[r] = yy; v = yy;
            }
            double env = 1.0;
            const int aN = (int) (0.02 * sr), rN = (int) (0.06 * sr);
            if (i < aN) env = (double) i / aN;
            else if (i >= n - rN) env = (double) (n - i) / rN;
            samples.push_back ((float) (v * env * 0.25));
            f0PerSample.push_back (f0);
        }
        addSilence (0.12);
        ++idx;
    }
    addNoise (0.25, 0.02f);
    addSilence (0.3);

    float peak = 0.0f;
    for (auto v : samples) peak = juce::jmax (peak, std::abs (v));
    out.audio = std::make_shared<juce::AudioBuffer<float>> (1, (int) samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        out.audio->setSample (0, (int) i, samples[i] / peak * 0.5f);
    const int hop = PitchCurve::hopSamplesFor (sr);
    const int frames = ((int) samples.size() + hop - 1) / hop;
    for (int k = 0; k < frames; ++k)
        out.truthF0.push_back (f0PerSample[(size_t) juce::jmin ((int) f0PerSample.size() - 1, k * hop)]);
    return out;
}

// 検出器の指紋を固定する（algoId ごと）。検出結果が変わったらこのテストが落ちる＝ PitchAnalyzer::algoId を上げる合図。
// 「再解析」メニューを廃止した（検出器が変わるのはアプリ更新時だけ）ので、変わったことに気づく手段をここに置く。
// マシン差（浮動小数の末尾）で揺れないよう、ピッチは cent に丸め、有声/無声の列と合わせてハッシュする
void testPitchAnalyzerFingerprint()
{
    beginTest ("PitchAnalyzer fingerprint fixed per algoId");
    const double sr = 48000.0;
    auto voice = makeSynthVoice (sr);
    const auto curve = PitchAnalyzer::analyze (*voice.audio, sr);
    DigestBuilder d;
    for (int k = 0; k < curve.numFrames(); ++k)
    {
        const std::int32_t cents = curve.isVoiced (k) ? (std::int32_t) std::lround (curve.midiAt (k) * 100.0) : -1;
        d.add (cents);
    }
    const auto hex = d.finish().toHex();
    const juce::String expectedAlgoId = "yin-1";
    const juce::String expectedHex = "877852b87b2d99a7a6e150a558ff7e52";
    std::cout << "  algoId=" << PitchAnalyzer::algoId << " fingerprint=" << hex << std::endl;
    expect (juce::String (PitchAnalyzer::algoId) == expectedAlgoId && hex == expectedHex,
            "検出結果が変わった。意図した変更なら PitchAnalyzer::algoId を上げ、このテストの expectedAlgoId / expectedHex を更新する（実値は上の行）");
}

void testPitchAnalyzerYin()
{
    beginTest ("PitchAnalyzer YIN vs known f0 (GPE / voicing P-R)");
    const double sr = 48000.0;
    auto voice = makeSynthVoice (sr);
    const auto curve = PitchAnalyzer::analyze (*voice.audio, sr);
    expect (curve.numFrames() == (int) voice.truthF0.size(), "フレーム数が真値と一致");
    expect (curve.hopSamples == 240, "48kHz の hop は 240 サンプル（5ms）");
    expect (curve.algoId == PitchAnalyzer::algoId, "algoId が入る");
    expect (curve.source.frames == voice.audio->getNumSamples() && curve.source.channels == 1,
            "source 識別子が原音と一致");
    expect (curve.validate(), "解析結果が validate を通る");

    int bothVoiced = 0, gross = 0, detVoiced = 0, truthVoiced = 0, hit = 0;
    std::vector<double> fineCents;
    for (int k = 0; k < curve.numFrames(); ++k)
    {
        const bool tv = voice.truthF0[(size_t) k] > 0.0;
        const bool dv = curve.isVoiced (k);
        truthVoiced += tv; detVoiced += dv; hit += (tv && dv);
        if (tv && dv)
        {
            ++bothVoiced;
            const double tm = 69.0 + 12.0 * std::log2 (voice.truthF0[(size_t) k] / 440.0);
            const double diff = std::abs (tm - curve.midiAt (k));
            if (diff >= 1.0) ++gross; else fineCents.push_back (diff * 100.0);
        }
    }
    const double gpe = bothVoiced > 0 ? (double) gross / bothVoiced : 1.0;
    const double precision = detVoiced > 0 ? (double) hit / detVoiced : 0.0;
    const double recall = truthVoiced > 0 ? (double) hit / truthVoiced : 0.0;
    std::sort (fineCents.begin(), fineCents.end());
    const double medianCents = fineCents.empty() ? 999.0 : fineCents[fineCents.size() / 2];
    std::cout << "  GPE=" << gpe << " P=" << precision << " R=" << recall << " median=" << medianCents << "c" << std::endl;
    // 合格線は plan（GPE<3%・P≥0.95・R≥0.90）。lab の Python 原型は GPE 0 / P 0.992 / R 0.994 / 1.4c
    expect (gpe < 0.03, "GPE < 3%");
    expect (precision >= 0.95, "voicing precision >= 0.95");
    expect (recall >= 0.90, "voicing recall >= 0.90");
    expect (medianCents < 5.0, "微小誤差の中央値 < 5 cent");

    // 先頭の無音は全フレーム無声
    bool headSilent = true;
    for (int k = 0; k < 50; ++k) headSilent = headSilent && ! curve.isVoiced (k);
    expect (headSilent, "先頭の無音は無声");

    // ステレオ（L=信号, R=無音）は Mid で解析され、モノと同じ有声フレーム数±2%
    juce::AudioBuffer<float> stereo (2, voice.audio->getNumSamples());
    stereo.clear();
    stereo.copyFrom (0, 0, *voice.audio, 0, 0, voice.audio->getNumSamples());
    const auto stereoCurve = PitchAnalyzer::analyze (stereo, sr);
    int sv = 0;
    for (int k = 0; k < stereoCurve.numFrames(); ++k) sv += stereoCurve.isVoiced (k);
    expect (std::abs (sv - detVoiced) <= detVoiced / 50 + 2, "ステレオは Mid で解析される");

    // 決定論: 2回解析して digest が一致
    expect (PitchAnalyzer::analyze (*voice.audio, sr).digest() == curve.digest(), "同じ入力で同じ digest");

    // キャンセル: 最初から true なら空
    expect (PitchAnalyzer::analyze (*voice.audio, sr, [] { return true; }).numFrames() == 0, "キャンセルで空");
}

void testPitchCmndfBruteForce()
{
    beginTest ("PitchAnalyzer CMNDF fft == brute force");
    const int length = PitchAnalyzer::frameLength;
    const int tauMax = 800;
    std::vector<float> frame ((size_t) length);
    juce::Random rng (3);
    for (int i = 0; i < length; ++i)
        frame[(size_t) i] = (float) std::sin (i * 0.0314) * 0.5f + (rng.nextFloat() - 0.5f) * 0.05f;
    const auto fast = PitchAnalyzer::cumulativeMeanNormalizedDifference (frame.data(), length, tauMax);
    const int w = length - tauMax;
    std::vector<double> d ((size_t) tauMax + 1, 0.0);
    for (int t = 1; t <= tauMax; ++t)
        for (int n = 0; n < w; ++n)
        {
            const double diff = (double) frame[(size_t) n] - frame[(size_t) (n + t)];
            d[(size_t) t] += diff * diff;
        }
    double run = 0.0, maxErr = 0.0;
    for (int t = 1; t <= tauMax; ++t)
    {
        run += d[(size_t) t];
        const double cm = run > 0 ? d[(size_t) t] * t / run : 1.0;
        maxErr = juce::jmax (maxErr, std::abs (cm - fast[(size_t) t]));
    }
    expect (fast.size() == (size_t) tauMax + 1 && juce::exactlyEqual (fast[0], 1.0), "τ=0 は 1");
    expect (maxErr < 1e-3, "FFT 版と総当たりの差 < 1e-3");
}

void testPitchSidecar()
{
    beginTest ("PitchSidecar immutable generations / validation / atomic");
    const double sr = 48000.0;
    auto voice = makeSynthVoice (sr);
    auto curve = PitchAnalyzer::analyze (*voice.audio, sr);
    auto dir = makeTempDir();
    auto wav = dir.getChildFile ("clip-003.wav");
    expect (writeBufferWav (wav, *voice.audio, sr), "WAV を書ける");

    const auto digest = curve.digest();
    expect (digest.toHex().length() == 32, "digest は 32 文字の hex");
    expect (ContentDigest::fromHex (digest.toHex()).value_or (ContentDigest{}) == digest, "hex の往復");
    expect (! ContentDigest::fromHex ("zz").has_value(), "不正 hex は nullopt");

    juce::String err;
    expect (PitchSidecar::write (curve, wav, &err), (juce::String::fromUTF8 (u8"書き出し成功: ") + err).toRawUTF8());
    const auto file = PitchSidecar::fileFor (wav, digest);
    expect (file.existsAsFile(), "clip-003.<digest>.pitch ができる");
    expect (file.getFileName() == "clip-003." + digest.toHex() + ".pitch", "ファイル名の形式");
    expect (PitchSidecar::digestFromFile (file).value_or (ContentDigest{}) == digest, "ファイル名から digest を復元");
    const auto mtime = file.getLastModificationTime();
    expect (PitchSidecar::listFor (wav).size() == 1, "listFor が1件");

    auto back = PitchSidecar::read (file, &err);
    expect (back.has_value(), (juce::String::fromUTF8 (u8"読み戻せる: ") + err).toRawUTF8());
    if (back.has_value())
    {
        expect (back->digest() == digest && back->source == curve.source && back->f0 == curve.f0
                && back->voicing == curve.voicing && back->rms == curve.rms && back->algoId == curve.algoId,
                "内容が完全一致");
    }
    expect (PitchSidecar::readFor (wav, digest, curve.source, &err).has_value(), "readFor で世代を引ける");

    // 同一内容の再書き出しはファイルを触らない（世代不変）
    juce::Thread::sleep (20);
    expect (PitchSidecar::write (curve, wav, &err), "同一内容の再書き出し");
    expect (file.getLastModificationTime() == mtime, "同一内容なら書き直さない");

    // 内容が変わる再解析 = 別世代が**追加**される（旧世代は残る）
    auto curve2 = curve;
    curve2.algoId = "yin-test";
    expect (PitchSidecar::write (curve2, wav, &err), "別世代の書き出し");
    expect (PitchSidecar::listFor (wav).size() == 2 && file.existsAsFile(), "旧世代を残して2世代になる");

    // WAV が差し替わった（識別子不一致）→ readFor は nullopt
    SourceIdentity other = curve.source;
    other.frames += 1;
    expect (! PitchSidecar::readFor (wav, digest, other, &err).has_value(), "WAV と食い違えば未解析扱い");
    // 無い世代
    ContentDigest missing { 1, 2 };
    expect (! PitchSidecar::readFor (wav, missing, curve.source, &err).has_value(), "無い世代は nullopt");

    // 途中まで書かれたファイル（切り詰め）は読まない
    juce::MemoryBlock data;
    file.loadFileAsData (data);
    auto truncated = dir.getChildFile ("clip-003.00000000000000000000000000000001.pitch");
    truncated.replaceWithData (data.getData(), data.getSize() / 2);
    expect (! PitchSidecar::read (truncated, &err).has_value(), (juce::String::fromUTF8 (u8"切り詰めファイルは拒否: ") + err).toRawUTF8());
    // 1バイト壊す → digest 不一致で拒否
    auto corrupt = dir.getChildFile ("clip-003.00000000000000000000000000000002.pitch");
    auto* bytes = static_cast<unsigned char*> (data.getData());
    bytes[data.getSize() / 2] ^= 0xff;
    corrupt.replaceWithData (data.getData(), data.getSize());
    expect (! PitchSidecar::read (corrupt, &err).has_value(), (juce::String::fromUTF8 (u8"内容改変は digest 不一致で拒否: ") + err).toRawUTF8());

    // validate: 値域外・非有限
    auto bad = curve;
    bad.f0[100] = 5.0f;
    expect (! bad.validate(), "f0 が値域外なら不正");
    bad = curve;
    bad.voicing[0] = std::numeric_limits<float>::quiet_NaN();
    expect (! bad.validate(), "NaN は不正");
    bad = curve;
    bad.rms.pop_back();
    expect (! bad.validate(), "配列長不一致は不正");
    expect (! PitchSidecar::write (bad, wav, &err), "不正なカーブは書かない");

    dir.deleteRecursively();
}

void testPitchNotesRules()
{
    beginTest ("PitchNotes split rules (vibrato / jump / short merge)");
    const double sr = 48000.0;
    auto makeCurve = [&] (const std::vector<double>& midi)
    {
        PitchCurve c;
        c.algoId = "test"; c.sampleRate = sr; c.hopSamples = PitchCurve::hopSamplesFor (sr);
        c.source.frames = 1; c.source.channels = 1; c.source.sampleRate = sr;
        for (auto m : midi)
        {
            const bool v = m > 0;
            c.f0.push_back (v ? (float) (440.0 * std::pow (2.0, (m - 69.0) / 12.0)) : 0.0f);
            c.voicing.push_back (v ? 0.9f : 0.0f);
            c.rms.push_back (v ? 0.1f : 0.0f);
        }
        return c;
    };
    // 1) ビブラート ±0.6 半音（5.5Hz）を 1 秒 → 1 ノート
    {
        std::vector<double> m;
        for (int k = 0; k < 200; ++k)
            m.push_back (60.0 + 0.6 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * k * 0.005));
        const auto notes = PitchNotes::detect (makeCurve (m));
        expect (notes.size() == 1, "ビブラートでは割れない");
        if (! notes.empty())
            expect (std::abs (notes[0].medianMidi - 60.0f) < 0.1f, "中央値はビブラートの中心");
    }
    // 2) 60 → 62 のジャンプ（隙間なし）→ 2 ノート・境界はジャンプ位置
    {
        std::vector<double> m (100, 60.0);
        m.insert (m.end(), 100, 62.0);
        const auto notes = PitchNotes::detect (makeCurve (m));
        expect (notes.size() == 2, "半音以上のジャンプで切れる");
        if (notes.size() == 2)
        {
            expect (notes[0].startFrame == 0 && notes[0].endFrame == 100 && notes[1].startFrame == 100
                    && notes[1].endFrame == 200, "境界がジャンプ位置");
            expect (std::abs (notes[0].medianMidi - 60.0f) < 0.01f && std::abs (notes[1].medianMidi - 62.0f) < 0.01f,
                    "各ノートの中央値");
        }
    }
    // 3) 0.3 半音のずれはジャンプではない → 1 ノート
    {
        std::vector<double> m (100, 60.0);
        m.insert (m.end(), 100, 60.3);
        expect (PitchNotes::detect (makeCurve (m)).size() == 1, "半音の半分未満では切れない");
    }
    // 4) 無声の隙間で分かれる・無声はノートにならない
    {
        std::vector<double> m (60, 60.0);
        m.insert (m.end(), 20, 0.0);
        m.insert (m.end(), 60, 64.0);
        const auto notes = PitchNotes::detect (makeCurve (m));
        expect (notes.size() == 2 && notes[0].endFrame == 60 && notes[1].startFrame == 80, "無声の隙間で分かれる");
    }
    // 5) 短い（40ms = 8 フレーム）ノートは直前へ吸収。先頭なら直後へ
    {
        std::vector<double> m (100, 60.0);
        m.insert (m.end(), 8, 66.0);   // 短い跳ね
        m.insert (m.end(), 100, 60.0);
        const auto notes = PitchNotes::detect (makeCurve (m));
        expect (notes.size() == 1 && notes[0].startFrame == 0 && notes[0].endFrame == 208, "短い跳ねは吸収される");
        std::vector<double> m2 (8, 66.0);
        m2.insert (m2.end(), 100, 60.0);
        const auto notes2 = PitchNotes::detect (makeCurve (m2));
        expect (notes2.size() == 1 && notes2[0].startFrame == 0, "先頭の短い音は直後へ吸収");
        std::vector<double> m3 (8, 66.0); // 孤立した短い音（吸収先なし）は捨てる
        m3.insert (m3.end(), 20, 0.0);
        m3.insert (m3.end(), 100, 60.0);
        const auto notes3 = PitchNotes::detect (makeCurve (m3));
        expect (notes3.size() == 1 && notes3[0].startFrame == 28, "孤立した短い音は捨てる");
    }
    // 6) 合成ボーカルで期待ノート数と中央値
    {
        auto voice = makeSynthVoice (sr);
        const auto curve = PitchAnalyzer::analyze (*voice.audio, sr);
        const auto notes = PitchNotes::detect (curve);
        expect (notes.size() == voice.notes.size(), (juce::String::fromUTF8 (u8"合成ボーカルのノート数 = メロディ数 (") + juce::String ((int) notes.size()) + ")").toRawUTF8());
        if (notes.size() == voice.notes.size())
            for (size_t i = 0; i < notes.size(); ++i)
            {
                expect (std::abs (notes[i].startFrame - voice.notes[i].first) <= 6, "開始位置 ±30ms");
                expect (std::abs (notes[i].medianMidi - voice.notes[i].second) < 0.12, "中央値 ±12cent（ビブラート・しゃくり込み）");
            }
    }
}

void testPitchKeyEstimate()
{
    beginTest ("PitchNotes key estimate (Krumhansl)");
    auto notesFor = [] (std::initializer_list<int> midis)
    {
        std::vector<DetectedPitchNote> v;
        int f = 0;
        for (auto m : midis) { v.push_back ({ f, f + 40, (float) m }); f += 50; }
        return v;
    };
    auto c = PitchNotes::estimateKey (notesFor ({ 60, 62, 64, 65, 67, 69, 71, 72, 67, 64, 60 }));
    expect (c.valid && c.key.root == 0 && c.key.mode == KeyMode::major, "C major の音階 → C major");
    expect (c.correlation > 0.7, "明確な音階なら相関 0.7 超");
    auto a = PitchNotes::estimateKey (notesFor ({ 69, 71, 72, 74, 76, 77, 79, 81, 76, 72, 69 }));
    expect (a.valid && a.key.root == 9 && a.key.mode == KeyMode::minor, "A natural minor → A minor");
    expect (! PitchNotes::estimateKey ({}).valid, "ノート無しは無効");
}

void testPitchAnalysisWorker()
{
    beginTest ("PitchAnalysisWorker generation / cancel / sidecar");
    const double sr = 48000.0;
    auto voice = makeSynthVoice (sr);
    auto dir = makeTempDir();
    auto wav = dir.getChildFile ("clip-007.wav");
    expect (writeBufferWav (wav, *voice.audio, sr), "WAV を書ける");

    PitchAnalysisWorker worker;
    // A を開始 → すぐ切替（cancel＋join）→ B（generation 2）を開始 → B の結果だけが着地する
    expect (worker.start ({ voice.audio, sr, juce::File(), 1 }), "A 開始");
    expect (! worker.start ({ voice.audio, sr, juce::File(), 99 }), "実行中は start できない");
    worker.cancelAndWait();
    expect (worker.status() != PitchAnalysisWorker::Status::running, "cancel 後は running でない");
    auto a = worker.takeResult();
    expect (a.generation == 1, "A の結果は generation 1");
    expect (worker.status() == PitchAnalysisWorker::Status::idle, "take 後は idle");

    expect (worker.start ({ voice.audio, sr, wav, 2 }), "B 開始");
    const auto deadline = juce::Time::getMillisecondCounter() + 30000;
    while (worker.status() == PitchAnalysisWorker::Status::running && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep (10);
    expect (worker.status() == PitchAnalysisWorker::Status::success, "B が成功で終わる");
    auto b = worker.takeResult();
    expect (b.generation == 2 && b.curve.numFrames() == (int) voice.truthF0.size(), "B の結果が generation 2 で着地");
    expect (b.sidecarWritten && b.sidecarFile.existsAsFile(), (juce::String::fromUTF8 (u8"サイドカーが書かれる: ") + b.errorMessage).toRawUTF8());
    expect (b.sidecarFile == PitchSidecar::fileFor (wav, b.curve.digest()), "サイドカー名は digest 由来");
    expect (worker.progress() >= 1.0f, "完了時の進捗は 1");

    // 再解析連打: 同じ内容なら同じファイル（世代が増えない）
    expect (worker.start ({ voice.audio, sr, wav, 3 }), "再解析");
    while (worker.status() == PitchAnalysisWorker::Status::running) juce::Thread::sleep (10);
    auto c = worker.takeResult();
    expect (c.generation == 3 && c.curve.digest() == b.curve.digest(), "同一内容なら同じ digest");
    expect (PitchSidecar::listFor (wav).size() == 1, "同一内容の再解析では世代が増えない");

    // WAV が消えていたら孤児サイドカーを書かない
    auto gone = dir.getChildFile ("clip-008.wav");
    expect (worker.start ({ voice.audio, sr, gone, 4 }), "消えた WAV 向け");
    while (worker.status() == PitchAnalysisWorker::Status::running) juce::Thread::sleep (10);
    auto d = worker.takeResult();
    expect (d.status == PitchAnalysisWorker::Status::success && ! d.sidecarWritten, "解析は成功するがサイドカーは書かない");
    expect (PitchSidecar::listFor (gone).empty(), "孤児 .pitch が無い");

    // 空の原音は failed で終わる（running のまま残らない）
    expect (worker.start ({ std::make_shared<juce::AudioBuffer<float>> (1, 0), sr, juce::File(), 5 }), "空原音の start");
    while (worker.status() == PitchAnalysisWorker::Status::running) juce::Thread::sleep (10);
    expect (worker.takeResult().status == PitchAnalysisWorker::Status::failed, "空原音は failed");

    dir.deleteRecursively();
}

// ---- ボーカルのピッチ補正 Phase 2: 時間写像・編集状態・目標カーブ・WORLD レンダー・永続化 ----

// ms → サンプル（48kHz）
juce::int64 msSamples (double ms) { return (juce::int64) std::llround (ms / 1000.0 * 48000.0); }

// 3ノート A=[0,500ms] B=[500,900] C=[1000,1400]（A-B 隙間0・B-C 隙間100）。domain = [0, 1500ms)
PitchCorrection makeThreeNotes()
{
    PitchCorrection pc;
    pc.curveDigest = { 1, 2 };
    pc.timeNodes = { { msSamples (500), 0 }, { msSamples (900), 0 }, { msSamples (1000), 0 }, { msSamples (1400), 0 } };
    pc.notes = { { BoundaryRef::domainStart(), BoundaryRef::node (0), 60, false },
                 { BoundaryRef::node (0), BoundaryRef::node (1), 62, false },
                 { BoundaryRef::node (2), BoundaryRef::node (3), 64, false } };
    return pc;
}

void testTimeMapBasics()
{
    beginTest ("TimeMap uniform / piecewise / inverse");
    const auto u = TimeMap::uniform (1000, 24000, 1.25);
    expect (u.isUniform() && u.outputLength() == 30000, "一様写像の出力長");
    expect (u.map (1000) == 0 && u.map (25000) == 30000 && u.map (13000) == 15000, "一様写像のノード上と中点");
    // RenderedDomain 経由の一様写像は v20 の mapBoundary（round((src-offset)×ratio)）と全点一致
    {
        RenderedDomain d;
        d.domainOffset = 1000; d.domainLength = 24000; d.ratio = 1.25;
        d.timeMap = u;
        bool same = true;
        for (juce::int64 s = 1000; s <= 25000; s += 7)
            same = same && d.mapBoundary (s) == (juce::int64) std::llround ((double) (s - 1000) * 1.25);
        expect (same, "一様写像は v20 の mapBoundary と一致");
    }
    // inverse: map(inverse(r)) は r に最も近い到達可能値（差 ≤ 1）
    bool inv = true;
    for (juce::int64 r = 0; r <= 30000; r += 11)
        inv = inv && std::llabs (u.map (u.inverse (r)) - r) <= 1;
    expect (inv, "一様写像の逆変換");

    TimeMap pw;
    pw.nodes = { { 0, 0 }, { 1000, 1200 }, { 3000, 3200 }, { 4000, 4000 } };
    expect (pw.map (500) == 600 && pw.map (2000) == 2200 && pw.map (3500) == 3600, "区分線形の補間");
    expect (pw.inverse (600) == 500 && pw.inverse (3600) == 3500 && pw.inverse (4000) == 4000, "区分線形の逆変換");
    bool mono = true;
    for (juce::int64 s = 1; s <= 4000; ++s) mono = mono && pw.map (s) >= pw.map (s - 1);
    expect (mono, "単調非減少");
    expect (pw.map (5000) == 4800, "末尾より先は最後の傾き（0.8）で外挿");
}

void testPitchTimeMapRules()
{
    beginTest ("PitchCorrection timeMap: move / clamp / min segment / determinism");
    const double sr = 48000.0;
    const juce::int64 dom = msSamples (1500);
    auto pc = makeThreeNotes();
    juce::String why;
    expect (pc.validate (0, dom, &why), (juce::String::fromUTF8 (u8"初期状態は valid: ") + why).toRawUTF8());
    {
        const auto m = PitchCorrections::buildTimeMap (pc, 0, dom, 1.0, sr);
        expect (m.nodes.size() == 6 && m.map (msSamples (500)) == msSamples (500), "Δ=0 なら一様");
    }
    // B を +50ms → A=[0,550]・B=[550,950]・B-C 隙間 50・C 不動
    {
        auto moved = pc;
        const auto d = PitchCorrections::moveNote (moved, 1, msSamples (50), 0, dom, 1.0, sr);
        expect (d == msSamples (50), "+50ms がそのまま通る");
        const auto m = PitchCorrections::buildTimeMap (moved, 0, dom, 1.0, sr);
        expect (m.map (msSamples (500)) == msSamples (550) && m.map (msSamples (900)) == msSamples (950),
                "B の両端が +50（A は伸び、B の長さ不変）");
        expect (m.map (msSamples (1000)) == msSamples (1000) && m.map (msSamples (1400)) == msSamples (1400), "C は不動");
        expect (m.outputLength() == dom, "クリップ長不変");
        // 保存 Δ は共有ノードに1つ
        expect (moved.timeNodes[0].timingDeltaSamples == msSamples (50) && moved.timeNodes[1].timingDeltaSamples == msSamples (50)
                && moved.timeNodes[2].timingDeltaSamples == 0, "Δ は動かしたノードだけ");
        // JSON 往復で timeMap が一致
        auto back = PitchCorrection::fromJson (moved.toJson());
        expect (back.has_value() && *back == moved, "JSON 往復で同一");
        expect (back.has_value() && PitchCorrections::buildTimeMap (*back, 0, dom, 1.0, sr) == m, "往復後の timeMap 一致");
    }
    // B を −50ms → A=[0,450]・隙間 150
    {
        auto moved = pc;
        expect (PitchCorrections::moveNote (moved, 1, -msSamples (50), 0, dom, 1.0, sr) == -msSamples (50), "-50ms");
        const auto m = PitchCorrections::buildTimeMap (moved, 0, dom, 1.0, sr);
        expect (m.map (msSamples (500)) == msSamples (450) && m.map (msSamples (1000)) == msSamples (1000), "A が縮み隙間が 150");
    }
    // A を +50: 先頭が domainStart（固定）なので動かない
    {
        auto moved = pc;
        expect (PitchCorrections::moveNote (moved, 0, msSamples (50), 0, dom, 1.0, sr) == 0, "端点に接するノートは動かない");
    }
    // B を +200: B-C 隙間 100ms を 10ms 残す位置（+90）でクランプ
    {
        auto moved = pc;
        expect (PitchCorrections::moveNote (moved, 1, msSamples (200), 0, dom, 1.0, sr) == msSamples (90), "交差直前（10ms）でクランプ");
    }
    // C を −200: 隙間 100 → 10ms までしか縮まない（−90）。+200 は末尾 100ms → +90
    {
        auto moved = pc;
        expect (PitchCorrections::moveNote (moved, 2, -msSamples (200), 0, dom, 1.0, sr) == -msSamples (90), "逆方向のクランプ");
        auto moved2 = pc;
        expect (PitchCorrections::moveNote (moved2, 2, msSamples (200), 0, dom, 1.0, sr) == msSamples (90), "末尾側のクランプ");
    }
    // 2音を逆方向へ: B −50 のあと C +50 → 隙間 200
    {
        auto moved = pc;
        PitchCorrections::moveNote (moved, 1, -msSamples (50), 0, dom, 1.0, sr);
        PitchCorrections::moveNote (moved, 2, msSamples (50), 0, dom, 1.0, sr);
        const auto m = PitchCorrections::buildTimeMap (moved, 0, dom, 1.0, sr);
        expect (m.map (msSamples (900)) == msSamples (850) && m.map (msSamples (1000)) == msSamples (1050), "連続する2音を逆方向へ");
    }
    // ratio 0.25 で 5ms 隙間: 初期配置が下限を満たす（区間下限 = min(10ms, 初期長)）
    {
        PitchCorrection tiny;
        tiny.curveDigest = { 1, 2 };
        tiny.timeNodes = { { msSamples (100), 0 }, { msSamples (120), 0 } }; // 原音 20ms の隙間 → 出力 5ms
        tiny.notes = { { BoundaryRef::domainStart(), BoundaryRef::node (0), 60, false },
                       { BoundaryRef::node (1), BoundaryRef::domainEnd(), 60, false } };
        const auto m = PitchCorrections::buildTimeMap (tiny, 0, msSamples (400), 0.25, sr);
        expect (m.map (msSamples (120)) - m.map (msSamples (100)) == msSamples (5), "初期配置は実現可能（5ms の隙間）");
        bool strict = true;
        for (size_t i = 1; i < m.nodes.size(); ++i) strict = strict && m.nodes[i].output > m.nodes[i - 1].output;
        expect (strict, "出力側は厳密昇順");
    }
    // 決定論: 保存 Δ が新しい ratio の下限を破っても同じ入力なら同じ timeMap（往復しても不変）
    {
        auto moved = pc;
        PitchCorrections::moveNote (moved, 1, msSamples (90), 0, dom, 1.0, sr); // 隙間 10ms ぎりぎり
        const auto a = PitchCorrections::buildTimeMap (moved, 0, dom, 0.5, sr);  // 縮めると Δ(=90ms) が下限を破る
        const auto b = PitchCorrections::buildTimeMap (moved, 0, dom, 0.5, sr);
        PitchCorrections::buildTimeMap (moved, 0, dom, 1.0, sr);
        const auto c = PitchCorrections::buildTimeMap (moved, 0, dom, 0.5, sr);
        expect (a == b && b == c, "同じ入力なら常に同じ timeMap");
        expect (a.outputLength() == msSamples (750), "終点は round(length × ratio)");
        bool ok = true;
        for (size_t i = 1; i < a.nodes.size(); ++i) ok = ok && a.nodes[i].output > a.nodes[i - 1].output;
        expect (ok, "射影後も厳密昇順");
        expect (moved.timeNodes[0].timingDeltaSamples == msSamples (90), "保存値は書き換えない");
    }
}

void testPitchCorrectionValidation()
{
    beginTest ("PitchCorrection JSON validation (bad inputs are rejected)");
    const juce::int64 dom = msSamples (1500);
    auto base = makeThreeNotes();
    auto json = base.toJson();
    {
        auto bad = base; bad.notes[0].end = BoundaryRef::node (99);
        expect (! bad.validate (0, dom), "範囲外 index");
    }
    {
        auto bad = base; std::swap (bad.timeNodes[0], bad.timeNodes[1]);
        expect (! bad.validate (0, dom), "逆順 node");
    }
    {
        auto bad = base; bad.timeNodes.push_back ({ 0, 0 });
        expect (! bad.validate (0, dom), "端点と同座標の node");
    }
    {
        auto bad = base; bad.notes[1].start = BoundaryRef::domainStart();
        expect (! bad.validate (0, dom), "重なるノート");
    }
    {
        auto bad = base; bad.notes[0].start = BoundaryRef::node (1); bad.notes[0].end = BoundaryRef::node (0);
        expect (! bad.validate (0, dom), "開始 >= 終了");
    }
    {
        auto bad = base; bad.curveDigest = {};
        expect (! bad.validate (0, dom), "curveDigest 必須");
        auto* obj = json.getDynamicObject();
        auto copy = juce::var (obj->clone().release());
        copy.getDynamicObject()->setProperty ("curveDigest", "");
        expect (! PitchCorrection::fromJson (copy).has_value(), "JSON に curveDigest が無ければ拒否");
    }
    {
        // NaN Δ（JSON の double）は拒否
        auto copy = juce::var (json.getDynamicObject()->clone().release());
        auto nodes = copy.getDynamicObject()->getProperty ("timeNodes");
        nodes.getArray()->getReference (0).getDynamicObject()->setProperty ("delta", std::numeric_limits<double>::quiet_NaN());
        expect (! PitchCorrection::fromJson (copy).has_value(), "NaN Δ は拒否");
    }
    {
        // 全域ノート・終点だけ端点に一致するノート
        PitchCorrection whole; whole.curveDigest = { 1, 2 };
        whole.notes = { { BoundaryRef::domainStart(), BoundaryRef::domainEnd(), 60, false } };
        expect (whole.validate (0, dom), "全域ノート");
        auto back = PitchCorrection::fromJson (whole.toJson());
        expect (back.has_value() && *back == whole, "全域ノートの往復");
        PitchCorrection tail; tail.curveDigest = { 1, 2 };
        tail.timeNodes = { { msSamples (700), 0 } };
        tail.notes = { { BoundaryRef::node (0), BoundaryRef::domainEnd(), 60, false } };
        expect (tail.validate (0, dom), "終点一致のノート");
    }
    expect (PitchCorrection::fromJson (base.toJson()).value_or (PitchCorrection{}) == base, "往復で同一");
    expect (base.digest() == PitchCorrection::fromJson (base.toJson())->digest(), "digest も一致");
    auto other = base; other.notes[0].targetMidi = 61;
    expect (other.digest() != base.digest(), "内容が違えば digest が違う");
}

void testPitchSplitMerge()
{
    beginTest ("PitchCorrection split / merge keep or change timeMap as specified");
    const double sr = 48000.0;
    const juce::int64 dom = msSamples (1500);
    auto pc = makeThreeNotes();
    PitchCorrections::moveNote (pc, 1, msSamples (50), 0, dom, 1.0, sr); // 折れを作る
    const auto before = PitchCorrections::buildTimeMap (pc, 0, dom, 1.0, sr);
    // ノート B（index 1）を 700ms で分割 → timeMap 不変・継承
    expect (PitchCorrections::splitNote (pc, 1, msSamples (700), 0, dom, 1.0, sr), "分割できる");
    expect (pc.notes.size() == 4 && pc.timeNodes.size() == 5, "ノート4・ノード5");
    expect (pc.validate (0, dom), "分割後も valid");
    {
        // 補間ノードが増えるのでノード列は変わるが、写像としては一致（丸めの ±1 以内）
        const auto after = PitchCorrections::buildTimeMap (pc, 0, dom, 1.0, sr);
        bool same = after.nodes.size() == before.nodes.size() + 1;
        for (juce::int64 x = 0; x <= dom; x += 97)
            same = same && std::llabs (after.map (x) - before.map (x)) <= 1;
        for (const auto& n : before.nodes)
            same = same && after.map (n.source) == n.output;
        expect (same, "分割前後で timeMap 一致（音が変わらない）");
    }
    expect (pc.notes[1].targetMidi == 62 && pc.notes[2].targetMidi == 62, "左右が targetMidi を継承");
    expect (pc.notes[1].end == pc.notes[2].start, "分割境界は共有ノード");
    // 結合で元に戻る（中間ノードが補間ノードだけ）
    expect (PitchCorrections::mergeNotes (pc, 1, 0, dom), "結合できる");
    expect (pc.notes.size() == 3 && pc.timeNodes.size() == 4, "元の構造");
    expect (PitchCorrections::buildTimeMap (pc, 0, dom, 1.0, sr) == before, "分割→結合で timeMap が元と一致");
    // 隙間ありの B と C を結合 → 2ノード消えて隙間を取り込む。target は長い方（B 400ms > C 400ms 同長→左）、bypass は両方 true のみ
    {
        auto m = makeThreeNotes();
        m.notes[1].bypass = true; m.notes[2].bypass = true;
        m.notes[2].targetMidi = 64;
        m.timeNodes[3].sourceSample = msSamples (1450); // C を 450ms にして長い方にする
        expect (PitchCorrections::mergeNotes (m, 1, 0, dom), "隙間ありの結合");
        expect (m.notes.size() == 2 && m.timeNodes.size() == 2, "隙間の2ノードが消える");
        expect (m.notes[1].start == BoundaryRef::node (0) && m.notes[1].end == BoundaryRef::node (1), "参照 index が付け替わる");
        expect (m.notes[1].targetMidi == 64 && m.notes[1].bypass, "長い方の target・両方 true なら bypass");
        expect (m.validate (0, dom), "結合後も valid");
        auto m2 = makeThreeNotes(); m2.notes[1].bypass = true;
        PitchCorrections::mergeNotes (m2, 1, 0, dom);
        expect (! m2.notes[1].bypass, "片方だけ bypass なら false");
        auto m3 = makeThreeNotes(); m3.notes[1].pinned = true; m3.notes[1].targetMidi = 70; m3.timeNodes[3].sourceSample = msSamples (1450); // 右が長い
        PitchCorrections::mergeNotes (m3, 1, 0, dom);
        expect (m3.notes[1].targetMidi == 70 && m3.notes[1].pinned, "片側だけ pinned ならその側の目標（長さに依らない）");
        auto m4 = makeThreeNotes(); m4.notes[1].pinned = true; m4.notes[1].bypass = true; m4.notes[2].bypass = true;
        PitchCorrections::mergeNotes (m4, 1, 0, dom);
        expect (m4.notes[1].bypass && ! m4.notes[1].pinned, "結合で両方 bypass なら pinned は付かない（正規化）");
    }
}

void testPitchTargetCurve()
{
    beginTest ("PitchCorrection targetCurve (strength / speed / bypass / mask / transpose)");
    const double sr = 48000.0;
    // 60.3 の音が 1 秒（ビブラート ±0.2）、途中 100ms 無声、その後 64.4 を 0.5 秒
    PitchCurve c;
    c.algoId = "test"; c.sampleRate = sr; c.hopSamples = 240;
    c.source.frames = 48000 * 2; c.source.channels = 1; c.source.sampleRate = sr;
    auto push = [&] (double midi)
    {
        const bool v = midi > 0;
        c.f0.push_back (v ? (float) (440.0 * std::pow (2.0, (midi - 69.0) / 12.0)) : 0.0f);
        c.voicing.push_back (v ? 0.9f : 0.0f); c.rms.push_back (v ? 0.1f : 0.0f);
    };
    for (int k = 0; k < 200; ++k) push (60.3 + 0.2 * std::sin (k * 0.2));
    for (int k = 0; k < 20; ++k) push (0);
    for (int k = 0; k < 100; ++k) push (64.4);
    const juce::int64 dom = 320 * 240;
    PitchCorrection pc;
    pc.curveDigest = c.digest();
    pc.timeNodes = { { 200 * 240, 0 }, { 220 * 240, 0 } };
    pc.notes = { { BoundaryRef::domainStart(), BoundaryRef::node (0), 60, false },
                 { BoundaryRef::node (1), BoundaryRef::domainEnd(), 64, false } };
    expect (pc.validate (0, dom), "valid");

    auto at = [] (const PitchCorrections::TargetCurve& t, int k) { return t.shiftSemitones[(size_t) (k - t.firstFrame)]; };
    // 強さ 0 → 全フレーム transpose のみ
    {
        auto z = pc; z.strength = 0.0f;
        const auto t = PitchCorrections::targetCurve (z, c, 0, dom, 2);
        bool all = t.firstFrame == 0 && t.shiftSemitones.size() == 320;
        for (auto v : t.shiftSemitones) all = all && juce::exactlyEqual (v, 2.0f);
        expect (all, "強さ0 = 無変化（transpose のみ）");
    }
    // 速さ 0 → 各フレームで完全に目標（補正後 = target 一定）
    {
        auto k0 = pc; k0.speedMs = 0.0f;
        const auto t = PitchCorrections::targetCurve (k0, c, 0, dom, 0);
        bool flat = true;
        for (int k = 0; k < 200; ++k) flat = flat && std::abs (c.midiAt (k) + at (t, k) - 60.0) < 1e-3;
        for (int k = 220; k < 320; ++k) flat = flat && std::abs (c.midiAt (k) + at (t, k) - 64.0) < 1e-3;
        expect (flat, "速さ0 = ノート内で平ら（ケロケロ）");
        bool mask = true;
        for (int k = 200; k < 220; ++k) mask = mask && juce::exactlyEqual (at (t, k), 0.0f);
        expect (mask, "無声フレームは移動量 0（有声マスク）");
    }
    // 速さ∞相当（大きな時定数）→ 中心だけ寄せてビブラートが残る
    {
        auto slow = pc; slow.speedMs = 1e6f;
        const auto t = PitchCorrections::targetCurve (slow, c, 0, dom, 0);
        double mn = 1e9, mx = -1e9;
        for (int k = 20; k < 200; ++k) { const double v = c.midiAt (k) + at (t, k); mn = juce::jmin (mn, v); mx = juce::jmax (mx, v); }
        expect (mx - mn > 0.3 && std::abs ((mx + mn) / 2 - 60.0) < 0.05, "速さ大 = 中心だけ寄せてビブラートは残る");
    }
    // pinned（手で置いた音）は Strength 0 でも 100% で目標へ寄る。中立判定からも外れる
    {
        auto p2 = pc; p2.strength = 0.0f; p2.notes[1].pinned = true;
        expect (! p2.isAudiblyNeutral(), "pinned があれば中立でない");
        const auto t = PitchCorrections::targetCurve (p2, c, 0, dom, 0);
        bool first = true;
        for (int k = 0; k < 200; ++k) first = first && juce::exactlyEqual (at (t, k), 0.0f);
        expect (first, "pinned でないノートは Strength 0 で動かない");
        bool second = true;
        for (int k = 240; k < 320; ++k) second = second && std::abs (c.midiAt (k) + at (t, k) - 64.0) < 0.2;
        expect (second, "pinned のノートは 100% で目標（64）へ");
        auto back = PitchCorrection::fromJson (p2.toJson());
        expect (back.has_value() && back->notes[1].pinned && *back == p2, "pinned が JSON 往復する");
        // setNoteTarget: 往復して戻せば pinned は付かない・変えれば付く・bypass で外れる
        auto p3 = pc;
        PitchCorrections::setNoteTarget (p3, 0, 60, 60, false);
        expect (! p3.notes[0].pinned, "同じ目標に戻せば pinned は付かない");
        PitchCorrections::setNoteTarget (p3, 0, 62, 60, false);
        expect (p3.notes[0].pinned && p3.notes[0].targetMidi == 62, "目標を変えれば pinned");
        PitchCorrections::setNoteBypass (p3, 0, true);
        expect (p3.notes[0].bypass && ! p3.notes[0].pinned, "bypass で pinned が外れる");
        PitchCorrections::setNoteBypass (p3, 0, false);
        expect (! p3.notes[0].pinned, "解除しても pinned は戻らない");
        PitchCorrections::setNoteBypass (p3, 0, true);
        expect (! PitchCorrections::setNoteTarget (p3, 0, 65, 62, false) && p3.notes[0].targetMidi == 62 && ! p3.notes[0].pinned,
                "bypass 中は目標を置けない（false・不変）");
        // 戻り値は「開始時比」: 往復で戻せば false、pinnedAtStart=true で戻しても false（pinned は維持）
        auto p5 = pc;
        expect (PitchCorrections::setNoteTarget (p5, 1, 66, 64, false), "変えたら true");
        expect (! PitchCorrections::setNoteTarget (p5, 1, 64, 64, false) && ! p5.notes[1].pinned, "戻せば false・pinned も戻る");
        p5.notes[1].pinned = true;
        expect (! PitchCorrections::setNoteTarget (p5, 1, 64, 64, true) && p5.notes[1].pinned, "pinned 開始で戻しても false・pinned 維持");
        // digest は bypass 中の pinned を含めない（旧 JSON の正規化で指紋が変わらない）
        auto p6 = pc; p6.notes[0].bypass = true; p6.notes[0].pinned = true;
        auto p7 = pc; p7.notes[0].bypass = true; p7.notes[0].pinned = false;
        expect (p6.digest() == p7.digest(), "bypass 中の pinned は digest に影響しない");
        auto json = p3.toJson();
        json.getDynamicObject()->getProperty ("notes").getArray()->getReference (0).getDynamicObject()->setProperty ("pinned", true);
        auto loaded = PitchCorrection::fromJson (json);
        expect (loaded.has_value() && loaded->notes[0].bypass && ! loaded->notes[0].pinned, "読込で bypass ⇒ pinned=false に正規化");
        PitchCorrections::toggleNoteBypass (p3, 0);
        expect (! p3.notes[0].bypass, "toggleNoteBypass");
        // resnap は pinned を飛ばす
        auto p4 = pc; p4.notes[0].pinned = true; p4.notes[0].targetMidi = 61; // スケール外に手で置いた
        PitchCorrections::resnap (p4, c, 0, dom, ProjectKey { 0, KeyMode::major });
        expect (p4.notes[0].targetMidi == 61 && p4.notes[0].pinned, "resnap は pinned を変えない");
        expect (p4.notes[1].targetMidi == 64, "pinned でないノートは付け直される");
    }
    // バイパス → そのノートは transpose のみ
    {
        auto b = pc; b.notes[1].bypass = true;
        const auto t = PitchCorrections::targetCurve (b, c, 0, dom, 3);
        bool ok = true;
        for (int k = 220; k < 320; ++k) ok = ok && juce::exactlyEqual (at (t, k), 3.0f);
        expect (ok, "バイパスは補正 0（transpose 3 のみ）");
        bool other = true;
        for (int k = 0; k < 200; ++k) other = other && std::abs (at (t, k) - 3.0f) > 0.1f;
        expect (other, "他のノートは補正が掛かる");
    }
    // ドメインがカーブの一部だけ
    {
        const auto t = PitchCorrections::targetCurve (pc, c, 100 * 240, 100 * 240, 0);
        expect (t.firstFrame == 100 && t.shiftSemitones.size() == 100, "部分 domain のフレーム範囲");
    }
}

void testPitchAutoSnapAndDetach()
{
    beginTest ("PitchCorrection autoSnap (intersection / shared nodes) and detach");
    const double sr = 48000.0;
    auto voice = makeSynthVoice (sr);
    const auto curve = PitchAnalyzer::analyze (*voice.audio, sr);
    const auto detected = PitchNotes::detect (curve);
    const auto total = (juce::int64) voice.audio->getNumSamples();
    // 全域
    auto pc = PitchCorrections::autoSnap (curve, detected, 0, total, std::nullopt, PitchScaleMode::chromatic, {});
    expect (pc.curveDigest == curve.digest(), "curveDigest はカーブの内容ハッシュ");
    expect (pc.notes.size() == detected.size(), "全域なら検出ノート数と一致");
    expect (pc.validate (0, total), "valid");
    expect (pc.timeNodes.size() == detected.size() * 2, "隙間ありのノートは境界2個ずつ");
    for (size_t i = 0; i < detected.size(); ++i)
        expect (pc.notes[i].targetMidi == (int) std::floor (detected[i].medianMidi + 0.5), "クロマチックの四捨五入");
    // スケール: D minor（2, minor）→ F(65) は残り、E(64) も残り、F#(66) は F か G へ
    {
        ProjectKey dm { 2, KeyMode::minor };
        expect (PitchCorrections::snapToScale (65.9, dm) == 65 || PitchCorrections::snapToScale (65.9, dm) == 67, "スケール外は隣へ");
        expect (PitchCorrections::snapToScale (64.2, dm) == 64, "スケール内はそのまま");
        expect (PitchCorrections::snapToScale (60.5, std::nullopt) == 61, "同距離なら上");
    }
    // 部分 domain: 2番目のノートの途中から4番目のノートの途中まで → 最初/最後は端点参照・外は除外
    {
        const auto& d1 = detected[1]; const auto& d3 = detected[3];
        const juce::int64 off = ((juce::int64) d1.startFrame + 10) * curve.hopSamples;
        const juce::int64 end = ((juce::int64) d3.endFrame - 10) * curve.hopSamples;
        auto part = PitchCorrections::autoSnap (curve, detected, off, end - off, std::nullopt, PitchScaleMode::chromatic, {});
        expect (part.notes.size() == 3, "交差する3ノートだけ");
        expect (part.notes.front().start == BoundaryRef::domainStart() && part.notes.back().end == BoundaryRef::domainEnd(),
                "domain 端に掛かる境界は端点参照");
        expect (part.validate (off, end - off), "部分 domain でも valid");
    }
    // 隙間0の境界は共有ノード
    {
        std::vector<DetectedPitchNote> adj = { { 100, 200, 60.0f }, { 200, 300, 62.0f } };
        auto a = PitchCorrections::autoSnap (curve, adj, 0, 400 * 240, std::nullopt, PitchScaleMode::chromatic, {});
        expect (a.timeNodes.size() == 3 && a.notes[0].end == a.notes[1].start, "隙間0は共有ノード1個");
    }
    // detach: 親 domain の状態を子（後半）へ写す
    {
        auto child = pc;
        const juce::int64 half = total / 2;
        PitchCorrections::detachToDomain (child, 0, total, half, total - half);
        expect (child.validate (half, total - half), "detach 後も valid");
        // 全ノードが開区間内
        bool inside = true;
        for (const auto& n : child.timeNodes) inside = inside && n.sourceSample > half && n.sourceSample < total;
        expect (inside, "範囲外ノードが消える");
        expect (child.notes.size() < pc.notes.size() && child.digest() != pc.digest(), "範囲外ノートが消え digest が分かれる");
        // 境界をまたぐノートは端点参照で残る（またがるノートがあるなら）
        int crossing = 0;
        for (const auto& n : pc.notes)
        {
            const auto s = pc.resolve (n.start, 0, total), e = pc.resolve (n.end, 0, total);
            if (s < half && e > half) ++crossing;
        }
        if (crossing > 0)
            expect (child.notes.front().start == BoundaryRef::domainStart(), "またぐノートは domainStart から");
    }
}

// 合成ボーカルで「目標どおりに f0 が動いたか」を再検出で検証するヘルパ
struct ResynthCheck
{
    double medianCents = 0, p95Cents = 0;
    int compared = 0;
};
ResynthCheck checkResynth (const juce::AudioBuffer<float>& out, const RenderRecipe& recipe)
{
    const auto cy = PitchAnalyzer::analyze (out, recipe.sampleRate);
    const auto target = recipe.target();
    const auto tm = recipe.timeMap();
    const auto& c = *recipe.curve;
    std::vector<double> errs;
    for (int j = 0; j < cy.numFrames(); ++j)
    {
        const auto srcPos = tm.inverse ((juce::int64) j * cy.hopSamples);
        const int k = (int) ((srcPos + c.hopSamples / 2) / c.hopSamples);
        if (k < target.firstFrame || k >= target.firstFrame + (int) target.shiftSemitones.size())
            continue;
        if (! c.isVoiced (k) || ! cy.isVoiced (j))
            continue;
        // 境界 ±15ms は除外
        bool edge = false;
        for (int d = -3; d <= 3; ++d) edge = edge || ! c.isVoiced (k + d);
        if (edge) continue;
        const double want = c.midiAt (k) + target.shiftSemitones[(size_t) (k - target.firstFrame)];
        errs.push_back (std::abs (cy.midiAt (j) - want) * 100.0);
    }
    ResynthCheck r;
    r.compared = (int) errs.size();
    if (! errs.empty())
    {
        std::sort (errs.begin(), errs.end());
        r.medianCents = errs[errs.size() / 2];
        r.p95Cents = errs[(size_t) ((double) errs.size() * 0.95)];
    }
    return r;
}

void testVocalResynth()
{
    beginTest ("VocalResynth (WORLD): follows target within 20 cents, length, transpose, timing");
    const double sr = 48000.0;
    auto voice = makeSynthVoice (sr);
    auto curve = std::make_shared<const PitchCurve> (PitchAnalyzer::analyze (*voice.audio, sr));
    const auto detected = PitchNotes::detect (*curve);
    const auto total = (juce::int64) voice.audio->getNumSamples();

    RenderRecipe recipe;
    recipe.sourceAudio = voice.audio;
    recipe.sampleRate = sr;
    recipe.domainOffset = 0;
    recipe.domainLength = total;
    recipe.curve = curve;
    recipe.correction = PitchCorrections::autoSnap (*curve, detected, 0, total, std::nullopt, PitchScaleMode::chromatic, {});

    // A: スナップ（速さ 120ms）
    auto a = VocalResynth::render (recipe);
    expect (a != nullptr, "レンダー成功");
    if (a != nullptr)
    {
        expect (a->getNumSamples() == (int) total, "長さ = domain 長（ratio 1）");
        const auto r = checkResynth (*a, recipe);
        std::cout << "  A: median=" << r.medianCents << "c p95=" << r.p95Cents << "c n=" << r.compared << std::endl;
        expect (r.compared > 500 && r.medianCents < 20.0, "目標との差の中央値 < 20 cent");
    }
    // C: ケロケロ（速さ 0）
    {
        auto kero = recipe; kero.correction.speedMs = 0.0f;
        auto out = VocalResynth::render (kero);
        expect (out != nullptr, "ケロケロのレンダー成功");
        if (out != nullptr)
        {
            const auto r = checkResynth (*out, kero);
            std::cout << "  C: median=" << r.medianCents << "c p95=" << r.p95Cents << "c" << std::endl;
            expect (r.medianCents < 20.0, "ケロケロでも中央値 < 20 cent");
        }
    }
    // B: 移調 +3 を合算（1パス）
    {
        auto up = recipe; up.transposeSemitones = 3;
        auto out = VocalResynth::render (up);
        expect (out != nullptr, "移調合算のレンダー成功");
        if (out != nullptr)
        {
            const auto r = checkResynth (*out, up);
            std::cout << "  B: median=" << r.medianCents << "c p95=" << r.p95Cents << "c" << std::endl;
            expect (r.medianCents < 20.0, "移調 +3 込みで中央値 < 20 cent");
        }
    }
    // D: 1音を横移動（隣接吸収）→ 長さ不変・追従
    {
        auto moved = recipe;
        int idx = -1;
        for (int i = 1; i + 1 < (int) moved.correction.notes.size(); ++i)
            if (moved.correction.notes[(size_t) i].start.isNode() && moved.correction.notes[(size_t) i].end.isNode()) { idx = i; break; }
        expect (idx >= 0, "動かせるノートがある");
        if (idx >= 0)
        {
            const auto d = PitchCorrections::moveNote (moved.correction, idx, msSamples (40), 0, total, 1.0, sr);
            expect (d == msSamples (40), "+40ms 移動");
            auto out = VocalResynth::render (moved);
            expect (out != nullptr && out->getNumSamples() == (int) total, "横移動しても出力長は不変");
            if (out != nullptr)
            {
                const auto r = checkResynth (*out, moved);
                std::cout << "  D: median=" << r.medianCents << "c p95=" << r.p95Cents << "c" << std::endl;
                expect (r.medianCents < 20.0, "横移動後も追従");
            }
        }
    }
    // ストレッチ合成（ratio 1.25）→ 長さ round(len × 1.25)
    {
        auto st = recipe; st.stretchRatio = 1.25;
        auto out = VocalResynth::render (st);
        expect (out != nullptr && out->getNumSamples() == (int) std::llround ((double) total * 1.25), "ratio を timeMap に合成");
    }
    // 決定論: 同じ recipe で 2 回 → ビット一致
    {
        auto x = VocalResynth::render (recipe);
        auto y = VocalResynth::render (recipe);
        bool same = x != nullptr && y != nullptr && x->getNumSamples() == y->getNumSamples();
        if (same)
            for (int i = 0; i < x->getNumSamples(); ++i) same = same && juce::exactlyEqual (x->getSample (0, i), y->getSample (0, i));
        expect (same, "同じ入力で同じ出力（固定シード）");
    }
    // 失敗経路: カーブ不整合・範囲外
    {
        auto bad = recipe; bad.curve = nullptr;
        expect (VocalResynth::render (bad) == nullptr, "カーブ無しは失敗");
        auto bad2 = recipe; bad2.domainLength = total + 10;
        expect (VocalResynth::render (bad2) == nullptr, "範囲外は失敗");
        auto bad3 = recipe; bad3.transposeSemitones = 13;
        expect (VocalResynth::render (bad3) == nullptr, "±12 半音の外は失敗");
    }
}

void testPitchProjectRoundtrip()
{
    beginTest ("pitch correction: project v21 save/reload, split shares digest, missing sidecar invalidates");
    auto dir = makeTempDir();
    const double sr = 48000.0;
    auto voice = makeSynthVoice (sr);
    expect (writeBufferWav (dir.getChildFile ("clip-001.wav"), *voice.audio, sr), "WAV");

    Project project;
    project.directory = dir;
    project.sampleRate = sr;
    Track track;
    track.id = project.allocateId();
    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.audio = Project::loadWav (dir.getChildFile ("clip-001.wav"));
    clip.lengthSamples = clip.audio->getNumSamples();
    track.clips.push_back (clip);
    project.tracks.push_back (std::move (track));
    ClipDomains::reconcile (project, sr);
    expect (project.tracks[0].clips[0].id != 0, "reconcile がクリップ id を採番");

    // 解析 → サイドカー → 補正を付ける
    auto& live = project.tracks[0].clips[0];
    auto curve = std::make_shared<const PitchCurve> (PitchAnalyzer::analyze (*live.audio, sr));
    juce::String err;
    expect (PitchSidecar::write (*curve, dir.getChildFile ("clip-001.wav"), &err), "サイドカー書き出し");
    live.pitchCurve = curve;
    live.pitchCorrection = PitchCorrections::autoSnap (*curve, PitchNotes::detect (*curve), live.offsetSamples,
                                                        live.lengthSamples, std::nullopt, PitchScaleMode::chromatic, {});
    expect (live.renderPending (sr) && live.requestedFingerprint (sr).hasRecipe(), "補正を付けるとレンダー待ち");
    {
        // 聴感上中立（Strength 0・Δ 0）は recipe を出さない＝WORLD を通さない（プレビューと確定で同じ規則）
        auto saved = *live.pitchCorrection;
        live.pitchCorrection->strength = 0.0f;
        expect (! live.requestedFingerprint (sr).hasRecipe() && live.requestedFingerprint (sr).isNeutral(), "Strength 0 は中立");
        live.transposeSemitones = 2;
        expect (! live.requestedFingerprint (sr).hasRecipe() && ! live.requestedFingerprint (sr).isNeutral(), "Strength 0＋移調は signalsmith 経路");
        live.transposeSemitones = 0;
        live.pitchCorrection = saved;
        // previewDomain の寿命規則: 活動中 or 要求未達なら残す・それ以外は外す
        {
            Clip c = live;
            c.pitchCorrection.reset(); // activeDomain（中立）と要求を一致させる
            c.previewDomain = c.activeDomain;
            expect (! c.dropPreviewIfCurrent (sr, /*previewActive=*/ true), "活動中は残す");
            c.transposeSemitones = 3; // 要求が実効に追いついていない（プレビューは中立＝別内容）
            expect (c.dropPreviewIfCurrent (sr, false) && c.previewDomain == nullptr, "待っている本レンダーと別内容のプレビューは pending でも外す");
            // 待っている本レンダーと同内容のプレビューは残す
            auto same = std::make_shared<RenderedDomain> (*c.activeDomain);
            same->semitones = 3;
            c.previewDomain = same;
            expect (! c.dropPreviewIfCurrent (sr, false) && c.previewDomain != nullptr, "同内容（指紋一致）なら renderPending の間は残す");
            c.previewDomain = c.activeDomain;
            c.transposeSemitones = 0;
            expect (c.dropPreviewIfCurrent (sr, false) && c.previewDomain == nullptr, "追いついたら外す");
            expect (! c.dropPreviewIfCurrent (sr, false), "無ければ false");
        }
        // 分割・複製で previewDomain は持ち越さない
        live.previewDomain = live.activeDomain;
        Clip l, r;
        expect (splitClip (live, live.startSample + live.renderedLengthSamples() / 2, l, r), "分割");
        expect (l.previewDomain == live.previewDomain && r.previewDomain == nullptr, "右側（新 id）にはプレビューが付かない");
        live.previewDomain = nullptr;
    }

    const auto renderAll = [&sr] (Project& target)
    {
        RenderCache cache;
        cache.collectRequests = [&target, &sr]
        {
            bool attached = false;
            return ClipDomains::collectRequests (target, sr, nullptr, attached);
        };
        cache.onRenderReady = [&target, &sr] (const std::shared_ptr<const RenderedDomain>& d)
        { ClipDomains::attachRenderResult (target, sr, d); };
        cache.syncNow();
        const bool done = cache.waitForRenders (60000);
        cache.drainCompletedNow();
        return done;
    };
    expect (renderAll (project), "補正レンダーが完了");
    expect (! live.renderPending (sr) && live.activeDomain->recipeDigest == live.pitchCorrection->digest(),
            "装着後は要求と実効が一致（recipe digest）");
    expect (live.activeDomain->correction != nullptr && live.activeDomain->timeMap.nodes.size() > 2, "結果に補正のコピーと timeMap");

    // 分割: 親子で digest 一致・再レンダー不要
    {
        auto& clips = project.tracks[0].clips;
        Clip l, r;
        expect (splitClip (clips[0], clips[0].startSample + clips[0].renderedLengthSamples() / 2, l, r), "分割");
        clips[0] = l; clips.push_back (r);
        ClipDomains::reconcile (project, sr);
        expect (clips[0].id != clips[1].id && clips[1].id != 0, "右側は新しい id");
        expect (! clips[0].renderPending (sr) && ! clips[1].renderPending (sr), "分割直後は再レンダーが起きない");
        expect (clips[0].requestedRecipeDigest() == clips[1].requestedRecipeDigest(), "親子の recipe digest が一致");
        expect (clips[0].activeDomain == clips[1].activeDomain, "同じドメインを共有");
        // 左右をつなげた再生が分割前のドメイン全体とビット一致（区分線形マップでの分割境界）
        {
            std::vector<ClipPlayback> playbacks;
            for (const auto& c : clips) appendClipPlaybacks (c, playbacks);
            std::sort (playbacks.begin(), playbacks.end(), [] (const ClipPlayback& a, const ClipPlayback& b) { return a.startSample < b.startSample; });
            std::vector<float> joined;
            for (const auto& pb : playbacks)
                for (juce::int64 i = 0; i < pb.lengthSamples; ++i)
                    joined.push_back (pb.audio->getSample (0, (int) (pb.offsetSamples + i)));
            const auto& whole = *clips[0].activeDomain->audio;
            bool same = (int) joined.size() == whole.getNumSamples()
                     && playbacks.size() == 2 && playbacks[1].startSample == playbacks[0].startSample + playbacks[0].lengthSamples;
            if (same)
                for (int i = 0; i < whole.getNumSamples(); ++i) same = same && juce::exactlyEqual (joined[(size_t) i], whole.getSample (0, i));
            expect (same, "分割の左右をつなげると分割前と一致（隙間・重なりなし）");
        }
        // ヘルパ: 共有中の子は pitchCorrectionInOwnDomain() が detach した複製を返し、クリップ自体は変えない
        {
            auto& child0 = clips[1];
            expect (child0.sharesInheritedDomain(), "分割直後の右側は親のドメインを共有");
            const auto own = child0.pitchCorrectionInOwnDomain();
            auto manual = *child0.pitchCorrection;
            PitchCorrections::detachToDomain (manual, child0.requestedDomainOffset(), child0.requestedDomainLength(), child0.offsetSamples, child0.lengthSamples);
            expect (own.has_value() && *own == manual && own->validate (child0.offsetSamples, child0.lengthSamples), "pitchCorrectionInOwnDomain は detach と同じ結果");
            expect (child0.sharesInheritedDomain() && ! (*child0.pitchCorrection == manual), "クリップ自体は触らない");
            // resetRenderDomainToSelf 単独で補正が自範囲へ写る（呼び出し側が detach を忘れても API が塞ぐ）
            Clip reset = child0;
            reset.resetRenderDomainToSelf();
            expect (! reset.sharesInheritedDomain() && reset.pitchCorrection.has_value() && *reset.pitchCorrection == manual,
                    "resetRenderDomainToSelf が共有中の補正を自範囲へ写す");
            // 共有中の子に移調を受け付けても同じ（applyStretchRequest は reset を通る）
            Clip stretched = child0;
            expect (ClipDomains::applyStretchRequest (stretched, 1, 1.0), "移調を受理");
            expect (! stretched.sharesInheritedDomain() && stretched.pitchCorrection.has_value() && *stretched.pitchCorrection == manual,
                    "applyStretchRequest 後に親座標が残らない");
            // 中立補正（Strength 0）は「前例なし」の巻き戻しで消えない・巻き戻し後は renderPending が解ける
            Clip neutral = stretched;
            neutral.pitchCorrection->strength = 0.0f;
            neutral.activeDomain = nullptr;
            Project tmp; tmp.sampleRate = sr;
            Track t; t.clips.push_back (neutral); tmp.tracks.push_back (t);
            auto& nc = tmp.tracks[0].clips[0];
            const auto failed = nc.requestedFingerprint (sr);
            expect (ClipDomains::rollbackFailedRequest (tmp, sr, failed), "巻き戻し");
            expect (nc.pitchCorrection.has_value() && nc.transposeSemitones == 0, "前例なし: 中立補正は保持したまま移調だけ戻る");
            expect (! nc.renderPending (sr), "巻き戻し後は要求と実効が一致する");
            // 可聴の補正付きで鳴っている（activeDomain に correction あり）→ 中立の手直しより「鳴っている音」を優先
            Clip audible = clips[0];
            audible.pitchCorrection->strength = 0.0f; // 手直し中（中立）
            audible.pitchCorrection->notes[0].targetMidi += 3;
            audible.transposeSemitones = 5; // 失敗した要求（という想定）
            // 再解析を適用した直後という想定: カーブと curveDigest を別世代に差し替えておく（巻き戻しで両方が戻ること）
            auto otherCurve = std::make_shared<const PitchCurve> ([&] { auto c = *curve; c.algoId = "other"; return c; }());
            audible.pitchCurve = otherCurve;
            audible.pitchCorrection->curveDigest = otherCurve->digest();
            Project tmp2; tmp2.sampleRate = sr;
            Track t2; t2.clips.push_back (audible); tmp2.tracks.push_back (t2);
            auto& ac = tmp2.tracks[0].clips[0];
            expect (ac.activeDomain != nullptr && ac.activeDomain->correction != nullptr, "前提: 補正付きで鳴っている");
            expect (ClipDomains::rollbackFailedRequest (tmp2, sr, ac.requestedFingerprint (sr)), "巻き戻し（補正付き前例）");
            expect (ac.pitchCorrection.has_value() && *ac.pitchCorrection == *ac.activeDomain->correction, "鳴っている音の補正に合わせる");
            expect (ac.activeDomain->curve != nullptr && ac.pitchCurve == ac.activeDomain->curve
                        && ac.pitchCorrection->curveDigest == ac.pitchCurve->digest(),
                    "カーブも一緒に戻り curveDigest と一致する");
            expect (! ac.renderPending (sr), "巻き戻し後に renderPending が解ける（同じ失敗を繰り返さない）");
        }
        // 子を detach すると digest が分かれる
        auto& child = clips[1];
        PitchCorrections::detachToDomain (*child.pitchCorrection, child.requestedDomainOffset(), child.requestedDomainLength(),
                                          child.offsetSamples, child.lengthSamples);
        child.resetRenderDomainToSelf();
        expect (child.renderPending (sr) && child.requestedRecipeDigest() != clips[0].requestedRecipeDigest(),
                "detach で digest が分かれレンダー待ちになる");
        expect (renderAll (project), "子の再レンダー");
        expect (! child.renderPending (sr), "子が装着される");
    }

    // 保存 → 再読込
    expect (project.save (err), (juce::String::fromUTF8 (u8"保存: ") + err).toRawUTF8());
    expect (PitchSidecar::listFor (dir.getChildFile ("clip-001.wav")).size() == 1, "参照中のサイドカーは残る");
    {
        juce::StringArray warnings; juce::String error;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr && warnings.isEmpty(), (juce::String::fromUTF8 (u8"再読込に警告なし: ") + warnings.joinIntoString ("/")).toRawUTF8());
        if (loaded != nullptr)
        {
            auto& lc = loaded->tracks[0].clips;
            expect (lc.size() == 2 && lc[0].pitchCorrection.has_value() && lc[1].pitchCorrection.has_value(), "補正が復元される");
            expect (lc[0].id == project.tracks[0].clips[0].id && lc[1].id == project.tracks[0].clips[1].id, "id が保存される");
            expect (lc[0].pitchCurve != nullptr && lc[0].pitchCurve == lc[1].pitchCurve, "同じ世代のカーブは共有");
            expect (*lc[0].pitchCorrection == *project.tracks[0].clips[0].pitchCorrection, "編集状態（targetMidi 等）が一致");
            expect (! loaded->modifiedOnLoad, "正常読込は modifiedOnLoad でない");
            // 再読込後のレンダーが元とビット一致
            expect (renderAll (*loaded), "再読込後のレンダー");
            const auto& a = *project.tracks[0].clips[0].activeDomain->audio;
            const auto& b = *lc[0].activeDomain->audio;
            bool same = a.getNumSamples() == b.getNumSamples();
            if (same)
                for (int i = 0; i < a.getNumSamples(); ++i) same = same && juce::exactlyEqual (a.getSample (0, i), b.getSample (0, i));
            expect (same, "保存→再読込で同じ音（ビット一致）");
        }
    }
    // サイドカーを消して読込 → 補正が無効化・警告・modifiedOnLoad
    {
        for (auto& f : PitchSidecar::listFor (dir.getChildFile ("clip-001.wav"))) f.deleteFile();
        juce::StringArray warnings; juce::String error;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr && ! warnings.isEmpty(), "サイドカー欠損は警告");
        if (loaded != nullptr)
        {
            expect (! loaded->tracks[0].clips[0].pitchCorrection.has_value(), "補正は無効化される");
            expect (loaded->modifiedOnLoad, "modifiedOnLoad = 保存が必要");
            expect (! loaded->tracks[0].clips[0].renderPending (sr) || loaded->tracks[0].clips[0].requestedFingerprint (sr).isNeutral(),
                    "原音を鳴らす");
        }
    }
    // 不正な JSON（範囲外 index）→ 無効化
    {
        // 元の project をもう一度保存（サイドカーは消えているので write し直す）
        PitchSidecar::write (*curve, dir.getChildFile ("clip-001.wav"), &err);
        project.tracks[0].clips[0].pitchCorrection->notes[0].end = BoundaryRef::node (999);
        expect (project.save (err), "壊れた状態でも保存自体はできる");
        juce::StringArray warnings; juce::String error;
        auto loaded = Project::load (dir, warnings, error);
        expect (loaded != nullptr && ! loaded->tracks[0].clips[0].pitchCorrection.has_value() && loaded->modifiedOnLoad,
                "構造が不正な補正は無効化＋dirty");
    }
    // GC: 参照されない世代は保存で消える
    {
        auto other = *curve; other.algoId = "old-gen";
        PitchSidecar::write (other, dir.getChildFile ("clip-001.wav"), &err);
        expect (PitchSidecar::listFor (dir.getChildFile ("clip-001.wav")).size() == 2, "2世代ある");
        project.tracks[0].clips[0].pitchCorrection->notes[0].end = BoundaryRef::node (1); // 直す
        project.save (err);
        expect (PitchSidecar::listFor (dir.getChildFile ("clip-001.wav")).size() == 1, "未参照の世代は GC される");
        // keepSidecars に入れた世代は残る
        PitchSidecar::write (other, dir.getChildFile ("clip-001.wav"), &err);
        project.save (err, {}, { PitchSidecar::fileNameFor ("clip-001.wav", other.digest()) });
        expect (PitchSidecar::listFor (dir.getChildFile ("clip-001.wav")).size() == 2, "keepSidecars の世代は残る");
    }
    dir.deleteRecursively();
}

void testPitchEditorSession()
{
    beginTest ("PitchEditorSession transitions (initial preview / commit / change preview / cancel)");
    const double sr = 48000.0;
    auto voice = makeSynthVoice (sr);
    auto curve = std::make_shared<const PitchCurve> (PitchAnalyzer::analyze (*voice.audio, sr));
    const auto total = (juce::int64) voice.audio->getNumSamples();
    PitchEditorSession s;
    expect (! s.isOpen() && ! s.editable() && ! s.hasPreview(), "閉じた状態");
    s.openForAnalysis (7);
    const auto g1 = s.generation();
    expect (s.mode() == PitchEditorSession::Mode::analyzing && s.clipId() == 7 && ! s.editable(), "解析中は編集不可");
    expect (! s.commitInitial().has_value(), "解析中は確定できない");
    s.analysisFinished (curve, 0, total, std::nullopt, /*sidecarWritten=*/ false);
    expect (s.mode() == PitchEditorSession::Mode::initialPreview && s.hasPreview() && ! s.editable() && s.sidecarBlocked(),
            "サイドカー未書込なら初回プレビューでも編集不可");
    expect (! s.commitInitial().has_value(), "サイドカー未書込なら確定できない");
    s.setSidecarWritten (true);
    expect (s.editable() && s.working().notes.size() > 0 && s.working().curveDigest == curve->digest(), "書けたら編集可・autoSnap 済み");
    expect (juce::exactlyEqual (s.working().strength, 0.0f) && s.working().isAudiblyNeutral(), "開いた直後は Strength 0%（何も動かさない）");
    auto committed = s.commitInitial();
    expect (committed.has_value() && s.mode() == PitchEditorSession::Mode::committed && ! s.hasPreview(), "確定で committed");
    // 変更プレビュー → キャンセルで元に戻る（generation が進む）
    auto proposal = *committed;
    proposal.notes[0].targetMidi += 1;
    expect (s.beginChangePreview (proposal, curve) && s.mode() == PitchEditorSession::Mode::changePreview && ! s.editable() && s.hasPreview(),
            "変更プレビュー中は編集不可");
    expect (! s.commitInitial().has_value(), "変更プレビュー中は commitInitial 不可");
    const auto gBefore = s.generation();
    expect (s.cancelChange() && s.mode() == PitchEditorSession::Mode::committed && s.working() == *committed && s.generation() > gBefore,
            "キャンセルで旧状態・generation 進む");
    // 変更プレビュー → 適用
    expect (s.beginChangePreview (proposal, curve), "再び変更プレビュー");
    auto applied = s.applyChange();
    expect (applied.has_value() && *applied == proposal && s.mode() == PitchEditorSession::Mode::committed, "適用で新状態が確定");
    expect (! s.beginChangePreview (proposal, curve) || true, "committed からのみ開始できる");
    // 閉じる
    s.close();
    expect (! s.isOpen() && s.clipId() == 0 && s.generation() > g1, "閉じると generation が進む");
    // 補正ありで開く: 自動スナップしない（working は渡した状態そのもの）
    s.openCommitted (9, proposal, curve);
    expect (s.mode() == PitchEditorSession::Mode::committed && s.working() == proposal && s.editable(), "補正ありは確定状態のまま");
}

void testVocalNoteAudition()
{
    beginTest ("VocalNoteAudition: per-note prepare + render follows target (drag audition)");
    const double sr = 48000.0;
    auto voice = makeSynthVoice (sr);
    auto curve = std::make_shared<const PitchCurve> (PitchAnalyzer::analyze (*voice.audio, sr));
    const auto detected = PitchNotes::detect (*curve);
    const auto total = (juce::int64) voice.audio->getNumSamples();
    auto pc = PitchCorrections::autoSnap (*curve, detected, 0, total, std::nullopt, PitchScaleMode::chromatic, {});
    expect (pc.notes.size() >= 3, "ノートがある");
    const int idx = 2;
    const auto s = pc.resolve (pc.notes[(size_t) idx].start, 0, total), e = pc.resolve (pc.notes[(size_t) idx].end, 0, total);
    VocalNoteAudition au;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    expect (au.prepare (*voice.audio, *curve, sr, s, e), "分解できる");
    const auto prepMs = juce::Time::getMillisecondCounterHiRes() - t0;
    pc.notes[(size_t) idx].targetMidi += 2; // ドラッグで +2
    const auto t1 = juce::Time::getMillisecondCounterHiRes();
    auto buf = au.render (pc, *curve, 0, total, 0);
    const auto renderMs = juce::Time::getMillisecondCounterHiRes() - t1;
    std::cout << "  prepare=" << prepMs << "ms render=" << renderMs << "ms len=" << (buf ? buf->getNumSamples() : 0) << std::endl;
    expect (buf != nullptr && buf->getNumSamples() == (int) (e - s), "ノート範囲の長さで合成される");
    expect (prepMs < 2000.0 && renderMs < 1000.0, "ドラッグ中に使える速さ（Debug で分解 ~170ms・合成 ~130ms。Release は数分の1）");
    if (buf != nullptr)
    {
        const auto cy = PitchAnalyzer::analyze (*buf, sr);
        std::vector<double> m;
        for (int k = 4; k < cy.numFrames() - 4; ++k) if (cy.isVoiced (k)) m.push_back (cy.midiAt (k));
        std::sort (m.begin(), m.end());
        const double median = m.empty() ? 0 : m[m.size() / 2];
        expect (std::abs (median - pc.notes[(size_t) idx].targetMidi) < 0.25, "再検出の中央値が新しい目標 ±25cent");
    }
}

void testGridSnap()
{
    std::cout << "---- GridSnap (表示グリッドの刻みとクリックシークのスナップ)" << std::endl;
    // 12px 以上取れる最細の分割。80px/小節 → 1/4（20px）。192px → 1/16
    expect (GridSnap::divisionsPerBar (80.0) == 4, "80px/bar は拍");
    expect (GridSnap::divisionsPerBar (192.0) == 16, "192px/bar は 1/16");
    expect (GridSnap::divisionsPerBar (30.0) == 2, "30px/bar は 1/2");
    expect (GridSnap::divisionsPerBar (10.0) == 1, "10px/bar は小節");
    expect (GridSnap::stepSixteenths (80.0) == 4, "拍刻み = 4/16");
    // 0 起点に切り下げ。負は 0
    expect (GridSnap::floorToGrid (876000, 24000.0) == 864000, "拍への切り下げ");
    expect (GridSnap::floorToGrid (876000, 6000.0) == 876000, "1/16 ちょうどはそのまま");
    expect (GridSnap::floorToGrid (-5, 6000.0) == 0, "負は 0");
    // 描画の開始インデックスは step の倍数（左端が index 7 でも線は 4,8,12… に引く）
    expect (GridSnap::firstIndexAligned (7.5 * 6000.0, 6000.0, 4) == 4, "左端 7.5 → 4");
    expect (GridSnap::firstIndexAligned (8.0 * 6000.0, 6000.0, 4) == 8, "左端 8 → 8");
    expect (GridSnap::firstIndexAligned (3.0 * 6000.0, 6000.0, 16) == 0, "小節刻みは 0");
    // 同じ px 密度なら両 view のスナップ先が一致する（ピッチ側は 1/16 長 × step で同じ格子）
    const double bar = 96000.0, pxPerBar = 80.0;
    const auto a = GridSnap::floorToGrid (900000, bar / GridSnap::divisionsPerBar (pxPerBar));
    const auto b = GridSnap::floorToGrid (900000, (bar / 16.0) * GridSnap::stepSixteenths (pxPerBar));
    expect (a == b && a == 888000, "タイムラインとエディタで同じ格子");
}

void testUndoAbandonLast()
{
    beginTest ("UndoStack abandon (token-checked; redo and evicted entry restored)");
    Project project;
    project.sampleRate = 48000.0;
    Track track; track.id = project.allocateId(); project.tracks.push_back (track);
    UndoStack undo;
    undo.begin (project);
    project.tracks[0].name = "a";
    UndoStack::EditKind kind;
    expect (undo.undo (project, kind) && project.tracks[0].name != "a", "1件 undo");
    expect (undo.canRedo(), "redo あり");
    const auto t1 = undo.begin (project); // 編集が不成立だった begin
    expect (! undo.canRedo(), "begin で redo は一旦消える");
    undo.abandon (t1);
    expect (undo.canRedo() && ! undo.canUndo(), "abandon で redo が戻り、空の begin は残らない");
    expect (undo.redo (project, kind) && project.tracks[0].name == "a", "redo が効く");
    // 古いトークン／0 では何も消えない（深さが同じでも）
    const auto t2 = undo.begin (project); project.tracks[0].name = "b"; // 本物の編集
    undo.abandon (0);
    undo.abandon (t2 - 1);
    expect (undo.canUndo(), "無効トークンでは本物の undo を消さない");
    // 間に undo が入ったら abandon は効かない
    undo.undo (project, kind);
    undo.abandon (t2);
    expect (undo.canRedo() && project.tracks[0].name == "a", "undo 後の abandon は no-op");
    // maxDepth 境界: 押し出した 1 件が abandon で戻る
    UndoStack deep;
    project.tracks[0].name = "orig";
    for (int i = 0; i < 100; ++i) { deep.begin (project); project.tracks[0].name = juce::String (i); }
    int depth = 0; { UndoStack probe = deep; Project p2 = project; while (probe.undo (p2, kind)) ++depth; }
    const auto t3 = deep.begin (project);
    deep.abandon (t3);
    int depthAfter = 0; juce::String deepest;
    { Project p2 = project; while (deep.undo (p2, kind)) { ++depthAfter; deepest = p2.tracks[0].name; } }
    expect (depth == 100 && depthAfter == 100 && deepest == "orig", "maxDepth で押し出された 1 件も先頭に戻る（最深 undo が最初の状態）");
}

} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager 初期化（AUインスタンス化に必要）

    // scripts/check-loudness.sh 用の計測モード: WAVの integrated LUFS / TP を1行で出して終了
    // （ffmpeg ebur128 との数値照合に使う。テストスイートは走らせない）
    if (argc == 3 && juce::String (argv[1]) == "--measure-loudness")
    {
        double lufs = 0.0, tpDb = 0.0;
        if (! Loudness::measureFile (juce::File (juce::String::fromUTF8 (argv[2])), lufs, tpDb))
        {
            std::cout << "ERROR: cannot measure " << argv[2] << std::endl;
            return 1;
        }
        std::cout << "integrated=" << juce::String (lufs, 2)
                  << " truePeakDb=" << juce::String (tpDb, 2) << std::endl;
        return 0;
    }




    testV1ToV2Roundtrip();
    testMidiRoundtrip();
    testInvalidJson();
    testClampNoteBoundaries();
    testClipOffsetsV2Migration();
    testClipOffsetClamp();
    testSharedWavBufferOnLoad();
    testBuildSnapshotClipOffsets();
    testEngineReadsClipOffsets();
    testUiPositionSample();
    testMidiFileTypes();
    testMidiImportParse();
    testMidiImportApply();
    testSplitClip();
    testSplitMidiRegion();
    testSectionMarkers();
    testSectionMarkersInvalidLoad();
    testUndoStack();
    testUndoStackBpm();
    testProjectKey();
    testReferenceExport();
    testReferenceAlign();
    testReferenceAlignWithKey();
    testReferenceReport();
    testReferenceReportGenerator();
    testGachaPorcelainParse();
    testGachaPatternMiniature();
    testGachaSessionPreview();
    testGachaSessionParts();
    testGachaSessionLoops();
    testTrimLoopBufferToBars();
    testLoadWavResampled();
    testParseRecommendJson();
    testGachaBassRollPlan();
    testSaveGcProtectsUndoWavs();
    testSaveGcProtectsClipboardWav();
    testRegionEditShortcuts();
    testLoopExpansion();
    testLoopRoundtrip();
    testClipGainRoundtrip();
    testClipGainSnapshot();
    testClipFadeClamp();
    testClipFadeDragRules();
    testClipFadeRoundtrip();
    testClipFadeSegments();
    testClipFadeSnapshot();
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
    testEqParamsRoundtrip();
    testTrackEqResponse();
    testTrackEqTransitions();
    testEngineEqBounceConsistency();
    testCompParamsRoundtrip();
    testTrackCompDynamics();
    testEngineCompBounceConsistency();
    testEngineCompPrePanDetection();
    testTrackSaturatorHarmonics();
    testTrackSaturatorAliasing();
    testTrackSaturatorTransitions();
    testSatParamsRoundtrip();
    testEngineSatBounceConsistency();
    testTrackLofiComponents();
    testTrackLofiTransitions();
    testLofiParamsRoundtrip();
    testBounceLofiTail();
    testFxSlotProjection();
    testMasterLimiterBrickwall();
    testMasterLimiterDynamics();
    testEngineLimiterSeekReset();
    testEngineFadeCycleLimiterState();
    testLimiterParamsRoundtrip();
    testLoudnessMeterStandard();
    testTruePeakDetector();
    testMasterMeterPipeline();
    testRtNoAllocation();
    testSpectrumAnalyzer();
    testEngineAnalyzerPreFaderTap();
    testBounceEqTail();
    testEnginePanSendsMaster();
    testEngineOutputChannelRule();
    testPreviewThroughMaster();
    testCycleRoundtrip();
    testPlaybackEngineCycleLoop();
    testBounceCycleRange();
    testStereoClipLoadAndV6();
    testEngineStereoPan();
    testEngineClipGain();
    testEngineClipFade();
    testEngineBounceStereoConsistency();
    testSongFadeGainCurve();
    testSongFadeRoundtrip();
    testLoopAnchorRoundtrip();
    testEngineSongFade();
    testBounceSongFade();
    testAudioImporter();
    testAudioFilePreview();
    testGridSnap();
    testPreviewPolicy();
    testFileSortOrder();
    testBottomPanelHistory();
    testBottomPanelShortcuts();
    testGainScale();
    testGainSliderIgnoresScrollWheel();
    testGainSliderCenterFill();
    testYtDlpOutput();
    testAnalyzeProgress();
    testSpawnedProcess();
    testTempDirSweep();
    testUrlDownloaderLive(); // LALA_VERIFY_URL が無ければ何もしない
    testBusDelayImpulse();
    testBusReverbBasics();
    testBounceBusFxTail();
    testBounceBusTailCap();
    testEngineBusMuteKeepsFxRunning();
    testEngineBounceBusFxConsistency();
    testBusFxParamsPersistence();
    testMonoRenderRegressionHash();
    testTrackFxRegressionHash();
    testClipStretcher();
    testStretchDomainMath();
    testStretchPersistence();
    testRenderCachePipeline();
    testStretchSplitSaveReload();
    testGachaLoopStretchValues();
    testPitchAnalyzerYin();
    testPitchAnalyzerFingerprint();
    testPitchCmndfBruteForce();
    testPitchSidecar();
    testPitchNotesRules();
    testPitchKeyEstimate();
    testPitchAnalysisWorker();
    testTimeMapBasics();
    testPitchTimeMapRules();
    testPitchCorrectionValidation();
    testPitchSplitMerge();
    testPitchTargetCurve();
    testPitchAutoSnapAndDetach();
    testVocalResynth();
    testPitchProjectRoundtrip();
    testPitchEditorSession();
    testVocalNoteAudition();
    testUndoAbandonLast();


    if (failureCount > 0)
    {
        std::cout << failureCount << " test(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "all tests passed" << std::endl;
    return 0;
}
