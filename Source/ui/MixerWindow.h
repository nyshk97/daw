#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "MixerOverlay.h"
#include "Theme.h"
#include "../shared/Log.h"

// X で開くミキサーの独立ウィンドウ（Logicのミキサーウィンドウ相当）。
// メインウィンドウの枠に縛られず自由に移動・リサイズできる。閉じる（X/Esc/クローズボタン）は
// 非表示化のみで、位置・サイズはセッション内で維持される。
// キー操作は中身（MixerOverlay::onKey）経由でMainComponentの集中ハンドラへ転送されるため、
// ミキサーにフォーカスがあってもSpace再生・m/s等がそのまま効く
class MixerWindow : public juce::DocumentWindow
{
public:
    MixerWindow()
        : juce::DocumentWindow ("Mixer", Theme::windowBg, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentNonOwned (&panel, false);
        setResizable (true, false);
        setResizeLimits (620, 420, 4096, 2160);
    }

    ~MixerWindow() override { clearContentComponent(); }

    MixerOverlay& content() { return panel; }

    // 開く（初回はメインウィンドウ中央・以後は前回の位置とサイズ）
    void openOver (juce::Component* alignTo, int selectedTrack)
    {
        if (! placed)
        {
            centreAroundComponent (alignTo, 980, 560);
            placed = true;
        }
        setVisible (true);
        toFront (true);
        panel.sync (selectedTrack);
        panel.grabKeyboardFocus();
    }

    // 閉じる＝非表示（X/Esc/クローズボタンの全経路がここを通る）。onDismissed で通知する
    // （FXパネルの表示対象を選択トラック追従に戻すため）
    void dismiss()
    {
        if (! isVisible())
            return;
        setVisible (false);
        if (onDismissed)
            onDismissed();
    }

    std::function<void()> onDismissed;

private:
    void closeButtonPressed() override
    {
        Log::info ("mixer.close", "source=windowclose");
        dismiss();
    }

    MixerOverlay panel;
    bool placed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerWindow)
};
