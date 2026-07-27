#pragma once

#include <memory>
#include <juce_audio_utils/juce_audio_utils.h>

#include "AddTrackOverlay.h"
#include "BottomPanelHistory.h"
#include "BounceOverlay.h"
#include "DeviceSettingsWindow.h"
#include "FxDetailView.h"
#include "FxEditorView.h"
#include "InstrumentDetailView.h"
#include "IconButton.h"
#include "MixerWindow.h"
#include "PianoRollView.h"
#include "RightPanel.h"
#include "ShortcutListOverlay.h"
#include "TimelineView.h"
#include "TrackHeadersView.h"
#include "TransportLcd.h"
#include "UrlImportOverlay.h"
#include "../audio/AudioImporter.h"
#include "../audio/AudioFilePreview.h"
#include "../audio/BounceRenderer.h"
#include "../audio/PlaybackEngine.h"
#include "../audio/UrlDownloader.h"
#include "../shared/PreviewFifo.h"
#include "../shared/PlaybackSnapshot.h"
#include "../shared/Project.h"
#include "../shared/SynthBank.h"
#include "../shared/TransportState.h"
#include "../shared/UndoStack.h"

// 1プロジェクト分のメイン画面。AudioAppComponent のオーディオコールバックは
// PlaybackEngine（audio/）への転送だけを行い、ここに処理を書かない。
class MainComponent : public juce::AudioAppComponent,
                      public juce::DragAndDropContainer,
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

    // オーディオファイルの取り込み。メニューから呼ばれる入口と、閉じる/終了フロー用の中断API
    void startImportFlow();
    bool isImporting() const { return importActive; }
    void cancelImportForClose(); // 取り込み中ならキャンセル→ワーカーjoin→一時ファイル削除まで待つ

    // URLからの取り込み（yt-dlp）。URL入力中とダウンロード中の両方を isUrlImporting() が指す
    // （どちらの状態でも他の取り込み・書き出しを弾き、Fileメニューをdisableにする）
    void startUrlImportFlow();
    bool isUrlImporting() const { return urlStage != UrlStage::idle; }
    void cancelUrlImportForClose(); // DL中ならキャンセル→join→一時ディレクトリ削除まで待つ
    // 前回クラッシュ等で残った一時ディレクトリの掃除。アプリ起動時に1回呼ぶ
    // （MainComponent生成前＝選択画面の時点で走らせたいので static かつ public）
    static void sweepStaleUrlTempDirs();
    juce::String windowTitle() const;
    std::function<void (const juce::String&)> onTitleChanged;
    std::function<void()> onOpenChooserRequested; // ⌘O: プロジェクトを閉じて選択画面へ（未保存確認はMainWindow側）

