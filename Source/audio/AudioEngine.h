#pragma once

#include <memory>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "Recorder.h"
#include "../shared/SharedAudioState.h"
#include "../shared/WaveformFifo.h"

// 1トラック分のオーディオ処理。getNextAudioBlock() はオーディオスレッドで走る。
// UIへの直接参照は持たず、受け渡しは shared/ の構造（atomic / FIFO）を経由する。
class AudioEngine
{
public:
    AudioEngine (SharedAudioState& sharedState, WaveformFifo& fifo);
    ~AudioEngine();

    // ---- AudioAppComponent から転送される（getNextAudioBlockのみオーディオスレッド）----
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate);
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill);
    void releaseResources();

    // ---- メッセージスレッド専用 ----
    void startRecording (const juce::File& file);
    void stopRecording();
    bool isRecording() const;

    bool loadRecording (const juce::File& file); // 再生準備。成功でtrue
    void startPlayback();
    void stopPlayback();
    bool isPlaying() const;

private:
    SharedAudioState& shared;
    WaveformFifo& waveformFifo;

    Recorder recorder;

    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioTransportSource transport;

    double currentSampleRate = 0.0;
};
