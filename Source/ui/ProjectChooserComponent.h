#pragma once

#include <functional>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"

// 起動時のプロジェクト選択画面。~/Music/daw/ のフォルダ一覧＋新規作成。
class ProjectChooserComponent : public juce::Component,
                                private juce::ListBoxModel
{
public:
    ProjectChooserComponent();

    std::function<void (std::unique_ptr<Project>)> onProjectOpened;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g,
                           int width, int height, bool rowIsSelected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) override;

    void refreshList();
    void openRow (int row);
    void createNewProject();

    juce::Array<juce::File> projectDirs;

    juce::Label titleLabel, newCaptionLabel, errorLabel;
    juce::ListBox listBox { {}, this };
    juce::TextButton openButton, newButton;
    juce::TextEditor nameEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProjectChooserComponent)
};
