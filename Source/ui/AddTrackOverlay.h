#pragma once

#include <cmath>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"
#include "TrackIcons.h"
#include "../shared/Project.h"

// トラック追加メニュー。juce::PopupMenuはOSの「使用可能画面領域」（Dock除け）に
// クランプされるため、全画面表示だと画面最下部の＋ボタンに隣接表示できない
// （下向きは余白が出る・withParentComponentでも左上へ飛ぶ）。項目2つの固定メニュー
// なので、親コンポーネント全面を透明に覆いanchor（＋ボタン）の直上にパネルを
// 自前描画するオーバーレイにする。パネル外クリックで閉じる
class AddTrackOverlay : public juce::Component
{
public:
    AddTrackOverlay()
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    std::function<void (TrackType)> onPick;

    // anchorBounds: 親コンポーネント座標での＋ボタンのbounds
    void show (juce::Rectangle<int> anchorBounds)
    {
        anchor = anchorBounds;
        hoveredItem = -1;
        setVisible (true);
        toFront (false);
        repaint();
    }

    void dismiss() { setVisible (false); }

    void setAnchor (juce::Rectangle<int> anchorBounds)
    {
        anchor = anchorBounds;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto panel = panelBounds().toFloat();
        g.setColour (Theme::popupBg);
        g.fillRoundedRectangle (panel, 5.0f);
        g.setColour (Theme::popupBorder);
        g.drawRoundedRectangle (panel.reduced (0.5f), 5.0f, 1.0f);

        for (int i = 0; i < numItems; ++i)
        {
            const auto row = itemBounds (i);
            if (i == hoveredItem)
            {
                g.setColour (Theme::accent);
                g.fillRoundedRectangle (row.reduced (3, 1).toFloat(), 3.0f);
            }
            g.setColour (juce::Colours::white.withAlpha (0.9f));

            const auto iconArea = juce::Rectangle<float> ((float) iconSize, (float) iconSize)
                                      .withCentre ({ (float) row.getX() + itemInsetX + (float) iconSize * 0.5f,
                                                     (float) row.getCentreY() });
            if (i == 0)
                TrackIcons::drawWaveform (g, iconArea);
            else
                TrackIcons::drawKeyboard (g, iconArea);

            g.setFont (Fonts::body());
            g.drawText (itemLabel (i),
                        row.withTrimmedLeft (itemInsetX + iconSize + iconTextGap).withTrimmedRight (8),
                        juce::Justification::centredLeft);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        int item = -1;
        for (int i = 0; i < numItems; ++i)
            if (itemBounds (i).contains (e.getPosition()))
                item = i;
        if (item != hoveredItem)
        {
            hoveredItem = item;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoveredItem != -1)
        {
            hoveredItem = -1;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < numItems; ++i)
        {
            if (itemBounds (i).contains (e.getPosition()))
            {
                dismiss();
                if (onPick)
                    onPick (i == 0 ? TrackType::audio : TrackType::midi);
                return;
            }
        }
        dismiss(); // パネル外クリック
    }

private:
    static constexpr int numItems = 2;
    static constexpr int itemHeight = 26;
    static constexpr int panelPaddingY = 4;
    static constexpr int itemInsetX = 12;
    static constexpr int iconSize = 16;
    static constexpr int iconTextGap = 8;

    static juce::String itemLabel (int i)
    {
        return juce::String::fromUTF8 (i == 0 ? u8"オーディオトラック"
                                              : u8"ソフトウェア音源トラック");
    }

    juce::Rectangle<int> panelBounds() const
    {
        // アイコン分のインセットが付くとanchor幅（＋ボタン幅）では文字が入り切らない
        // ことがあるため、最長ラベルの実測幅ぶんまで右に広げる
        float maxTextW = 0.0f;
        for (int i = 0; i < numItems; ++i)
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (Fonts::body(), itemLabel (i), 0.0f, 0.0f);
            maxTextW = juce::jmax (maxTextW, ga.getBoundingBox (0, -1, true).getWidth());
        }
        const int w = juce::jmax (anchor.getWidth(),
                                  itemInsetX + iconSize + iconTextGap
                                      + juce::roundToInt (std::ceil (maxTextW)) + 14);
        const int h = numItems * itemHeight + panelPaddingY * 2;
        return { anchor.getX(), anchor.getY() - 2 - h, w, h };
    }

    juce::Rectangle<int> itemBounds (int i) const
    {
        const auto p = panelBounds().reduced (0, panelPaddingY);
        return { p.getX(), p.getY() + i * itemHeight, p.getWidth(), itemHeight };
    }

    juce::Rectangle<int> anchor;
    int hoveredItem = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddTrackOverlay)
};
