#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "audio/PlayerEngine.h"
#include "audio/RegionExport.h"
#include "audio/StemSeparator.h"
#include "shared/SalvaSettings.h"
#include "ui/SalvaLookAndFeel.h"
#include "ui/RecordView.h"
#include "ui/StemPanel.h"
#include "ui/WaveformView.h"

// Salvaのメイン画面（1カラム: ヘッダー / 波形 / トランスポート＋BPM / 下部バー）。
// レイアウトは2026-08-15確定のHTMLモックに従う。ステムM/S行（縦リスト＋レベルメーター）は
// Phase 5、録音ビュー（波形と入れ替え）はPhase 4で足す。
// 判断ロジック（BPM逆算・ファイル名）は shared/BpmMath.h にあり、ここは表示だけ
class SalvaMainComponent : public juce::Component,
                           public juce::FileDragAndDropTarget,
                           private juce::Timer
{
public:
    SalvaMainComponent();
    ~SalvaMainComponent() override;

    void openFile (const juce::File& file);
    // ウィンドウの×は「1段戻る」: 録音モード退出 or ファイルを閉じてスタート画面へ。
    // true = ここで消費（アプリは終了しない）。スタート画面でのみ false（=終了してよい）
    bool handleCloseRequest();

#if JUCE_DEBUG
    // dev版の自動動作確認用（Main.cppの --autoplay / --select / --stemgroup）。実機能と同一経路を通す
    void autoPlayForVerification() { togglePlay(); }
    void selectForVerification (juce::int64 start, juce::int64 end)
    {
        waveform.setSelection (start, end);
        selectionChanged (waveform.selectionStart(), waveform.selectionEnd());
    }
    void selectStemGroupForVerification (int index) { stemPanel.selectGroup (index); }
    void separateForVerification() { startSeparation(); }
    void exportForVerification() { startExport(); }
    // ポップアップの見た目確認用（--popup-cache / --popup-output）。実機能と同一経路
    void showCacheMenuForVerification() { showCacheDeleteMenu(); }
    void showOutputPopupForVerification() { outputDeviceBox.showPopup(); }
#endif

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;

private:
    void timerCallback() override;
    void togglePlay();
    void toggleRecordMode();
    void toggleRecording();
    void finishRecording();
    void selectionChanged (juce::int64 start, juce::int64 end);
    void selectionCleared();
    void updateBpmDisplay();
    // --- ステム分離・M/S・書き出し（Phase 5） ---
    juce::File resolveVenvPython() const; // 設定上書き→既定 ~/daw/tools/reference/.venv
    void startSeparation();
    void refreshStemCacheState();  // 現在ファイルのmanifest検出→パネル反映
    void applyStemConfig();        // パネルのグループ/M/S → engineへ
    void updateCacheSizeLabel();
    void showCacheDeleteMenu();
    void startExport();
    void showToast (const juce::String& message);
    void updateHeader();
    void rebuildOutputDeviceBox();
    void openRecentAt (int index);
    void openFileChooser();          // 「ファイルを選択…」ボタン・⌘O
    void closeFileToStart();         // ファイルを閉じてスタート画面（空状態）へ
    void updateSeparateButtonState(); // ファイル有無・録音モード・分離中から算出

    // --- 空状態（Vinyl Warm・2026-08-15確定モック案B） ---
    // レイアウト計算を描画・ボタン配置・ヒットテストで共有する（座標ズレ防止）
    struct EmptyLayout
    {
        juce::Rectangle<int> disc, title, cardFile, cardRecord, recentHeader, rowsTop;
        int visibleRows = 0; // エリアに収まる最近ファイル行数
    };
    EmptyLayout computeEmptyLayout() const;
    juce::Rectangle<int> emptyRecentRowRect (const EmptyLayout& el, int index) const;
    void refreshShownRecentFiles();
    void paintEmptyState (juce::Graphics& g, juce::Rectangle<int> area);
    void paintDisc (juce::Graphics& g, juce::Rectangle<float> bounds);

    // 選択の秒数（BPM逆算の入力）
    double selectionSeconds() const;
    int currentBeats() const;

    PlayerEngine engine;
    SalvaSettings settings;
    SalvaLookAndFeel salvaLnf; // Vinyl Warmテーマ（ボタン・コンボ・ポップアップメニューの描画）

    WaveformView waveform;
    RecordView recordView;
    std::unique_ptr<juce::FileChooser> recordFileChooser;
    bool recordMode = false;

    juce::TextButton playButton { juce::String::fromUTF8 (u8"▶") };
    juce::Label timeLabel;
    juce::TextButton barsButton;
    juce::Label bpmLabel;
    juce::Label exportNameLabel;
    juce::TextButton exportButton;
    juce::Label outputLabel;
    juce::ComboBox outputDeviceBox;
    juce::TextButton recordModeButton;
    juce::TextButton openFileButton;    // 空状態のみ表示（D&D以外の入口）
    juce::TextButton recordEntryButton; // 空状態のみ表示（録音モードへの導線）
    juce::Label starvedLabel; // read-ahead枯渇の可視化（原因調査の入口。通常は非表示）

    // ステム分離（ヘッダー）
    juce::TextButton separateButton;
    juce::Label separateProgressLabel;
    juce::Label cacheSizeLabel;
    StemPanel stemPanel;
    StemSeparator separator;
    StemCache::Manifest currentManifest; // 現在ファイルの検証済みmanifest（無効=未分離）
    juce::String activeGroupId;          // engineへロード済みのグループ（切替検出用）
    ExportWorker exportWorker;
    juce::Label toastLabel;
    int toastTicks = 0;

    int beatsOverride = 0; // 0 = 自動（BpmMath::autoBeats）
    juce::uint64 lastStarved = 0;
    bool initialFocusDone = false;
    juce::Rectangle<int> emptyStateArea; // 最近のファイル行のクリック判定用
    juce::StringArray shownRecentFiles;
    bool dragOver = false;   // ファイルドラッグ中（盤面の回転・「ここにドロップ」表示）
    float discAngle = 0.0f;  // 盤面の回転角（ドラッグ中のみ進む）
    int hoveredRecent = -1;  // 最近ファイル行のホバー中インデックス（-1 = なし）
    int hoveredCard = -1;    // 入口カードのホバー（0 = ファイルから, 1 = レコードから）

    void mouseUp (const juce::MouseEvent& e) override; // 空状態の最近ファイル行クリック
    void mouseMove (const juce::MouseEvent& e) override; // 行ホバーの追跡
    void mouseExit (const juce::MouseEvent& e) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SalvaMainComponent)
};
