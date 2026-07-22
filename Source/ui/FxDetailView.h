#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"

// 下部のFX詳細エディタ。左のFXパネル（FxEditorView）のスロットクリックで開き、
// そのFXの操作UIを横幅フルで表示する（Logicのフローティングウィンドウの代替）。
// 中身は後続スライスで埋める（スライス3=EQカーブ / 4=Compノブ / 5=Reverb・Delay・Limiter）。
// ピアノロールと同じ下部スロットを使い、排他（後勝ち）は MainComponent が制御する。
class FxDetailView : public juce::Component
{
public:
    static constexpr int preferredHeight = 260; // ピアノロールと同じ高さ

    FxDetailView()
    {
        addAndMakeVisible (closeButton);
        closeButton.getProperties().set ("flatButton", true);
        closeButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        closeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.55f));
        closeButton.onClick = [this] { if (onCloseRequested) onCloseRequested(); };
        closeButton.setWantsKeyboardFocus (false);
        closeButton.setMouseClickGrabsKeyboardFocus (false);
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    bool isOpen() const { return open; }

    // 表示（タイトルの更新のみでも呼ぶ。レイアウトはMainComponentのresizedが行う）
    void show (const juce::String& fxNameToShow, const juce::String& channelNameToShow)
    {
        fxName = fxNameToShow;
        channelName = channelNameToShow;
        open = true;
        setVisible (true);
        repaint();
    }

    void close()
    {
        open = false;
        setVisible (false);
    }

    std::function<void()> onCloseRequested; // ✕ボタン

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::timelineBg);
        g.setColour (Theme::panelBorder);
        g.drawHorizontalLine (0, 0.0f, (float) getWidth());

        // タイトル: "EQ — チャンネル名"
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (Fonts::bodyStrong());
        const auto title = fxName + juce::String::fromUTF8 (u8" — ") + channelName;
        g.setFont (Fonts::forText (Fonts::bodyStrong(), title));
        g.drawText (title, 12, 0, getWidth() - 60, titleHeight, juce::Justification::centredLeft);

        // 中身領域（各FXのUIは後続スライスでここに載る。今は一段沈めた空パネル）
        auto body = getLocalBounds().withTrimmedTop (titleHeight).reduced (8, 0);
        body.removeFromBottom (8);
        g.setColour (Theme::headerBg);
        g.fillRoundedRectangle (body.toFloat(), 6.0f);
    }

    void resized() override
    {
        closeButton.setBounds (getLocalBounds().removeFromTop (titleHeight)
                                   .removeFromRight (32).withSizeKeepingCentre (22, 20));
    }

private:
    static constexpr int titleHeight = 28;

    bool open = false;
    juce::String fxName, channelName;
    juce::TextButton closeButton { juce::String::fromUTF8 (u8"×") };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxDetailView)
};
