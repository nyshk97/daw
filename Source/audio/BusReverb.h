#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "BiquadFilter.h"
#include "DelayLine.h"
#include "../shared/ReverbParams.h"

// バスReverbのDSP本体（full wet）。send用固定バス1/2（"Reverb A"/"Reverb B"）の常在FX。
// A/Bで同一クラスの2インスタンス（初期値だけが違う）。
// plan: docs/plans/2026-08-16-2305-fx-batch4-delay-reverb.md
//
// 構成: Low Cut（Biquad HP・残響へ送る前に低域を落とす）→ Pre-delay（DelayLine・
// 原音と残響の隙間）→ juce::Reverb（wet 100% / dry 0）。
// Size/Damp/Width の連続変化は juce::Reverb 内部の SmoothedValue が平滑する。
// Low Cut は係数のブロック頭更新（TrackEqと同じ流儀）、Pre-delay 長の変更は
// BusDelayと同じ50msクロスフェード。
//
// リセット契約・スレッド契約は BusDelay と同じ（snapToは再生開始・明示シーク・バウンス開始のみ。
// Mute中も process は止めない。prepare はヒープ確保するためメッセージ/ワーカースレッド専用 —
// juce::Reverb::setSampleRate も確保を伴うので必ず prepare 経由で行う）。
class BusReverb
{
public:
    BusReverb() = default;

    void prepare (double sampleRate);

    // 平滑を挟まず即座に目標状態へ（残響・pre-delay・フィルタ履歴を全消去）
    void snapTo (const Reverb::Values& targets);

    // インプレース処理（ステレオ必須）。full wet: バッファ内容（send合算）をwetで置き換える
    void process (float* left, float* right, int numSamples, const Reverb::Values& targets);

private:
    void applyParameters (const Reverb::Values& targets);
    int preDelaySamplesFor (float preDelayMs) const;

    double preparedRate = 0.0;

    juce::Reverb reverb;
    DelayLine preL, preR;
    Biquad lowCut;
    juce::SmoothedValue<float> lowCutHz; // 周波数を20msで平滑してから係数更新（TrackEqと同じ流儀）
    float lastLowCutHz = -1.0f;

    // Pre-delay長のクロスフェード（BusDelayのディレイ長と同じ仕組み。0 = 直結）
    int currentPre = 0;
    int oldPre = 0;
    int xfadeTotal = 1;
    int xfadeLeft = 0;

    JUCE_DECLARE_NON_COPYABLE (BusReverb)
};
