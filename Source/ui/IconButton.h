#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// トランスポート用のアイコンボタン（Logic Pro風）。背景はLookAndFeelのTextButton描画に
// 合わせ、文字の代わりにPathでアイコンを描く。名前（AX用）はコンストラクタで渡す
class IconButton : public juce::Button
{
public:
    enum class Icon { play, stop, record, metronome };

    IconButton (Icon initialIcon, const juce::String& accessibleName)
        : juce::Button (accessibleName), icon (initialIcon) {}

    void setIcon (Icon newIcon)
    {
        if (icon != newIcon)
        {
            icon = newIcon;
            repaint();
        }
    }

    void setIconColour (juce::Colour newColour)
    {
        if (iconColour != newColour)
        {
            iconColour = newColour;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto bg = findColour (getToggleState() ? juce::TextButton::buttonOnColourId
                                                     : juce::TextButton::buttonColourId);
        getLookAndFeel().drawButtonBackground (g, *this, bg, highlighted, down);

        g.setColour (isEnabled() ? iconColour : iconColour.withAlpha (0.35f));

        const auto bounds = getLocalBounds().toFloat();
        const float side = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.42f;
        const auto r = juce::Rectangle<float> (side, side).withCentre (bounds.getCentre());

        switch (icon)
        {
            case Icon::play:
            {
                // 三角は左に重く見えるので少し右に寄せる
                juce::Path p;
                p.addTriangle (r.getX(), r.getY(), r.getX(), r.getBottom(),
                               r.getRight(), r.getCentreY());
                p.applyTransform (juce::AffineTransform::translation (side * 0.08f, 0.0f));
                g.fillPath (p);
                break;
            }
            case Icon::stop:
                g.fillRoundedRectangle (r.reduced (side * 0.04f), side * 0.12f);
                break;
            case Icon::record:
                g.fillEllipse (r);
                break;
            case Icon::metronome:
            {
                const auto m = juce::Rectangle<float> (side * 1.25f, side * 1.25f)
                                   .withCentre (bounds.getCentre());
                const float stroke = juce::jmax (1.5f, side * 0.13f);
                juce::Path p; // 上すぼまりの台形（本体）
                p.startNewSubPath (m.getX() + m.getWidth() * 0.36f, m.getY());
                p.lineTo (m.getX() + m.getWidth() * 0.64f, m.getY());
                p.lineTo (m.getRight(), m.getBottom());
                p.lineTo (m.getX(), m.getBottom());
                p.closeSubPath();
                g.strokePath (p, juce::PathStrokeType (stroke, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
                // 振り子（右上へ振れて本体からはみ出す）
                g.drawLine (m.getCentreX(), m.getBottom() - m.getHeight() * 0.22f,
                            m.getX() + m.getWidth() * 0.92f, m.getY() - m.getHeight() * 0.08f,
                            stroke);
                break;
            }
        }
    }

private:
    Icon icon;
    juce::Colour iconColour { juce::Colours::white.withAlpha (0.85f) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
};
