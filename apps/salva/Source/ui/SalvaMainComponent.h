#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "audio/PlayerEngine.h"
#include "audio/RegionExport.h"
#include "audio/StemSeparator.h"
#include "shared/SalvaSettings.h"
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
#endif

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

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
    void paintEmptyState (juce::Graphics& g, juce::Rectangle<int> area);

    // 選択の秒数（BPM逆算の入力）
    double selectionSeconds() const;
    int currentBeats() const;

    PlayerEngine engine;
    SalvaSettings settings;

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

    void mouseUp (const juce::MouseEvent& e) override; // 空状態の最近ファイル行クリック

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SalvaMainComponent)
};
