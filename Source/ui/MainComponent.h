#pragma once

#include <memory>
#include <juce_audio_utils/juce_audio_utils.h>

#include "TimelineView.h"
#include "TrackHeadersView.h"
#include "../audio/PlaybackEngine.h"
#include "../shared/PlaybackSnapshot.h"
#include "../shared/Project.h"
#include "../shared/TransportState.h"

// 1プロジェクト分のメイン画面。AudioAppComponent のオーディオコールバックは
// PlaybackEngine（audio/）への転送だけを行い、ここに処理を書かない。
class MainComponent : public juce::AudioAppComponent,
                      private juce::Timer
{
public:
    explicit MainComponent (std::unique_ptr<Project> projectToOpen);
    ~MainComponent() override;

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

    bool hasUnsavedChanges() const { return dirty; }
    bool trySave(); // 成功でtrue（終了確認からも呼ばれる）
    juce::String windowTitle() const;
    std::function<void (const juce::String&)> onTitleChanged;

private:
    void timerCallback() override;

    void togglePlay();
    void toggleRecord();
    void startRecordingFlow();
    void finishRecording();
    void requestDeleteSelectedClip();
    void requestDeleteTrack (int index);
    void addTrack();
    void selectTrack (int index);
    void showDeviceSettings();
    void applyBpmText();
    void pushSnapshot();
    void setDirty (bool nowDirty);
    void updateTransportButtons();
    void updatePositionLabel();
    void updateSampleRateWarning();

    // 宣言順 = 初期化順。engine は transport/snapshots への参照を取るので後に置く
    TransportState transport;
    SnapshotExchange snapshots;
    std::unique_ptr<Project> project;
    PlaybackEngine engine { transport, snapshots };

    TimelineView timeline { transport };
    TrackHeadersView headers;

    juce::TextButton playButton, recordButton, addTrackButton, settingsButton;
    juce::ToggleButton clickButton;
    juce::Label bpmCaption, bpmValue, positionLabel, srWarningLabel;

    juce::Rectangle<int> meterArea;
    float meterLevel = 0.0f; // 描画用（メッセージスレッド専用）

    int selectedTrack = -1;
    bool dirty = false;
    bool focusGrabbed = false;

    // 録音中のクリップ情報（停止時にクリップ化する）
    juce::File pendingRecordFile;
    juce::int64 pendingPunchIn = 0;
    int pendingRecordTrack = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
