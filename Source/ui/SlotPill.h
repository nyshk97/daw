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
    // kind は固有色（LED・選択枠・ミニGRバー）用。neutral（Instrument・Ext）はLEDを描かない
    void configure (const juce::String& nameToUse, std::atomic<bool>* enabledAtomic, bool grayedToUse,
                    FxVisualKind kindToUse = FxVisualKind::neutral)
    {
        name = nameToUse;
        enabled = enabledAtomic;
        grayed = grayedToUse;
        kind = kindToUse;
        repaint();
    }

    FxVisualKind visualKind() const { return kind; }

    // 下部詳細エディタで開いているスロットの白枠（FXパネルのみ使用）
    void setActiveOutline (bool shouldOutline)
    {
        if (activeOutline == shouldOutline)
            return;
        activeOutline = shouldOutline;
        repaint();
    }

    // ミニGRバー（Compピルのみ使用）。エディタを開かなくても掛かり具合が見える
    //（Logicのチャンネルストリップ常設GR表示と同じ用途）。値は正の減衰量dBで、
    // MainComponentのメータータイマーが一元消費した値の配布を受ける（atomicは読まない）。
    // 負値（-1等）で非表示＝Comp以外のピルは触らなければ何も描かれない
    void setGainReductionDb (float grDbToShow)
    {
        if (std::abs (grDbToShow - grDb) < 0.05f && (grDbToShow > 0.0f) == (grDb > 0.0f))
            return;
        grDb = grDbToShow;
        repaint();
    }

    std::function<void()> onOpenEditor;   // エディタ側クリック（呼び出し側で詳細エディタを開く）
    std::function<void()> onPowerToggled; // ON/OFFトグル後（dirty化・相方UIのrefresh用）

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const bool isOn = enabled == nullptr || enabled->load();
        const bool hovered = ! grayed && isMouseOverOrDragging();

        const auto hue = Theme::fxHue (kind);
        const bool hasLed = kind != FxVisualKind::neutral;

        if (enabled != nullptr && hovered)
        {
            // 電源｜エディタの2分割（Logicの3分割から、固定スロットに不要な差し替え矢印を除いた形）。
            // LED側（左）が電源、右がエディタ
            const bool hoverPower = getMouseXYRelative().x < getWidth() / 2;
            StripParts::drawHardwareButton (g, bounds);

            juce::Graphics::ScopedSaveState save (g);
            juce::Path clip;
            clip.addRoundedRectangle (bounds, 5.0f);
            g.reduceClipRegion (clip);

            auto left = bounds;
            const auto right = left.removeFromRight (bounds.getWidth() * 0.5f);
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRect (hoverPower ? left : right);
            g.setColour (juce::Colours::black.withAlpha (0.45f)); // 分割線
            g.fillRect (juce::Rectangle<float> (right.getX() - 0.5f, bounds.getY(),
                                                1.0f, bounds.getHeight()));

            if (hasLed)
                StripParts::drawLed (g, bounds, hue, isOn);
            g.setColour (Theme::hwValue);
            drawPowerIcon (g, hasLed ? left.withTrimmedLeft (14.0f) : left);
            drawEditIcon (g, right);
        }
        else if (hovered)
        {
            StripParts::drawSlotPill (g, getLocalBounds(), name, isOn, grayed, kind);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRoundedRectangle (bounds, 5.0f);
        }
        else
        {
            StripParts::drawSlotPill (g, getLocalBounds(), name, isOn, grayed, kind);
        }

        // ミニGRバー: 下端2pxを右から左へ伸ばす（GR=下げている量。スケールは0〜24dBで
        // Compエディタと同じ）。ON かつ 掛かっているときだけ・hover中の操作UIには重ねない
        if (grDb > 0.05f && isOn && ! hovered)
        {
            const float fraction = juce::jmin (grDb, 24.0f) / 24.0f;
            const float barWidth = (bounds.getWidth() - 8.0f) * fraction;
            g.setColour (hue.withAlpha (0.9f));
            g.fillRect (juce::Rectangle<float> (bounds.getRight() - 4.0f - barWidth,
                                                bounds.getBottom() - 4.0f, barWidth, 2.0f));
        }

        if (activeOutline)
        {
            g.setColour (hasLed ? hue : juce::Colours::white.withAlpha (0.85f));
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
    FxVisualKind kind = FxVisualKind::neutral;
    bool activeOutline = false;
    float grDb = -1.0f; // ミニGRバー（負=非表示。Compピルのみ配布を受ける）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotPill)
};
