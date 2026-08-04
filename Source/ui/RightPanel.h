#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"
#include "AudioFileBrowserView.h"
#include "GachaPanelView.h"

// メモ本文の入力欄。フォーカスは枠の色でなく地の明度で示す
// （面積が大きく四方を囲むため、青枠だとパネル内で枠が主役になってしまう）。
// 枠はJUCEデフォルトだとフォーカス時に2pxへ太るので、自前で1pxに固定する
class MemoEditor : public juce::TextEditor
{
public:
    MemoEditor();

    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType cause) override;
    void paintOverChildren (juce::Graphics& g) override;
};

// メイン画面右側のドック。内容の切替状態はセッション内だけ保持し、
// プロジェクトにはメモ本文だけを保存する。
class RightPanel : public juce::Component
{
public:
    enum class Mode { notes, files, gacha };

    RightPanel();

    void setProject (Project* projectToUse);
    void open (Mode newMode);
    void close();

    bool isOpen() const { return openState; }
    Mode mode() const { return currentMode; }
    void focusNotesEditor();
    AudioFileBrowserView& fileBrowser() { return browser; }
    GachaPanelView& gachaPanel() { return gacha; }

    std::function<void()> onMemoChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void refreshMode();

    Project* project = nullptr;
    bool openState = false;
    bool bindingMemo = false;
    Mode currentMode = Mode::notes;

    juce::Label titleLabel;
    juce::Label scopeLabel;
    MemoEditor memoEditor;
    AudioFileBrowserView browser;
    GachaPanelView gacha;

    static constexpr int headerHeight = 42;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RightPanel)
};
