#pragma once

#include <algorithm>
#include <functional>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"

// ファイルブラウザのパス表示 兼 階層ナビゲーション。
// 「… › d0ne1s › Downloads」のように現在地までの階層を並べ、クリックでその階層へ移動する。
//
// 「パス表示」と「上へ移動」を1つにまとめるための部品:
// 狭い右パネル（240〜480px）でボタン列に幅を取られず、1つ左のセグメント＝親フォルダなので
// 専用の「↑」ボタンが要らない。祖先へも一発で飛べる。
// 幅が足りないときは古い（左の）階層から畳んで先頭に「…」を出し、押すとポップアップで選べる。
// 現在フォルダは常に見えるようにする（末尾優先。左寄せのフルパス表示だと逆に切れていた）。
class BreadcrumbBar final : public juce::Component
{
public:
    BreadcrumbBar()
    {
        setWantsKeyboardFocus (false);
    }

    // クリックされた階層へ移動する要求。現在フォルダ自身は押せないので呼ばれない
    std::function<void (const juce::File&)> onNavigate;

    void setPath (const juce::File& directory)
    {
        chain.clear();
        for (auto dir = directory; dir != juce::File(); dir = dir.getParentDirectory())
        {
            chain.insert (chain.begin(), dir);
            if (dir.getParentDirectory() == dir) // ルートに到達（親が自分自身）
                break;
        }
        hovered = -1;
        layoutSegments();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setFont (font());
        if (hasHidden)
            paintChip (g, dotsBounds, juce::String::fromUTF8 (u8"…"), false, hovered == dotsIndex);

        for (int i = 0; i < (int) segments.size(); ++i)
        {
            const auto& s = segments[(size_t) i];
            if (i > 0 || hasHidden)
                paintSeparator (g, s.bounds.getX());
            paintChip (g, s.bounds, s.label, s.chainIndex == (int) chain.size() - 1,
                       hovered == i);
        }
    }

    void resized() override { layoutSegments(); }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int hit = indexAt (e.getPosition());
        if (hit == hovered)
            return;
        hovered = hit;
        setMouseCursor (hit >= 0 || hit == dotsIndex ? juce::MouseCursor::PointingHandCursor
                                                     : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hovered == -1)
            return;
        hovered = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int hit = indexAt (e.getPosition());
        if (hit == dotsIndex)
            showHiddenMenu();
        else if (hit >= 0 && onNavigate != nullptr)
            onNavigate (chain[(size_t) segments[(size_t) hit].chainIndex]);
    }

