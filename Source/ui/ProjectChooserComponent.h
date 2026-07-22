#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"

// 起動時のプロジェクト選択画面。~/Music/daw/ のフォルダ一覧＋新規作成。
// 開く操作はダブルクリック・Return・プロジェクトフォルダのドラッグ&ドロップの3経路
// （「開く」ボタンは置かない）。Finderで見つけたフォルダを窓に放り込めば開ける。
// 新規作成は名前欄にプリフィルされる日付+英単語の候補名で自動命名する。
class ProjectChooserComponent : public juce::Component,
                                public juce::FileDragAndDropTarget,
                                private juce::ListBoxModel
{
public:
    ProjectChooserComponent();

    std::function<void (std::unique_ptr<Project>)> onProjectOpened;

    // ~/Music/daw/ を走査し直して一覧を作り直す。選択行は名前で追従、消えていたら先頭へ。
    // MainWindowがフォーカス復帰時に呼ぶ（Finderでのリネーム・削除を反映するため）
    void refreshList();

    void paint (juce::Graphics& g) override;
    void resized() override;
    void parentHierarchyChanged() override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g,
                           int width, int height, bool rowIsSelected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent& e) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) override;
    void returnKeyPressed (int lastRowSelected) override;

    void openRow (int row);
    void openDirectory (const juce::File& dir);
    void createNewProject();
    void updateHoveredRow (const juce::MouseEvent& e);

    struct Entry
    {
        juce::File dir;
        juce::Time modified; // project.jsonの最終保存時刻（表示・ソート用）
    };
    std::vector<Entry> entries;
    juce::String suggestedName; // 入力欄にプリフィルした自動命名の候補（ユーザー編集の判定に使う）
    int hoveredRow = -1;
    bool dragHover = false; // プロジェクトフォルダをドラッグで重ねている間のハイライト
    juce::Rectangle<int> listPanelArea;

    juce::Label titleLabel, emptyLabel, errorLabel;
    juce::ListBox listBox { {}, this };
    juce::TextButton newButton;
    juce::TextEditor nameEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProjectChooserComponent)
};
