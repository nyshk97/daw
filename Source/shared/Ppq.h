#pragma once

#include <juce_core/juce_core.h>

// PPQ（MIDIの時間単位）: 1四分音符 = 960 tick 固定。
// 1/32音符 = 120 tick、1/32三連符 = 80 tick が整数で表せる。
// オーディオクリップ（サンプル基準）と異なり、BPM変更後もMIDIは音楽的位置を維持する。
namespace Ppq
{
inline constexpr juce::int64 ticksPerQuarter = 960;
inline constexpr juce::int64 ticksPerBar = ticksPerQuarter * 4; // 拍子は4/4固定

// サンプル位置⇄PPQの換算係数（オーディオスレッドからも呼ばれる。実数演算のみ）
inline double ticksPerSample (double bpm, double sampleRate)
{
    return (bpm * (double) ticksPerQuarter) / (60.0 * sampleRate);
}
}
