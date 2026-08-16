#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/PlaybackSnapshot.h"

// 下部エディタ（FxDetailView）に載せるバスReverb（A/B）の操作UI。
// plan: docs/plans/2026-08-16-2305-fx-batch4-delay-reverb.md
//
// ノブ5点: Size / Damp / Width / Pre-delay / Low Cut（種類セレクタは持たない —
// 単一アルゴリズムのプリセットに過ぎず、ノブの中身が見えなくなる）。
// A/Bは同じビューで、setBus の対象切替だけで値が入れ替わる。
// ダブルクリックの戻り先はバスindex別の既定値（Reverb::defaultsForBus）。
//
// モデル（busParams->reverb の atomic）への書き込みはこのクラスが直接行い
//（必ず Reverb::normalized を通す。ドラッグ中も即時反映＝音が追従する）、
// dirty化は onEdited で MainComponent に委ねる（他のFXと同じくundo対象外）
class ReverbEditorView : public juce::Component
{
public:
    ReverbEditorView();

    // 表示対象（バスの共有TrackParams）。nullptr = 空表示。busIndex は既定値解決用（0/1）
    void setBus (TrackParams* busParamsToShow, int busIndex);
    TrackParams* shownBus() const { return bus; }

    // 全変更経路（ノブ）で呼ぶ
    std::function<void()> onEdited;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void applyToModel();  // スライダー→normalized→store（dirty化は呼び出し側）
    void loadFromModel(); // atomic→スライダー（通知なし）
    void configureKnob (juce::Slider& slider, double min, double max, double step,
                        double skewMidpoint);
    void applyDoubleClickDefaults();

    TrackParams* bus = nullptr;
    int shownBusIndex = 0;

    juce::Slider sizeSlider, dampSlider, widthSlider, preDelaySlider, lowCutSlider;
    bool loadingFromModel = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbEditorView)
};
