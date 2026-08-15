#include "CompEditorView.h"

#include <cmath>

#include "Fonts.h"
#include "Theme.h"
#include "../shared/CompParams.h"

namespace
{
constexpr float axisMinDb = -60.0f; // 伝達カーブの両軸（-60〜0dB。メーターと同じ実量スケール）
constexpr int knobRowHeight = 74;
constexpr float pointHideBelowDb = -60.0f;  // 無音時は光点を出さない（表示レンジ外）
constexpr float pointDecayPerTick = 1.5f;   // 光点の減衰（30Hzで約45dB/s。メーターの落ちと同程度）

// GR表示の色（掛かり具合の注意喚起。メーターの黄と同系でLogicのGR表示の読み方に合わせる）
const juce::Colour grColour { 0xffd9a13c };

juce::String msText (float ms)
{
    return (ms < 10.0f ? juce::String (ms, 1) : juce::String ((int) std::lround (ms))) + " ms";
}
} // namespace

CompEditorView::CompEditorView()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);

    configureKnob (thresholdSlider, Comp::minThresholdDb, Comp::maxThresholdDb, 0.1,
                   Comp::defaults.thresholdDb, 0.0); // 線形（dBはそれ自体が対数）
    configureKnob (ratioSlider, Comp::minRatio, Comp::maxRatio, 0.1,
                   Comp::defaults.ratio, 4.0);
    configureKnob (attackSlider, Comp::minAttackMs, Comp::maxAttackMs, 0.1,
                   Comp::defaults.attackMs, 10.0);
    configureKnob (releaseSlider, Comp::minReleaseMs, Comp::maxReleaseMs, 1.0,
                   Comp::defaults.releaseMs, 100.0);
    configureKnob (makeupSlider, Comp::minMakeupDb, Comp::maxMakeupDb, 0.1,
                   Comp::defaults.makeupDb, 0.0);

    hpfToggle.setWantsKeyboardFocus (false);
    hpfToggle.setMouseClickGrabsKeyboardFocus (false);
    hpfToggle.setColour (juce::ToggleButton::textColourId, juce::Colours::white.withAlpha (0.8f));
    hpfToggle.setColour (juce::ToggleButton::tickColourId, juce::Colours::white.withAlpha (0.9f));
    hpfToggle.setColour (juce::ToggleButton::tickDisabledColourId, juce::Colours::white.withAlpha (0.4f));
    hpfToggle.onClick = [this] { applyToModel(); if (onEdited) onEdited(); };
    addAndMakeVisible (hpfToggle);
}

void CompEditorView::configureKnob (juce::Slider& slider, double min, double max, double step,
                                    double defaultValue, double skewMidpoint)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    slider.setRange (min, max, step);
    if (skewMidpoint > 0.0)
        slider.setSkewFactorFromMidPoint (skewMidpoint); // 時間・比は対数的に触るのが自然
    slider.setDoubleClickReturnValue (true, defaultValue); // 復元手段（undo対象外の代わり）
    slider.setScrollWheelEnabled (false); // 変更経路を増やさない（GainSliderと同じ流儀）
    slider.setWantsKeyboardFocus (false);
    slider.setMouseClickGrabsKeyboardFocus (false);
    slider.setColour (juce::Slider::rotarySliderFillColourId, Theme::accent);
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, Theme::controlBg);
    slider.onValueChange = [this]
    {
        if (loadingFromModel)
            return;
        applyToModel();
        if (onEdited)
            onEdited();
    };
    addAndMakeVisible (slider);
}

void CompEditorView::setTrack (Track* trackToShow)
{
    track = trackToShow;
    history.fill (0.0f);
    historyWrite = 0;
    currentGrDb = 0.0f;
    pointDb = -120.0f;
    if (track != nullptr)
        loadFromModel();
    repaint();
}

void CompEditorView::refreshFromModel()
{
    if (track != nullptr)
        loadFromModel();
    repaint();
}

void CompEditorView::loadFromModel()
{
    const auto values = Comp::load (track->params->comp);
    loadingFromModel = true;
    thresholdSlider.setValue (values.thresholdDb, juce::dontSendNotification);
    ratioSlider.setValue (values.ratio, juce::dontSendNotification);
    attackSlider.setValue (values.attackMs, juce::dontSendNotification);
    releaseSlider.setValue (values.releaseMs, juce::dontSendNotification);
    makeupSlider.setValue (values.makeupDb, juce::dontSendNotification);
    hpfToggle.setToggleState (values.detectorHpf, juce::dontSendNotification);
    loadingFromModel = false;
}

