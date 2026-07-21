#pragma once

#include <cmath>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
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
        g.setColour (juce::Colour (0xff2c2c30));
        g.fillRoundedRectangle (panel, 5.0f);
        g.setColour (juce::Colour (0xff55555a));
        g.drawRoundedRectangle (panel.reduced (0.5f), 5.0f, 1.0f);

        for (int i = 0; i < numItems; ++i)
        {
            const auto row = itemBounds (i);
            if (i == hoveredItem)
            {
                g.setColour (juce::Colour (0xff4a6ea9));
                g.fillRoundedRectangle (row.reduced (3, 1).toFloat(), 3.0f);
            }
            g.setColour (juce::Colours::white.withAlpha (0.9f));

            const auto iconArea = juce::Rectangle<float> ((float) iconSize, (float) iconSize)
                                      .withCentre ({ (float) row.getX() + itemInsetX + (float) iconSize * 0.5f,
                                                     (float) row.getCentreY() });
            if (i == 0)
                drawWaveformIcon (g, iconArea);
            else
                drawKeyboardIcon (g, iconArea);

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

    // オーディオトラック: 波形（中心線対称の縦バー）
    static void drawWaveformIcon (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const float heights[] = { 0.35f, 0.75f, 1.0f, 0.55f, 0.85f, 0.3f };
        const int n = juce::numElementsInArray (heights);
        const float barW = 1.8f;
        const float step = (r.getWidth() - barW) / (float) (n - 1);
        for (int i = 0; i < n; ++i)
        {
            const float h = r.getHeight() * heights[i];
            g.fillRoundedRectangle (r.getX() + step * (float) i, r.getCentreY() - h * 0.5f,
                                    barW, h, barW * 0.5f);
        }
    }

    // ソフトウェア音源トラック: 鍵盤（外枠＋黒鍵2つ＋黒鍵下の区切り線）
    static void drawKeyboardIcon (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto kb = r.reduced (0.0f, r.getHeight() * 0.16f); // 横長にする
        const float stroke = 1.2f;
        g.drawRoundedRectangle (kb.reduced (stroke * 0.5f), 2.0f, stroke);

        const float keyW = kb.getWidth() / 3.0f;
        const float blackH = kb.getHeight() * 0.52f;
        for (int k = 1; k <= 2; ++k)
        {
            const float x = kb.getX() + keyW * (float) k;
            const float blackW = keyW * 0.55f;
            g.fillRect (x - blackW * 0.5f, kb.getY() + stroke, blackW, blackH);
            g.drawLine (x, kb.getY() + blackH, x, kb.getBottom() - stroke, stroke);
        }
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
