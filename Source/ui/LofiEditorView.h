#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/LofiParams.h"
#include "../shared/Project.h"

// 下部エディタ（FxDetailView）に載せるトラックLo-fiの操作UI。
// plan: docs/plans/2026-08-16-2058-fx-batch3-saturation-lofi.md Phase 6（モック確定案）
//
// 構成:
//   左   = Toneのローパスカーブ（DSPと同じRBJ設計式から描く）＋内部並びの静的ラベル
//          「Wow → Crush → Tone → Noise」（固定順の概念だけ見せる）
//   右   = 成分別4ノブ（Wow / Tone / Noise / Crush）。Mixノブは意図的に無い
//          （ピッチ揺れ成分とdryを混ぜるとコーラス＝別エフェクトになるため full wet）
//
// 数値表示は概念が読める単位: Wow=±セント・Tone=カットオフHz・Noise=%・Crush=ビット深度。
// モデルへの書き込みは Lofi::normalized → store（EQ/Comp/Satと同じ・undo対象外・
// ダブルクリックで既定値復帰）
class LofiEditorView : public juce::Component
{
public:
    LofiEditorView();

    void setTrack (Track* trackToShow);
    Track* shownTrack() const { return track; }

    void refreshFromModel();

    // カーブ描画に使うSR（デバイス追従。未確定は48kで描く。EqEditorViewと同じ流儀）
    std::function<double()> getSampleRate;

    std::function<void()> onEdited;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void applyToModel();
    void loadFromModel();
    void configureKnob (juce::Slider& slider, double defaultValue);

    Track* track = nullptr;

    juce::Slider wowSlider, toneSlider, noiseSlider, crushSlider;
    bool loadingFromModel = false;

    juce::Rectangle<int> toneCurveArea, knobRowArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LofiEditorView)
};
