#pragma once

#include <cmath>
#include <juce_core/juce_core.h>

// パン法則。リアルタイムエンジン（PlaybackEngine）とオフラインレンダラー（BounceRenderer）で
// 共有する（両者の出力が一致しないと「聞こえているまま」書き出しが崩れる）。
namespace Pan
{
// モノソース→ステレオ: 等パワー・センター補正型。
// センターで両ch 1.0（既存プロジェクトの音量を変えない）、振り切った側は+3dB（√2）になる
inline void monoGains (float pan, float& left, float& right)
{
    const float p = juce::jlimit (-1.0f, 1.0f, pan);
    const float theta = (p + 1.0f) * juce::MathConstants<float>::pi * 0.25f; // 0..π/2
    left = std::cos (theta) * juce::MathConstants<float>::sqrt2;
    right = std::sin (theta) * juce::MathConstants<float>::sqrt2;
}

// ステレオソース（MIDIシンセ出力）: バランス型。センター0dB・振った反対側だけ減衰
inline void stereoGains (float pan, float& left, float& right)
{
    const float p = juce::jlimit (-1.0f, 1.0f, pan);
    left = p > 0.0f ? 1.0f - p : 1.0f;
    right = p < 0.0f ? 1.0f + p : 1.0f;
}
} // namespace Pan
