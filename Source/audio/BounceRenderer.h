#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../shared/PlaybackSnapshot.h"

struct Track; // shared/Project.h（buildItemRenderの実装側でinclude）

// オフラインバウンス（書き出し）。オーディオデバイス・リアルタイムスレッドとは無関係で、
// 専用のバックグラウンドスレッド（juce::Thread）がレンダリングとファイルIOの全てを行う。
//
// スレッド境界の前提:
// - start() / cancel() / cancelAndWait() / status() / progress() / takeResult() はメッセージスレッド専用
// - Request の中身は start() 以降ワーカーが専有する。SynthInstance はリアルタイム再生用と
//   共有してはならない（processBlock が並走する）。必ず SynthBank::createIndependent() で
//   作った専用インスタンスを渡すこと
// - mute/solo/gain は開始時に確定した値を Request に焼き込む（共有atomicの TrackParams は参照しない）
// - 完了通知は push 型でなく pull 型: 呼び出し側（MainComponentのTimer）が status() をポーリングする
//
// 書き出しは2パス（メモリに全曲を蓄積しない）:
//   パス1: ブロックごとにミックスして一時WAV（32bit float）へストリーム書き出し＋ピーク計測
//   パス2: ピーク>1.0なら全体をスケールしつつ 24bit WAV の一時ファイルへ変換
//   成功時のみ POSIX rename() で出力先を原子的に置換する（キャンセル・失敗時は既存ファイル不変）
class BounceRenderer : private juce::Thread
{
public:
    static constexpr int renderBlockSize = 1024; // 専用synthはこのブロックサイズで prepareToPlay しておくこと

    struct TrackRender
    {
        std::vector<ClipPlayback> clips;      // オーディオトラックのみ
        std::vector<MidiNotePlayback> notes;  // MIDIトラックのみ。startPpq昇順（buildSnapshotが保証）
        std::shared_ptr<SynthInstance> synth; // MIDIトラックのみ。バウンス専用インスタンス
        float gain = 1.0f;                    // 開始時に固定済み（非可聴トラックはRequestに入れない）
        float pan = 0.0f;                     // -1..+1（クリップは等パワー補正型・シンセはバランス型。RTと同じ法則）
        float sends[numSendBuses] { 0.0f, 0.0f, 0.0f }; // post-fader send量
    };

    struct Request
    {
        std::vector<TrackRender> tracks;
        double sampleRate = 0.0;
        double bpm = 120.0;
        juce::int64 startSample = 0; // レンダリング範囲。通常は 0〜曲末、サイクルON時はその範囲
        juce::int64 endSample = 0;
        bool wantTail = false;       // 可聴なMIDIトラックがあるときだけ余韻テールを付ける
        juce::File targetFile;

        // 固定バス・Master（開始時に固定済み。RTのprocessと同じく素通しバス→Masterゲインの順）
        float busGain[numSendBuses] { 1.0f, 1.0f, 1.0f };
        bool busMute[numSendBuses] { false, false, false };
        float masterGain = 1.0f;
    };

    // 選択された1アイテム（クリップ or MIDIリージョン）だけをレンダリング対象にした
    // TrackRender とレンダリング範囲を組み立てる（⌘Eのリージョン書き出し用。純粋なモデル→Request変換で
    // テスト対象）。トラックのmute/solo・アイテム自身のmutedは見ない（明示選択が優先）。
    // クリップ範囲はモデル上の区間（クランプで再生長が縮んだ分は末尾無音）、MIDIはリージョン境界を
    // PPQ→サンプル換算した厳密長。synthは呼び出し側が生成して埋める。
    // index範囲外・クリップの参照WAVなしは false（ノート空のMIDIリージョンは notes 空で true）
    static bool buildItemRender (const Track& track, int itemIndex, double bpm, double sampleRate,
                                 TrackRender& out, juce::int64& rangeStart, juce::int64& rangeEnd);

    enum class Status { idle, running, success, cancelled, failed };

    struct Result
    {
        Status status = Status::idle;
        juce::String errorMessage;      // failed のとき
        juce::int64 writtenSamples = 0; // 最終WAVの長さ（テール込み）
        float peak = 0.0f;              // スケール前のミックスピーク
        bool scaled = false;            // ピーク>1.0でスケールダウンしたか
    };

    BounceRenderer() : juce::Thread ("Bounce Renderer") {}

    ~BounceRenderer() override
    {
        cancelAndWait();
    }

    // 実行中なら false。開始後、Request の所有はワーカーに移る
    bool start (Request&& requestToRun)
    {
        if (isThreadRunning())
            return false;
        request = std::move (requestToRun);
        result = {};
        progressValue.store (0.0f);
        currentStatus.store (Status::running);
        startThread();
        return true;
    }

    // 非同期キャンセル要求（ワーカーは次のブロック境界で中断し、一時ファイルを削除して終了する）
    void cancel() { signalThreadShouldExit(); }

    // キャンセル要求＋ワーカー終了まで待つ（閉じる/終了フロー用。レンダリングは数秒想定）
    void cancelAndWait()
    {
        signalThreadShouldExit();
        stopThread (-1);
    }

    Status status() const { return currentStatus.load(); }
    float progress() const { return progressValue.load(); }

    // 終了後（status != running）に結果を取り出して idle に戻す
    Result takeResult()
    {
        jassert (! isThreadRunning());
        auto taken = std::move (result);
        result = {};
        currentStatus.store (Status::idle);
        return taken;
    }

private:
    void run() override;

    Status renderAndWrite();
    // パス1/パス2。false = 中断（キャンセル、またはrenderFailed=trueのときIO失敗）
    bool renderPass (juce::AudioFormatWriter& writer);
    bool convertPass (juce::AudioFormatWriter& writer);

    // MIDIトラックの直線走査状態（ワーカー専用）。RTエンジンと違いシーク・プレビュー・
    // resoundが存在しないので、次ノートのインデックスと発音中リストだけで足りる
    struct SynthCursor
    {
        size_t nextNote = 0;
        std::vector<std::pair<juce::int64, int>> active; // (endPpq, pitch)
    };

    void scheduleBlockMidi (const TrackRender& track, SynthCursor& cursor,
                            juce::int64 pos, int numSamples, double tps);
    void renderSynthInto (juce::AudioBuffer<float>& mix, std::vector<juce::AudioBuffer<float>>& busMix,
                          const TrackRender& track, int numSamples);

    // 素通しバス（busGain/busMute適用）をmixへ合流 → Masterゲイン（RTのprocessと同じ順序）
    void mixBusesAndMaster (juce::AudioBuffer<float>& mix,
                            std::vector<juce::AudioBuffer<float>>& busMix, int numSamples);

    Request request;
    Result result;
    std::atomic<Status> currentStatus { Status::idle };
    std::atomic<float> progressValue { 0.0f };

    juce::File tempFloatFile, tempFinalFile;
    juce::MidiBuffer midiScratch;
    juce::AudioBuffer<float> synthScratch;
    std::unique_ptr<juce::AudioFormatReader> convertReader; // パス2で一時float WAVを読み戻す
    std::vector<SynthCursor> cursors;
    float runningPeak = 0.0f;
    juce::int64 samplesWritten = 0;
    bool renderFailed = false; // パスがfalseを返した理由の区別（true=IO失敗 / false=キャンセル）

    JUCE_DECLARE_NON_COPYABLE (BounceRenderer)
};
