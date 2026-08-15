#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

// 録音画面のビュー（確定モック案A: 下部バーのトグルで波形と入れ替え）。
// 入力デバイス＋ステレオペア選択・L/Rレベルメーター・録音ボタン・経過時間・伸びるバー。
// 録音の実行はMainComponent（保存ダイアログ→engine）に委ね、ここは表示と操作だけ
class RecordView : public juce::Component
{
public:
    RecordView();

    void setInputDevices (const juce::StringArray& names, const juce::String& current);
    void setChannelPairs (int totalInputChannels, int currentPairStart);
    // 保存先フォルダの表示（録音はダイアログなしでここへ自動保存されることを伝える）
    void setSaveFolderText (const juce::String& folderDisplay);
    // Timerから流し込む（peakはリニア振幅）
    void update (float peakL, float peakR, bool recording, double elapsedSeconds);

    std::function<void (juce::String)> onInputDeviceChanged;
    std::function<void (int pairStart)> onChannelPairChanged;
    std::function<void()> onRecordToggle;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    juce::Label inputLabel;
    juce::ComboBox inputDeviceBox;
    juce::ComboBox channelPairBox;
    juce::TextButton recordButton;
    juce::Label elapsedLabel;
    juce::Label hintLabel;

    float levels[2] = { 0.0f, 0.0f }; // 表示用（減衰込み）
    bool recording = false;
    double elapsed = 0.0;

    juce::Rectangle<int> meterArea, growBarArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecordView)
};