private:
    struct Segment
    {
        int chainIndex;
        juce::String label;
        juce::Rectangle<int> bounds;
    };

    static constexpr int dotsIndex = -2; // hovered/indexAt が返す「…」の識別子
    static constexpr int chipPadding = 6;
    static constexpr int separatorWidth = 11;
    static constexpr int chipHeight = 22;
    static constexpr int minChipWidth = 46; // 縮めても数文字＋ellipsisは読める幅

    static juce::Font font() { return Fonts::body(); }

    static juce::String labelFor (const juce::File& dir)
    {
        const auto name = dir.getFileName();
        return name.isEmpty() ? "/" : name; // ルートはgetFileName()が空
    }

    int chipWidth (const juce::String& label) const
    {
        return juce::GlyphArrangement::getStringWidthInt (font(), label) + chipPadding * 2;
    }

    // 右（現在フォルダ）から詰めて、入らなかった祖先は「…」に畳む。
    // 親と現在フォルダの2つは常に残す（1つ左＝親、が「上へ」の導線なので消してはいけない）。
    // それでも入らないときは長いチップから順に縮めてellipsis表示にする。
    void layoutSegments()
    {
        segments.clear();
        hasHidden = false;
        hiddenCount = 0;
        if (chain.empty() || getWidth() <= 0)
            return;

        const int dotsChip = chipWidth (juce::String::fromUTF8 (u8"…"));
        const int last = (int) chain.size() - 1;

        int firstVisible = fitFrom (getWidth());
        if (firstVisible > 0) // 畳むなら「…」＋区切りの幅を先に確保してやり直す
        {
            hasHidden = true;
            firstVisible = juce::jmin (fitFrom (getWidth() - dotsChip - separatorWidth),
                                       juce::jmax (0, last - 1));
            hiddenCount = firstVisible;
        }

        const int count = last - firstVisible + 1;
        std::vector<int> widths;
        int total = 0;
        for (int i = firstVisible; i <= last; ++i)
        {
            widths.push_back (chipWidth (labelFor (chain[(size_t) i])));
            total += widths.back();
        }
        int available = getWidth() - separatorWidth * (count - 1)
                      - (hasHidden ? dotsChip + separatorWidth : 0);
        // 溢れた分は幅の広いチップから削る（最小幅までは残して数文字は読めるようにする）
        while (total > available)
        {
            const auto widest = std::max_element (widths.begin(), widths.end());
            if (widest == widths.end() || *widest <= minChipWidth)
                break;
            const int cut = juce::jmin (total - available, *widest - minChipWidth);
            *widest -= cut;
            total -= cut;
        }

        int x = 0;
        if (hasHidden)
        {
            dotsBounds = { 0, chipY(), dotsChip, chipHeight };
            x = dotsChip + separatorWidth;
        }
        for (int i = firstVisible; i <= last; ++i)
        {
            const int w = widths[(size_t) (i - firstVisible)];
            segments.push_back ({ i, labelFor (chain[(size_t) i]), { x, chipY(), w, chipHeight } });
            x += w + separatorWidth;
        }
    }

    // available幅に右から何個入るかを返す（返り値 = 表示を開始するchainの添字）
    int fitFrom (int available) const
    {
        const int last = (int) chain.size() - 1;
        int used = 0;
        int first = last;
        for (int i = last; i >= 0; --i)
        {
            const int need = chipWidth (labelFor (chain[(size_t) i])) + (i < last ? separatorWidth : 0);
            if (i < last && used + need > available)
                break;
            used += need;
            first = i;
        }
        return first;
    }

    int chipY() const { return (getHeight() - chipHeight) / 2; }

    int indexAt (juce::Point<int> p) const
    {
        if (hasHidden && dotsBounds.contains (p))
            return dotsIndex;
        for (int i = 0; i < (int) segments.size(); ++i)
        {
            // 現在フォルダ（末尾）は移動先が無いので反応させない
            if (segments[(size_t) i].chainIndex == (int) chain.size() - 1)
                continue;
            if (segments[(size_t) i].bounds.contains (p))
                return i;
        }
        return -1;
    }

    void paintChip (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& label,
                    bool current, bool hover) const
    {
        if (hover)
        {
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRoundedRectangle (area.toFloat(), 4.0f);
        }
        g.setColour (juce::Colours::white.withAlpha (current ? 0.90f : hover ? 0.85f : 0.48f));
        g.drawText (label, area.reduced (chipPadding, 0), juce::Justification::centredLeft, true);
    }

    void paintSeparator (juce::Graphics& g, int chipX) const
    {
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawText (juce::String::fromUTF8 (u8"›"),
                    juce::Rectangle<int> (chipX - separatorWidth, chipY(), separatorWidth, chipHeight),
                    juce::Justification::centred, false);
    }

    void showHiddenMenu()
    {
        juce::PopupMenu menu;
        for (int i = 0; i < hiddenCount; ++i)
            menu.addItem (i + 1, labelFor (chain[(size_t) i]));

        juce::Component::SafePointer<BreadcrumbBar> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (this)
                                .withTargetScreenArea (dotsBounds + getScreenPosition()),
                            [safe] (int result)
                            {
                                // メニュー表示中に移動していると chain が入れ替わっている
                                if (safe == nullptr || result <= 0
                                    || result > (int) safe->chain.size() || safe->onNavigate == nullptr)
                                    return;
                                safe->onNavigate (safe->chain[(size_t) (result - 1)]);
                            });
    }

    std::vector<juce::File> chain; // ルート→現在フォルダ
    std::vector<Segment> segments; // 実際に表示しているもの
    juce::Rectangle<int> dotsBounds;
    bool hasHidden = false;
    int hiddenCount = 0;
    int hovered = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BreadcrumbBar)
};
