#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "WaveformDisplay.h"
#include "../audio/AudioEngine.h"
#include "../shared/SharedAudioState.h"
#include "../shared/WaveformFifo.h"

// AudioAppComponent 継承の最小構成。
// prepareToPlay / getNextAudioBlock / releaseResources はオーディオスレッド側の
// コールバックなので、AudioEngine（audio/）への転送だけを行い、ここに処理を書かない。
class MainComponent : public juce::AudioAppComponent,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateButtonStates();

    // 宣言順 = 初期化順。engine は shared/fifo への参照を取るので後に置く
    SharedAudioState sharedState;
    WaveformFifo waveformFifo;
    AudioEngine engine { sharedState, waveformFifo };

    WaveformDisplay waveformDisplay { waveformFifo };
    juce::TextButton recordButton { juce::String::fromUTF8 (u8"録音") };
    juce::TextButton playButton { juce::String::fromUTF8 (u8"再生") };
    juce::Label playheadLabel;

    juce::Rectangle<int> meterArea;
    float meterLevel = 0.0f; // 描画用（メッセージスレッド専用）

    juce::File recordingFile;
    bool hasRecording = false;
};
