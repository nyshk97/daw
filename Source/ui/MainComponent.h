#pragma once

#include <memory>
#include <juce_audio_utils/juce_audio_utils.h>

#include "AddTrackOverlay.h"
#include "BounceOverlay.h"
#include "FxDetailView.h"
#include "FxEditorView.h"
#include "IconButton.h"
#include "MixerOverlay.h"
#include "PianoRollView.h"
#include "ShortcutListOverlay.h"
#include "TimelineView.h"
#include "TrackHeadersView.h"
#include "TransportLcd.h"
#include "../audio/BounceRenderer.h"
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

    // バウンス（書き出し）。メニュー（⌘B）から呼ばれる入口と、閉じる/終了フロー用の中断API
    void startBounceFlow();
    bool isBouncing() const { return bounceActive; }
    void cancelBounceForClose(); // バウンス中ならキャンセル→ワーカーjoin→一時ファイル削除まで待つ
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
    void toggleSoloTracks();             // sキー: ソロ中なら全解除、なければ直近のソロ構成を再適用
    void toggleMuteSelectedItem();       // Ctrl+M: 選択中のクリップ/リージョンをミュート
    void splitSelectedItemAtPlayhead();  // ⌘T: 選択中のクリップ/リージョンを再生ヘッド位置で分割
    void requestDeleteSelectedClip();    // 選択を読んで requestDeleteClipAt に渡す薄いラッパー
    void deleteSelectedRegion();         // 同上（deleteRegionAt へ）
    void requestDeleteClipAt (int trackIndex, int clipIndex);   // 確認ダイアログあり
    void deleteRegionAt (int trackIndex, int regionIndex);      // MIDIリージョンは確認なしで削除（undo可能なので）
    void requestDeleteTrack (int index);
    void showAddTrackMenu();
    void addTrack (TrackType type);
    void selectTrack (int index);         // 内部同期用（削除後の詰め直し・undo復元等）。FXエディタの表示対象は変えない
    void selectTrackFromUser (int index); // ユーザーのトラック選択（ヘッダー/タイムライン/ミキサー）。FXエディタも追従させる
    bool selectedTrackIsMidi() const;
    void performUndo();
    void performRedo();
    void afterHistoryRestore();          // undo/redo後のUI・スナップショット同期
    void openPianoRoll (int trackIndex, int regionIndex);
    void closePianoRoll();
    void toggleFxEditor();               // Iキー（左のFXパネル）
    void openFxEditor();
    void closeFxEditor();                // パネルを閉じるときは下部詳細も道連れ
    void toggleFxDetailSlot (int slot);  // FXパネルのスロットクリック（下部詳細の開閉）
    void closeFxDetail();
    void syncFxDetail();                 // FXパネルの表示対象変更に下部詳細を追従（不整合なら閉じる）
    void showDeviceSettings();
    void applyBpmText();
    void beginBounce (const juce::File& target); // 保存先確定後: パラメータ固定→専用synth生成→ワーカー開始
    void pollBounce();                           // Timerからの完了ポーリング・進捗反映
    static void refreshMacMenu();                // Fileメニューのenable状態を組み直させる
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
    FxEditorView fxEditor; // 左のFXパネル（概要・基本常設・Iで開閉。ピアノロールとは独立）
    FxDetailView fxDetail; // 下部のFX詳細（スロットクリックで開く。ピアノロールと排他・後勝ち）
    int fxDetailSlot = -1;        // 詳細が表示中のスロット（FXパネルの並びに対応）
    juce::String fxDetailKey;     // 詳細が対象にしているチャンネル（fxEditor.targetKey()と比較して追従判定）

    // 下部パネル（ピアノロール/FX詳細）の高さを変えるドラッグハンドル（パネル上端の細い帯）。
    // 高さは両パネル共通・セッション内で保持
    class BottomResizeBar : public juce::Component
    {
    public:
        std::function<void()> onDragStart;
        std::function<void (int)> onDragged; // ドラッグ開始からの累計Δy（上方向が負）

        BottomResizeBar() { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); }
        void mouseDown (const juce::MouseEvent&) override { if (onDragStart) onDragStart(); }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (onDragged)
                onDragged (e.getDistanceFromDragStartY());
        }
    };
    BottomResizeBar bottomResizeBar;
    int bottomPanelHeight = 320;      // 既定をピアノロール従来値（260）より広めに
    int bottomHeightAtDragStart = 0;
    static constexpr int bottomPanelMinHeight = 180;

    IconButton playButton { IconButton::Icon::play, juce::String::fromUTF8 (u8"再生") };
    IconButton recordButton { IconButton::Icon::record, juce::String::fromUTF8 (u8"録音") };
    IconButton clickButton { IconButton::Icon::metronome, juce::String::fromUTF8 (u8"クリック") };
    IconButton settingsButton { IconButton::Icon::gear, juce::String::fromUTF8 (u8"オーディオ設定") };
    IconButton addTrackButton { IconButton::Icon::plus, juce::String::fromUTF8 (u8"トラックを追加") };
    AddTrackOverlay addTrackOverlay;
    ShortcutListOverlay shortcutOverlay; // ⌘?のショートカット一覧（表示中のみ可視）
    BounceOverlay bounceOverlay;         // バウンス進捗（表示中のみ可視・モーダル）
    MixerOverlay mixerOverlay;           // Xのミキサー（表示中のみ可視・キーは奪わない）
    TransportLcd lcd; // BPM・小節位置・時間のLCD風パネル（バー中央に置く）
    juce::Label srWarningLabel;
    juce::TooltipWindow tooltipWindow { this }; // アイコンのみのボタン（歯車等）のホバー説明用

    static constexpr int topBarHeight = 54; // paint（グラデーション帯）とresizedで共有

    int selectedTrack = -1;
    bool dirty = false;
    std::vector<juce::uint64> lastSoloIds; // sキーで解除したソロ構成（トラックID）。次のsで再適用する。セッション内のみ保持
    bool focusGrabbed = false;

    // ミキサーオーバーレイが覆う領域（ヘッダー＋タイムライン。上部バーと下部パネルは覆わない）。
    // resizedで更新し、Xで開くときに使う
    juce::Rectangle<int> mixerArea;

    // メーター値の配布用（timerCallbackで毎tick詰め直す。peakLevelのexchange(0)は
    // ヘッダーとミキサーで取り合わないようここで一元的に行う）
    std::vector<float> meterPeaks;

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

    // バウンス。ワーカーは専用スナップショット・専用synth・固定済みgainを自己所有し、
    // MainComponentの他メンバーを参照しない（デストラクタでcancelAndWaitされる）
    BounceRenderer bounceRenderer;
    std::unique_ptr<juce::FileChooser> bounceChooser; // 非同期ダイアログの生存保持
    bool bounceActive = false;   // running中のみtrue（完了表示中はfalse）
    int bounceDoneTicks = 0;     // 完了表示の自動クローズ用カウントダウン（30Hz Timer）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
