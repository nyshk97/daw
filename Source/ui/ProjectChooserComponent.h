#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"
#include "ProjectThumbnails.h"

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

    // 通常時のウィンドウサイズ。これより大きい領域を与えられたとき（フルスクリーン中の
    // 遷移や手動でのウィンドウ拡大）は、引き伸ばさず中央にこのサイズで置く
    static constexpr int designWidth = 520, designHeight = 584;

    std::function<void (std::unique_ptr<Project>)> onProjectOpened;

    // ~/Music/daw/ を走査し直して一覧を作り直す。選択行は名前で追従、消えていたら先頭へ。
    // MainWindowがフォーカス復帰時に呼ぶ（Finderでのリネーム・削除を反映するため）
    void refreshList();

    void paint (juce::Graphics& g) override;
    void resized() override;
    void parentHierarchyChanged() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    // リストは entries[1..]（直近= entries[0] はヒーローカードが担う）。
    // 行番号 row ⇔ entries[row + 1] の対応は listEntry() を経由する
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g,
                           int width, int height, bool rowIsSelected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent& e) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) override;
    void selectedRowsChanged (int lastRowSelected) override;

    void openRow (int row);
    void openDirectory (const juce::File& dir);
    void createNewProject();
    void updateHoveredRow (const juce::MouseEvent& e);

    struct Entry
    {
        juce::File dir;
        juce::Time modified;      // project.jsonの最終保存時刻（表示・ソート用）
        ProjectOverview overview; // ミニ波形＋メタ情報（非同期到着。bpm==0 && numTracks==0 = 未着）
    };
    std::vector<Entry> entries;
    const Entry& listEntry (int row) const { return entries[(size_t) row + 1]; }
    bool heroSelected() const; // ヒーロー選択 ⇔ リストに選択行がない
    void applyOverview (const juce::File& dir, ProjectOverview overview);
    void showRevealMenu (const juce::File& dir);
    void updateHero();

    // 直近プロジェクト（entries[0]）の大型カード。リストには出さず、選択モデルに参加する
    // （起動時はヒーローが選択状態。Returnで開く・↓でリストへ・↑で戻る。単クリックでも開く）
    class HeroCard : public juce::Component
    {
    public:
        std::function<void()> onOpen;
        std::function<void()> onReveal;

        void update (const juce::String& name, const juce::String& meta,
                     juce::Colour colour, std::vector<float> peaks);
        void setSelected (bool nowSelected);

        void paint (juce::Graphics& g) override;
        void mouseEnter (const juce::MouseEvent& e) override;
        void mouseExit (const juce::MouseEvent& e) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

    private:
        juce::String name, meta;
        juce::Colour colour;
        std::vector<float> peaks;
        bool hover = false;
        bool selected = false;
    };
    juce::String suggestedName; // 入力欄にプリフィルした自動命名の候補（ユーザー編集の判定に使う）
    int hoveredRow = -1;
    bool dragHover = false; // プロジェクトフォルダをドラッグで重ねている間のハイライト
    juce::Rectangle<int> listPanelArea;

    juce::Label titleLabel, emptyLabel, errorLabel;
    HeroCard hero;
    juce::ListBox listBox { {}, this };
    juce::TextButton newButton;
    juce::TextEditor nameEditor;
    ProjectThumbnailLoader thumbnails;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProjectChooserComponent)
};
