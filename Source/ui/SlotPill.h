#pragma once

#include <atomic>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "StripParts.h"
#include "Theme.h"

// FXスロットのピル1個（Logic風・FXパネルとミキサーのストリップで共有）。
// EQ/Comp（enabledあり）はhoverで「電源｜エディタ」の2分割に変わり、
// 電源=ON/OFFトグル・エディタ側クリックで onOpenEditor。
// enabledなし（バスのReverb等・常時ON）はhoverで全体が明るくなりクリックで onOpenEditor。
// 空きスロット（grayed）は反応しない
class SlotPill : public juce::Component
{
public:
    SlotPill()
    {
        setRepaintsOnMouseActivity (true);
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    // enabledAtomic の実体は TrackParams（Trackが所有）。bind対象が変わるたびに差し替えること
    void configure (const juce::String& nameToUse, std::atomic<bool>* enabledAtomic, bool grayedToUse)
    {
        name = nameToUse;
        enabled = enabledAtomic;
        grayed = grayedToUse;
        repaint();
    }

    // 下部詳細エディタで開いているスロットの白枠（FXパネルのみ使用）
    void setActiveOutline (bool shouldOutline)
    {
        if (activeOutline == shouldOutline)
            return;
        activeOutline = shouldOutline;
        repaint();
    }

    std::function<void()> onOpenEditor;   // エディタ側クリック（呼び出し側で詳細エディタを開く）
    std::function<void()> onPowerToggled; // ON/OFFトグル後（dirty化・相方UIのrefresh用）

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const bool isOn = enabled == nullptr || enabled->load();
        const bool hovered = ! grayed && isMouseOverOrDragging();

        if (enabled != nullptr && hovered)
        {
            // 電源｜エディタの2分割（Logicの3分割から、固定スロットに不要な差し替え矢印を除いた形）
            const auto base = isOn ? Theme::accent : Theme::controlBg;
            const bool hoverPower = getMouseXYRelative().x < getWidth() / 2;

            juce::Graphics::ScopedSaveState save (g);
            juce::Path clip;
            clip.addRoundedRectangle (bounds, 5.0f);
            g.reduceClipRegion (clip);

            auto left = bounds;
            const auto right = left.removeFromRight (bounds.getWidth() * 0.5f);
            g.setColour (hoverPower ? base.brighter (0.18f) : base);
            g.fillRect (left);
            g.setColour (hoverPower ? base : base.brighter (0.18f));
            g.fillRect (right);
            g.setColour (juce::Colours::black.withAlpha (0.35f)); // 分割線
            g.fillRect (juce::Rectangle<float> (right.getX() - 0.5f, bounds.getY(),
                                                1.0f, bounds.getHeight()));

            g.setColour (juce::Colours::white.withAlpha (0.95f));
            drawPowerIcon (g, left);
            drawEditIcon (g, right);
        }
        else if (hovered)
        {
            const auto base = isOn ? Theme::accent : Theme::controlBg;
            g.setColour (base.brighter (0.1f));
            g.fillRoundedRectangle (bounds, 5.0f);
            g.setColour (juce::Colours::white.withAlpha (isOn ? 0.95f : 0.75f));
            g.setFont (Fonts::smallStrong());
            g.drawText (name, getLocalBounds(), juce::Justification::centred);
        }
        else
        {
            StripParts::drawSlotPill (g, getLocalBounds(), name, isOn, grayed);
        }

        if (activeOutline)
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawRoundedRectangle (bounds.reduced (0.75f), 5.0f, 1.5f);
        }
    }

    void mouseMove (const juce::MouseEvent&) override
    {
        repaint(); // 2分割の左右ハイライトをカーソルに追従させる
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (grayed)
            return;
        if (enabled != nullptr && e.x < getWidth() / 2)
        {
            enabled->store (! enabled->load()); // 左半分=電源トグル
            repaint();
            if (onPowerToggled)
                onPowerToggled();
            return;
        }
        if (onOpenEditor)
            onOpenEditor();
    }

private:
    // 電源アイコン（円弧＋上の縦線）。areaの中心に描く
    static void drawPowerIcon (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto c = area.getCentre();
        const float r = 4.5f;
        juce::Path p;
        p.addCentredArc (c.x, c.y + 1.0f, r, r, 0.0f, 0.7f,
                         juce::MathConstants<float>::twoPi - 0.7f, true);
        p.startNewSubPath (c.x, c.y - r - 1.0f);
        p.lineTo (c.x, c.y - 0.5f);
        g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    // エディタアイコン（スライダー2本: 横線＋つまみの丸）。areaの中心に描く
    static void drawEditIcon (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto c = area.getCentre();
        for (int i = 0; i < 2; ++i)
        {
            const float ly = c.y + (i == 0 ? -3.0f : 3.0f);
            const float thumbX = c.x + (i == 0 ? -2.0f : 2.0f);
            g.fillRect (juce::Rectangle<float> (c.x - 5.5f, ly - 0.7f, 11.0f, 1.4f));
            g.fillEllipse (juce::Rectangle<float> (3.6f, 3.6f).withCentre ({ thumbX, ly }));
        }
    }

    juce::String name;
    std::atomic<bool>* enabled = nullptr;
    bool grayed = false;
    bool activeOutline = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotPill)
};
