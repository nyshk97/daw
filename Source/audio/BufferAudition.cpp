#include "BufferAudition.h"

#include <cmath>

BufferAudition::~BufferAudition()
{
    delete pending.exchange (nullptr);
    delete retired.exchange (nullptr);
    delete current.exchange (nullptr);
}

void BufferAudition::start (std::shared_ptr<const juce::AudioBuffer<float>> audio, juce::int64 start,
                            juce::int64 length, double sampleRate)
{
    if (audio == nullptr || length <= 0 || sampleRate <= 0.0)
        return;
    auto* snap = new Snapshot { std::move (audio), start, length, sampleRate, nextGeneration++ };
    playingGeneration.store (snap->generation);
    delete pending.exchange (snap); // 未消費の前回分は差し替え（メッセージスレッドで delete）
}

void BufferAudition::stop() { playingGeneration.store (0); }

void BufferAudition::deleteRetired() { delete retired.exchange (nullptr); }

void BufferAudition::mixInto (juce::AudioBuffer<float>& output, int startSample, int numSamples)
{
    // 交換: pending があり retired が空なら current を入れ替える（retired の解放はメッセージスレッド）
    if (pending.load() != nullptr && retired.load() == nullptr)
        if (auto* next = pending.exchange (nullptr))
        {
            retired.store (current.load());
            current.store (next);
        }
    auto* snap = current.load();
    const auto generation = playingGeneration.load();
    if (snap == nullptr || generation == 0 || snap->generation != generation || outputSampleRate <= 0.0)
        return;
    if (audioGeneration != generation)
    {
        audioGeneration = generation;
        sourcePosition = 0.0;
    }
    const auto& src = *snap->audio;
    const int sourceChannels = src.getNumChannels();
    const int channels = juce::jmin (2, output.getNumChannels());
    const double ratio = snap->sampleRate / outputSampleRate;
    const auto total = (juce::int64) src.getNumSamples();
    for (int i = 0; i < numSamples; ++i)
    {
        if (sourcePosition >= (double) snap->length)
        {
            playingGeneration.store (0);
            break;
        }
        const auto pos = (double) snap->start + sourcePosition;
        const auto index = (juce::int64) std::floor (pos);
        const auto next = juce::jmin (index + 1, total - 1);
        const float frac = (float) (pos - (double) index);
        if (index < 0 || index >= total)
            break;
        for (int ch = 0; ch < channels; ++ch)
        {
            const int sc = juce::jmin (ch, sourceChannels - 1);
            const float a = src.getSample (sc, (int) index), b = src.getSample (sc, (int) next);
            output.addSample (ch, startSample + i, (a + (b - a) * frac) * gain);
        }
        sourcePosition += ratio;
    }
}
