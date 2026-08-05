#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"

// 画面右下に数秒だけ出る通知（レポート生成の完了/失敗など、モーダルにするほどでない知らせ）。
// クリックで onClick を実行して消える。モーダルではなく、他の操作を一切妨げない。
// 置き場所は親（MainComponent）の resized が右下に固定する
class ToastBar : public juce::Component,
                 private juce::Timer
{
public:
    ToastBar() { setInterceptsMouseClicks (true, false); }

    // 表示する（表示中に再度呼ばれたら上書き）。isError で左端のアクセント色が変わる。
    // onClickAction は省略可（nullptr ならクリックでただ消える）
    void show (const juce::String& text, bool isError, std::function<void()> onClickAction)
    {
        message = text;
        error = isError;
        onClick = std::move (onClickAction);
        setVisible (true);
        toFront (false);
        repaint();
        startTimer (6000);
    }

    void dismiss()
    {
        stopTimer();
        setVisible (false);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (Theme::windowBg.brighter (0.15f));
        g.fillRoundedRectangle (bounds, 6.0f);
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (error ? juce::Colour (0xffc47b7b) : Theme::playGreen);
        g.fillRoundedRectangle (bounds.removeFromLeft (4.0f), 2.0f);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (Fonts::small());
        g.drawFittedText (message, getLocalBounds().reduced (14, 6), juce::Justification::centredLeft, 2);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition()))
            return;
        const auto action = onClick; // dismiss 後に実行（action が別ウィンドウを開くため）
        dismiss();
        if (action != nullptr)
            action();
    }

    void mouseEnter (const juce::MouseEvent&) override
    {
        if (onClick != nullptr)
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

private:
    void timerCallback() override { dismiss(); }

    juce::String message;
    bool error = false;
    std::function<void()> onClick;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToastBar)
};
