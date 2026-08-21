#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PitchEditorView.h"
#include "Theme.h"
#include "../shared/Log.h"

// ピッチ補正エディタの独立ウィンドウ（MixerWindow の型。1枚を使い回し・別リージョンを開くと中身が
// 入れ替わる・位置サイズはセッション内維持・閉じる＝非表示）。キーは中身（PitchEditorView::onKey）経由で
// MainComponent の集中ハンドラへ転送される（Space / ⌘Z / , . がそのまま効く）
class PitchEditorWindow : public juce::DocumentWindow
{
public:
    PitchEditorWindow()
        : juce::DocumentWindow ("Pitch", Theme::windowBg, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentNonOwned (&view, false);
        setResizable (true, false);
        setResizeLimits (720, 360, 4096, 2160);
    }
    ~PitchEditorWindow() override { clearContentComponent(); }

    PitchEditorView& content() { return view; }

    void openOver (juce::Component* alignTo)
    {
        if (! placed)
        {
            centreAroundComponent (alignTo, 1100, 520);
            placed = true;
        }
        setVisible (true);
        toFront (true);
        view.grabKeyboardFocus();
    }

    // 閉じる＝非表示（クローズボタン・Esc・切替の全経路がここを通る）。onDismissed でプレビューの破棄を通知
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
        Log::info ("pitch.close", "source=windowclose");
        dismiss();
    }

    PitchEditorView view;
    bool placed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchEditorWindow)
};
