#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "../shared/SatParams.h"
#include "TrackFxBase.h"

// トラックサチュレーションのDSP本体（非対称ソフトクリップ・2ch）。
// plan: docs/plans/2026-08-16-2058-fx-batch3-saturation-lofi.md Phase 2
// 状態機械（serial・リセット分岐・chainMix・snapTo骨格）は TrackFxBase に集約済み。
// 伝達カーブ・不定積分・補償ゲインの数式は SatParams.h（UIと共有＝描画と音が同じ数式）。
//
// 信号の流れ（1サンプル・ch独立。パラメータはL/R共通なのでステレオリンクは自明）:
//   dry → [1次ADAA付き伝達カーブ] → [DC除去HPF] → ×補償ゲイン → Mix(dry/wet)
//   → chainMix(ON/OFFクロスフェード・FxBase)
//
// - 1次ADAA: wet = (F(x[n]) − F(x[n−1])) / (x[n] − x[n−1])。差分が微小なときは中点評価に
//   フォールバック（0除算と桁落ち防御）。エイリアスを追加バッファ・レイテンシなしで抑える
//   （wetの実効約0.5サンプル遅れによる処理/未処理トラック間の高域位相差は受容する）。
//   パラメータ平滑中は F・f とも現在サンプルの g/b で評価（曲線が動く微小誤差は許容）
// - DC除去: 非対称カーブはDCオフセットを出すため一次DCブロッカ（〜5Hz）を wet 側に常設
// - 補償ゲイン: SatParams::compensationGain（-18dBFS基準）。driveのターゲットが変わった
//   ブロック頭でのみ再計算し（64点求積を毎サンプル呼ばない）、SmoothedValue で適用
//
// 置き場所と寿命はTrackEqと同じ（RT = TrackParams::rtSat・バウンス = TrackRender が独立所有）。
// RT安全性: 全状態固定サイズ・確保なし
class TrackSaturator : public TrackFxBase<TrackSaturator>
{
public:
    // 高速パス（既存経路の完全素通し）でよいか。中立（drive==0 || mix==0）は素通し。
    // false のあいだは呼び出し側が active経路で process() を呼び続けること
    bool needsActivePath (bool satEnabled, const Sat::Values& targets) const
    {
        if (satEnabled && ! Sat::isNeutral (targets))
            return true;
        return ! settled;
    }

    // インプレース処理（left は必須・right は nullptr でモノ）。
    // serial / timelineJumped の意味は TrackEq::process と同じ（共通のFX連番を渡す）
    void process (float* left, float* right, int numSamples, double sampleRate,
                  juce::uint64 serial, bool timelineJumped,
                  bool satEnabled, const Sat::Values& targets);

    // 平滑を挟まず即座に目標状態にする（バウンスの開始前に1回呼ぶ。以降 process の serial は
    // 1 から連番で渡す）。RT側は使わない（再進入時は dry からのフェードインで繋ぐ）
    void snapTo (double sampleRate, bool satEnabled, const Sat::Values& targets);

private:
    friend class TrackFxBase<TrackSaturator>;

    // FxBaseフック
    void fxResetSmootherRates (double sampleRate);
    void fxSnapToTargets (const Sat::Values& targets);
    void fxResetHistory(); // ADAA前サンプル＋DCブロッカ履歴
    void fxSampleRateChanged();

    juce::SmoothedValue<float> drive, mix, compGain;
    // 中立フェード（0..1）: Drive目標が0のときwet経路（DCブロッカ残留を含む）をdryへ
    // フェードアウトする。これが無いと、settled→高速パス切替の瞬間に「DCブロッカを通った
    // 出力」から「raw入力」へ不連続に切り替わり、DC/低域を含む素材でポップ音になる
    juce::SmoothedValue<float> neutralFade;
    float lastCompDrive = -1.0f; // 補償ゲイン再計算判定（-1 = 未計算）

    float prevInput[2] {};              // ADAAの x[n−1]（ch別）
    float dcPrevIn[2] {}, dcPrevOut[2] {}; // DCブロッカ履歴（ch別）
    float dcCoeff = 0.0f;               // R = 1 − 2π·fc/fs（fs依存）
};
