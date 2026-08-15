#include "TrackComp.h"

#include <cmath>
#include <juce_dsp/juce_dsp.h>

namespace
{
// パラメータ（Threshold/Ratio/Make Up）とON/OFFクロスフェードの平滑化時間。
// 手動ノブ操作には即応と感じる速さで、Make Upのゲイン段差を十分短く均せる長さ
constexpr double rampSeconds = 0.01;

// 状態変数のdenormal掃除（TrackEqと同じ流儀）。ScopedNoDenormals は処理中のFTZのみで、
// 保存された微小状態が次回呼び出しまで残るため
void snapToZero (float& value) noexcept
{
    if (! (std::fabs (value) > 1.0e-8f))
        value = 0.0f;
}
} // namespace

void TrackComp::resetSmootherRates (double sampleRate)
{
    thresholdDb.reset (sampleRate, rampSeconds);
    ratio.reset (sampleRate, rampSeconds);
    makeupDb.reset (sampleRate, rampSeconds);
    chainMix.reset (sampleRate, rampSeconds);
}

void TrackComp::snapAllToTargets (bool compEnabled, const Comp::Values& targets)
{
    thresholdDb.setCurrentAndTargetValue (targets.thresholdDb);
    ratio.setCurrentAndTargetValue (targets.ratio);
    makeupDb.setCurrentAndTargetValue (targets.makeupDb);
    chainMix.setCurrentAndTargetValue (compEnabled ? 1.0f : 0.0f);
}

void TrackComp::resetDynamicsState()
{
    grEnvDb = 0.0f;
    hpf.resetState();
}

void TrackComp::updateHpfCoefficients()
{
    // 80Hz・12dB/oct（Butterworth）。ArrayCoefficients は確保なし（Coefficients::make*() は
    // 内部で new するためコールバック内で使わない）
    const auto c = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (
        preparedRate, Comp::detectorHpfHz, 0.70710678f);
    const float inv = 1.0f / c[3];
    hpf.b0 = c[0] * inv;
    hpf.b1 = c[1] * inv;
    hpf.b2 = c[2] * inv;
    hpf.a1 = c[4] * inv;
    hpf.a2 = c[5] * inv;
}

void TrackComp::updateAlphas (const Comp::Values& targets)
{
    // α = exp(-1/(τ·fs))。ステップ入力でτ後に最終GRの63.2%へ到達する定義。
    // Attack/Release は時定数係数なので平滑化せず即時反映（クリックは出ない）
    if (targets.attackMs == lastAttackMs && targets.releaseMs == lastReleaseMs)
        return;
    lastAttackMs = targets.attackMs;
    lastReleaseMs = targets.releaseMs;
    alphaAttack = (float) std::exp (-1.0 / ((double) targets.attackMs * 0.001 * preparedRate));
    alphaRelease = (float) std::exp (-1.0 / ((double) targets.releaseMs * 0.001 * preparedRate));
}

void TrackComp::snapTo (double sampleRate, bool compEnabled, const Comp::Values& targets)
{
    preparedRate = sampleRate;
    resetSmootherRates (sampleRate);
    snapAllToTargets (compEnabled, targets);
    resetDynamicsState();
    updateHpfCoefficients();
    lastAttackMs = lastReleaseMs = -1.0f; // 次のprocessで必ずαを計算する
    lastHpfOn = targets.detectorHpf;
    lastSerial = 0; // 呼び出し側は serial=1 から連番で渡す
    settled = ! compEnabled;
}

