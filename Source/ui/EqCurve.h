#pragma once

#include <cmath>
#include <vector>
#include <juce_dsp/juce_dsp.h>

#include "../shared/EqParams.h"

// EQの合成周波数応答（カーブ描画用）。EqEditorView の大カーブと StripParts のEQサムネイルが
// 同じ計算を使う＝「描画と音が同じ数式」（DSPと同じRBJ係数の応答）。
// Coefficients::make* は内部で new するため、オーディオスレッドでは呼ばないこと（UI描画専用）
namespace EqCurve
{
inline constexpr float minFreqShown = 20.0f;
inline constexpr float maxFreqShown = 20000.0f;

// 対数等分割の周波数列と合成応答（線形倍率）を freqs / mags へ埋める。
// カーブはSR依存（ナイキスト近傍の折れ・クランプ）なので sampleRate は実デバイス値を渡す。
// HPは有効なときだけ寄与（ベル/シェルフの0dBは元々素通しなので常に掛けてよい）
inline void response (const Eq::Values& bands, double sampleRate, int numPoints,
                      std::vector<double>& freqs, std::vector<double>& mags)
{
    freqs.resize ((size_t) numPoints);
    mags.assign ((size_t) numPoints, 1.0);
    for (int i = 0; i < numPoints; ++i)
        freqs[(size_t) i] = (double) minFreqShown
                            * std::pow ((double) (maxFreqShown / minFreqShown),
                                        (double) i / (numPoints - 1));

    std::vector<double> bandMags ((size_t) numPoints);
    for (int band = 0; band < Eq::numBands; ++band)
    {
        const auto& value = bands[(size_t) band];
        juce::dsp::IIR::Coefficients<float>::Ptr coefficients;
        if (band == Eq::highpass)
        {
            if (! value.enabled)
                continue;
            coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (
                sampleRate, juce::jmin (value.freqHz, (float) (sampleRate * 0.49)), Eq::fixedQ);
        }
        else if (band == Eq::highShelf)
        {
            coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
                sampleRate, juce::jmin (value.freqHz, (float) (sampleRate * 0.49)), Eq::fixedQ,
                juce::Decibels::decibelsToGain (value.gainDb));
        }
        else
        {
            coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                sampleRate, juce::jmin (value.freqHz, (float) (sampleRate * 0.49)), value.q,
                juce::Decibels::decibelsToGain (value.gainDb));
        }
        coefficients->getMagnitudeForFrequencyArray (freqs.data(), bandMags.data(),
                                                     (size_t) numPoints, sampleRate);
        for (int i = 0; i < numPoints; ++i)
            mags[(size_t) i] *= bandMags[(size_t) i];
    }
}
} // namespace EqCurve