void CompEditorView::applyToModel()
{
    if (track == nullptr)
        return;
    Comp::Values values;
    values.thresholdDb = (float) thresholdSlider.getValue();
    values.ratio = (float) ratioSlider.getValue();
    values.attackMs = (float) attackSlider.getValue();
    values.releaseMs = (float) releaseSlider.getValue();
    values.makeupDb = (float) makeupSlider.getValue();
    values.detectorHpf = hpfToggle.getToggleState();
    Comp::store (track->params->comp, Comp::normalized (values));
    repaint(); // 伝達カーブがThreshold/Ratioに追従する
}

void CompEditorView::pushLevels (float grDb, float detectorPeak)
{
    if (track == nullptr || ! isShowing())
        return;
    currentGrDb = grDb;
    history[(size_t) historyWrite] = grDb;
    historyWrite = (historyWrite + 1) % historyLength;

    // 光点: 立ち上がりは即時・下降は減衰ホールド（瞬発音でも見える）
    const float newDb = juce::Decibels::gainToDecibels (detectorPeak, Comp::silenceFloorDb);
    pointDb = juce::jmax (newDb, pointDb - pointDecayPerTick);
    repaint();
}

void CompEditorView::resized()
{
    auto area = getLocalBounds().reduced (10, 8);
    auto knobRow = area.removeFromBottom (knobRowHeight);
    area.removeFromBottom (4);

    // 上段: 伝達カーブ（正方形）｜GR履歴｜GRメーター
    curveArea = area.removeFromLeft (juce::jmin (area.getHeight() + 16, area.getWidth() / 3));
    area.removeFromLeft (10);
    meterArea = area.removeFromRight (16);
    area.removeFromRight (8);
    historyArea = area;

    // 下段: 5ノブ＋（Knee/HPFの右カラム）。ラベルと数値の描画は paint 側（ノブのboundsを基準にする）
    auto knobs = knobRow;
    auto rightColumn = knobs.removeFromRight (juce::jmax (150, knobs.getWidth() / 5));
    const int cell = knobs.getWidth() / 5;
    juce::Slider* sliders[5] = { &thresholdSlider, &ratioSlider, &attackSlider,
                                 &releaseSlider, &makeupSlider };
    for (auto* slider : sliders)
    {
        auto cellArea = knobs.removeFromLeft (cell);
        cellArea.removeFromTop (13);    // ノブ名
        cellArea.removeFromBottom (15); // 数値
        slider->setBounds (cellArea.withSizeKeepingCentre (
            juce::jmin (cellArea.getWidth(), cellArea.getHeight() + 8),
            cellArea.getHeight()));
    }
    hpfToggle.setBounds (rightColumn.removeFromTop (knobRowHeight / 2).reduced (0, 4));
}

