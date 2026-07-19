#include "AudioEngine.h"

AudioEngine::AudioEngine (SharedAudioState& sharedState, WaveformFifo& fifo)
    : shared (sharedState), waveformFifo (fifo)
{
    formatManager.registerBasicFormats();
}

AudioEngine::~AudioEngine()
{
    transport.setSource (nullptr);
}

void AudioEngine::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate = sampleRate;
    shared.sampleRate.store (sampleRate);
    transport.prepareToPlay (samplesPerBlockExpected, sampleRate);
}

void AudioEngine::getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill)
{
    auto& buffer = *bufferToFill.buffer;

    if (buffer.getNumChannels() == 0)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    // 入力（マイク）はチャンネル0に入っている。出力で上書きする前に消費する
    const float* input = buffer.getReadPointer (0, bufferToFill.startSample);

    const float* recordChannels[] = { input };
    recorder.write (recordChannels, bufferToFill.numSamples);

    waveformFifo.push (input, bufferToFill.numSamples);
    shared.peakLevel.store (buffer.getMagnitude (0, bufferToFill.startSample, bufferToFill.numSamples));

    // 出力: 停止中は transport が無音でクリアしてくれる。入力のモニタ出力はしない（フィードバック防止）
    transport.getNextAudioBlock (bufferToFill);
    shared.playheadSamplePos.store (transport.getNextReadPosition());
}

void AudioEngine::releaseResources()
{
    transport.releaseResources();
}

void AudioEngine::startRecording (const juce::File& file)
{
    recorder.startRecording (file, currentSampleRate);
}

void AudioEngine::stopRecording()
{
    recorder.stop();
}

bool AudioEngine::isRecording() const
{
    return recorder.isRecording();
}

bool AudioEngine::loadRecording (const juce::File& file)
{
    transport.stop();
    transport.setSource (nullptr);
    readerSource.reset();

    if (auto* reader = formatManager.createReaderFor (file))
    {
        readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
        transport.setSource (readerSource.get(), 0, nullptr, reader->sampleRate);
        return true;
    }
    return false;
}

void AudioEngine::startPlayback()
{
    transport.setPosition (0.0);
    transport.start();
}

void AudioEngine::stopPlayback()
{
    transport.stop();
}

bool AudioEngine::isPlaying() const
{
    return transport.isPlaying();
}
