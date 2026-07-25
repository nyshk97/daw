#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"
#include "AudioFileBrowserView.h"

// メイン画面右側のドック。内容の切替状態はセッション内だけ保持し、
// プロジェクトにはメモ本文だけを保存する。
class RightPanel : public juce::Component
{
public:
    enum class Mode { notes, files };

    RightPanel();

    void setProject (Project* projectToUse);
    void open (Mode newMode);
    void close();

    bool isOpen() const { return openState; }
    Mode mode() const { return currentMode; }
    void focusNotesEditor();
    AudioFileBrowserView& fileBrowser() { return browser; }

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
    juce::TextEditor memoEditor;
    AudioFileBrowserView browser;

    static constexpr int headerHeight = 42;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RightPanel)
};
