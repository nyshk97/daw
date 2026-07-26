#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Shortcuts.h"
#include "Theme.h"

// 下部のFX詳細エディタ。左のFXパネル（FxEditorView）のスロットクリックで開き、
// そのFXの操作UIを横幅フルで表示する（Logicのフローティングウィンドウの代替）。
// 中身（body）は差し替え式で、所有はしない: MainComponent が対象スロットに応じた
// コンポーネント（Instrumentエディタ等）を setBody で載せる。EQ/Compのエディタも同じ土台に乗る。
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
        // 同じキーで開き直せる（閉じても履歴に残る）ことを知る手掛かりをここに置く
        closeButton.setTooltip (juce::String::fromUTF8 (u8"閉じる") + " ("
                                + Shortcuts::keyText (Shortcuts::ID::toggleBottomPanel) + ")");
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
        setBody (nullptr); // 載せていた中身は手放す（所有はしていない）
    }

    // 中身の差し替え（所有しない。呼び出し側がbodyより長生きすることを保証する）。
    // nullptr で外す。載せ替えのたびに resized() でレイアウトし直す
    void setBody (juce::Component* newBody)
    {
        if (body == newBody)
            return;
        if (body != nullptr)
            removeChildComponent (body);
        body = newBody;
        if (body != nullptr)
            addAndMakeVisible (*body);
        resized();
        repaint();
    }

    juce::Component* currentBody() const { return body; }

    // 中身を置く領域（タイトル行の下・一段沈めた角丸パネルの内側）
    juce::Rectangle<int> bodyArea() const
    {
        auto area = getLocalBounds().withTrimmedTop (titleHeight).reduced (8, 0);
        area.removeFromBottom (8);
        return area;
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

        // 中身領域（bodyがここに載る。未実装のFXでは一段沈めた空パネルのまま）
        g.setColour (Theme::headerBg);
        g.fillRoundedRectangle (bodyArea().toFloat(), 6.0f);
    }

    void resized() override
    {
        closeButton.setBounds (getLocalBounds().removeFromTop (titleHeight)
                                   .removeFromRight (32).withSizeKeepingCentre (22, 20));
        if (body != nullptr)
            body->setBounds (bodyArea());
    }

private:
    static constexpr int titleHeight = 28;

    bool open = false;
    juce::String fxName, channelName;
    juce::Component* body = nullptr; // 非所有（MainComponentが所有する）
    juce::TextButton closeButton { juce::String::fromUTF8 (u8"×") };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxDetailView)
};
