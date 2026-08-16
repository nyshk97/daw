#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "DelayLine.h"
#include "../shared/DelayParams.h"

// バスDelayのDSP本体（テンポ同期・full wet）。send用固定バス3（"Delay"）の常在FX。
// plan: docs/plans/2026-08-16-2305-fx-batch4-delay-reverb.md
//
// 構成: ステレオ入力 → （Ping-pong時はモノ化）→ ディレイライン → ループ内ローパス（Tone）
// → wet出力。フィードバックはローパス後の信号を書き戻す＝繰り返すたびに暗くなる。
// Ping-pong は L→R→L の交互で、減衰は1タップごとに feedback（ストレートと同じ耳あたり）。
//
// クリック回避の契約:
// - ディレイ長の変更（Time切替・BPM変更）は旧タップ→新タップの50msクロスフェード
// - Feedback は50msの per-sample 平滑・Tone は係数のブロック頭更新（Lo-fi Toneと同じ契約）
// - Ping-pong 切替は出力ゲインを10msで落としてからトポロジを切り替えて戻す
//   （リング内容は連続だが L/R の出所が入れ替わるため、瞬時切替はクリックになる）
//
// リセット契約（MasterLimiterと同じ）: snapTo は「再生開始・明示シーク・バウンス開始」のみ。
// サイクルラップでは呼ばない（エコーはループを跨いで続く＝連続ストリーム）。
// バスMute/Gain 0 中も呼び出し側は process を止めない（状態凍結→解除時に古いエコーが
// 復活するのを防ぐ。出力の加算だけを止める）。
//
// スレッド契約: prepare はメッセージ/ワーカースレッド専用（ヒープ確保）。
// snapTo / process は確保なし（processはオーディオスレッドでよい）。
class BusDelay
{
public:
    BusDelay() = default;

    // デバイスSR確定時に呼ぶ。DelayLine の容量契約（1/2音符×BPM下限30 = 4秒）ぶん確保する
    void prepare (double sampleRate);

    // 平滑を挟まず即座に目標状態にし、ディレイ・フィルタ履歴を全消去する
    void snapTo (const Delay::Values& targets, double bpm);

    // インプレース処理（ステレオ必須）。full wet: バッファ内容（send合算）をwetで置き換える
    void process (float* left, float* right, int numSamples, double bpm,
                  const Delay::Values& targets);

private:
    int delaySamplesFor (double bpm, int timeIndex) const;

    double preparedRate = 0.0;

    DelayLine lineL, lineR;

    juce::SmoothedValue<float> feedback; // 50ms per-sample
    juce::SmoothedValue<float> toneKnob; // ブロック頭で skip（係数はブロック単位の階段）
    juce::SmoothedValue<float> wetGain;  // Ping-pong切替の出力ランプ（10ms）

    // ループ内ローパス（one-pole。テープエコーの帯域の狭まり方）
    float lpL = 0.0f, lpR = 0.0f;
    float lpCoeff = 1.0f;
    float lastCutoffHz = -1.0f;

    // ディレイ長のクロスフェード（oldDelay → currentDelay。xfadeLeft>0 の間が遷移中）
    int currentDelay = 1;
    int oldDelay = 1;
    int xfadeTotal = 1;
    int xfadeLeft = 0;

    bool activePingPong = false;

    JUCE_DECLARE_NON_COPYABLE (BusDelay)
};
