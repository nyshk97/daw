#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"

// 「リファレンスとして分析」の進捗オーバーレイ。BounceOverlay と同じ「親全面を覆い
// パネルを自前描画」方式。分析は工程数が読めない（demucs 分離が支配的・約4分）ので
// 進捗バーは不定型（流れるハイライト）にし、analyze.sh の stdout 最新行を添える。
class ReferenceAnalysisOverlay : public juce::Component,
                                 private juce::Timer
{
public:
    std::function<void()> onCancel;

    ReferenceAnalysisOverlay()
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    void show (const juce::String& referenceName)
    {
        name = referenceName;
        statusLine.clear();
        phase = 0.0f;
        startMs = juce::Time::currentTimeMillis();
        lastElapsedSeconds = -1;
        setInterceptsMouseClicks (true, true); // 表示中はモーダル（背後のUIを塞ぐ）
        setVisible (true);
        toFront (false);
        startTimerHz (30);
        repaint();
    }

    void dismiss()
    {
        stopTimer();
        setVisible (false);
    }

    void setStatusLine (const juce::String& line)
    {
        if (line == statusLine)
            return;
        statusLine = line;
        repaint (panelBounds());
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.45f));

        const auto panel = panelBounds();
        g.setColour (Theme::popupBg);
        g.fillRoundedRectangle (panel.toFloat(), 8.0f);
        g.setColour (Theme::popupBorder);
        g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), 8.0f, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.95f));
        g.setFont (Fonts::title());
        g.drawText (juce::String::fromUTF8 (u8"リファレンスを分析中…"),
                    panel.withHeight (titleHeight).withTrimmedTop (padY),
                    juce::Justification::centred);

        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.setFont (Fonts::small());
        g.drawText (name + juce::String::fromUTF8 (u8"（経過 ") + elapsedText()
                        + juce::String::fromUTF8 (u8" ／ 目安 約4分）"),
                    panel.withTrimmedTop (padY + titleHeight).withHeight (16),
                    juce::Justification::centred);

        // 不定型バー: 幅30%のハイライトが左右に往復する
        const auto bar = progressBarBounds().toFloat();
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.fillRoundedRectangle (bar, bar.getHeight() / 2.0f);
        const float span = bar.getWidth() * 0.3f;
        const float travel = bar.getWidth() - span;
        const float t = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f; // 0→1→0 の往復
        g.setColour (Theme::accent);
        g.fillRoundedRectangle (bar.withWidth (span).withX (bar.getX() + travel * t),
                                bar.getHeight() / 2.0f);

        // analyze.sh の最新行（「==> ステム分離」等）
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (Fonts::small());
        g.drawText (statusLine, statusBounds(), juce::Justification::centred, true);

        const auto cancel = cancelButtonBounds().toFloat();
        g.setColour (juce::Colours::white.withAlpha (cancelHovered ? 0.14f : 0.08f));
        g.fillRoundedRectangle (cancel, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (Fonts::body());
        g.drawText (juce::String::fromUTF8 (u8"キャンセル"), cancelButtonBounds(),
                    juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (cancelButtonBounds().contains (e.getPosition()) && onCancel != nullptr)
            onCancel();
        // パネル外クリックでは閉じない（キャンセルは明示操作のみ）
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool over = cancelButtonBounds().contains (e.getPosition());
        if (over != cancelHovered)
        {
            cancelHovered = over;
            repaint (cancelButtonBounds());
        }
    }

private:
    static constexpr int panelWidth = 420;
    static constexpr int panelHeight = 190;
    static constexpr int titleHeight = 40;
    static constexpr int padX = 28;
    static constexpr int padY = 16;
    static constexpr int barHeight = 6;
    static constexpr int cancelWidth = 96;
    static constexpr int cancelHeight = 28;

    void timerCallback() override
    {
        phase += 1.0f / 90.0f; // 往復3秒
        if (phase >= 1.0f)
            phase -= 1.0f;
        repaint (progressBarBounds().expanded (0, 2));

        const auto seconds = (int) ((juce::Time::currentTimeMillis() - startMs) / 1000);
        if (seconds != lastElapsedSeconds)
        {
            lastElapsedSeconds = seconds;
            const auto panel = panelBounds();
            repaint (panel.withTrimmedTop (padY + titleHeight).withHeight (16));
        }
    }

    juce::String elapsedText() const
    {
        const auto seconds = (int) ((juce::Time::currentTimeMillis() - startMs) / 1000);
        return juce::String (seconds / 60) + ":" + juce::String (seconds % 60).paddedLeft ('0', 2);
    }

    juce::Rectangle<int> panelBounds() const
    {
        return juce::Rectangle<int> (panelWidth, panelHeight).withCentre (getLocalBounds().getCentre());
    }

    juce::Rectangle<int> progressBarBounds() const
    {
        const auto panel = panelBounds();
        return { panel.getX() + padX, panel.getY() + padY + titleHeight + 24,
                 panel.getWidth() - padX * 2, barHeight };
    }

    juce::Rectangle<int> statusBounds() const
    {
        const auto panel = panelBounds();
        return { panel.getX() + padX, progressBarBounds().getBottom() + 8,
                 panel.getWidth() - padX * 2, 16 };
    }

    juce::Rectangle<int> cancelButtonBounds() const
    {
        const auto panel = panelBounds();
        return { panel.getCentreX() - cancelWidth / 2, panel.getBottom() - padY - cancelHeight,
                 cancelWidth, cancelHeight };
    }

    juce::String name;
    juce::String statusLine;
    float phase = 0.0f;
    juce::int64 startMs = 0;
    int lastElapsedSeconds = -1;
    bool cancelHovered = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReferenceAnalysisOverlay)
};
