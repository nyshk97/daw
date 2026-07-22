#pragma once

#include <memory>
#include <juce_audio_utils/juce_audio_utils.h>

#include "AddTrackOverlay.h"
#include "IconButton.h"
#include "PianoRollView.h"
#include "ShortcutListOverlay.h"
#include "TimelineView.h"
#include "TrackHeadersView.h"
#include "TransportLcd.h"
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
    void finishRecordingForClose(); // 閉じる/終了フロー専用: 録音中なら確定（クリップ化）する。汎用の外部停止APIにはしない
    juce::String windowTitle() const;
    std::function<void (const juce::String&)> onTitleChanged;
    std::function<void()> onOpenChooserRequested; // ⌘O: プロジェクトを閉じて選択画面へ（未保存確認はMainWindow側）

private:
    void timerCallback() override;

    void togglePlay();
    void toggleRecord();
    void startRecordingFlow();
    void finishRecording();
    void seekByStep (int direction, bool wholeBar, int keyCode);  // ,/.キー: 1拍（Shiftで1小節）単位で再生ヘッドを移動
    void seekToSection (int direction, int keyCode);              // ⌥,/.キー: 前/次のセクション頭へ（厳密に前/次の境界）
    void pauseForKeySeek (int keyCode);  // キーシーク共通: 再生中なら一時停止し、自動再開の監視キーに登録する
    void toggleMuteSelectedTrack();      // mキー
    void toggleMuteSelectedItem();       // Ctrl+M: 選択中のクリップ/リージョンをミュート
    void splitSelectedItemAtPlayhead();  // ⌘T: 選択中のクリップ/リージョンを再生ヘッド位置で分割
    void requestDeleteSelectedClip();    // 選択を読んで requestDeleteClipAt に渡す薄いラッパー
    void deleteSelectedRegion();         // 同上（deleteRegionAt へ）
    void requestDeleteClipAt (int trackIndex, int clipIndex);   // 確認ダイアログあり
    void deleteRegionAt (int trackIndex, int regionIndex);      // MIDIリージョンは確認なしで削除（undo可能なので）
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
    void updateLcdTime();
    void updateSampleRateWarning();
    void applyProjectSampleRate(); // デバイスSRをプロジェクトSRに自動で合わせる（Timerから。Logicと同じ挙動）
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

    IconButton playButton { IconButton::Icon::play, juce::String::fromUTF8 (u8"再生") };
    IconButton recordButton { IconButton::Icon::record, juce::String::fromUTF8 (u8"録音") };
    IconButton clickButton { IconButton::Icon::metronome, juce::String::fromUTF8 (u8"クリック") };
    IconButton settingsButton { IconButton::Icon::gear, juce::String::fromUTF8 (u8"オーディオ設定") };
    IconButton addTrackButton { IconButton::Icon::plus, juce::String::fromUTF8 (u8"トラックを追加") };
    AddTrackOverlay addTrackOverlay;
    ShortcutListOverlay shortcutOverlay; // ⌘?のショートカット一覧（表示中のみ可視）
    TransportLcd lcd; // BPM・小節位置・時間のLCD風パネル（バー中央に置く）
    juce::Label srWarningLabel;
    juce::TooltipWindow tooltipWindow { this }; // アイコンのみのボタン（歯車等）のホバー説明用

    static constexpr int topBarHeight = 54; // paint（グラデーション帯）とresizedで共有

    int selectedTrack = -1;
    bool dirty = false;
    bool focusGrabbed = false;

    // 再生中の ,/. シークは一時停止し、キーが離れて少し経ってから自動再開する（timerCallbackで判定）。
    // 押下継続の判定は文字でなくkeyPressedで受けたキーコードで行う（非US配列で<>に化けても追跡できるように）。
    // ,と.の同時押しでも取りこぼさないよう、再開待ち中に認識したキーコードを全部保持する
    bool seekResumePending = false;
    juce::uint32 lastSeekKeyMs = 0;

    // 停止時に戻る再生開始位置（Logicのlast locate position相当）。シーク・録音でも更新される
    juce::int64 playStartSample = 0;
    static constexpr int maxSeekKeyCodes = 4;
    int seekKeyCodes[maxSeekKeyCodes] = {};
    int numSeekKeyCodes = 0;

    // デバイスSRのプロジェクトSR合わせは1デバイスにつき1回だけ試す
    // （ユーザーが設定画面で手動変更したSRと戦わないため。デバイスが替わったらリセットして再適用）
    bool projectRateApplied = false;

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
