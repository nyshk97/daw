#pragma once

#include <functional>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"
#include "../shared/Project.h"

// send量ノブ（ロータリー＋豆ラベル）。通常はバスの頭文字（A/B/D）、ドラッグ中は送り量0〜100を表示。
// 値は TrackParams::sends の atomic に直接書く。ミキサーのストリップと下部FXエディタのSends区画で共有する
class SendKnob : public juce::Component
{
public:
    static constexpr int labelHeight = 12;

    explicit SendKnob (int busIndex) : bus (busIndex)
    {
        addAndMakeVisible (knob);
        knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setRange (0.0, 1.0);
        knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob.setDoubleClickReturnValue (true, 0.0);
        knob.setColour (juce::Slider::rotarySliderFillColourId, Theme::accent);
        knob.setColour (juce::Slider::rotarySliderOutlineColourId, Theme::controlBg);
        knob.setWantsKeyboardFocus (false);          // Space（再生/停止）を奪わせない
        knob.setMouseClickGrabsKeyboardFocus (false);
        knob.onValueChange = [this]
        {
            if (params != nullptr)
                params->sends[bus].store ((float) knob.getValue());
            repaint (labelArea); // ラベルの値表示を追従させる
            if (onChanged)
                onChanged();
        };
        knob.onDragStart = [this] { dragging = true; repaint (labelArea); };
        knob.onDragEnd = [this] { dragging = false; repaint (labelArea); };
    }

    void bind (std::shared_ptr<TrackParams> newParams)
    {
        params = std::move (newParams);
        refreshValue();
    }

    // 表示のみ更新（同じatomicを表示する別UI＝ミキサー/エディタ側での変更を反映する）
    void refreshValue()
    {
        if (params != nullptr)
            knob.setValue (params->sends[bus].load(), juce::dontSendNotification);
        repaint (labelArea);
    }

    std::function<void()> onChanged;

    void resized() override
    {
        auto area = getLocalBounds();
        labelArea = area.removeFromBottom (labelHeight);
        const int diameter = juce::jmin (area.getWidth(), area.getHeight());
        knob.setBounds (area.withSizeKeepingCentre (diameter, diameter));
    }

    void paint (juce::Graphics& g) override
    {
        if (params == nullptr)
            return;
        const auto text = dragging
                              ? juce::String (juce::roundToInt (params->sends[bus].load() * 100.0f))
                              : juce::String (SendBuses::shortNames[bus]);
        g.setColour (juce::Colours::white.withAlpha (dragging ? 0.8f : 0.45f));
        g.setFont (Fonts::small());
        g.drawText (text, labelArea, juce::Justification::centred);
    }

private:
    int bus;
    std::shared_ptr<TrackParams> params;
    juce::Slider knob;
    bool dragging = false;
    juce::Rectangle<int> labelArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SendKnob)
};
