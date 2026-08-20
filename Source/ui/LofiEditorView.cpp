#include "LofiEditorView.h"

#include <cmath>
#include <juce_dsp/juce_dsp.h>

#include "Fonts.h"
#include "HardwarePanelStyle.h"
#include "Theme.h"

namespace
{
constexpr int knobSize = 64;
constexpr float curveMinDb = -30.0f; // Toneカーブの表示レンジ

// Wowノブ→±セント表示（深さ比率 p → 1200·log2(1+p)）
juce::String wowText (float knob)
{
    if (knob <= 0.0f)
        return "OFF";
    const float cents = 1200.0f * std::log2 (1.0f + Lofi::wowDepthRatio (knob));
    return juce::String::fromUTF8 (u8"±") + juce::String (cents, 1) + " ct";
}

juce::String toneText (float knob)
{
    if (knob <= 0.0f)
        return juce::String::fromUTF8 (u8"開放");
    const float hz = Lofi::toneCutoffHz (knob);
    return hz >= 1000.0f ? juce::String (hz / 1000.0f, 1) + " kHz"
                         : juce::String ((int) std::lround (hz)) + " Hz";
}

juce::String crushText (float knob)
{
    if (knob <= 0.0f)
        return "OFF";
    return juce::String (Lofi::crushBits (knob), 1) + " bit";
}
} // namespace

LofiEditorView::LofiEditorView()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    configureKnob (wowSlider, Lofi::defaults.wow);
    configureKnob (toneSlider, Lofi::defaults.tone);
    configureKnob (noiseSlider, Lofi::defaults.noise);
    configureKnob (crushSlider, Lofi::defaults.crush);
}

void LofiEditorView::configureKnob (juce::Slider& slider, double defaultValue)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    slider.setRange (0.0, 1.0, 0.001);
    slider.setDoubleClickReturnValue (true, defaultValue); // 復元手段（undo対象外の代わり）
    slider.setScrollWheelEnabled (false); // 変更経路を増やさない（GainSliderと同じ流儀）
    slider.setWantsKeyboardFocus (false);
    slider.setMouseClickGrabsKeyboardFocus (false);
    slider.setColour (juce::Slider::rotarySliderFillColourId, Theme::fxHue (FxVisualKind::lofi));
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

void LofiEditorView::setTrack (Track* trackToShow)
{
    track = trackToShow;
    if (track != nullptr)
        loadFromModel();
    repaint();
}

void LofiEditorView::refreshFromModel()
{
    if (track != nullptr)
        loadFromModel();
    repaint();
}

void LofiEditorView::loadFromModel()
{
    const auto values = Lofi::load (track->params->lofi);
    loadingFromModel = true;
    wowSlider.setValue (values.wow, juce::dontSendNotification);
    toneSlider.setValue (values.tone, juce::dontSendNotification);
    noiseSlider.setValue (values.noise, juce::dontSendNotification);
    crushSlider.setValue (values.crush, juce::dontSendNotification);
    loadingFromModel = false;
}

void LofiEditorView::applyToModel()
{
    if (track == nullptr)
        return;
    Lofi::Values values;
    values.wow = (float) wowSlider.getValue();
    values.tone = (float) toneSlider.getValue();
    values.noise = (float) noiseSlider.getValue();
    values.crush = (float) crushSlider.getValue();
    Lofi::store (track->params->lofi, Lofi::normalized (values));
    repaint(); // ToneカーブがToneノブに追従する
}

void LofiEditorView::resized()
{
    auto area = getLocalBounds().reduced (10, 8);
    toneCurveArea = area.removeFromLeft (juce::jmax (170, area.getWidth() / 4))
                        .withTrimmedBottom (34); // 下は内部並びラベル（paint側）
    area.removeFromLeft (14);
    knobRowArea = area;

    auto knobs = knobRowArea;
    const int cell = knobs.getWidth() / 4;
    juce::Slider* sliders[4] = { &wowSlider, &toneSlider, &noiseSlider, &crushSlider };
    for (auto* slider : sliders)
    {
        auto cellArea = knobs.removeFromLeft (cell);
        cellArea = cellArea.withSizeKeepingCentre (cellArea.getWidth(), knobSize + 28);
        cellArea.removeFromTop (13);    // ノブ名
        cellArea.removeFromBottom (15); // 数値
        slider->setBounds (cellArea.withSizeKeepingCentre (
            juce::jmin (knobSize, cellArea.getWidth()), juce::jmin (knobSize, cellArea.getHeight())));
    }
}

