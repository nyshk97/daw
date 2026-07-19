#include "MainComponent.h"

MainComponent::MainComponent()
{
    recordingFile = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                        .getChildFile ("daw_tier0_recording.wav");

    addAndMakeVisible (waveformDisplay);
    addAndMakeVisible (recordButton);
    addAndMakeVisible (playButton);
    addAndMakeVisible (playheadLabel);

    recordButton.onClick = [this]
    {
        if (engine.isRecording())
        {
            engine.stopRecording();
            hasRecording = engine.loadRecording (recordingFile);
        }
        else
        {
            engine.startRecording (recordingFile);
        }
        updateButtonStates();
    };

    playButton.onClick = [this]
    {
        if (engine.isPlaying())
            engine.stopPlayback();
        else
            engine.startPlayback();
        updateButtonStates();
    };

    // 前回の録音が残っていればそのまま再生できるようにする
    if (recordingFile.existsAsFile())
        hasRecording = engine.loadRecording (recordingFile);

    updateButtonStates();

    setSize (720, 400);
    setAudioChannels (1, 2); // 入力1ch（マイク）・出力2ch
    startTimerHz (30);       // GOTCHAS.md: 通知はpush型でなくpull型（Timerポーリング）
}

MainComponent::~MainComponent()
{
    // engine より先にオーディオコールバックを止める
    shutdownAudio();
}

void MainComponent::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    engine.prepareToPlay (samplesPerBlockExpected, sampleRate);
}

void MainComponent::getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill)
{
    engine.getNextAudioBlock (bufferToFill);
}

void MainComponent::releaseResources()
{
    engine.releaseResources();
}

void MainComponent::timerCallback()
{
    meterLevel = sharedState.peakLevel.load();

    const auto sampleRate = sharedState.sampleRate.load();
    const auto seconds = sampleRate > 0.0
        ? (double) sharedState.playheadSamplePos.load() / sampleRate
        : 0.0;
    playheadLabel.setText (juce::String (seconds, 2) + " s", juce::dontSendNotification);

    updateButtonStates(); // 再生が末尾まで到達して止まったときのボタン表示を追従させる
    repaint (meterArea);
}

void MainComponent::updateButtonStates()
{
    recordButton.setButtonText (engine.isRecording()
        ? juce::String::fromUTF8 (u8"録音停止")
        : juce::String::fromUTF8 (u8"録音"));
    playButton.setButtonText (engine.isPlaying()
        ? juce::String::fromUTF8 (u8"停止")
        : juce::String::fromUTF8 (u8"再生"));
    playButton.setEnabled (hasRecording && ! engine.isRecording());
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    // レベルメーター
    g.setColour (juce::Colours::darkgrey);
    g.fillRect (meterArea);
    g.setColour (meterLevel > 0.9f ? juce::Colours::red : juce::Colours::limegreen);
    auto bar = meterArea;
    g.fillRect (bar.removeFromLeft ((int) (meterLevel * (float) meterArea.getWidth())));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto topRow = area.removeFromTop (32);
    recordButton.setBounds (topRow.removeFromLeft (100));
    topRow.removeFromLeft (8);
    playButton.setBounds (topRow.removeFromLeft (100));
    topRow.removeFromLeft (8);
    playheadLabel.setBounds (topRow);

    meterArea = area.removeFromBottom (20);
    area.removeFromBottom (8);
    waveformDisplay.setBounds (area.reduced (0, 8));
}
