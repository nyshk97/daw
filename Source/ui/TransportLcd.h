#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"

// トランスポート情報（BPM・経過時間）をまとめて表示するLCD風パネル
// （Logic Proの中央ディスプレイのイメージ）。表示に徹してモデルへの参照を持たない。
// BPMはクリックで編集できるLabelのままにし、値の検証・反映は所有側が
// tempoLabel().onTextChange で行う
class TransportLcd : public juce::Component
{
public:
    static constexpr int preferredWidth = 156;

    TransportLcd()
    {
        const auto setupValue = [this] (juce::Label& label)
        {
            addAndMakeVisible (label);
            label.setFont (Fonts::mono (20.0f)); // 数字はLCDの主役なので大きめ（BPM/TIME見出しとの階層を付ける）
            label.setJustificationType (juce::Justification::centred);
            label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.87f));
        };
        setupValue (tempo);
        setupValue (time);

        time.setInterceptsMouseClicks (false, false);

        tempo.setEditable (true, false, false);
        tempo.setMouseCursor (juce::MouseCursor::IBeamCursor);
        tempo.setColour (juce::Label::backgroundWhenEditingColourId, Theme::lcdEditBg);
        tempo.setColour (juce::Label::outlineWhenEditingColourId, Theme::accent);
        tempo.setColour (juce::Label::textWhenEditingColourId, juce::Colours::white);
    }

    juce::Label& tempoLabel() { return tempo; }

    void setTimeText (const juce::String& text) { time.setText (text, juce::dontSendNotification); }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        constexpr float corner = 6.0f;

        g.setColour (Theme::lcdBg);
        g.fillRoundedRectangle (bounds, corner);

        // 上端内側に影を落として「バーにはめ込まれた液晶」に見せる
        {
            juce::Graphics::ScopedSaveState save (g);
            juce::Path clip;
            clip.addRoundedRectangle (bounds, corner);
            g.reduceClipRegion (clip);
            g.setGradientFill (juce::ColourGradient (
                juce::Colours::black.withAlpha (0.30f), 0.0f, bounds.getY(),
                juce::Colours::transparentBlack, 0.0f, bounds.getY() + 5.0f, false));
            g.fillRect (bounds.withHeight (5.0f));
        }

        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), corner, 1.0f);

        g.setFont (Fonts::small().withExtraKerningFactor (0.06f));
        for (int i = 0; i < numSections; ++i)
        {
            const auto sec = sectionBounds (i);
            g.setColour (Theme::lcdLabel);
            g.drawText (captions[i], sec.withHeight (captionHeight).withTrimmedTop (3),
                        juce::Justification::centred);
            if (i > 0)
            {
                g.setColour (juce::Colours::white.withAlpha (0.07f));
                g.fillRect (sec.getX(), 6, 1, getHeight() - 12);
            }
        }
    }

    void resized() override
    {
        juce::Label* labels[numSections] = { &tempo, &time };
        for (int i = 0; i < numSections; ++i)
            labels[i]->setBounds (sectionBounds (i).withTrimmedTop (captionHeight));
    }

private:
    static constexpr int numSections = 2;
    static constexpr int captionHeight = 14;
    static constexpr int sectionWidths[numSections] = { 64, 92 };
    static constexpr const char* captions[numSections] = { "BPM", "TIME" };

    juce::Rectangle<int> sectionBounds (int index) const
    {
        int x = 0;
        for (int i = 0; i < index; ++i)
            x += sectionWidths[i];
        return { x, 0, sectionWidths[index], getHeight() };
    }

    juce::Label tempo, time;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportLcd)
};
