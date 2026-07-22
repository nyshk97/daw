#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"

// 起動時のプロジェクト選択画面。~/Music/daw/ のフォルダ一覧＋新規作成。
// 開く操作はダブルクリックとReturnの2経路（「開く」ボタンは置かない）。
// 新規作成は名前空欄なら日付ベース（2026-07-22, 2026-07-22-2, ...）で自動命名する。
class ProjectChooserComponent : public juce::Component,
                                private juce::ListBoxModel
{
public:
    ProjectChooserComponent();

    std::function<void (std::unique_ptr<Project>)> onProjectOpened;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void parentHierarchyChanged() override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

private:
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g,
                           int width, int height, bool rowIsSelected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) override;
    void returnKeyPressed (int lastRowSelected) override;

    void refreshList();
    void openRow (int row);
    void createNewProject();
    void updateHoveredRow (const juce::MouseEvent& e);

    struct Entry
    {
        juce::File dir;
        juce::Time modified; // project.jsonの最終保存時刻（表示・ソート用）
    };
    std::vector<Entry> entries;
    int hoveredRow = -1;
    juce::Rectangle<int> listPanelArea;

    juce::Label titleLabel, emptyLabel, errorLabel;
    juce::ListBox listBox { {}, this };
    juce::TextButton newButton;
    juce::TextEditor nameEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProjectChooserComponent)
};
