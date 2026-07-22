#pragma once

#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Shortcuts.h"

// ⌘? のショートカット一覧オーバーレイ。AddTrackOverlayと同じ「親全面を透明に覆い
// パネルを自前描画」方式。項目は Shortcuts::table の走査で自動生成するので、
// ショートカット追加時にこのファイルを触る必要はない。
// 閉じる: パネル外クリック / Esc / ⌘?（キー処理は MainComponent::keyPressed。
// 表示中は閉じる操作以外のキーも keyPressed 側で消費しモーダルにする）
class ShortcutListOverlay : public juce::Component
{
public:
    ShortcutListOverlay()
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    void show()
    {
        setVisible (true);
        toFront (false);
        repaint();
    }

    void dismiss() { setVisible (false); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.45f)); // 背後を暗くして一覧に集中させる

        const auto panel = panelBounds();
        g.setColour (juce::Colour (0xff2c2c30));
        g.fillRoundedRectangle (panel.toFloat(), 8.0f);
        g.setColour (juce::Colour (0xff55555a));
        g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), 8.0f, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.95f));
        g.setFont (Fonts::title());
        g.drawText (juce::String::fromUTF8 (u8"キーボードショートカット"),
                    panel.withHeight (titleHeight).withTrimmedTop (padY),
                    juce::Justification::centred);

        for (int col = 0; col < 2; ++col)
        {
            const int x = panel.getX() + padX + col * (columnWidth + columnGap);
            int y = panel.getY() + padY + titleHeight;

            for (const auto category : columnCategories (col))
            {
                g.setColour (juce::Colours::white.withAlpha (0.5f));
                g.setFont (Fonts::bodyStrong());
                g.drawText (Shortcuts::categoryName (category),
                            x, y, columnWidth, headingHeight, juce::Justification::bottomLeft);
                y += headingHeight + headingGap;

                for (const auto& e : Shortcuts::table)
                {
                    if (e.category != category)
                        continue;
                    g.setColour (juce::Colours::white.withAlpha (0.75f));
                    g.setFont (Fonts::mono (12.0f));
                    g.drawText (juce::String::fromUTF8 (e.keyLabel),
                                x, y, keyColumnWidth, rowHeight, juce::Justification::centredRight);
                    g.setColour (juce::Colours::white.withAlpha (0.9f));
                    g.setFont (Fonts::body());
                    g.drawText (juce::String::fromUTF8 (e.name),
                                x + keyColumnWidth + keyNameGap, y,
                                columnWidth - keyColumnWidth - keyNameGap, rowHeight,
                                juce::Justification::centredLeft);
                    y += rowHeight;
                }
                y += sectionGap;
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! panelBounds().contains (e.getPosition()))
            dismiss();
    }

private:
    static constexpr int rowHeight = 22;
    static constexpr int headingHeight = 20;
    static constexpr int headingGap = 4;
    static constexpr int sectionGap = 14;
    static constexpr int titleHeight = 40;
    static constexpr int keyColumnWidth = 82;
    static constexpr int keyNameGap = 12;
    static constexpr int columnWidth = 300;
    static constexpr int columnGap = 32;
    static constexpr int padX = 28;
    static constexpr int padY = 18;

    // 2カラム構成。カテゴリの振り分けだけはここで決める（行数は自動追従）
    static std::vector<Shortcuts::Category> columnCategories (int col)
    {
        using C = Shortcuts::Category;
        if (col == 0)
            return { C::transport, C::editing, C::track };
        return { C::pianoRoll, C::view, C::project };
    }

    static int countRows (Shortcuts::Category category)
    {
        int n = 0;
        for (const auto& e : Shortcuts::table)
            if (e.category == category)
                ++n;
        return n;
    }

    juce::Rectangle<int> panelBounds() const
    {
        int maxColumnH = 0;
        for (int col = 0; col < 2; ++col)
        {
            int h = 0;
            for (const auto category : columnCategories (col))
                h += headingHeight + headingGap + countRows (category) * rowHeight + sectionGap;
            maxColumnH = juce::jmax (maxColumnH, h - sectionGap); // 最終セクションの後隙間は不要
        }
        const int w = padX * 2 + columnWidth * 2 + columnGap;
        const int h = padY * 2 + titleHeight + maxColumnH;
        return juce::Rectangle<int> (w, h).withCentre (getLocalBounds().getCentre());
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShortcutListOverlay)
};
