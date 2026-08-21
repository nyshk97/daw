#pragma once

#include <atomic>
#include <memory>
#include <juce_events/juce_events.h>

#include "../shared/PitchCurve.h"

// ピッチ解析のバックグラウンドワーカー（pull 型: juce::Thread ＋ atomic status。GOTCHAS「pull型ワーカー」の3点）。
// クリップの「ピッチ補正…」初回オープン時にソース WAV 単位で解析し、完了時にサイドカーを書く。
//
// 古い結果の着地規則（plan Phase 1）:
// - Request は原音の強参照・generation を持つ。takeResult() で返る Result にも同じ generation が入るので、
//   呼び出し側は「現在の generation と一致する結果だけ」を UI へ反映する
// - 対象切替・再解析・ウィンドウ破棄・プロジェクト切替では cancelAndWait() してから次を start する
// - サイドカー書き出しは完了時に WAV の存在と識別子（フレーム数・ch・SR）を再確認してから
//   （削除済み WAV の孤児 `.pitch` を書かない）。wavFile が空なら書かない（テスト・メモリ内解析）
class PitchAnalysisWorker : private juce::Thread
{
public:
    struct Request
    {
        std::shared_ptr<const juce::AudioBuffer<float>> source;
        double sampleRate = 0.0;
        juce::File wavFile;          // サイドカーの書き先（空 = 書かない）
        juce::uint64 generation = 0; // 呼び出し側が進める。結果の着地判定に使う
    };

    enum class Status { idle, running, success, cancelled, failed };

    struct Result
    {
        Status status = Status::idle;
        juce::uint64 generation = 0;
        PitchCurve curve;              // success のとき
        bool sidecarWritten = false;   // success かつ wavFile が有効で書けたとき
        juce::File sidecarFile;
        juce::String errorMessage;     // failed のとき／サイドカー書込失敗の理由（success でも入り得る）
    };

    PitchAnalysisWorker() : juce::Thread ("Pitch Analysis") {}
    ~PitchAnalysisWorker() override { cancelAndWait(); }

    // idle 以外（running・未回収の結果あり）なら false
    bool start (Request&& requestToRun);

    void cancel() { signalThreadShouldExit(); }
    void cancelAndWait()
    {
        signalThreadShouldExit();
        stopThread (-1);
    }

    Status status() const { return currentStatus.load(); }
    float progress() const { return progressValue.load(); }
    juce::uint64 runningGeneration() const { return currentGeneration.load(); }

    // 終了後（status != running）に結果を取り出して idle に戻す。スレッド終了を待ってから move する
    Result takeResult();

private:
    void run() override;

    Request request;
    Result result;
    std::atomic<Status> currentStatus { Status::idle };
    std::atomic<float> progressValue { 0.0f };
    std::atomic<juce::uint64> currentGeneration { 0 };

    JUCE_DECLARE_NON_COPYABLE (PitchAnalysisWorker)
};
