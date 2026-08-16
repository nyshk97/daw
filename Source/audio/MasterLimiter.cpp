#include "MasterLimiter.h"

#include <cmath>

namespace
{
// Gain/Ceilingノブの平滑化時間（TrackCompのrampSecondsと同じ理由の10ms）
constexpr double rampSeconds = 0.01;
} // namespace

void MasterLimiter::configureForRate (double sampleRate)
{
    preparedRate = sampleRate;
    lookahead = lookaheadForRate (sampleRate);
    // アタック時定数 τ = L/4 サンプル: ピークがディレイを抜けるまでのLサンプルで
    // 目標GRの (1 - e^-4) ≈ 98.2% に到達する。残りは最終クランプが刈る
    alphaAttack = (float) std::exp (-4.0 / (double) lookahead);
    gainDb.reset (sampleRate, rampSeconds);
    ceilingDb.reset (sampleRate, rampSeconds);
    lastReleaseMs = -1.0f; // αはfsに依存する
}

void MasterLimiter::updateReleaseAlpha (float releaseMs)
{
    if (releaseMs == lastReleaseMs)
        return;
    lastReleaseMs = releaseMs;
    alphaRelease = (float) std::exp (-1.0 / ((double) releaseMs * 0.001 * preparedRate));
}

void MasterLimiter::resetState()
{
    for (int i = 0; i < maxLookaheadSamples; ++i)
    {
        delayL[i] = 0.0f;
        delayR[i] = 0.0f;
        ceilRingDb[i] = ceilingDb.getTargetValue();
    }
    ringIdx = 0;
    dqHead = dqTail = 0;
    samplePos = 0;
    grEnvDb = 0.0f;
    blockMaxGr = 0.0f;
}

void MasterLimiter::snapTo (double sampleRate, const Limiter::Values& targets)
{
    configureForRate (sampleRate);
    gainDb.setCurrentAndTargetValue (targets.gainDb);
    ceilingDb.setCurrentAndTargetValue (targets.ceilingDb);
    updateReleaseAlpha (targets.releaseMs);
    resetState();
}

void MasterLimiter::process (float* left, float* right, int numSamples, double sampleRate,
                             const Limiter::Values& targets)
{
    if (left == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return;

    juce::ScopedNoDenormals noDenormals;

    if (sampleRate != preparedRate)
        snapTo (sampleRate, targets); // SR変更 = 時間不連続。リセット契約どおり全消去

    updateReleaseAlpha (targets.releaseMs);
    gainDb.setTargetValue (targets.gainDb);
    ceilingDb.setTargetValue (targets.ceilingDb);

    const juce::uint64 window = (juce::uint64) lookahead + 1; // sliding max の窓幅 W = L+1
    blockMaxGr = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        // 入力Gainは検波・ディレイ格納より前に適用（検波と出力が同じ信号を見る）
        const float gainNowDb = gainDb.getNextValue();
        const float ceilNowDb = ceilingDb.getNextValue();
        const float inGain = juce::Decibels::decibelsToGain (gainNowDb);
        const float xL = left[i] * inGain;
        const float xR = right != nullptr ? right[i] * inGain : xL;

        // 検波: サンプルピーク・ステレオリンク（大きい方）
        const float level = juce::jmax (std::fabs (xL), std::fabs (xR));
        const float levelDb = juce::Decibels::gainToDecibels (level, Limiter::silenceFloorDb);
        const float grTarget = juce::jmax (0.0f, levelDb - ceilNowDb);

        // lookahead窓の sliding max（単調deque）: 未来Lサンプル以内に出るピークの最悪GR
        while (dqTail > dqHead && dqVal[(dqTail - 1) & dequeMask] <= grTarget)
            --dqTail;
        dqVal[dqTail & dequeMask] = grTarget;
        dqPos[dqTail & dequeMask] = samplePos;
        ++dqTail;
        // 窓 = [samplePos-L, samplePos]（いま出力するサンプル x[n-L] のGRを含む幅W）
        while (dqPos[dqHead & dequeMask] + window <= samplePos)
            ++dqHead;
        const float windowMax = dqVal[dqHead & dequeMask];
        ++samplePos;

        // GR包絡: 上昇はアタック（lookahead内に到達）、下降はリリース
        const float alpha = windowMax > grEnvDb ? alphaAttack : alphaRelease;
        grEnvDb = alpha * grEnvDb + (1.0f - alpha) * windowMax;

        // ディレイライン（Lサンプル遅延）＋ceilingの整列リング
        const float outL = delayL[ringIdx];
        const float outR = delayR[ringIdx];
        const float outCeilDb = ceilRingDb[ringIdx];
        delayL[ringIdx] = xL;
        delayR[ringIdx] = xR;
        ceilRingDb[ringIdx] = ceilNowDb;
        if (++ringIdx >= lookahead)
            ringIdx = 0;

        const float g = juce::Decibels::decibelsToGain (-grEnvDb);
        float yL = outL * g;
        float yR = outR * g;

        // 最終安全クランプ（ステレオリンク・整列済みceiling基準）。
        // エンベロープの到達残差（≈GRの1.8%）とパラメータ急変をここで絶対保証に変える
        const float peak = juce::jmax (std::fabs (yL), std::fabs (yR));
        const float ceilLin = juce::Decibels::decibelsToGain (outCeilDb);
        float appliedGr = grEnvDb;
        if (peak > ceilLin)
        {
            const float scale = ceilLin / peak;
            yL *= scale;
            yR *= scale;
            appliedGr -= juce::Decibels::gainToDecibels (scale); // scale<1 なので負dB＝GRに加算
        }
        blockMaxGr = juce::jmax (blockMaxGr, appliedGr);

        left[i] = yL;
        if (right != nullptr)
            right[i] = yR;
    }

    // denormal掃除（release減衰で0に漸近するGR。1e-4dBは聴こえない）
    if (grEnvDb < 1.0e-4f)
        grEnvDb = 0.0f;
}
