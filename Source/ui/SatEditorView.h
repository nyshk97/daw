#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"
#include "../shared/SatParams.h"

// 下部エディタ（FxDetailView）に載せるトラックSatの操作UI。
// plan: docs/plans/2026-08-16-2058-fx-batch3-saturation-lofi.md Phase 4（モック案A確定）
//
// 構成（3列）:
//   左   = 伝達カーブ（入力→出力・線形軸±1.0・対角線=素通し）。Sat::transfer
//          ＝DSPと同じ式から描く（描画と音が同じ数式）
//   中央 = 倍音バー（H2〜H8・基本波比dBc・-60dB床）。-18dBFS正弦を同じカーブに通した
//          理論値をdrive変更時に計算する（リアルタイムFFTではなく決定的な静的表示）
//   右   = Drive / Mix の2ノブ
//
// モデル（TrackParams::sat の atomic）への書き込みはこのクラスが直接行い
//（必ず Sat::normalized を通す。ドラッグ中も即時反映）、dirty化は onEdited で
// MainComponent に委ねる（EQ/Compと同じくundo対象外・ダブルクリックで既定値復帰）
class SatEditorView : public juce::Component
{
public:
    SatEditorView();

    // 表示対象。nullptr = 空表示（トラック削除・追従切れへの防御）
    void setTrack (Track* trackToShow);
    Track* shownTrack() const { return track; }

    // satEnabled の切替等、外（スロットピル）からの変更の反映
    void refreshFromModel();

    // 全変更経路（ノブ）で呼ぶ
    std::function<void()> onEdited;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void applyToModel();  // スライダー→normalized→store→倍音再計算→repaint
    void loadFromModel(); // atomic→スライダー（通知なし）
    void configureKnob (juce::Slider& slider, double defaultValue);
    void recomputeHarmonics(); // 現在のDriveから理論倍音レベル（dBc）を計算

    static constexpr int numHarmonics = 7;   // H2〜H8
    static constexpr float barFloorDb = -60.0f;

    Track* track = nullptr;

    juce::Slider driveSlider, mixSlider;
    bool loadingFromModel = false;

    float harmonicsDbc[numHarmonics] {}; // 基本波比dB（drive変更時に更新）

    juce::Rectangle<int> curveArea, harmonicsArea, knobColumn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SatEditorView)
};
