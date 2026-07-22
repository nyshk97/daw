#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "SendKnob.h"
#include "../shared/Project.h"

// ミキサーの1本分のストリップ。トラック（sendノブ3・Pan・フェーダー・メーター・M/S・名前）、
// バス（フェーダー・メーター・M・名前）、Master（フェーダー・メーター・名前）を kind で切り替える。
// 値の変更は TrackParams の atomic に直接書く（スナップショット再構築は不要）。
class MixerStrip : public juce::Component
{
public:
    enum class Kind { track, bus, master };

    static constexpr int preferredWidth = 92;

    explicit MixerStrip (Kind kindToUse);

    void bind (const juce::String& name, std::shared_ptr<TrackParams> paramsToBind, bool isSelected);
    void updateMeter (float incoming); // 30Hz。減衰込みの表示更新（MixerOverlayが集約値を配る）

    std::function<void()> onSelect;  // ストリップクリック（トラックのみ配線される）
    std::function<void()> onChanged; // 値変更（dirtyマーク用）

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    void setupKnob (juce::Slider& knob, double min, double max, double returnValue);

    Kind kind;
    std::shared_ptr<TrackParams> params;
    juce::String stripName;
    bool selected = false;
    float meterDisplay = 0.0f;
    bool panDragging = false;                     // ドラッグ中はラベルをライブ値表示にする

    juce::Rectangle<int> meterArea; // resizedで確定、paintで描く
    juce::Rectangle<int> nameArea;
    juce::Rectangle<int> panLabelArea;

    SendKnob sendKnobs[numSendBuses] { SendKnob (0), SendKnob (1), SendKnob (2) }; // トラックのみ
    juce::Slider panKnob;                 // トラックのみ
    juce::Slider fader;
    juce::TextButton muteButton { "M" };  // トラック・バス
    juce::TextButton soloButton { "S" };  // トラックのみ

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerStrip)
};

// X で開くミキサーオーバーレイ。AddTrackOverlay等と同じ「親の一部を覆いパネルを自前描画」方式。
// タイムライン＋ヘッダー領域だけを覆い、上部バーと下部（ピアノロール等）は隠さない。
// キーは奪わない（Space再生・シーク等は MainComponent::keyPressed がそのまま処理する。
// 閉じる操作 X/Esc も keyPressed 側）。パネル外クリックで閉じる。
// パネルはタイトルバー（MIXER）や隙間のドラッグで移動できる（位置はセッション内で保持）。
// 構成: [トラックのストリップ（横スクロール）| Reverb A / Reverb B / Delay | Master]
class MixerOverlay : public juce::Component
{
public:
    MixerOverlay();

    void setProject (Project* p);
    void showOver (juce::Rectangle<int> areaToCover, int selectedTrack);

    // 閉じる（X/Esc/パネル外クリックの全経路がここを通る）。onDismissed で通知する
    // （FXエディタの表示対象を選択トラック追従に戻すため）
    void dismiss()
    {
        if (! isVisible())
            return;
        setVisible (false);
        if (onDismissed)
            onDismissed();
    }

    // ストリップ構成・選択ハイライト・表示値をモデルに同期する（トラック増減・選択変更・m/sキー操作後に呼ぶ。
    // 非表示中は何もしない: showOver が開くときに必ず同期する）
    void sync (int selectedTrack);

    // 表示値のみrebind（FXエディタ側でのsend変更を反映する）
    void refreshValues() { sync (selectedTrack); }

    // メーター値の配布（30Hz）。peakLevel の exchange(0) は MainComponent が一元的に行い、
    // ここへは読み取り済みの値が渡ってくる（2箇所でexchangeするとピークを取り合うため）
    void updateMeters (const std::vector<float>& trackPeaks,
                       const float (&busPeaks)[numSendBuses], float masterPeak);

    std::function<void (int)> onSelectTrack;
    std::function<void (int)> onSelectBus; // バスストリップクリック（FXエディタの表示切替）
    std::function<void()> onSelectMaster;  // Masterストリップクリック（同上）
    std::function<void()> onDismissed;     // 閉じ通知（全経路）
    std::function<void()> onChanged; // 値変更（dirtyマーク用）

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;

private:
    juce::Rectangle<int> panelBounds() const;
    void rebuildTrackStrips();

    Project* project = nullptr;
    int selectedTrack = -1;

    // パネルのドラッグ移動。offsetは中央位置からのずれ（panelBoundsが領域内にクランプする）
    juce::Point<int> panelOffset;
    juce::Point<int> dragAnchor;
    bool draggingPanel = false;

    juce::Viewport viewport; // トラックストリップの横スクロール
    juce::Component stripRow;
    std::vector<std::unique_ptr<MixerStrip>> trackStrips;
    MixerStrip busStrips[numSendBuses] { MixerStrip (MixerStrip::Kind::bus),
                                         MixerStrip (MixerStrip::Kind::bus),
                                         MixerStrip (MixerStrip::Kind::bus) };
    MixerStrip masterStrip { MixerStrip::Kind::master };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerOverlay)
};