void TrackComp::process (float* left, float* right, int numSamples, double sampleRate,
                         juce::uint64 serial, bool timelineJumped,
                         bool compEnabled, const Comp::Values& targets)
{
    if (left == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return;

    juce::ScopedNoDenormals noDenormals;

    const bool firstCall = preparedRate <= 0.0;
    const bool srChanged = sampleRate != preparedRate;
    if (srChanged)
    {
        preparedRate = sampleRate;
        resetSmootherRates (sampleRate);
        updateHpfCoefficients();
        lastAttackMs = lastReleaseMs = -1.0f; // αはfsに依存する
    }
    const bool reentry = serial != lastSerial + 1;
    lastSerial = serial;

    if (timelineJumped || (srChanged && ! firstCall))
    {
        // 時間不連続（シーク・SR変更）: 音自体が不連続なのでフェード不要。全て目標へスナップ
        resetDynamicsState();
        snapAllToTargets (compEnabled, targets);
    }
    else if (reentry || firstCall)
    {
        // 再進入（高速パス・ミュート・停止から復帰）: バイパス中の出力＝dryなので、
        // dry(0) からのフェードインで連続にする。平滑化GR・検波HPF履歴は凍結された
        // 古い値のままなので必ずリセットする（強いGR中にOFF→時間経過→ONの操作列で
        // 古いGRからwet信号が復帰する事故を防ぐ）
        resetDynamicsState();
        snapAllToTargets (compEnabled, targets);
        chainMix.setCurrentAndTargetValue (0.0f);
    }

    // 検波HPFトグルのfalse→true: OFF中に凍結した古いIIR履歴を復活させない。
    // 検波信号だけの不連続で、GR目標の跳びはattack/release平滑化を通るためフェード不要
    if (targets.detectorHpf && ! lastHpfOn)
        hpf.resetState();
    lastHpfOn = targets.detectorHpf;

    updateAlphas (targets);

    // 目標値の更新（同値なら SmoothedValue 側で no-op）
    thresholdDb.setTargetValue (targets.thresholdDb);
    ratio.setTargetValue (targets.ratio);
    makeupDb.setTargetValue (targets.makeupDb);
    chainMix.setTargetValue (compEnabled ? 1.0f : 0.0f);

    const bool hpfOn = targets.detectorHpf;
    blockMaxGr = 0.0f;
    blockMaxDetector = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float l = left[i];
        const float r = right != nullptr ? right[i] : l;

        // 検波: L/R別々にHPF → 大きい方（max）。maxを取ってからHPFは不可
        //（整流済み信号のフィルタは意味が壊れる）
        float detectL = l, detectR = r;
        if (hpfOn)
        {
            detectL = hpf.processSample (0, l);
            detectR = right != nullptr ? hpf.processSample (1, r) : detectL;
        }
        const float level = juce::jmax (std::fabs (detectL), std::fabs (detectR));
        blockMaxDetector = juce::jmax (blockMaxDetector, level);

        // パラメータはサンプル単位で進める（skip方式はブロックサイズで掛かり方が変わり、
        // Attack 0.1ms級では聴こえ方自体が変わる。ゲイン計算は軽いので最適化自体が不要）
        const float thresholdNow = thresholdDb.getNextValue();
        const float ratioNow = ratio.getNextValue();
        const float makeupNow = makeupDb.getNextValue();
        const float mix = chainMix.getNextValue();

        // ゲイン計算（dBドメイン）→ GRをdB単位で平滑化（smooth branching）
        const float inputDb = juce::Decibels::gainToDecibels (level, Comp::silenceFloorDb);
        const float grTarget = Comp::gainReductionDb (inputDb, thresholdNow, ratioNow);
        const float alpha = grTarget > grEnvDb ? alphaAttack : alphaRelease;
        grEnvDb = alpha * grEnvDb + (1.0f - alpha) * grTarget;
        blockMaxGr = juce::jmax (blockMaxGr, grEnvDb);

        // 両chに同量適用（Make Upも同じ乗算に畳む）＋ dry/wetクロスフェード
        const float wetGain = juce::Decibels::decibelsToGain (makeupNow - grEnvDb);
        const float g = 1.0f + mix * (wetGain - 1.0f);
        left[i] = l * g;
        if (right != nullptr)
            right[i] = r * g;
    }

    // denormal掃除: HPF状態と、release減衰で0に漸近するGR（1e-4dB≒ゲイン比0.99999は聴こえない）
    for (int ch = 0; ch < 2; ++ch)
    {
        snapToZero (hpf.s1[ch]);
        snapToZero (hpf.s2[ch]);
    }
    if (grEnvDb < 1.0e-4f)
        grEnvDb = 0.0f;

    // 高速パスへ移れる条件: OFF目標 かつ 平滑（クロスフェード含む）が全て完了
    const bool smoothing = chainMix.isSmoothing() || thresholdDb.isSmoothing()
                           || ratio.isSmoothing() || makeupDb.isSmoothing();
    settled = ! compEnabled && ! smoothing;
}
