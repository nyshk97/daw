#pragma once

#include <array>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"

// 下部エディタ（FxDetailView）に載せるトラックCompの操作UI。
// plan: docs/plans/2026-08-15-1650-track-comp.md
//
// 構成:
//   上段左  = 伝達カーブ（-60〜0dB両軸・Make Up適用前の静的圧縮特性・現在入力レベルの光点）。
//             カーブは Comp::computeOutputDb ＝DSPと同じ関数から描く（描画と音が同じ数式）
//   上段右  = GR履歴（横に流れる「何dB下がってるか」の軌跡。0〜-24dB固定・約5秒窓）＋GRメーター
//   下段    = 5ノブ（Threshold/Ratio/Attack/Release/Make Up）＋「Knee: 6dB (soft)」静的ラベル
//             ＋「Detector HPF 80Hz」トグル
//
// 操作はノブのみ（カーブは表示専用。同じ値を変える経路を増やさない＝undo・dirtyの区切りを
// 揃える負担を増やさない）。数値の直接入力は後回し・ホイールは無効（GainSliderと同じ流儀）。
//
// モデル（TrackParams::comp の atomic）への書き込みはこのクラスが直接行い
//（必ず Comp::normalized を通す。ドラッグ中も即時反映＝音が追従する）、
// dirty化は onEdited で MainComponent に委ねる（Compはフェーダー・EQと同じくundo対象外）。
//
// GR・検波レベルは MainComponent のメータータイマーが atomic を exchange(0) で一元消費し、
// pushLevels() で30Hz配布する（複数UIが直接読むとピークを奪い合うため。atomicは読まない）
class CompEditorView : public juce::Component
{
public:
    CompEditorView();

    // 表示対象。nullptr = 空表示（トラック削除・追従切れへの防御）
    void setTrack (Track* trackToShow);
    Track* shownTrack() const { return track; }

    // compEnabled の切替等、外（スロットピル）からの変更の反映
    void refreshFromModel();

    // MainComponentのメータータイマーから30Hzで呼ばれる。
    // grDb = ブロック最大GR（正の減衰量dB）・detectorPeak = 検波入力ピーク（線形振幅）
    void pushLevels (float grDb, float detectorPeak);

    // 全変更経路（ノブ・HPFトグル）で呼ぶ
    std::function<void()> onEdited;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void applyToModel();      // スライダー→normalized→store→repaint（dirty化は呼び出し側）
    void loadFromModel();     // atomic→スライダー（通知なし）
    void configureKnob (juce::Slider& slider, double min, double max, double step,
                        double defaultValue, double skewMidpoint);

    static constexpr int historyLength = 150; // 30Hz × 5秒
    static constexpr float historyRangeDb = 24.0f; // GR履歴・GRメーターのスケール（0〜-24dB）

    Track* track = nullptr;

    juce::Slider thresholdSlider, ratioSlider, attackSlider, releaseSlider, makeupSlider;
    juce::ToggleButton hpfToggle { juce::String ("Detector HPF 80Hz") };
    bool loadingFromModel = false; // setValue の通知ループ・track切替時のdirty化を防ぐ

    // GR履歴リングバッファ（正のdB）と現在値・光点レベル
    std::array<float, historyLength> history {};
    int historyWrite = 0;
    float currentGrDb = 0.0f;
    float pointDb = -120.0f; // 光点の表示レベル（dB。減衰ホールド付き・-60未満は非表示）

    juce::Rectangle<int> curveArea, historyArea, meterArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompEditorView)
};
