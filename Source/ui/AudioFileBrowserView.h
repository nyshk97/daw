#pragma once

#include <functional>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/AudioBrowserNavigation.h"
#include "BreadcrumbBar.h"
#include "FileSortOrder.h"
#include "IconButton.h"
#include "PreviewPolicy.h"

class AudioFileBrowserView final : public juce::Component,
                                   private juce::ListBoxModel,
                                   private juce::ChangeListener,
                                   private juce::Timer
{
public:
    AudioFileBrowserView();
    ~AudioFileBrowserView() override;

    std::function<void (const juce::File&)> onPreviewRequested;
    std::function<void()> onPreviewStopRequested;
    std::function<void (const juce::File&)> onImportRequested;

    void setPreviewState (bool loading, bool playing, const juce::String& error);
    void setImporting (bool importing);
    // 曲の再生中はオートプレビューしない（手動▶は走行中でも鳴らせる）
    void setTransportRunning (bool running);
    // パネルを閉じる・取り込みを始めるとき、予約中のオートプレビューごと畳む
    void cancelPreview();
    void stopPreviewUi();

    // 履歴の戻る/進む（⌘[ / ⌘]）。移動できたらtrue
    bool navigateHistory (int delta);

    void paint (juce::Graphics& g) override;
    void resized() override;

    // listBox配下のマウスを監視してホバー行を追う（イベントは奪わない＝選択・D&Dはそのまま）
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseWheelMove (const juce::MouseEvent& event,
                         const juce::MouseWheelDetails& wheel) override;

private:
    class AudioFilter final : public juce::FileFilter
    {
    public:
        AudioFilter();
        bool isFileSuitable (const juce::File& file) const override;
        bool isDirectorySuitable (const juce::File& file) const override;
    };

    class RowIconComponent;

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                           bool selected) override;
    void selectedRowsChanged (int row) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    juce::Component* refreshComponentForRow (int row, bool selected,
                                             juce::Component* existing) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;
    bool mayDragToExternalWindows() const override { return false; }
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override; // オートプレビューのデバウンス満了

    void navigate (const juce::File& directory, bool addToHistory);
    void refreshSelection();
    void setSortOrder (FileSortOrder::Mode mode);
    void updateSortTooltip();
    void updateAutoPreviewButton();
    void rebuildDisplayOrder();
    juce::File fileAt (int row) const;
    bool fileInfoAt (int row, juce::DirectoryContentsList::FileInfo& info) const;
    int rowForFile (const juce::File& file) const;
    juce::File selectedFile() const;
    juce::String durationText (const juce::File& file);
    bool isPlayable (const juce::File& file) const;
    // PreviewPolicy の判定を実際の再生・タイマー・再描画に落とす
    void apply (const PreviewPolicy::Result& result);
    void repaintRowFor (const juce::File& file);
    void updateHover (const juce::MouseEvent& event);
    void setHoveredRow (int row);
    void iconClicked (int row);

    static constexpr int autoPreviewDelayMs = 200;
    static constexpr int iconAreaX = 9;      // アイコンを描く枠の左端
    static constexpr int iconAreaWidth = 22; // アイコンを描く枠の幅
    static constexpr int iconHitWidth = 34;  // クリックを受ける幅（実マウスで狙える24px以上）
    static constexpr int footerHeight = 56;  // 選択ファイル名・長さ・状態を出す下段

    PreviewPolicy policy;
    FileSortOrder::Mode sortOrder = FileSortOrder::Mode::dateAdded; // セッション内のみ保持
    juce::Array<int> displayOrder;                                  // 表示行 → contents のindex
    bool suppressAutoPreview = false; // 行アイコン由来の選択変更でオート予約を立てないためのガード
    int hoveredRow = -1;

    AudioFilter filter;
    juce::TimeSliceThread directoryThread { "Audio browser directory scan" };
    juce::DirectoryContentsList contents { &filter, directoryThread };
    juce::AudioFormatManager formats;
    AudioBrowserNavigation::History history;
    bool importInProgress = false;

    BreadcrumbBar breadcrumb;
    IconButton sortButton { IconButton::Icon::sort, "Sort order" };
    IconButton autoPreviewToggle { IconButton::Icon::speaker, "Auto preview" };
    juce::ListBox listBox { {}, this };
    juce::Label selectedNameLabel;
    juce::Label durationLabel;
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioFileBrowserView)
};
