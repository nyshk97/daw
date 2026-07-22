#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// トランスポート用のアイコンボタン（Logic Pro風）。背景はLookAndFeelのTextButton描画に
// 合わせ、文字の代わりにPathでアイコンを描く。名前（AX用）はコンストラクタで渡す
class IconButton : public juce::Button
{
public:
    enum class Icon { play, stop, record, metronome, gear, plus };

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

    // 枠・背景を描かない（歯車など補助ボタン用）。ホバー/押下時だけ薄い背景で反応を示す
    void setBorderless (bool shouldBeBorderless)
    {
        if (borderless != shouldBeBorderless)
        {
            borderless = shouldBeBorderless;
            repaint();
        }
    }

    // アイコン中心からの放射グロー（録音中の明滅表示用）。amount 0で消灯
    void setGlow (float amount, juce::Colour colour)
    {
        if (! juce::exactlyEqual (glowAmount, amount) || glowColour != colour)
        {
            glowAmount = amount;
            glowColour = colour;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        if (borderless)
        {
            if (highlighted || down)
            {
                // hoverは明るいオーバーレイ、押下は暗いオーバーレイで「沈む」表現に揃える
                g.setColour (down ? juce::Colours::black.withAlpha (0.25f)
                                  : juce::Colours::white.withAlpha (0.06f));
                g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
            }
        }
        else
        {
            const auto bg = findColour (getToggleState() ? juce::TextButton::buttonOnColourId
                                                         : juce::TextButton::buttonColourId);
            getLookAndFeel().drawButtonBackground (g, *this, bg, highlighted, down);
        }

        const auto bounds = getLocalBounds().toFloat();

        // グローはアイコンの下・背景の上に重ねる（録音中の明滅ハロー）
        if (glowAmount > 0.001f)
        {
            const auto centre = bounds.getCentre();
            const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.85f;
            g.setGradientFill (juce::ColourGradient (
                glowColour.withAlpha (0.85f * glowAmount), centre.x, centre.y,
                glowColour.withAlpha (0.0f), centre.x, centre.y - radius, true));
            g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f)
                               .withCentre (centre));
        }

        g.setColour (isEnabled() ? iconColour : iconColour.withAlpha (0.35f));

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
                const auto m = juce::Rectangle<float> (side * 1.05f, side * 1.05f)
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
            case Icon::gear:
            {
                const auto centre = bounds.getCentre();
                const float rOuter = side * 0.75f;
                const float rBody  = side * 0.55f;
                const float rHole  = side * 0.27f;
                const int teeth = 8;
                const float step = juce::MathConstants<float>::twoPi / (float) teeth;
                const float half = step * 0.22f; // 歯の角度幅の半分

                juce::Path p;
                for (int i = 0; i < teeth; ++i)
                {
                    const float a = step * (float) i;
                    auto pt = [&] (float radius, float angle)
                    {
                        return centre.getPointOnCircumference (radius, angle);
                    };
                    if (i == 0)
                        p.startNewSubPath (pt (rOuter, a - half));
                    else
                        p.lineTo (pt (rOuter, a - half));
                    p.lineTo (pt (rOuter, a + half));
                    p.lineTo (pt (rBody, a + half * 1.9f));
                    p.lineTo (pt (rBody, a + step - half * 1.9f));
                }
                p.closeSubPath();
                auto rounded = p.createPathWithRoundedCorners (side * 0.06f);
                rounded.addEllipse (juce::Rectangle<float> (rHole * 2.0f, rHole * 2.0f).withCentre (centre));
                rounded.setUsingNonZeroWinding (false); // 中心の穴を抜くため偶奇塗り（rounded生成後に設定しないと消える）
                g.fillPath (rounded);
                break;
            }
            case Icon::plus:
            {
                const auto pr = r.reduced (side * 0.10f); // ＋は他アイコンより一回り控えめにする
                const float stroke = juce::jmax (1.5f, side * 0.14f);
                juce::Path p;
                p.startNewSubPath (pr.getCentreX(), pr.getY());
                p.lineTo (pr.getCentreX(), pr.getBottom());
                p.startNewSubPath (pr.getX(), pr.getCentreY());
                p.lineTo (pr.getRight(), pr.getCentreY());
                g.strokePath (p, juce::PathStrokeType (stroke, juce::PathStrokeType::mitered,
                                                       juce::PathStrokeType::rounded));
                break;
            }
        }
    }

private:
    Icon icon;
    bool borderless = false;
    float glowAmount = 0.0f;
    juce::Colour glowColour;
    juce::Colour iconColour { juce::Colours::white.withAlpha (0.85f) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
};
