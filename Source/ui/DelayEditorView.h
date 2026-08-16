#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/PlaybackSnapshot.h"

// 下部エディタ（FxDetailView）に載せるバスDelayの操作UI。
// plan: docs/plans/2026-08-16-2305-fx-batch4-delay-reverb.md
//
// 構成: Time（音価4値のボタン列 1/16・1/8・1/4・1/2）＋ Feedback / Tone ノブ ＋
// Ping-pong トグル。send バスの返しなので Mix ノブは無い（full wet）。
//
// モデル（busParams[2]->delay の atomic）への書き込みはこのクラスが直接行い
//（必ず Delay::normalized を通す。操作中も即時反映＝音が追従する）、
// dirty化は onEdited で MainComponent に委ねる（他のFXと同じくundo対象外）
class DelayEditorView : public juce::Component
{
public:
    DelayEditorView();

    // 表示対象（Delayバスの共有TrackParams）。nullptr = 空表示
    void setBus (TrackParams* busParamsToShow);
    TrackParams* shownBus() const { return bus; }

    // 全変更経路（ボタン・ノブ・トグル）で呼ぶ
    std::function<void()> onEdited;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void applyToModel();  // UI→normalized→store（dirty化は呼び出し側）
    void loadFromModel(); // atomic→UI（通知なし）
    void configureKnob (juce::Slider& slider, double min, double max, double step,
                        double defaultValue);

    TrackParams* bus = nullptr;

    juce::TextButton timeButtons[4]; // Delay::timeLabels の並び（ラジオ動作）
    juce::Slider feedbackSlider, toneSlider;
    juce::TextButton pingPongButton { "PING-PONG" };
    bool loadingFromModel = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayEditorView)
};
