#include "DelayEditorView.h"

#include <cmath>

#include "Fonts.h"
#include "Theme.h"
#include "../shared/DelayParams.h"

namespace
{
constexpr int knobRowHeight = 74;
constexpr int timeRadioGroup = 0x4c44; // 'LD'（このビュー内で一意ならよい）
}

DelayEditorView::DelayEditorView()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);

    for (int i = 0; i < Delay::numTimeChoices; ++i)
    {
        auto& button = timeButtons[i];
        button.setButtonText (Delay::timeLabels[i]);
        button.setClickingTogglesState (true);
        button.setRadioGroupId (timeRadioGroup);
        button.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0)
                                  | (i < Delay::numTimeChoices - 1 ? juce::Button::ConnectedOnRight : 0));
        button.setColour (juce::TextButton::buttonColourId, Theme::controlBg);
        button.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
        button.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.7f));
        button.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        button.setWantsKeyboardFocus (false);
        button.setMouseClickGrabsKeyboardFocus (false);
        button.onClick = [this, i]
        {
            if (loadingFromModel || ! timeButtons[i].getToggleState())
                return;
            applyToModel();
            if (onEdited)
                onEdited();
        };
        addAndMakeVisible (button);
    }

    configureKnob (feedbackSlider, 0.0, (double) Delay::maxFeedback * 100.0, 1.0,
                   (double) Delay::defaults.feedback * 100.0);
    configureKnob (toneSlider, 0.0, 1.0, 0.01, (double) Delay::defaults.tone);

    pingPongButton.setClickingTogglesState (true);
    pingPongButton.setColour (juce::TextButton::buttonColourId, Theme::controlBg);
    pingPongButton.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    pingPongButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.7f));
    pingPongButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    pingPongButton.setTooltip (juce::String::fromUTF8 (u8"エコーを左右交互に振る"));
    pingPongButton.setWantsKeyboardFocus (false);
    pingPongButton.setMouseClickGrabsKeyboardFocus (false);
    pingPongButton.onClick = [this]
    {
        if (loadingFromModel)
            return;
        applyToModel();
        if (onEdited)
            onEdited();
    };
    addAndMakeVisible (pingPongButton);
}

void DelayEditorView::configureKnob (juce::Slider& slider, double min, double max, double step,
                                     double defaultValue)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    slider.setRange (min, max, step);
    slider.setDoubleClickReturnValue (true, defaultValue); // 復元手段（undo対象外の代わり）
    slider.setScrollWheelEnabled (false); // 変更経路を増やさない（Limiter・Compと同じ流儀）
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

void DelayEditorView::setBus (TrackParams* busParamsToShow)
{
    bus = busParamsToShow;
    if (bus != nullptr)
        loadFromModel();
    repaint();
}

void DelayEditorView::loadFromModel()
{
    const auto values = Delay::load (bus->delay);
    loadingFromModel = true;
    for (int i = 0; i < Delay::numTimeChoices; ++i)
        timeButtons[i].setToggleState (i == values.timeIndex, juce::dontSendNotification);
    feedbackSlider.setValue ((double) values.feedback * 100.0, juce::dontSendNotification);
    toneSlider.setValue ((double) values.tone, juce::dontSendNotification);
    pingPongButton.setToggleState (values.pingPong, juce::dontSendNotification);
    loadingFromModel = false;
}

void DelayEditorView::applyToModel()
{
    if (bus == nullptr)
        return;
    Delay::Values values;
    values.timeIndex = Delay::defaults.timeIndex;
    for (int i = 0; i < Delay::numTimeChoices; ++i)
        if (timeButtons[i].getToggleState())
            values.timeIndex = i;
    values.feedback = (float) (feedbackSlider.getValue() / 100.0);
    values.tone = (float) toneSlider.getValue();
    values.pingPong = pingPongButton.getToggleState();
    Delay::store (bus->delay, Delay::normalized (values));
    repaint();
}

void DelayEditorView::resized()
{
    auto area = getLocalBounds().reduced (10, 8);

    // 左: Time ボタン列（縦中央）→ Feedback/Tone ノブ → Ping-pong トグル
    auto timeColumn = area.removeFromLeft (juce::jmin (240, area.getWidth() / 3));
    auto buttonRow = timeColumn.withHeight (26).withY (
        timeColumn.getY() + (timeColumn.getHeight() - 26) / 2 - 10);
    const int buttonWidth = buttonRow.getWidth() / Delay::numTimeChoices;
    for (auto& button : timeButtons)
        button.setBounds (buttonRow.removeFromLeft (buttonWidth));

    area.removeFromLeft (24);
    auto knobRow = area.removeFromLeft (200).withHeight (knobRowHeight);
    knobRow.setY (getLocalBounds().getCentreY() - knobRowHeight / 2 - 2);
    juce::Slider* sliders[2] = { &feedbackSlider, &toneSlider };
    for (auto* slider : sliders)
    {
        auto cellArea = knobRow.removeFromLeft (100);
        cellArea.removeFromTop (13);
        cellArea.removeFromBottom (15);
        slider->setBounds (cellArea.withSizeKeepingCentre (
            juce::jmin (cellArea.getWidth(), cellArea.getHeight() + 8), cellArea.getHeight()));
    }

    area.removeFromLeft (24);
    pingPongButton.setBounds (area.removeFromLeft (110).withSizeKeepingCentre (110, 26));
}

void DelayEditorView::paint (juce::Graphics& g)
{
    // Timeボタン列の見出し
    if (timeButtons[0].getBounds().getWidth() > 0)
    {
        const auto first = timeButtons[0].getBounds();
        const auto last = timeButtons[Delay::numTimeChoices - 1].getBounds();
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (Fonts::small());
        g.drawText ("TIME", first.getX(), first.getY() - 15,
                    last.getRight() - first.getX(), 12, juce::Justification::centred);
    }

    // ノブ名・数値（Limiterエディタと同じ体裁）
    struct KnobText { const juce::Slider* slider; const char* name; juce::String value; };
    const KnobText texts[2] = {
        { &feedbackSlider, "FEEDBACK",
          juce::String ((int) std::lround (feedbackSlider.getValue())) + " %" },
        { &toneSlider, "TONE",
          juce::String ((int) std::lround (toneSlider.getValue() * 100.0)) + " %" },
    };
    for (const auto& text : texts)
    {
        const auto bounds = text.slider->getBounds();
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (Fonts::small());
        g.drawText (text.name, bounds.getX() - 14, bounds.getY() - 13, bounds.getWidth() + 28, 12,
                    juce::Justification::centred);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawText (text.value, bounds.getX() - 14, bounds.getBottom() + 2, bounds.getWidth() + 28, 13,
                    juce::Justification::centred);
    }
}
