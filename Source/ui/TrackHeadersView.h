#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"
#include "StereoMeter.h"

// トラック1本分のヘッダ（名前・M/S・音量。MIDIトラックは楽器ドロップダウンも）。
// トラック削除は右クリックメニュー（または⌘Delete。MainComponent側で処理）。
// 音量・ミュート・ソロは TrackParams の atomic に直接書く（スナップショット再構築は不要）。
class TrackHeaderComponent : public juce::Component
{
public:
    TrackHeaderComponent();

    void bind (Track* trackToBind, bool isSelected, bool anySolo); // rebuild/選択変更時に呼ぶ

    // 30Hz Timerから。減衰・スライダー再描画を行う。peakL/peakR の exchange(0) は
    // MainComponent が一元的に行い、読み取り済みの値が渡ってくる
    // （ミキサーと2箇所でexchangeするとピークを取り合うため）
    void updateMeter (StereoPeak incoming);

    // 30Hz Timerから。ミキサー・キー操作等どの経路のミュート/ソロ変更もpull型で拾い、
    // M/S点灯とグレーアウト表示を同期する（変化がなければ何もしない）。
    // anySolo はプロジェクト全体のソロ有無（他トラックのソロで自分が実質ミュートになるため外から渡す）
    void syncStateVisual (bool anySolo);

    std::function<void()> onSelect;
    std::function<void()> onDeleteClicked; // 右クリックメニューの「トラックを削除」
    std::function<void()> onChanged;             // M/S・音量の変更（dirtyマーク用）
    std::function<void()> onWillChangeStructure; // リネーム・楽器変更の直前（undoスナップショット用）
    std::function<void()> onInstrumentChanged;   // 楽器変更の確定後（pushSnapshotで音源差し替え）

    // ドラッグ並び替え。開始できるのはヘッダ背景＋種別アイコン領域のみ（nameLabel・M/S・
    // 音量・楽器の上からは開始しない）。Yは親（container）座標
    std::function<bool()> canReorder;        // 録音中はfalseが返る（判定はMainComponent）
    std::function<void (int)> onReorderDrag; // 並び替えドラッグ中（挿入インジケータ更新用）
    std::function<void (int)> onReorderDrop; // ドロップ（並び替えの確定要求）

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    static constexpr int iconSlotWidth = 20; // トラック名の左のトラック種別アイコン領域

    juce::Rectangle<float> typeIconArea() const;
    void applyDimVisual (bool dimmed); // 名前・アイコン・音量・楽器を減光（M/Sボタンは操作の主役なので沈めない）

    Track* track = nullptr;
    bool selected = false;
    bool reorderDragging = false; // 並び替えドラッグ中（閾値超え＋canReorder通過後）
    bool dimmedVisual = false; // グレーアウト表示中か＝聞こえない状態（ミュート or 他トラックのソロ）。paintのアイコン減光にも使う
    Meters::ChannelDisplay meterDisplay[2]; // メーターのL/R表示状態（減衰＋ピークホールド）

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
    void updateMeters (const std::vector<StereoPeak>& peaks); // 30Hz Timerから（各ヘッダへの転送のみ）
    void setSelectedTrack (int index);
    void setViewY (int y);

    std::function<void (int)> onSelect;
    std::function<void (int)> onDeleteRequested;
    std::function<void()> onChanged;
    std::function<void()> onWillChangeStructure; // リネーム・楽器変更の直前（undo用）
    std::function<void()> onInstrumentChanged;   // 楽器変更の確定後
    std::function<void (float)> onWheel;

    // ドラッグ並び替え
    std::function<bool()> canReorder;                  // 録音中はfalse（判定はMainComponent）
    std::function<void (int, int)> onReorderRequested; // (from, 挿入先の隙間番号 0..tracks.size())

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void paintOverChildren (juce::Graphics& g) override; // 挿入位置インジケータ

private:
    void refreshBindings();
    bool anySoloActive() const; // どれかのトラックがソロ中か（減光判定用）
    int gapForY (int containerY) const;            // container座標Y → 挿入先の隙間番号（クランプ済み）
    void updateReorderIndicator (int containerY);  // ドラッグ中のインジケータ更新
    void finishReorder (int from, int containerY); // ドロップ確定（no-op位置なら何もしない）

    Project* project = nullptr;
    int selectedTrack = 0;
    int reorderGap = -1; // 並び替えドラッグ中の挿入位置インジケータ（-1 = 非表示）

    juce::Component container;
    std::vector<std::unique_ptr<TrackHeaderComponent>> items;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeadersView)
};