private:
    void timerCallback() override;

    void togglePlay();
    void toggleRecord();
    void startRecordingFlow();
    void finishRecording();
    // ---- 再生位置（ヘッド＝今いる場所・編集の基準 / 開始位置＝次に鳴る場所）----
    void locate (juce::int64 samplePos);       // 両方を同時に動かす（クリックシーク・キーシーク・サイクル頭補正）
    void setPlayStart (juce::int64 samplePos); // 開始位置だけ動かす（録音まわり専用。ヘッドはエンジンが動かす）
    void seekByStep (int direction, bool wholeBar, int keyCode);  // ,/.キー: 1拍（Shiftで1小節）単位で再生ヘッドを移動
    void seekToSection (int direction, int keyCode);              // ⌥,/.キー: 前/次のセクション頭へ（厳密に前/次の境界）
    void pauseForKeySeek (int keyCode);  // キーシーク共通: 再生中なら一時停止し、自動再開の監視キーに登録する
    void toggleCycle();                  // Cキー: サイクル（ループ範囲）の入/切。範囲未設定ならno-op
    void syncCycleToTransport();         // Project のサイクル範囲をサンプル換算して TransportState へ書く
    void toggleMuteSelectedTrack();      // mキー
    void toggleSoloTracks();             // sキー: ソロ中なら全解除、なければ直近のソロ構成を再適用
    void toggleMuteSelectedItem();       // Ctrl+M: 選択中のクリップ/リージョンをミュート
    void splitSelectedItemAtPlayhead();  // ⌘T: 選択中のクリップ/リージョンを再生ヘッド位置で分割
    void repeatSelectedItem();           // ⌘R: 選択中のクリップ/リージョンを終端直後へ複製（連打で伸ばせる）
    bool copySelectedItem();             // ⌘C: 選択中のクリップ/リージョンをクリップボードへ（録音中も可）
    bool pasteItemAtPlayhead();          // ⌘V: 再生ヘッド位置×選択トラックへ貼る（型が合わなければ何もしない）
    void requestDeleteSelectedClip();    // 選択を読んで requestDeleteClipAt に渡す薄いラッパー
    void deleteSelectedRegion();         // 同上（deleteRegionAt へ）
    void requestDeleteClipAt (int trackIndex, int clipIndex);   // 確認ダイアログあり
    void deleteRegionAt (int trackIndex, int regionIndex);      // MIDIリージョンは確認なしで削除（undo可能なので）
    void requestDeleteTrack (int index);
    void reorderTrack (int from, int to); // ヘッダのドラッグ並び替え（to = 挿入先の隙間番号 0..tracks.size()）
    void showAddTrackMenu();
    void addTrack (TrackType type);
    void selectTrack (int index);         // 内部同期用（削除後の詰め直し・undo復元等）。FXエディタの表示対象は変えない
    void selectTrackFromUser (int index); // ユーザーのトラック選択（ヘッダー/タイムライン/ミキサー）。FXエディタも追従させる
    bool selectedTrackIsMidi() const;
    void performUndo();
    void performRedo();
    // undo/redo後のUI・スナップショット同期。kind がサンプル値だけの復元ならスナップショットを
    // 再pushしない（発音中の音を切らないため。詳細は UndoStack::EditKind のコメント）
    void afterHistoryRestore (UndoStack::EditKind kind);
    void resetTrackPeakHolds();          // トラック構造変更時（削除・並び替え・undo等）に呼ぶ
    void openPianoRoll (int trackIndex, int regionIndex);
    void closePianoRoll();
    void toggleFxEditor();               // Iキー（左のFXパネル）
    void openFxEditor();
    void closeFxEditor();                // パネルを閉じるときは下部詳細も道連れ
    void toggleFxDetailSlot (int slot);  // FXパネルのスロットクリック（下部詳細の開閉）
    void closeFxDetail();
    void syncFxDetail();                 // FXパネルの表示対象変更に下部詳細を追従（不整合なら閉じる）
    void updateFxDetailBody();           // 表示中スロットに応じて下部詳細の中身を載せ替える
    // ---- 下部エリアの表示トグル・履歴（E / [ / ]）----
    void toggleBottomPanel();                    // E: 開いていれば閉じ、閉じていれば直前の中身を復元
    void navigateBottomHistory (int direction);  // [ / ]: 履歴を戻る（-1）/進む（+1）
    // 履歴エントリを実際に開く。成功でtrue。既存のopen系関数を再利用するため、
    // 復元中は suppressHistoryPush で積み直しを止める
    bool restoreBottomEntry (const BottomPanelHistory::Entry& entry);
    bool bottomEntryIsValid (const BottomPanelHistory::Entry& entry) const; // 対象がまだ存在するか
    BottomPanelHistory::Entry currentPianoRollEntry() const;                // 表示中のピアノロール→エントリ
    BottomPanelHistory::Entry currentFxDetailEntry (int slot) const;        // 表示中のFX詳細→エントリ
    int trackIndexForId (juce::uint64 trackId) const;                       // 見つからなければ -1
    void toggleRightPanel (RightPanel::Mode mode);
    void closeRightPanel();
    void toggleDeviceSettings (const char* source); // 歯車ボタン／⌘,（開いていれば閉じる。sourceはログ用）
    void closeDeviceSettings();
    void applyBpmText();
    void beginBounce (const juce::File& target); // 保存先確定後: パラメータ固定→専用synth生成→ワーカー開始
    void exportSelectedItem();                   // ⌘E: 選択中のリージョン/クリップを書き出し（regionSelection優先）
    void startRegionExportFlow (int trackIndex, int itemIndex);  // リージョン書き出しの入口（保存ダイアログまで）
    void beginRegionBounce (const juce::File& target, int trackIndex, int itemIndex);
    void stopPlaybackForBounce();                // 書き出し前の再生停止（⌘B/⌘E共通）
    bool startBounceRequest (BounceRenderer::Request&& request); // レンダラー起動＋オーバーレイ表示（共通の尻尾）
    void pollBounce();                           // Timerからの完了ポーリング・進捗反映
    // 取り込みの実体。targetTrack = -1 で新規オーディオトラックを作成して配置。
    // othersSkipped = 複数ドロップの先頭のみ処理したとき（完了表示の文言に反映）。
    // displayName = 空でファイル名から作る（URL取り込みでは動画タイトルを渡す）。
    // 戻り値 = ワーカーを開始できたか（ガードに掛かった・SR未確定・開始失敗で false）
    bool startImport (const juce::File& source, int targetTrack, juce::int64 startSample,
                      bool othersSkipped = false, const juce::String& displayName = {});
    // MIDIトラックへのサンプル音源の割り当て（元のSRを保って取り込む）
    void startInstrumentImport (const juce::File& source, int trackIndex, bool othersSkipped = false);
    // 取り込みワーカーの起動（クリップ・サンプル共通の尻尾）。targetRate <= 0 で元SR保持
    bool beginImportWorker (const juce::File& source, double targetRate, bool othersSkipped,
                            const juce::String& displayName);
    // プロジェクトSRの確定（未確定ならデバイスレートで決める）。デバイス未準備なら false
    bool ensureProjectSampleRate (double& targetRate);
    void pollImport();                                    // Timerからの完了ポーリング・進捗反映
    void pollUrlImport();                                 // 同上（URLダウンロード）
    void cleanupUrlTempDir();                             // URL取り込みの一時ディレクトリを畳む
    void finishImport (const AudioImporter::Result& result); // 成功時: リネーム→クリップ/トラック追加→保存
    void finishInstrumentImport (const AudioImporter::Result& result); // 同上（サンプル音源の割り当て）
    static void refreshMacMenu();                // Fileメニューのenable状態を組み直させる
    void pushSnapshot();
    // オーディオ側の値（クリップゲイン）だけが変わったときの差し替え。MIDIの構成世代を据え置くので
    // エンジンの消音＋跨ぎノート再発音が走らず、鳴っているMIDIを乱さずに音を更新できる。
    // 呼び出し側は「ノート・リージョン・トラック構成・音源を変えていないこと」を保証すること
    void pushAudioValueSnapshot();
    void pushSnapshotWithChange (Project::SnapshotChange change); // 上記2つの共通尻尾（synth参照を埋めて渡す）
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
    AudioFilePreview filePreview;
    PlaybackEngine engine { transport, snapshots, previewFifo };

    TimelineView timeline { transport };
    TrackHeadersView headers;
    PianoRollView pianoRoll { transport };
    FxEditorView fxEditor; // 左のFXパネル（概要・基本常設・Iで開閉。ピアノロールとは独立）
    FxDetailView fxDetail; // 下部のFX詳細（スロットクリックで開く。ピアノロールと排他・後勝ち）
    // 下部詳細に載せる中身（fxDetailは非所有なので、こちらが所有してfxDetailより長生きさせる）
    InstrumentDetailView instrumentDetail;
    int fxDetailSlot = -1;        // 詳細が表示中のスロット（FXパネルの並びに対応）
    juce::String fxDetailKey;     // 詳細が対象にしているチャンネル（fxEditor.targetKey()と比較して追従判定）

    // 下部エリアで何を見ていたかの履歴（E で復元・[ / ] で行き来）。セッション内のみ保持。
    // suppressHistoryPush は履歴からの復元中だけ立てる（復元は既存のopen系関数を再利用するので、
    // そのままだと復元自体が履歴に積まれてしまう）
    BottomPanelHistory bottomHistory;
    bool suppressHistoryPush = false;

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

    class RightResizeBar : public juce::Component
    {
    public:
        std::function<void()> onDragStart;
        std::function<void (int)> onDragged;

        RightResizeBar() { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); }
        void mouseDown (const juce::MouseEvent&) override { if (onDragStart) onDragStart(); }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (onDragged)
                onDragged (e.getDistanceFromDragStartX());
        }
    };

    RightPanel rightPanel;
    RightResizeBar rightResizeBar;
    int rightPanelWidth = 300;
    int rightWidthAtDragStart = 300;
    static constexpr int rightPanelMinWidth = 240;
    static constexpr int rightPanelMaxWidth = 480;

    IconButton playButton { IconButton::Icon::play, juce::String::fromUTF8 (u8"再生") };
    IconButton recordButton { IconButton::Icon::record, juce::String::fromUTF8 (u8"録音") };
    IconButton clickButton { IconButton::Icon::metronome, juce::String::fromUTF8 (u8"クリック") };
    IconButton settingsButton { IconButton::Icon::gear, juce::String::fromUTF8 (u8"オーディオ設定") };
    IconButton notesButton { IconButton::Icon::notes, juce::String::fromUTF8 (u8"プロジェクトメモ") };
    IconButton filesButton { IconButton::Icon::folder, juce::String::fromUTF8 (u8"オーディオファイル") };
    IconButton addTrackButton { IconButton::Icon::plus, juce::String::fromUTF8 (u8"トラックを追加") };
    AddTrackOverlay addTrackOverlay;
    ShortcutListOverlay shortcutOverlay; // ⌘?のショートカット一覧（表示中のみ可視）
    BounceOverlay bounceOverlay;         // バウンス進捗（表示中のみ可視・モーダル）
    MixerWindow mixerWindow;             // Xのミキサー（独立ウィンドウ。移動・リサイズ自由）
    // 歯車ボタン／⌘,のデバイス設定（開いている間だけ生存。入力レベル測定を常駐させないため閉じたら破棄）
    std::unique_ptr<DeviceSettingsWindow> deviceSettingsWindow;
    TransportLcd lcd; // BPM・小節位置・時間のLCD風パネル（バー中央に置く）
    juce::Label srWarningLabel;
    juce::TooltipWindow tooltipWindow { this }; // アイコンのみのボタン（歯車等）のホバー説明用

    static constexpr int topBarHeight = 54; // paint（グラデーション帯）とresizedで共有
    juce::Rectangle<float> topBarSeparator; // 右上ボタン群の区切り線（resizedで算出しpaintで描く）

    // ⌘C/⌘V のクリップボード（アプリ内メモリ・セッション内のみ。システムのクリップボードとは無関係）。
    // MidiRegion / Clip を丸ごと値で持つ（フィールドを詰め替えると、後からモデルに項目が
    // 増えたときに取りこぼす）。ペーストで差し替えるのは開始位置とIDだけ。
    // MainComponent は1プロジェクトにつき1つ生成されるので、プロジェクトを閉じれば一緒に消える。
    // オーディオクリップを持っている間は fileName を保存時のGC保護リストへ渡す（trySave）
    struct ItemClipboard
    {
        enum class Kind { none, midiRegion, audioClip };
        Kind kind = Kind::none;
        MidiRegion region; // kind == midiRegion のときだけ有効
        Clip clip;         // kind == audioClip のときだけ有効
    };
    ItemClipboard itemClipboard;

    int selectedTrack = -1;
    bool dirty = false;
    std::vector<juce::uint64> lastSoloIds; // sキーで解除したソロ構成（トラックID）。次のsで再適用する。セッション内のみ保持
    bool focusGrabbed = false;
    juce::String previewError;

    // メーター値の配布用（timerCallbackで毎tick詰め直す。peakL/peakRのexchange(0)は
    // ヘッダー・ミキサー・FXパネルで取り合わないようここで一元的に行う）
    std::vector<StereoPeak> meterPeaks;
    // ミキサー・FXパネルのdB数値表示用の配布（maxSincePlayは再生開始エッジでリセットし蓄積。
    // 表示側でなくここで蓄積するのは、ミキサー非表示中に鳴った分を取りこぼさないため）
    std::vector<MeterFeed> meterFeeds;
    MeterFeed busFeeds[numSendBuses];
    MeterFeed masterFeed;
    bool meterWasPlaying = false;

    // 再生中の ,/. シークは一時停止し、キーが離れて少し経ってから自動再開する（timerCallbackで判定）。
    // 押下継続の判定は文字でなくkeyPressedで受けたキーコードで行う（非US配列で<>に化けても追跡できるように）。
    // ,と.の同時押しでも取りこぼさないよう、再開待ち中に認識したキーコードを全部保持する
    bool seekResumePending = false;
    juce::uint32 lastSeekKeyMs = 0;

    // 次に再生・録音が始まる位置（ルーラーに三角マーカーで表示される）。再生ヘッドとは別物で、
    // 停止してもここは動かない＝ヘッドは止めた場所に残り、次の再生はここから始まる。
    // 直接代入せず locate() / setPlayStart() を通す（片方だけ動かす漏れを防ぐため）
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

    // オーディオファイルの取り込み（変換コピー）。実行中は importOverlay がモーダルに塞ぎ、
    // 開始時に保持した配置先（トラック・位置）が完了まで有効であることを保証する。
    // ワーカーは一時ファイルにのみ書き、最終名へのリネームとモデル反映は finishImport が
    // メッセージスレッドで一続きに行う
    AudioImporter audioImporter;
    BounceOverlay importOverlay; // 取り込み進捗（BounceOverlayを文言差し替えで流用・モーダル）
    bool importActive = false;
    int importDoneTicks = 0;
    juce::File importTempFile;
    juce::String importDisplayName; // 元ファイル名（拡張子なし）。クリップ表示名・新規トラック名・サンプル名に使う
    bool importIsInstrument = false; // true = MIDIトラックへのサンプル音源割り当て（完了処理が別）
    int importTargetTrack = -1;     // -1 = 新規トラックを作成（サンプル割り当てでは必ず既存MIDIトラック）
    juce::int64 importStartSample = 0;
    std::unique_ptr<juce::FileChooser> importChooser; // 非同期ダイアログの生存保持
    // 進捗バーの割り当て。通常の取り込みは 0.0〜1.0、URL取り込みではダウンロード分を
    // 前半に取るので 0.7〜1.0 を AudioImporter に割り当てる（オーバーレイは1枚を通しで使う）
    float importProgressBase = 0.0f;
    float importProgressSpan = 1.0f;

    // URLからの取り込み（yt-dlp）。DL完了後は落ちてきたWAVを startImport() に渡すだけで、
    // 取り込み自体は既存経路（AudioImporter → finishImport）に相乗りする
    enum class UrlStage { idle, enteringUrl, downloading };
    UrlStage urlStage = UrlStage::idle;
    UrlDownloader urlDownloader;
    UrlImportOverlay urlOverlay;
    // ダウンロード成果物の置き場。worker から所有権を受け取ってから、取り込みが
    // 終わる（成功・失敗・キャンセルいずれでも）まで MainComponent が持つ
    juce::File urlTempDir;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
