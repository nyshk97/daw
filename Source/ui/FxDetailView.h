#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "FxVisualKind.h"
#include "HardwarePanelStyle.h"
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
    // kind は固有色・タイトル書式用（neutral＝Instrument 等はFX名がユーザー由来なので大文字化しない）
    void show (const juce::String& fxNameToShow, const juce::String& channelNameToShow,
               FxVisualKind kindToShow = FxVisualKind::neutral)
    {
        fxName = fxNameToShow;
        channelName = channelNameToShow;
        kind = kindToShow;
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

    // 中身を置く領域（タイトル行の下。bodyは透過で地の質感をそのまま見せる）
    juce::Rectangle<int> bodyArea() const
    {
        auto area = getLocalBounds().withTrimmedTop (titleHeight).reduced (8, 0);
        area.removeFromBottom (8);
        return area;
    }

    std::function<void()> onCloseRequested; // ✕ボタン

    void paint (juce::Graphics& g) override
    {
        // 機材の文法（HardwarePanelStyle）。上端1pxの縁でトラック画面（机）と切り替わる
        HardwarePanelStyle::paintPanelBackground (g, getLocalBounds());

        // タイトル: "COMP — チャンネル名"（FX名は大文字トラッキング・チャンネル名は通常）
        HardwarePanelStyle::drawTitle (g, juce::Rectangle<int> (12, 0, getWidth() - 60, titleHeight),
                                       fxName, kind != FxVisualKind::neutral,
                                       juce::String::fromUTF8 (u8"—  ") + channelName);
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
    FxVisualKind kind = FxVisualKind::neutral;
    juce::Component* body = nullptr; // 非所有（MainComponentが所有する）
    juce::TextButton closeButton { juce::String::fromUTF8 (u8"×") };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxDetailView)
};
