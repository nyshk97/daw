#pragma once

#include <juce_graphics/juce_graphics.h>

// ステムの色（行の順＝manifestの順: drums, bass, other, vocals, guitar, piano）。
// StemPanel のスウォッチと、分離中の波形の積み重ね（WaveformView）で同じ色を使う
namespace StemColours
{
inline const juce::Colour swatches[] = {
    juce::Colour (0xffd94a43), // drums = 赤
    juce::Colour (0xff4a6ea9), // bass = 青
    juce::Colour (0xffdfae4a), // other = 黄
    juce::Colour (0xff7bc47b), // vocals = 緑
    juce::Colour (0xffb06ac9), // guitar = 紫
    juce::Colour (0xff5bb8c4), // piano = シアン
};
inline const char* const names[] = { "drums", "bass", "other", "vocals", "guitar", "piano" };
constexpr int count = 6;

inline juce::Colour swatch (int index) { return swatches[juce::jmax (0, index) % count]; }
} // namespace StemColours
