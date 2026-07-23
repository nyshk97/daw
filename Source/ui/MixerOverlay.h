#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "SendRow.h"
#include "SlotPill.h"
#include "StereoMeter.h"
#include "../shared/Project.h"

// ミキサーの1本分のストリップ。FXパネルと同じ項目をLogicのミキサー列の並びで表示する:
//   トラック = EQサムネイル → スロット [EQ][Comp][Ext] → Sends行 → Panノブ → dB数値 → フェーダー/メーター → M/S → 名前
//   バス     = スロット [Reverb]/[Delay] → dB数値 → フェーダー/メーター → M → 名前、Master = [Limiter]（Mなし）
// スロットはFXパネルと同じSlotPill（hoverで電源｜エディタ2分割。エディタ側は onOpenSlot 経由で
// チャンネル選択＋下部詳細エディタを開く）。値の変更は TrackParams の atomic に直接書く（スナップショット再構築は不要）。
class MixerStrip : public juce::Component
{
public:
    enum class Kind { track, bus, master };

    static constexpr int preferredWidth = 104;

    explicit MixerStrip (Kind kindToUse, juce::String fxSlotNameToUse = {});

    void bind (const juce::String& name, std::shared_ptr<TrackParams> paramsToBind, bool isSelected);
    void updateMeter (const MeterFeed& feed); // 30Hz。表示更新（MixerOverlayが集約値を配る。減衰はStereoMeter側）

    std::function<void()> onSelect;      // ストリップクリック
    std::function<void()> onChanged;     // 値変更（dirtyマーク用）
    std::function<void (int)> onOpenSlot; // スロットのエディタ側クリック・EQサムネイルクリック（引数=スロットindex）

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    void setupKnob (juce::Slider& knob, double min, double max, double returnValue);

    Kind kind;
    juce::String fxSlotName;                      // バス/Masterの単一スロット名（Reverb/Delay/Limiter）
    std::shared_ptr<TrackParams> params;
    juce::String stripName;
    bool selected = false;
    float peakMaxDisplay = 0.0f;                  // 再生開始からの最大ピーク（dB数値表示用）

    juce::Rectangle<int> nameArea;
    juce::Rectangle<int> eqThumbArea;             // EQサムネイル（トラックのみ。クリック=EQエディタを開く）
    juce::Rectangle<int> readoutArea;             // dB数値ボックスのペア（設定値・ピーク）

    SlotPill slotPills[3];                        // トラック=EQ/Comp/Ext、バス/Master=[0]のみ（FXパネルと共有部品）

    SendRow sendRows[numSendBuses] { SendRow (0), SendRow (1), SendRow (2) }; // トラックのみ（FXパネルと共有部品）
    juce::Slider panKnob;                 // トラックのみ（Logic風ノブ描画はAppLookAndFeel::drawLogicPanKnob）
    juce::Slider fader;
    StereoMeter meter;                    // フェーダー右のL/Rメーター（Logicのストリップと同じ分離配置）
    juce::TextButton muteButton { "M" };  // トラック・バス
    juce::TextButton soloButton { "S" };  // トラックのみ

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerStrip)
};

// ミキサーウィンドウ（MixerWindow）の中身のパネル。
// 構成: [トラックのストリップ（横スクロール）| Reverb A / Reverb B / Delay | Master]。
// キーは処理せず onKey でMainComponentの集中ハンドラへ転送する（X/Escの閉じ操作もそちら側）。
// ウィンドウの表示・非表示・移動・リサイズは MixerWindow が担当する
class MixerOverlay : public juce::Component
{
public:
    MixerOverlay();

    void setProject (Project* p);

    // ストリップ構成・選択ハイライト・表示値をモデルに同期する（ウィンドウを開くとき・
    // トラック増減・選択変更・m/sキー操作後に呼ぶ。ウィンドウ非表示中は何もしない）
    void sync (int selectedTrack);

    // 表示値のみrebind（FXエディタ側でのsend変更を反映する）
    void refreshValues() { sync (selectedTrack); }

    // メーター値の配布（30Hz）。peakL/peakR の exchange(0) と maxSincePlay の蓄積・リセットは
    // MainComponent が一元的に行い、ここへは読み取り済みの値が渡ってくる
    void updateMeters (const std::vector<MeterFeed>& trackFeeds,
                       const MeterFeed (&busFeeds)[numSendBuses], const MeterFeed& masterFeed);

    std::function<void (int)> onSelectTrack;
    std::function<void (int)> onSelectBus; // バスストリップクリック（FXエディタの表示切替）
    std::function<void()> onSelectMaster;  // Masterストリップクリック（同上）
    std::function<void()> onChanged; // 値変更（dirtyマーク用）
    // スロットのエディタ側クリック（チャンネル選択＋下部詳細エディタを開く。MainComponentが配線）
    std::function<void (int trackIndex, int slot)> onOpenTrackSlot;
    std::function<void (int busIndex)> onOpenBusSlot;
    std::function<void()> onOpenMasterSlot;
    // ミキサーウィンドウにフォーカスがあるときのキー転送先（MainComponent::keyPressed）
    std::function<bool (const juce::KeyPress&)> onKey;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    void rebuildTrackStrips();

    Project* project = nullptr;
    int selectedTrack = -1;

    juce::Viewport viewport; // トラックストリップの横スクロール
    juce::Component stripRow;
    std::vector<std::unique_ptr<MixerStrip>> trackStrips;
    // スロット名はFXパネルのスロット表記（Reverb/Delay/Limiter）と揃える
    MixerStrip busStrips[numSendBuses] { MixerStrip (MixerStrip::Kind::bus, "Reverb"),
                                         MixerStrip (MixerStrip::Kind::bus, "Reverb"),
                                         MixerStrip (MixerStrip::Kind::bus, "Delay") };
    MixerStrip masterStrip { MixerStrip::Kind::master, "Limiter" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerOverlay)
};
