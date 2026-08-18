#include "ClipStretcher.h"

#include <cmath>
#include <limits>
#include <vector>

#include <signalsmith-stretch/signalsmith-stretch.h>

#include "../shared/RenderedDomain.h"

namespace
{
// 乱数シード固定（必須）。既定コンストラクタは std::random_device でシードするため、
// 「読込時に再生成」した音が起動ごとに変わってしまう（planの前提・GOTCHAS参照）
constexpr long fixedSeed = 0x5eed;

// 音色維持のトナリティ上限（Hz）。signalsmith-stretch 付属 cmd の既定値と同じ
constexpr double tonalityLimitHz = 8000.0;
} // namespace

std::unique_ptr<juce::AudioBuffer<float>> ClipStretcher::render (
    const juce::AudioBuffer<float>& source,
    juce::int64 domainOffset, juce::int64 domainLength,
    int semitones, double ratio, double sampleRate)
{
    using namespace ClipStretchLimits;

    // ---- 直呼びの検査（クランプせず失敗を返す）----
    const auto sourceLength = (juce::int64) source.getNumSamples();
    if (source.getNumChannels() < 1 || sourceLength <= 0)
        return nullptr;
    if (domainOffset < 0 || domainLength < 1 || domainLength > sourceLength
        || domainOffset > sourceLength - domainLength) // checked addition
        return nullptr;
    if (! std::isfinite (ratio) || ratio < minRatio || ratio > maxRatio)
        return nullptr;
    if (semitones < -maxSemitones || semitones > maxSemitones)
        return nullptr;

    const int numChannels = juce::jmin (2, source.getNumChannels());
    const auto outLength64 = (juce::int64) std::llround ((double) domainLength * ratio);
    if (outLength64 < 1)
        return nullptr;
    // 出力長の checked conversion とメモリ上限（int へ渡す前に int64 で検査する）
    if (outLength64 > (juce::int64) (std::numeric_limits<int>::max() / 2)
        || outLength64 * numChannels * (juce::int64) sizeof (float) > maxRenderBytes)
        return nullptr;

    const double sr = sampleRate > 0.0 && std::isfinite (sampleRate) ? sampleRate : 48000.0;

    signalsmith::stretch::SignalsmithStretch<float> stretch (fixedSeed);
    stretch.presetDefault (numChannels, (float) sr);
    stretch.setTransposeSemitones ((float) semitones, (float) (tonalityLimitHz / sr));

    // ---- 助走（パディング）----
    // exact() は先頭 outputSeekLength 分の入力をプリロールに使う。範囲の頭が分析窓の途中から
    // 始まると滲むうえ、入力が短いと exact() 自体が無音を返して false になる（20〜50msの
    // チョップが該当）。前後に原音を助走として足し、バッファ端では無音のまま埋める
    const double playbackRate = 1.0 / ratio;
    const auto padHead = (juce::int64) stretch.outputSeekLength ((float) playbackRate)
                       + stretch.intervalSamples();
    const auto padTail = (juce::int64) stretch.inputLatency() + stretch.intervalSamples();

    const auto extLength64 = padHead + domainLength + padTail;
    if (extLength64 > (juce::int64) (std::numeric_limits<int>::max() / 2))
        return nullptr;
    const int extLength = (int) extLength64;

    juce::AudioBuffer<float> extended (numChannels, extLength);
    extended.clear();
    {
        // 原音から [domainOffset - padHead, domainOffset + domainLength + padTail) を写す。
        // バッファ外は clear 済みの無音のまま
        const auto srcBegin = juce::jmax ((juce::int64) 0, domainOffset - padHead);
        const auto srcEnd = juce::jmin (sourceLength, domainOffset + domainLength + padTail);
        const auto destBegin = padHead - (domainOffset - srcBegin);
        for (int ch = 0; ch < numChannels; ++ch)
            extended.copyFrom (ch, (int) destBegin, source, ch, (int) srcBegin,
                               (int) (srcEnd - srcBegin));
    }

    // 出力側も同じ助走を持たせ、ドメイン部分だけを切り出す。パディング量は出力側で丸めるので
    // 実効レートは ratio と厳密には一致しないが、ずれは全長で±1サンプル未満（境界の丸めのみ）
    const auto outPadHead = (juce::int64) std::llround ((double) padHead * ratio);
    const auto outPadTail = (juce::int64) std::llround ((double) padTail * ratio);
    const auto extOutLength64 = outPadHead + outLength64 + outPadTail;
    if (extOutLength64 > (juce::int64) (std::numeric_limits<int>::max() / 2))
        return nullptr;

    juce::AudioBuffer<float> extOut (numChannels, (int) extOutLength64);
    extOut.clear();

    // AudioBuffer の [ch][index] アクセスを signalsmith の Inputs/Outputs 契約へ写す薄い皮
    struct ChannelPointers
    {
        float* const* pointers;
        float* operator[] (int ch) const { return pointers[ch]; }
    };
    const ChannelPointers inputPtrs { extended.getArrayOfWritePointers() };
    const ChannelPointers outputPtrs { extOut.getArrayOfWritePointers() };

    if (! stretch.exact (inputPtrs, extLength, outputPtrs, (int) extOutLength64))
        return nullptr;

    auto result = std::make_unique<juce::AudioBuffer<float>> (numChannels, (int) outLength64);
    for (int ch = 0; ch < numChannels; ++ch)
        result->copyFrom (ch, 0, extOut, ch, (int) outPadHead, (int) outLength64);
    return result;
}
