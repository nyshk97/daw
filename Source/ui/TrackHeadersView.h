#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"

// トラック1本分のヘッダ（名前・M/S・音量。MIDIトラックは楽器ドロップダウンも）。
// トラック削除は右クリックメニュー（または⌘Delete。MainComponent側で処理）。
// 音量・ミュート・ソロは TrackParams の atomic に直接書く（スナップショット再構築は不要）。
class TrackHeaderComponent : public juce::Component
{
public:
    TrackHeaderComponent();

    void bind (Track* trackToBind, bool isSelected); // rebuild/選択変更時に呼ぶ

    std::function<void()> onSelect;
    std::function<void()> onDeleteClicked; // 右クリックメニューの「トラックを削除」
    std::function<void()> onChanged;             // M/S・音量の変更（dirtyマーク用）
    std::function<void()> onWillChangeStructure; // リネーム・楽器変更の直前（undoスナップショット用）
    std::function<void()> onInstrumentChanged;   // 楽器変更の確定後（pushSnapshotで音源差し替え）

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    Track* track = nullptr;
    bool selected = false;

    juce::Label nameLabel;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };
    juce::Slider volumeSlider;
    juce::ComboBox instrumentBox; // MIDIトラックのみ表示

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeaderComponent)
};

// トラックヘッダの縦リスト。縦スクロールは TimelineView と同期する
// （setViewY で追従し、自分の上のホイールは onWheel で転送する）。
class TrackHeadersView : public juce::Component
{
public:
    static constexpr int preferredWidth = 200;

    TrackHeadersView();

    void setProject (Project* p);
    void rebuild(); // トラックの追加・削除後に呼ぶ
    void refreshValues(); // モデル側の値変更（キー操作でのミュート等）を表示に反映する
    void setSelectedTrack (int index);
    void setViewY (int y);

    std::function<void (int)> onSelect;
    std::function<void (int)> onDeleteRequested;
    std::function<void()> onChanged;
    std::function<void()> onWillChangeStructure; // リネーム・楽器変更の直前（undo用）
    std::function<void()> onInstrumentChanged;   // 楽器変更の確定後
    std::function<void (float)> onWheel;

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    void refreshBindings();

    Project* project = nullptr;
    int selectedTrack = 0;

    juce::Component container;
    std::vector<std::unique_ptr<TrackHeaderComponent>> items;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeadersView)
};
