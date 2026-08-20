#pragma once

#include <functional>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "FxSlotLayout.h"
#include "StripParts.h"
#include "Theme.h"
#include "../shared/Project.h"

// Sendsの1行（Logicのsendスロット風: バス名ピル＋右に小ノブ）。
// ピルはsend>0で点灯、ノブは緑の値アークでドラッグ中は送り量0〜100をポップアップ表示。
// 値は TrackParams::sends の atomic に直接書く。FXパネルとミキサーのストリップで共有する
class SendRow : public juce::Component
{
public:
    static constexpr int preferredHeight = 32; // 小ノブ30px（大ノブと同じ描き方・目盛り7本）が収まる高さ

    explicit SendRow (int busIndex) : bus (busIndex)
    {
        addAndMakeVisible (knob);
        knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setRange (0.0, 1.0);
        knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob.setDoubleClickReturnValue (true, 0.0);
        knob.setColour (juce::Slider::rotarySliderFillColourId, Theme::fxHue (FxSlots::busVisualKind (busIndex)));
        knob.setColour (juce::Slider::rotarySliderOutlineColourId, Theme::controlBg);
        knob.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)); };
        knob.setWantsKeyboardFocus (false);          // Space（再生/停止）を奪わせない
        knob.setMouseClickGrabsKeyboardFocus (false);
        knob.onValueChange = [this]
        {
            if (params != nullptr)
                params->sends[bus].store ((float) knob.getValue());
            repaint(); // ピルの点灯（send>0）を追従させる
            if (onChanged)
                onChanged();
        };
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    void parentHierarchyChanged() override
    {
        // ポップアップは行の外へはみ出すため、行自身でなくトップレベルに載せる
        knob.setPopupDisplayEnabled (true, false, getTopLevelComponent());
    }

    void bind (std::shared_ptr<TrackParams> newParams)
    {
        params = std::move (newParams);
        refreshValue();
    }

    // 表示のみ更新（同じatomicを表示する別UI＝ミキサー/FXパネルの相方での変更を反映する）
    void refreshValue()
    {
        if (params != nullptr)
            knob.setValue (params->sends[bus].load(), juce::dontSendNotification);
        repaint();
    }

    std::function<void()> onChanged;

    void resized() override
    {
        auto area = getLocalBounds();
        const int knobSize = juce::jmin (30, area.getHeight());
        knob.setBounds (area.removeFromRight (knobSize).withSizeKeepingCentre (knobSize, knobSize));
        area.removeFromRight (6);
        pillArea = area.withSizeKeepingCentre (area.getWidth(), juce::jmin (26, area.getHeight())); // ピルはFXスロットと同じ高さに揃える
    }

    void paint (juce::Graphics& g) override
    {
        // バスのピル: send>0 でLED点灯（色はバス固有色）。スロットピルと同じ部品で描く
        const bool active = params != nullptr && params->sends[bus].load() > 0.001f;
        StripParts::drawSlotPill (g, pillArea, SendBuses::names[bus], active, false,
                                  FxSlots::busVisualKind (bus));
    }

private:
    int bus;
    std::shared_ptr<TrackParams> params;
    juce::Slider knob;
    juce::Rectangle<int> pillArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SendRow)
};
