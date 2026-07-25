#pragma once

#include <functional>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Shortcuts.h"
#include "Theme.h"
#include "../shared/Log.h"

// 歯車ボタン／⌘, で開くオーディオデバイス設定のウィンドウ。MixerWindowと同じく非モーダルなので、
// 開いたままメイン画面を操作できる（＝歯車ボタンの再クリックで閉じられる）。
// 閉じる経路（歯車の再クリック・Esc・クローズボタン）はすべて onDismissed を通す
class DeviceSettingsWindow : public juce::DialogWindow
{
public:
    explicit DeviceSettingsWindow (juce::AudioDeviceManager& deviceManager)
        : juce::DialogWindow (juce::String::fromUTF8 (u8"オーディオデバイス設定"),
                              Theme::windowBg, true /* escapeKeyTriggersCloseButton */)
    {
        setUsingNativeTitleBar (true);

        auto* selector = new juce::AudioDeviceSelectorComponent (
            deviceManager, 1, 2, 2, 2, false, false, true, false);
        selector->setSize (500, 400);
        setContentOwned (selector, true);
        setResizable (false, false);
    }

    ~DeviceSettingsWindow() override { clearContentComponent(); }

    void openOver (juce::Component* alignTo)
    {
        centreAroundComponent (alignTo, getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    // Esc/クローズボタンからの閉じる要求。ウィンドウ自身のコールスタックから破棄されないよう、
    // 通知だけ行って実際の破棄は所有者（MainComponent）に任せる
    std::function<void()> onDismissed;

private:
    void notifyDismissed()
    {
        setVisible (false);
        if (onDismissed)
            onDismissed();
    }

    void closeButtonPressed() override
    {
        Log::info ("settings.close", "source=windowclose");
        notifyDismissed();
    }

    bool escapeKeyPressed() override
    {
        Log::info ("settings.close", "source=escape");
        notifyDismissed();
        return true;
    }

    // このウィンドウにフォーカスがあるときの⌘,（MainComponent::keyPressedには届かないため
    // ここで受ける）。他のキーはメイン画面へ転送しない＝設定中に誤って録音等が走らないようにする
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (Shortcuts::matches (key, Shortcuts::ID::audioSettings))
        {
            Log::info ("settings.close", "source=shortcut");
            notifyDismissed();
            return true;
        }
        return juce::DialogWindow::keyPressed (key);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceSettingsWindow)
};
