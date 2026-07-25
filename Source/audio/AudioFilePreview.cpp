#include "AudioFilePreview.h"

#include <cmath>
#include <utility>

#include "AudioImporter.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }
constexpr int decodeChunkFrames = 32768;
}

AudioFilePreview::Exchange::~Exchange()
{
    delete pending.exchange (nullptr);
    delete retired.exchange (nullptr);
    delete current.exchange (nullptr);
}

void AudioFilePreview::Exchange::push (std::unique_ptr<Snapshot> snapshot)
{
    delete pending.exchange (snapshot.release());
}

void AudioFilePreview::Exchange::deleteRetired()
{
    delete retired.exchange (nullptr);
}

AudioFilePreview::Snapshot* AudioFilePreview::Exchange::acquire()
{
    if (pending.load() != nullptr && retired.load() == nullptr)
        if (auto* next = pending.exchange (nullptr))
        {
            retired.store (current.load());
            current.store (next);
        }
    return current.load();
}

AudioFilePreview::AudioFilePreview()
    : juce::Thread ("Audio file preview")
{
}

AudioFilePreview::~AudioFilePreview()
{
    cancelAndWait();
}

bool AudioFilePreview::start (const juce::File& file)
{
    cancelAndWait();
    if (! file.existsAsFile())
    {
        setError (jp (u8"ファイルが見つかりません: ") + file.getFileName());
        currentStatus.store (Status::failed);
        return false;
    }

    const auto generation = requestedGeneration.fetch_add (1) + 1;
    {
        const juce::ScopedLock lock (requestLock);
        requestedFile = file;
    }
    {
        const juce::ScopedLock lock (errorLock);
        errorMessage.clear();
    }
    playingGeneration.store (0);
    currentStatus.store (Status::loading);
    startThread();
    return generation != 0;
}

void AudioFilePreview::stop()
{
    requestedGeneration.fetch_add (1);
    playingGeneration.store (0);
    currentStatus.store (Status::idle);
    signalThreadShouldExit();
}

void AudioFilePreview::cancelAndWait()
{
    stop();
    stopThread (5000);
}

juce::String AudioFilePreview::takeError()
{
    const juce::ScopedLock lock (errorLock);
    return std::exchange (errorMessage, {});
}

void AudioFilePreview::deleteRetired()
{
    exchange.deleteRetired();
}

void AudioFilePreview::prepareToPlay (double deviceSampleRate)
{
    outputSampleRate = deviceSampleRate;
    audioGeneration = 0;
    sourcePosition = 0.0;
}

void AudioFilePreview::releaseResources()
{
    stop();
    outputSampleRate = 0.0;
    audioGeneration = 0;
    sourcePosition = 0.0;
}

void AudioFilePreview::mixInto (juce::AudioBuffer<float>& output, int startSample, int numSamples)
{
    auto* snapshot = exchange.acquire();
    const auto generation = playingGeneration.load();
    if (snapshot == nullptr || generation == 0 || snapshot->generation != generation
        || outputSampleRate <= 0.0 || snapshot->sampleRate <= 0.0
        || snapshot->audio.getNumChannels() == 0 || snapshot->audio.getNumSamples() == 0)
        return;

    if (audioGeneration != generation)
    {
        audioGeneration = generation;
        sourcePosition = 0.0;
    }

    const int frames = snapshot->audio.getNumSamples();
    const int channels = juce::jmin (2, output.getNumChannels());
    const int sourceChannels = snapshot->audio.getNumChannels();
    const double ratio = snapshot->sampleRate / outputSampleRate;

    for (int i = 0; i < numSamples; ++i)
    {
        if (sourcePosition >= (double) frames)
        {
            playingGeneration.store (0);
            currentStatus.store (Status::idle);
            break;
        }

        const int index = (int) std::floor (sourcePosition);
        const int next = juce::jmin (index + 1, frames - 1);
        const float frac = (float) (sourcePosition - (double) index);
        for (int ch = 0; ch < channels; ++ch)
        {
            const int sourceChannel = juce::jmin (ch, sourceChannels - 1);
            const float a = snapshot->audio.getSample (sourceChannel, index);
            const float b = snapshot->audio.getSample (sourceChannel, next);
            output.addSample (ch, startSample + i, (a + (b - a) * frac) * previewGain);
        }
        sourcePosition += ratio;
    }
}

void AudioFilePreview::run()
{
    juce::File file;
    {
        const juce::ScopedLock lock (requestLock);
        file = requestedFile;
    }
    const auto generation = requestedGeneration.load();

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        if (requestedGeneration.load() == generation)
        {
            setError (jp (u8"対応していない形式か、壊れたファイルです: ") + file.getFileName());
            currentStatus.store (Status::failed);
            if (requestedGeneration.load() != generation)
                currentStatus.store (Status::idle);
        }
        return;
    }
    if (reader->lengthInSamples > AudioImporter::maxInputFrames)
    {
        if (requestedGeneration.load() == generation)
        {
            setError (jp (u8"ファイルが長すぎます（30分程度まで）: ") + file.getFileName());
            currentStatus.store (Status::failed);
            if (requestedGeneration.load() != generation)
                currentStatus.store (Status::idle);
        }
        return;
    }

    auto snapshot = std::make_unique<Snapshot>();
    snapshot->sampleRate = reader->sampleRate;
    snapshot->generation = generation;
    const int channels = reader->numChannels >= 2 ? 2 : 1;
    const int frames = (int) reader->lengthInSamples;
    snapshot->audio.setSize (channels, frames);

    for (int pos = 0; pos < frames; pos += decodeChunkFrames)
    {
        if (threadShouldExit() || requestedGeneration.load() != generation)
            return;
        const int count = juce::jmin (decodeChunkFrames, frames - pos);
        if (! reader->read (&snapshot->audio, pos, count, pos, true, channels >= 2))
        {
            if (requestedGeneration.load() == generation)
            {
                setError (jp (u8"デコードに失敗しました: ") + file.getFileName());
                currentStatus.store (Status::failed);
                if (requestedGeneration.load() != generation)
                    currentStatus.store (Status::idle);
            }
            return;
        }
    }

    if (threadShouldExit() || requestedGeneration.load() != generation)
        return;

    exchange.push (std::move (snapshot));
    currentStatus.store (Status::playing);
    playingGeneration.store (generation);
    if (requestedGeneration.load() != generation)
    {
        playingGeneration.store (0);
        currentStatus.store (Status::idle);
    }
}

void AudioFilePreview::setError (const juce::String& message)
{
    const juce::ScopedLock lock (errorLock);
    errorMessage = message;
}
