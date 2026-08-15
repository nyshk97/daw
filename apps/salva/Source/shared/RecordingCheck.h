#pragma once

#include <juce_core/juce_core.h>

// 録音停止時の整合検査（planの契約）: コールバックが書こうとした総サンプル数と
// 確定したWAVの実サンプル長が一致しない場合、欠けた録音を正常テイクとして
// 黙って開かず警告する。FIFO満杯（ディスクが追いつかない）で write() が
// falseを返した分がここで可視化される
namespace RecordingCheck
{
// 一致なら空文字列、欠落があれば警告文
inline juce::String mismatchWarning (juce::int64 expectedSamples, juce::int64 actualSamples, double sampleRate)
{
    if (actualSamples >= expectedSamples || sampleRate <= 0.0)
        return {};
    const auto missing = expectedSamples - actualSamples;
    return juce::String::fromUTF8 (u8"録音データの一部が失われました: ")
           + juce::String ((double) missing / sampleRate, 2)
           + juce::String::fromUTF8 (u8"秒（") + juce::String (missing)
           + juce::String::fromUTF8 (u8"サンプル）が書き込めていません。ディスクの空き・速度を確認してください");
}
} // namespace RecordingCheck