void LofiEditorView::paint (juce::Graphics& g)
{
    const bool hasTrack = track != nullptr;
    const bool enabled = hasTrack && track->params->lofiEnabled.load();
    const auto values = hasTrack ? Lofi::load (track->params->lofi) : Lofi::Values {};
    const float dim = enabled ? 1.0f : 0.4f; // バイパス中は沈める（EQ/Compと同じ扱い）

    const auto hue = Theme::fxHue (FxVisualKind::lofi);

    // ---- Toneカーブ（対数周波数軸 20Hz〜20kHz・0〜-30dB。DSPと同じRBJ設計式。メーター窓）----
    {
        HardwarePanelStyle::paintMeterWindow (g, toneCurveArea.toFloat());
        const auto area = HardwarePanelStyle::meterWindowInner (toneCurveArea.toFloat());

        const double sr = getSampleRate != nullptr && getSampleRate() > 0.0 ? getSampleRate()
                                                                            : 48000.0;
        g.setColour (HardwarePanelStyle::gridColour (hue));
        for (const float f : { 100.0f, 1000.0f, 10000.0f })
        {
            const float x = area.getX()
                            + (float) (std::log (f / 20.0f) / std::log (1000.0f)) * area.getWidth();
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        }

        if (hasTrack && values.tone <= 0.0f)
        {
            // Tone=0はDSPが完全素通し（toneMix=0でLPFを通らない）なので、20kHz LPFの応答でなく
            // 0dBの水平線を描く（描画と音を一致させる）
            g.setColour (hue.withAlpha (dim));
            g.drawLine (area.getX(), area.getY(), area.getRight(), area.getY(), 2.0f);
        }
        else if (hasTrack)
        {
            juce::Path path;
            const float cutoff = juce::jmin (Lofi::toneCutoffHz (values.tone), (float) (sr * 0.49));
            const auto coeffs = juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, cutoff,
                                                                                  0.70710678f);
            for (int i = 0; i <= 100; ++i)
            {
                const double f = 20.0 * std::pow (1000.0, (double) i / 100.0); // 20Hz..20kHz
                const double magDb = juce::Decibels::gainToDecibels (
                    coeffs->getMagnitudeForFrequency (juce::jmin (f, sr * 0.49), sr), -120.0);
                const float x = area.getX() + area.getWidth() * (float) i / 100.0f;
                const float y = area.getY()
                                + (float) (-juce::jmax ((double) curveMinDb, magDb) / -curveMinDb)
                                      * area.getHeight();
                if (i == 0)
                    path.startNewSubPath (x, y);
                else
                    path.lineTo (x, y);
            }
            g.setColour (hue.withAlpha (dim));
            g.strokePath (path, juce::PathStrokeType (2.0f));
        }

        g.setColour (HardwarePanelStyle::captionColour());
        g.setFont (Fonts::small());
        g.drawText ("Tone", toneCurveArea.reduced (8, 6), juce::Justification::topLeft);
    }

    // 内部の成分順（固定・概念だけ見せる静的ラベル）
    g.setColour (HardwarePanelStyle::captionColour());
    g.setFont (Fonts::small());
    g.drawText (juce::String::fromUTF8 (u8"Wow → Crush → Tone → Noise（固定順）"),
                toneCurveArea.withY (toneCurveArea.getBottom() + 8).withHeight (14),
                juce::Justification::centredLeft);

    // ---- ノブ名・数値 ----
    struct KnobText { const juce::Slider* slider; const char* name; juce::String value; };
    const KnobText texts[4] = {
        { &wowSlider, "WOW", wowText ((float) wowSlider.getValue()) },
        { &toneSlider, "TONE", toneText ((float) toneSlider.getValue()) },
        { &noiseSlider, "NOISE",
          juce::String ((int) std::lround (noiseSlider.getValue() * 100.0)) + " %" },
        { &crushSlider, "CRUSH", crushText ((float) crushSlider.getValue()) },
    };
    for (const auto& text : texts)
        HardwarePanelStyle::drawKnobLabel (g, text.slider->getBounds(), text.name, text.value);
}
