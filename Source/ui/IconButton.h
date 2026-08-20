#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// トランスポート用のアイコンボタン（Logic Pro風）。背景はLookAndFeelのTextButton描画に
// 合わせ、文字の代わりにPathでアイコンを描く。名前（AX用）はコンストラクタで渡す
class IconButton : public juce::Button
{
public:
    enum class Icon { play, stop, record, metronome, gear, plus, notes, folder, speaker,
                      speakerMuted, sort, dice, inspector };

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

    // borderlessボタンのON時のアイコン色（地はbuttonOnColourIdを薄く敷く）
    void setToggleIconColour (juce::Colour newColour)
    {
        if (toggleIconColour != newColour)
        {
            toggleIconColour = newColour;
            repaint();
        }
    }

    // 枠・背景を描かない（右上の補助ボタン用）。ホバー/押下時は薄い背景で反応を示し、
    // ON状態はアクセント色を薄く敷いた地＋色付きアイコンで示す（ベタ塗りより一段沈める）
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
            const bool on = getToggleState();
            if (on || highlighted || down)
            {
                // ONはアクセント地（hover/押下でさらに濃く）、OFFのhoverは明るいオーバーレイ、
                // 押下は暗いオーバーレイで「沈む」表現に揃える
                g.setColour (on ? findColour (juce::TextButton::buttonOnColourId)
                                      .withAlpha (highlighted || down ? 0.32f : 0.24f)
                                : (down ? juce::Colours::black.withAlpha (0.25f)
                                        : juce::Colours::white.withAlpha (0.07f)));
                g.fillRoundedRectangle (getLocalBounds().toFloat(), 7.0f);
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

        // borderlessはOFF/hoverを白の明度で、ONを色付きで示す。
        // 枠付きボタンは従来どおりiconColour（録音の赤など）をそのまま使う
        if (borderless)
        {
            if (getToggleState())
                g.setColour (isEnabled() ? toggleIconColour : toggleIconColour.withAlpha (0.35f));
            else
                g.setColour (juce::Colours::white.withAlpha (! isEnabled()         ? 0.30f
                                                             : (highlighted || down) ? 0.92f
                                                                                     : 0.62f));
        }
        else
        {
            g.setColour (isEnabled() ? iconColour : iconColour.withAlpha (0.35f));
        }

        // 線画アイコン（メモ・フォルダ・歯車）は塗り図形と同じ寸法だと軽く見えるので一回り大きくする
        const bool strokeIcon = icon == Icon::notes || icon == Icon::folder || icon == Icon::gear
                                || icon == Icon::speaker || icon == Icon::speakerMuted
                                || icon == Icon::sort || icon == Icon::dice || icon == Icon::inspector;
        const float side = juce::jmin (bounds.getWidth(), bounds.getHeight())
                           * (strokeIcon ? 0.57f : 0.42f);
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
                const float rOuter = design (side, 9.2f);
                const float rBody  = design (side, 6.4f);
                const float rHole  = design (side, 2.9f);
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
                const float stroke = strokeWidth (side);
                g.strokePath (p.createPathWithRoundedCorners (side * 0.06f),
                              juce::PathStrokeType (stroke, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
                g.drawEllipse (juce::Rectangle<float> (rHole * 2.0f, rHole * 2.0f).withCentre (centre),
                               stroke);
                break;
            }
            case Icon::inspector:
            {
                // ◯に「i」（Logicのインスペクタと同じ記号。左のFXパネル＝選択トラックの詳細を開く）
                const float stroke = strokeWidth (side);
                const auto centre = bounds.getCentre();
                const float rad = design (side, 9.0f);
                g.drawEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (centre), stroke);
                g.drawLine ({ at (r, 12.0f, 10.6f), at (r, 12.0f, 16.8f) }, stroke);
                g.fillEllipse (juce::Rectangle<float> (stroke * 1.5f, stroke * 1.5f)
                                   .withCentre (at (r, 12.0f, 7.6f)));
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
            case Icon::notes:
            {
                const float stroke = strokeWidth (side);
                g.drawRoundedRectangle ({ at (r, 4.6f, 3.6f), at (r, 19.4f, 20.4f) },
                                        design (side, 2.6f), stroke);
                // 罫線3本（最下段だけ短くして「書きかけのメモ」に見せる）
                const float rows[3][2] = { { 8.6f, 15.8f }, { 12.0f, 15.8f }, { 15.4f, 12.8f } };
                for (const auto& row : rows)
                    g.drawLine ({ at (r, 8.2f, row[0]), at (r, row[1], row[0]) }, stroke);
                break;
            }
            case Icon::sort:
            {
                const float stroke = strokeWidth (side);
                // 長さの違う横線3本（上が長い）＋右に下向き矢印
                juce::Path lines;
                const float rows[3][2] = { { 6.6f, 15.4f }, { 12.0f, 12.2f }, { 17.4f, 9.0f } };
                for (const auto& row : rows)
                {
                    lines.startNewSubPath (at (r, 3.4f, row[0]));
                    lines.lineTo (at (r, row[1], row[0]));
                }
                g.strokePath (lines, juce::PathStrokeType (stroke, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
                juce::Path arrow;
                arrow.startNewSubPath (at (r, 19.4f, 5.4f));
                arrow.lineTo (at (r, 19.4f, 18.6f));
                arrow.startNewSubPath (at (r, 16.4f, 15.4f));
                arrow.lineTo (at (r, 19.4f, 18.6f));
                arrow.lineTo (at (r, 22.4f, 15.4f));
                g.strokePath (arrow, juce::PathStrokeType (stroke, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
                break;
            }
            case Icon::speaker:
            case Icon::speakerMuted:
            {
                const float stroke = strokeWidth (side);
                // 本体（左の箱＋右へ広がるコーン）を一筆で描き、右側に音波を2本重ねる
                const float pts[6][2] = { { 3.2f, 9.4f },  { 7.0f, 9.4f },  { 12.0f, 4.4f },
                                          { 12.0f, 19.6f }, { 7.0f, 14.6f }, { 3.2f, 14.6f } };
                juce::Path body;
                body.startNewSubPath (at (r, pts[0][0], pts[0][1]));
                for (int i = 1; i < 6; ++i)
                    body.lineTo (at (r, pts[i][0], pts[i][1]));
                body.closeSubPath();
                g.strokePath (body.createPathWithRoundedCorners (design (side, 1.4f)),
                              juce::PathStrokeType (stroke, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

                const auto centre = at (r, 12.0f, 12.0f);
                const float quarter = juce::MathConstants<float>::halfPi * 0.5f;
                juce::Path waves; // 3時方向を中心に±45°の弧
                for (const float radius : { 4.4f, 7.4f })
                    waves.addCentredArc (centre.x, centre.y, design (side, radius),
                                         design (side, radius), 0.0f,
                                         juce::MathConstants<float>::halfPi - quarter,
                                         juce::MathConstants<float>::halfPi + quarter, true);
                g.strokePath (waves, juce::PathStrokeType (stroke, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));

                // 鳴らない側は全体に斜線を重ねる（色だけの違いにせず形で区別する）
                if (icon == Icon::speakerMuted)
                {
                    juce::Path slash;
                    slash.startNewSubPath (at (r, 3.8f, 20.6f));
                    slash.lineTo (at (r, 20.6f, 3.8f));
                    g.strokePath (slash, juce::PathStrokeType (stroke * 1.15f,
                                                              juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));
                }
                break;
            }
            case Icon::dice:
            {
                // サイコロ（3の目・対角線）。枠は線画、目は塗り
                const float stroke = strokeWidth (side);
                g.drawRoundedRectangle ({ at (r, 3.8f, 3.8f), at (r, 20.2f, 20.2f) },
                                        design (side, 4.4f), stroke);
                const float pip = design (side, 1.7f);
                const float centres[3][2] = { { 8.3f, 8.3f }, { 12.0f, 12.0f }, { 15.7f, 15.7f } };
                for (const auto& c : centres)
                    g.fillEllipse (juce::Rectangle<float> (pip * 2.0f, pip * 2.0f)
                                       .withCentre (at (r, c[0], c[1])));
                break;
            }
            case Icon::folder:
            {
                const float stroke = strokeWidth (side);
                // 左上にタブが立ち上がる形。角丸はcreatePathWithRoundedCornersで一括して付ける
                const float pts[6][2] = { { 3.4f, 4.6f },   { 8.9f, 4.6f },   { 11.0f, 7.1f },
                                          { 20.6f, 7.1f },  { 20.6f, 19.9f }, { 3.4f, 19.9f } };
                juce::Path p;
                p.startNewSubPath (at (r, pts[0][0], pts[0][1]));
                for (int i = 1; i < 6; ++i)
                    p.lineTo (at (r, pts[i][0], pts[i][1]));
                p.closeSubPath();
                g.strokePath (p.createPathWithRoundedCorners (design (side, 2.0f)),
                              juce::PathStrokeType (stroke, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
                break;
            }
        }
    }

private:
    // 線画アイコンは24x24のデザインボックス上で形を定義し、実サイズへ換算して描く
    // （SVGのモックからそのまま移植でき、サイズを変えても比率が崩れない）
    static float design (float side, float value) { return side * value / 24.0f; }

    static juce::Point<float> at (juce::Rectangle<float> box, float x, float y)
    {
        return { box.getX() + design (box.getWidth(), x), box.getY() + design (box.getHeight(), y) };
    }

    // 線画アイコン共通の線幅（デザインボックス上で1.6）。細くなりすぎないよう下限を置く
    static float strokeWidth (float side) { return juce::jmax (1.0f, design (side, 1.6f)); }

    Icon icon;
    bool borderless = false;
    float glowAmount = 0.0f;
    juce::Colour glowColour;
    juce::Colour iconColour { juce::Colours::white.withAlpha (0.85f) };
    juce::Colour toggleIconColour { juce::Colours::white }; // borderlessのON時（setToggleIconColourで上書き）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
};
