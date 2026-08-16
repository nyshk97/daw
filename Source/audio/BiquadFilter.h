#pragma once

#include <array>
#include <cmath>

// TrackEq / TrackComp が共有する biquad の最小部品。
// RT安全: 固定サイズ・確保なし・noexcept（オーディオスレッドから呼んでよい）。
// 係数の設計（ArrayCoefficients::make*）は使う側が行い、ここは実行と状態管理だけを持つ

// Direct Form II transposed。係数は a0 正規化済み
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float s1[2] {}, s2[2] {};

    float processSample (int ch, float x) noexcept
    {
        const float y = b0 * x + s1[ch];
        s1[ch] = b1 * x - a1 * y + s2[ch];
        s2[ch] = b2 * x - a2 * y;
        return y;
    }

    void resetState() noexcept { s1[0] = s1[1] = s2[0] = s2[1] = 0.0f; }

    // juce::dsp::IIR::ArrayCoefficients の {b0,b1,b2,a0,a1,a2} を a0 正規化して格納
    void setCoefficients (const std::array<float, 6>& c) noexcept
    {
        const float inv = 1.0f / c[3];
        b0 = c[0] * inv;
        b1 = c[1] * inv;
        b2 = c[2] * inv;
        a1 = c[4] * inv;
        a2 = c[5] * inv;
    }

    // 状態変数のdenormal掃除（juce::dsp::util::snapToZero と同じ閾値の流儀）。
    // ScopedNoDenormals は処理中のFTZのみで、保存された微小状態が次回呼び出しまで残るため
    void snapStatesToZero() noexcept
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            snapToZero (s1[ch]);
            snapToZero (s2[ch]);
        }
    }

    static void snapToZero (float& value) noexcept
    {
        if (! (std::fabs (value) > 1.0e-8f))
            value = 0.0f;
    }
};
