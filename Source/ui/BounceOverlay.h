#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"

// バウンス（書き出し）中の進捗オーバーレイ。ShortcutListOverlayと同じ
// 「親全面を覆いパネルを自前描画」方式。表示中はモーダル
// （キー処理は MainComponent::keyPressed 側で消費し、Escでキャンセル。
//  ただしネイティブメニューは覆えないため、メニュー側のdisable化は別途行う）。
// 完了後は短時間「書き出し完了」を表示して自動で消える（この間はクリックを素通しする）
class BounceOverlay : public juce::Component
{
public:
    std::function<void()> onCancel;

    BounceOverlay()
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    void show()
    {
        doneMode = false;
        progress = 0.0f;
        setInterceptsMouseClicks (true, true); // 進捗中はモーダル（背後のUIを塞ぐ）
        setVisible (true);
        toFront (false);
        repaint();
    }

    void showDone()
    {
        doneMode = true;
        setInterceptsMouseClicks (false, false); // 完了表示は情報のみ。操作は塞がない
        setVisible (true);
        repaint();
    }

    void dismiss() { setVisible (false); }

    void setProgress (float newProgress)
    {
        progress = juce::jlimit (0.0f, 1.0f, newProgress);
        repaint (panelBounds());
    }

    void paint (juce::Graphics& g) override
    {
        if (! doneMode)
            g.fillAll (juce::Colours::black.withAlpha (0.45f));

        const auto panel = panelBounds();
        g.setColour (Theme::popupBg);
        g.fillRoundedRectangle (panel.toFloat(), 8.0f);
        g.setColour (Theme::popupBorder);
        g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), 8.0f, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.95f));
        g.setFont (Fonts::title());
        g.drawText (juce::String::fromUTF8 (doneMode ? u8"書き出しが完了しました" : u8"書き出し中…"),
                    panel.withHeight (titleHeight).withTrimmedTop (padY),
                    juce::Justification::centred);

        if (doneMode)
            return;

        // 進捗バー
        const auto bar = progressBarBounds().toFloat();
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.fillRoundedRectangle (bar, bar.getHeight() / 2.0f);
        if (progress > 0.0f)
        {
            g.setColour (Theme::accent);
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (bar.getHeight(), bar.getWidth() * progress)),
                                    bar.getHeight() / 2.0f);
        }

        // キャンセルボタン（自前描画＋mouseDownヒットテスト）
        const auto cancel = cancelButtonBounds().toFloat();
        g.setColour (juce::Colours::white.withAlpha (cancelHovered ? 0.14f : 0.08f));
        g.fillRoundedRectangle (cancel, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (Fonts::body());
        g.drawText (juce::String::fromUTF8 (u8"キャンセル"), cancel, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! doneMode && cancelButtonBounds().contains (e.getPosition()) && onCancel != nullptr)
            onCancel();
        // パネル外クリックでは閉じない（キャンセルは明示操作のみ）
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool over = ! doneMode && cancelButtonBounds().contains (e.getPosition());
        if (over != cancelHovered)
        {
            cancelHovered = over;
            repaint (cancelButtonBounds());
        }
    }

private:
    static constexpr int panelWidth = 380;
    static constexpr int panelHeight = 150;
    static constexpr int titleHeight = 40;
    static constexpr int padX = 28;
    static constexpr int padY = 16;
    static constexpr int barHeight = 6;
    static constexpr int cancelWidth = 96;
    static constexpr int cancelHeight = 28;

    juce::Rectangle<int> panelBounds() const
    {
        const int h = doneMode ? padY * 2 + titleHeight : panelHeight;
        return juce::Rectangle<int> (panelWidth, h).withCentre (getLocalBounds().getCentre());
    }

    juce::Rectangle<int> progressBarBounds() const
    {
        const auto panel = panelBounds();
        return { panel.getX() + padX, panel.getY() + padY + titleHeight + 10,
                 panel.getWidth() - padX * 2, barHeight };
    }

    juce::Rectangle<int> cancelButtonBounds() const
    {
        const auto panel = panelBounds();
        return { panel.getCentreX() - cancelWidth / 2, panel.getBottom() - padY - cancelHeight,
                 cancelWidth, cancelHeight };
    }

    float progress = 0.0f;
    bool doneMode = false;
    bool cancelHovered = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BounceOverlay)
};
