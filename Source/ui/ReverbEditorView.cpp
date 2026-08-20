#include "ReverbEditorView.h"

#include <cmath>

#include "Fonts.h"
#include "HardwarePanelStyle.h"
#include "Theme.h"
#include "../shared/ReverbParams.h"

namespace
{
constexpr int knobRowHeight = 74;
}

ReverbEditorView::ReverbEditorView()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);

    configureKnob (sizeSlider, 0.0, 1.0, 0.01, 0.0);
    configureKnob (dampSlider, 0.0, 1.0, 0.01, 0.0);
    configureKnob (widthSlider, 0.0, 1.0, 0.01, 0.0);
    configureKnob (preDelaySlider, Reverb::minPreDelayMs, Reverb::maxPreDelayMs, 1.0, 0.0);
    configureKnob (lowCutSlider, Reverb::minLowCutHz, Reverb::maxLowCutHz, 1.0,
                   100.0); // 周波数は対数的に触るのが自然
    applyDoubleClickDefaults();
}

void ReverbEditorView::configureKnob (juce::Slider& slider, double min, double max, double step,
                                      double skewMidpoint)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    slider.setRange (min, max, step);
    if (skewMidpoint > 0.0)
        slider.setSkewFactorFromMidPoint (skewMidpoint);
    slider.setScrollWheelEnabled (false); // 変更経路を増やさない（Limiter・Compと同じ流儀）
    slider.setWantsKeyboardFocus (false);
    slider.setMouseClickGrabsKeyboardFocus (false);
    slider.setColour (juce::Slider::rotarySliderFillColourId, Theme::fxHue (FxVisualKind::reverbA));
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

void ReverbEditorView::applyDoubleClickDefaults()
{
    // ダブルクリックの戻り先はバスindex別の既定値（undo対象外の代わりの復元手段）
    const auto defaults = Reverb::defaultsForBus (shownBusIndex);
    sizeSlider.setDoubleClickReturnValue (true, defaults.size);
    dampSlider.setDoubleClickReturnValue (true, defaults.damp);
    widthSlider.setDoubleClickReturnValue (true, defaults.width);
    preDelaySlider.setDoubleClickReturnValue (true, defaults.preDelayMs);
    lowCutSlider.setDoubleClickReturnValue (true, defaults.lowCutHz);
}

void ReverbEditorView::setBus (TrackParams* busParamsToShow, int busIndex)
{
    bus = busParamsToShow;
    shownBusIndex = juce::jlimit (0, 1, busIndex);
    // A/Bで同じインスタンスを使い回すので、ノブの固有色（点灯目盛り）もここで切り替える
    const auto hue = Theme::fxHue (shownBusIndex == 1 ? FxVisualKind::reverbB : FxVisualKind::reverbA);
    for (auto* s : { &sizeSlider, &dampSlider, &widthSlider, &preDelaySlider, &lowCutSlider })
        s->setColour (juce::Slider::rotarySliderFillColourId, hue);
    applyDoubleClickDefaults();
    if (bus != nullptr)
        loadFromModel();
    repaint();
}

void ReverbEditorView::loadFromModel()
{
    const auto values = Reverb::load (bus->reverb);
    loadingFromModel = true;
    sizeSlider.setValue (values.size, juce::dontSendNotification);
    dampSlider.setValue (values.damp, juce::dontSendNotification);
    widthSlider.setValue (values.width, juce::dontSendNotification);
    preDelaySlider.setValue (values.preDelayMs, juce::dontSendNotification);
    lowCutSlider.setValue (values.lowCutHz, juce::dontSendNotification);
    loadingFromModel = false;
}

void ReverbEditorView::applyToModel()
{
    if (bus == nullptr)
        return;
    Reverb::Values values;
    values.size = (float) sizeSlider.getValue();
    values.damp = (float) dampSlider.getValue();
    values.width = (float) widthSlider.getValue();
    values.preDelayMs = (float) preDelaySlider.getValue();
    values.lowCutHz = (float) lowCutSlider.getValue();
    Reverb::store (bus->reverb, Reverb::normalized (values, Reverb::defaultsForBus (shownBusIndex)));
    repaint();
}

void ReverbEditorView::resized()
{
    auto area = getLocalBounds().reduced (10, 8);

    auto knobRow = area.withHeight (knobRowHeight)
                       .withY (area.getY() + (area.getHeight() - knobRowHeight) / 2 - 10);
    const int cell = juce::jmin (110, knobRow.getWidth() / 5);
    knobRow = knobRow.withWidth (cell * 5).withCentre (area.getCentre())
                  .withHeight (knobRowHeight);
    juce::Slider* sliders[5] = { &sizeSlider, &dampSlider, &widthSlider, &preDelaySlider,
                                 &lowCutSlider };
    for (auto* slider : sliders)
    {
        auto cellArea = knobRow.removeFromLeft (cell);
        cellArea.removeFromTop (13);    // ノブ名
        cellArea.removeFromBottom (15); // 数値
        slider->setBounds (cellArea.withSizeKeepingCentre (
            juce::jmin (cellArea.getWidth(), cellArea.getHeight() + 8), cellArea.getHeight()));
    }
}

void ReverbEditorView::paint (juce::Graphics& g)
{
    // ノブ名・数値（Limiterエディタと同じ体裁）
    struct KnobText { const juce::Slider* slider; const char* name; juce::String value; };
    const auto lowCut = lowCutSlider.getValue();
    const KnobText texts[5] = {
        { &sizeSlider, "SIZE", juce::String ((int) std::lround (sizeSlider.getValue() * 100.0)) + " %" },
        { &dampSlider, "DAMP", juce::String ((int) std::lround (dampSlider.getValue() * 100.0)) + " %" },
        { &widthSlider, "WIDTH", juce::String ((int) std::lround (widthSlider.getValue() * 100.0)) + " %" },
        { &preDelaySlider, "PRE-DELAY",
          juce::String ((int) std::lround (preDelaySlider.getValue())) + " ms" },
        { &lowCutSlider, "LOW CUT",
          lowCut <= (double) Reverb::minLowCutHz + 0.5 ? juce::String ("OFF")
                                                       : juce::String ((int) std::lround (lowCut)) + " Hz" },
    };
    for (const auto& text : texts)
        HardwarePanelStyle::drawKnobLabel (g, text.slider->getBounds(), text.name, text.value);
}
