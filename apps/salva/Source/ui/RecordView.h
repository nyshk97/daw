#pragma once

#include <deque>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

// 録音画面のビュー（2026-08-15確定モックA-1: ボイスメモ型・波形は録音中だけ）。
// 入力デバイス＋ステレオペア選択・dB目盛り付きL/Rメーター（ピークホールド）・
// 録音中だけ流れる波形ロール・大型タイマー・経過リング付き円形録音ボタン。
// 録音の実行はMainComponent（engine）に委ね、ここは表示と操作だけ
class RecordView : public juce::Component
{
public:
    RecordView();

    void setInputDevices (const juce::StringArray& names, const juce::String& current);
    void setChannelPairs (int totalInputChannels, int currentPairStart);
    // 保存先フォルダの表示（録音はダイアログなしでここへ自動保存されることを伝える）
    void setSaveFolderText (const juce::String& folderDisplay);
    // Timerから流し込む（peakはリニア振幅・30Hz）
    void update (float peakL, float peakR, bool recording, double elapsedSeconds);

    std::function<void (juce::String)> onInputDeviceChanged;
    std::function<void (int pairStart)> onChannelPairChanged;
    std::function<void()> onRecordToggle;
    std::function<void()> onBack; // 左上「← 戻る」（録音画面はフルブリードで、出口はここだけ）

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    // 円形録音ボタン。外周リング＝30分スケールの録音経過（伸びるバーの後継）
    class CircleRecordButton : public juce::Button
    {
    public:
        // AX名（VoiceOver・検証用AXPress）はボタンテキスト由来。描画は自前なので画面には出ない
        CircleRecordButton() : juce::Button (juce::String::fromUTF8 (u8"録音")) { setButtonText (getName()); }
        void setRecordingState (bool nowRecording);
        void setProgress (float newProgress); // 0..1
        void paintButton (juce::Graphics& g, bool highlighted, bool down) override;

    private:
        bool recordingState = false;
        float progress = 0.0f;
    };

    juce::Label inputLabel;
    juce::ComboBox inputDeviceBox;
    juce::ComboBox channelPairBox;
    CircleRecordButton recordButton;
    juce::TextButton backButton;
    juce::Label hintLabel;

    float levels[2] = { 0.0f, 0.0f };   // 表示用（減衰込み）
    float peakHold[2] = { 0.0f, 0.0f }; // ピークホールド（正規化値）
    float peakAge[2] = { 0.0f, 0.0f };  // ホールドからの経過秒
    bool recording = false;
    juce::String elapsedText { "0:00.0" };

    // 波形ロール（録音中のみ蓄積。1本 = 2tick（約66ms）のピーク）
    std::deque<float> waveBars;
    float wavePeakAcc = 0.0f;
    int waveTickCount = 0;

    juce::Rectangle<int> meterArea, scaleArea, waveArea, timerArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecordView)
};
