#include "TrackEq.h"

#include <cmath>
#include <juce_dsp/juce_dsp.h>

namespace
{
constexpr double paramRampSeconds = 0.02; // 周波数・ゲイン・Qの平滑（ドラッグのジッパーノイズ対策）
} // namespace

void TrackEq::fxResetSmootherRates (double sampleRate)
{
    for (int i = 0; i < Eq::numBands; ++i)
    {
        freq[i].reset (sampleRate, paramRampSeconds);
        gainDb[i].reset (sampleRate, paramRampSeconds);
        q[i].reset (sampleRate, paramRampSeconds);
    }
    hpMix.reset (sampleRate, chainMixRampSeconds);
}

void TrackEq::fxSnapToTargets (const Eq::Values& targets)
{
    for (int i = 0; i < Eq::numBands; ++i)
    {
        freq[i].setCurrentAndTargetValue (targets[(size_t) i].freqHz);
        gainDb[i].setCurrentAndTargetValue (targets[(size_t) i].gainDb);
        q[i].setCurrentAndTargetValue (targets[(size_t) i].q);
    }
    hpMix.setCurrentAndTargetValue (targets[Eq::highpass].enabled ? 1.0f : 0.0f);
}

void TrackEq::fxResetHistory()
{
    for (auto& filter : filters)
        filter.resetState();
}

void TrackEq::updateBandCoefficients (int band, float freqHz, float gainDbValue, float qValue)
{
    using Coeffs = juce::dsp::IIR::ArrayCoefficients<float>;

    // 平滑途中の値はナイキスト超えになり得ないが（normalize済みの目標間の補間）、
    // 低SRデバイスでの安全側クランプだけ掛けておく（超えると係数が発散する）
    const float f = juce::jmin (freqHz, (float) (preparedRate * 0.49));

    std::array<float, 6> c;
    if (band == Eq::highpass)
        c = Coeffs::makeHighPass (preparedRate, f, Eq::fixedQ);
    else if (band == Eq::highShelf)
        c = Coeffs::makeHighShelf (preparedRate, f, Eq::fixedQ,
                                   juce::Decibels::decibelsToGain (gainDbValue));
    else
        c = Coeffs::makePeakFilter (preparedRate, f, qValue,
                                    juce::Decibels::decibelsToGain (gainDbValue));

    filters[band].setCoefficients (c);
}

void TrackEq::snapTo (double sampleRate, bool eqEnabled, const Eq::Values& targets)
{
    snapToBase (sampleRate, eqEnabled, targets);
    for (int i = 0; i < Eq::numBands; ++i)
        lastFreq[i] = lastGain[i] = lastQ[i] = -1.0f; // 次のprocessで必ず係数を計算する
    settled = ! (eqEnabled && ! Eq::isNeutral (targets));
}

void TrackEq::process (float* left, float* right, int numSamples, double sampleRate,
                       juce::uint64 serial, bool timelineJumped,
                       bool eqEnabled, const Eq::Values& targets)
{
    juce::ScopedNoDenormals noDenormals;

    if (! beginBlock (left, numSamples, sampleRate, serial, timelineJumped, eqEnabled, targets))
        return;

    // 目標値の更新（同値なら SmoothedValue 側で no-op。chainMix は beginBlock が更新済み）
    for (int i = 0; i < Eq::numBands; ++i)
    {
        freq[i].setTargetValue (targets[(size_t) i].freqHz);
        gainDb[i].setTargetValue (targets[(size_t) i].gainDb);
        q[i].setTargetValue (targets[(size_t) i].q);
    }
    hpMix.setTargetValue (targets[Eq::highpass].enabled ? 1.0f : 0.0f);

    // クロスフェードはセグメント内を直線ランプで進める（セグメント単位の段差にするとクリックになる）
    const float chain0 = chainMix.getCurrentValue();
    const float hp0 = hpMix.getCurrentValue();
    const float chain1 = chainMix.skip (numSamples);
    const float hp1 = hpMix.skip (numSamples);

    // バンドの平滑値をセグメント長ぶん進め、動いた帯域だけ係数を再計算する
    // （セグメント単位の係数ステップで十分。ミックスと違い数dB×数十stepの階段は聴こえない）
    for (int i = 0; i < Eq::numBands; ++i)
    {
        const float f = freq[i].skip (numSamples);
        const float g = gainDb[i].skip (numSamples);
        const float qv = q[i].skip (numSamples);
        if (f != lastFreq[i] || g != lastGain[i] || qv != lastQ[i])
        {
            updateBandCoefficients (i, f, g, qv);
            lastFreq[i] = f;
            lastGain[i] = g;
            lastQ[i] = qv;
        }
    }

    const int numChannels = right != nullptr ? 2 : 1;
    const float invN = 1.0f / (float) numSamples;
    const float chainStep = (chain1 - chain0) * invN;
    const float hpStep = (hp1 - hp0) * invN;

    for (int i = 0; i < numSamples; ++i)
    {
        const float t = (float) (i + 1);
        const float mix = chain0 + chainStep * t;
        const float hpAmount = hp0 + hpStep * t;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* buffer = ch == 0 ? left : right;
            const float dry = buffer[i];

            // HPは常に処理して状態を温めておく（ONへのクロスフェードが現在の状態から滑らかに始まる）
            float x = dry;
            const float hpOut = filters[Eq::highpass].processSample (ch, x);
            x += hpAmount * (hpOut - x);
            x = filters[Eq::bell1].processSample (ch, x);
            x = filters[Eq::bell2].processSample (ch, x);
            x = filters[Eq::highShelf].processSample (ch, x);

            buffer[i] = dry + mix * (x - dry);
        }
    }

    for (auto& filter : filters)
        filter.snapStatesToZero();

    // 高速パスへ移れる条件: 目標がバイパス相当 かつ 平滑が全て完了
    // （中立の wet はほぼ dry なので、完了後の切替は不連続にならない）
    bool smoothing = chainMix.isSmoothing() || hpMix.isSmoothing();
    for (int i = 0; i < Eq::numBands && ! smoothing; ++i)
        smoothing = freq[i].isSmoothing() || gainDb[i].isSmoothing() || q[i].isSmoothing();
    settled = ! (eqEnabled && ! Eq::isNeutral (targets)) && ! smoothing;
}