void CompEditorView::paint (juce::Graphics& g)
{
    const bool hasTrack = track != nullptr;
    const bool enabled = hasTrack && track->params->compEnabled.load();
    const auto values = hasTrack ? Comp::load (track->params->comp) : Comp::Values {};
    const float dim = enabled ? 1.0f : 0.4f; // バイパス中は沈める（EQのカーブと同じ扱い）

    // ---- 伝達カーブ ----
    {
        const auto area = curveArea.toFloat();
        g.setColour (Theme::timelineBg);
        g.fillRoundedRectangle (area, 4.0f);

        auto xForDb = [&] (float db) { return area.getX() + (db - axisMinDb) / (0.0f - axisMinDb) * area.getWidth(); };
        auto yForDb = [&] (float db) { return area.getBottom() - (db - axisMinDb) / (0.0f - axisMinDb) * area.getHeight(); };

        // グリッド（12dB刻み）＋ユニティ対角線（圧縮なしの基準）
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        for (float db = axisMinDb + 12.0f; db < 0.0f; db += 12.0f)
        {
            g.drawVerticalLine ((int) xForDb (db), area.getY(), area.getBottom());
            g.drawHorizontalLine ((int) yForDb (db), area.getX(), area.getRight());
        }
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawLine (area.getX(), area.getBottom(), area.getRight(), area.getY(), 1.0f);

        if (hasTrack)
        {
            // Threshold位置の目印（縦の点線代わりの淡い線）
            g.setColour (juce::Colours::white.withAlpha (0.15f * dim));
            const float tx = xForDb (values.thresholdDb);
            g.drawVerticalLine ((int) tx, area.getY(), area.getBottom());

            // カーブ（Make Up適用前の静的圧縮特性。DSPと同じ Comp::computeOutputDb）
            juce::Path curve;
            for (int i = 0; i <= 100; ++i)
            {
                const float inDb = axisMinDb + (0.0f - axisMinDb) * (float) i / 100.0f;
                const float outDb = Comp::computeOutputDb (inDb, values.thresholdDb, values.ratio);
                const juce::Point<float> p (xForDb (inDb), yForDb (juce::jmax (axisMinDb, outDb)));
                if (i == 0)
                    curve.startNewSubPath (p);
                else
                    curve.lineTo (p);
            }
            g.setColour (Theme::eqThumbCurve.withAlpha (dim));
            g.strokePath (curve, juce::PathStrokeType (2.0f));

            // 光点: 検波レベルのカーブ上の現在位置（-60dB未満＝無音は非表示）
            if (enabled && pointDb > pointHideBelowDb)
            {
                const float inDb = juce::jmin (pointDb, 0.0f);
                const float outDb = Comp::computeOutputDb (inDb, values.thresholdDb, values.ratio);
                g.setColour (juce::Colours::white);
                g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f)
                                   .withCentre ({ xForDb (inDb), yForDb (juce::jmax (axisMinDb, outDb)) }));
            }
        }

        // 軸ラベル（実量スケールであることが読めるように最小限）
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.setFont (Fonts::small());
        g.drawText ("in dB",
                    curveArea.withTrimmedTop (curveArea.getHeight() - 14).reduced (4, 0),
                    juce::Justification::centredRight);
    }

    // ---- GR履歴（0〜-24dB・約5秒窓。右端が現在）----
    {
        const auto area = historyArea.toFloat();
        g.setColour (Theme::timelineBg);
        g.fillRoundedRectangle (area, 4.0f);

        g.setColour (juce::Colours::white.withAlpha (0.06f));
        for (float db = 6.0f; db < historyRangeDb; db += 6.0f)
            g.drawHorizontalLine ((int) (area.getY() + db / historyRangeDb * area.getHeight()),
                                  area.getX(), area.getRight());

        if (hasTrack)
        {
            juce::Path trace;
            const float stepX = area.getWidth() / (float) (historyLength - 1);
            trace.startNewSubPath (area.getX(), area.getY());
            for (int i = 0; i < historyLength; ++i)
            {
                const float gr = history[(size_t) ((historyWrite + i) % historyLength)];
                const float y = area.getY()
                                + juce::jmin (gr, historyRangeDb) / historyRangeDb * area.getHeight();
                trace.lineTo (area.getX() + stepX * (float) i, y);
            }
            g.setColour (grColour.withAlpha (0.9f * dim));
            g.strokePath (trace, juce::PathStrokeType (1.5f));
            // 上（0dB）から下がる面で「下げている量」を見せる
            trace.lineTo (area.getRight(), area.getY());
            trace.closeSubPath();
            g.setColour (grColour.withAlpha (0.18f * dim));
            g.fillPath (trace);
        }

        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.setFont (Fonts::small());
        g.drawText ("GR " + juce::String (-currentGrDb, 1) + " dB",
                    historyArea.reduced (6, 3), juce::Justification::topRight);
    }

    // ---- GRメーター（現在値の縦バー。上=0dBから下へ伸びる）----
    {
        const auto area = meterArea.toFloat();
        g.setColour (Theme::timelineBg);
        g.fillRoundedRectangle (area, 3.0f);
        if (hasTrack && currentGrDb > 0.02f)
        {
            const float h = juce::jmin (currentGrDb, historyRangeDb) / historyRangeDb * area.getHeight();
            g.setColour (grColour.withAlpha (dim));
            g.fillRoundedRectangle (area.withHeight (h), 3.0f);
        }
    }

    // ---- ノブ名・数値・固定値ラベル ----
    struct KnobText { const juce::Slider* slider; const char* name; juce::String value; };
    const KnobText texts[5] = {
        { &thresholdSlider, "THRESHOLD", juce::String (thresholdSlider.getValue(), 1) + " dB" },
        { &ratioSlider, "RATIO", juce::String (ratioSlider.getValue(), 1) + ":1" },
        { &attackSlider, "ATTACK", msText ((float) attackSlider.getValue()) },
        { &releaseSlider, "RELEASE", msText ((float) releaseSlider.getValue()) },
        { &makeupSlider, "MAKE UP", "+" + juce::String (makeupSlider.getValue(), 1) + " dB" },
    };
    for (const auto& text : texts)
    {
        const auto bounds = text.slider->getBounds();
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (Fonts::small());
        g.drawText (text.name, bounds.getX() - 12, bounds.getY() - 13, bounds.getWidth() + 24, 12,
                    juce::Justification::centred);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawText (text.value, bounds.getX() - 12, bounds.getBottom() + 2, bounds.getWidth() + 24, 13,
                    juce::Justification::centred);
    }

    // Kneeは固定値＋静的ラベルで概念だけ見せる（EQの「12dB/oct」と同じパターン）
    g.setColour (juce::Colours::white.withAlpha (0.5f));
    g.setFont (Fonts::small());
    g.drawText ("Knee: 6dB (soft)",
                hpfToggle.getBounds().withY (hpfToggle.getBottom() + 4).withHeight (14),
                juce::Justification::centredLeft);
}
