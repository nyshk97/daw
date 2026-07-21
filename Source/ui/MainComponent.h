#pragma once

#include <memory>
#include <juce_audio_utils/juce_audio_utils.h>

#include "PianoRollView.h"
#include "TimelineView.h"
#include "TrackHeadersView.h"
#include "../audio/PlaybackEngine.h"
#include "../shared/PreviewFifo.h"
#include "../shared/PlaybackSnapshot.h"
#include "../shared/Project.h"
#include "../shared/SynthBank.h"
#include "../shared/TransportState.h"
#include "../shared/UndoStack.h"

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
    void seekByStep (int direction, bool wholeBar, int keyCode);  // ,/.キー: 1拍（Shiftで1小節）単位で再生ヘッドを移動
    void toggleMuteSelectedTrack();      // mキー
    void requestDeleteSelectedClip();
    void deleteSelectedRegion();         // MIDIリージョンは確認なしで削除（undo可能なので）
    void requestDeleteTrack (int index);
    void showAddTrackMenu();
    void addTrack (TrackType type);
    void selectTrack (int index);
    bool selectedTrackIsMidi() const;
    void performUndo();
    void performRedo();
    void afterHistoryRestore();          // undo/redo後のUI・スナップショット同期
    void openPianoRoll (int trackIndex, int regionIndex);
    void closePianoRoll();
    void showDeviceSettings();
    void applyBpmText();
    void pushSnapshot();
    void setDirty (bool nowDirty);
    void updateTransportButtons();
    void updatePositionLabel();
    void updateSampleRateWarning();
    void logDeviceIfChanged();   // デバイス名・SR・ブロックサイズの確定/変化をログ（Timerから）
    void pollAudioAnomalies();   // オーディオスレッドの異常atomicを回収し、まとめてログ（Timerから）

    // 宣言順 = 初期化順。engine は transport/snapshots への参照を取るので後に置く
    TransportState transport;
    SnapshotExchange snapshots;
    std::unique_ptr<Project> project;
    SynthBank synthBank; // メッセージスレッド専用。MIDIトラックのGM音源を管理
    UndoStack undoStack; // 構造編集のundo/redo（メッセージスレッド専用）
    PreviewFifo previewFifo;
    PlaybackEngine engine { transport, snapshots, previewFifo };

    TimelineView timeline { transport };
    TrackHeadersView headers;
    PianoRollView pianoRoll { transport };

    juce::TextButton playButton, recordButton, addTrackButton, settingsButton;
    juce::ToggleButton clickButton;
    juce::Label bpmCaption, bpmValue, positionLabel, srWarningLabel;

    juce::Rectangle<int> meterArea;
    float meterLevel = 0.0f; // 描画用（メッセージスレッド専用）

    int selectedTrack = -1;
    bool dirty = false;
    bool focusGrabbed = false;

    // 再生中の ,/. シークは一時停止し、キーが離れて少し経ってから自動再開する（timerCallbackで判定）。
    // 押下継続の判定は文字でなくkeyPressedで受けたキーコードで行う（非US配列で<>に化けても追跡できるように）。
    // ,と.の同時押しでも取りこぼさないよう、再開待ち中に認識したキーコードを全部保持する
    bool seekResumePending = false;
    juce::uint32 lastSeekKeyMs = 0;
    static constexpr int maxSeekKeyCodes = 4;
    int seekKeyCodes[maxSeekKeyCodes] = {};
    int numSeekKeyCodes = 0;

    // ログ用の前回値・集約カウンタ（メッセージスレッド専用）
    double loggedSampleRate = -1.0;
    int loggedBlockSize = -1;
    juce::String loggedDeviceName;
    int pendingMidiDrops = 0;
    int pendingRecordDrops = 0;
    int anomalyFlushTicks = 0;

    // 録音中のクリップ情報（停止時にクリップ化する）
    juce::File pendingRecordFile;
    juce::int64 pendingPunchIn = 0;
    int pendingRecordTrack = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
