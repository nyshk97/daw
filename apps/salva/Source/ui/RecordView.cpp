#include "RecordView.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

const juce::Colour bg { 0xff17161a };
const juce::Colour textDim { 0xff97908a };
const juce::Colour meterBg { 0xff111114 };
const juce::Colour meterGreen { 0xff7bc47b };
const juce::Colour meterYellow { 0xffdfae4a };
const juce::Colour meterRed { 0xffd94a43 };
const juce::Colour recordRed { 0xffd94a43 };
const juce::Colour recordActiveBg { 0xff8e2a26 };

// -60dB..0dBFS を 0..1 に写す（LaLaのメーターと同じ固定dBスケールの読み方）
float meterNorm (float level)
{
    if (level <= 0.001f)
        return 0.0f;
    return juce::jlimit (0.0f, 1.0f, (20.0f * std::log10 (level) + 60.0f) / 60.0f);
}

juce::String formatElapsed (double seconds)
{
    const int m = (int) (seconds / 60.0);
    const double s = seconds - m * 60.0;
    return juce::String (m) + ":" + juce::String (s, 1).paddedLeft ('0', 4);
}
} // namespace

RecordView::RecordView()
{
    addAndMakeVisible (inputLabel);
    inputLabel.setText (jp (u8"入力:"), juce::dontSendNotification);
    inputLabel.setColour (juce::Label::textColourId, textDim);

    addAndMakeVisible (inputDeviceBox);
    inputDeviceBox.onChange = [this]
    {
        if (onInputDeviceChanged && inputDeviceBox.getText().isNotEmpty())
            onInputDeviceChanged (inputDeviceBox.getText());
    };

    addAndMakeVisible (channelPairBox);
    channelPairBox.onChange = [this]
    {
        if (onChannelPairChanged && channelPairBox.getSelectedId() > 0)
            onChannelPairChanged ((channelPairBox.getSelectedId() - 1) * 2);
    };

    addAndMakeVisible (recordButton);
    recordButton.setButtonText (jp (u8"●"));
    recordButton.setColour (juce::TextButton::textColourOffId, recordRed);
    recordButton.onClick = [this]
    {
        if (onRecordToggle)
            onRecordToggle();
    };
    recordButton.setWantsKeyboardFocus (false);

    addAndMakeVisible (elapsedLabel);
    elapsedLabel.setFont (juce::FontOptions (20.0f));
    elapsedLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8d0cf));
    elapsedLabel.setText ("0:00.0", juce::dontSendNotification);

    addAndMakeVisible (hintLabel);
    hintLabel.setFont (juce::FontOptions (11.0f));
    hintLabel.setColour (juce::Label::textColourId, textDim);
    hintLabel.setJustificationType (juce::Justification::centred);
    hintLabel.setText (jp (u8"録音開始時に保存先を指定 → そのファイルへ直接書き込み。停止でこのWAVを開く"),
                       juce::dontSendNotification);

    for (auto* c : { (juce::Component*) &inputDeviceBox, (juce::Component*) &channelPairBox })
        c->setWantsKeyboardFocus (false);
}

void RecordView::setInputDevices (const juce::StringArray& names, const juce::String& current)
{
    inputDeviceBox.clear (juce::dontSendNotification);
    int id = 1;
    for (const auto& n : names)
        inputDeviceBox.addItem (n, id++);
    const int idx = names.indexOf (current);
    if (idx >= 0)
        inputDeviceBox.setSelectedId (idx + 1, juce::dontSendNotification);
}

void RecordView::setChannelPairs (int totalInputChannels, int currentPairStart)
{
    channelPairBox.clear (juce::dontSendNotification);
    for (int start = 0; start + 1 < totalInputChannels; start += 2)
        channelPairBox.addItem (juce::String (start + 1) + "-" + juce::String (start + 2) + "ch",
                                start / 2 + 1);
    if (currentPairStart / 2 + 1 <= channelPairBox.getNumItems())
        channelPairBox.setSelectedId (currentPairStart / 2 + 1, juce::dontSendNotification);
}

void RecordView::update (float peakL, float peakR, bool nowRecording, double elapsedSeconds)
{
    // 立ち上がりは即・下りは減衰（読みやすいメーターの定番）
    levels[0] = juce::jmax (peakL, levels[0] * 0.85f);
    levels[1] = juce::jmax (peakR, levels[1] * 0.85f);
    if (recording != nowRecording)
    {
        recording = nowRecording;
        recordButton.setButtonText (recording ? jp (u8"■") : jp (u8"●"));
    }
    elapsed = elapsedSeconds;
    elapsedLabel.setText (formatElapsed (elapsed), juce::dontSendNotification);
    repaint (meterArea);
    repaint (growBarArea);
}

void RecordView::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    // L/Rメーター（-60〜0dBFSの固定スケール・緑→黄→赤）
    auto drawMeter = [&g] (juce::Rectangle<int> r, float level)
    {
        g.setColour (meterBg);
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
        const float w = meterNorm (level) * (float) r.getWidth();
        if (w <= 0.0f)
            return;
        juce::ColourGradient grad (meterGreen, r.toFloat().getTopLeft(),
                                   meterRed, r.toFloat().getTopRight(), false);
        grad.addColour (0.75, meterGreen);
        grad.addColour (0.85, meterYellow);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r.toFloat().withWidth (w), 3.0f);
    };
    auto meters = meterArea;
    drawMeter (meters.removeFromTop (10), levels[0]);
    meters.removeFromTop (4);
    drawMeter (meters.removeFromTop (10), levels[1]);

    // 伸びるバー（録音経過。30分でフルスケール）
    g.setColour (meterBg);
    g.fillRoundedRectangle (growBarArea.toFloat(), 3.0f);
    if (recording && elapsed > 0.0)
    {
        const float frac = juce::jlimit (0.0f, 1.0f, (float) (elapsed / (30.0 * 60.0)));
        g.setColour (recordRed);
        g.fillRoundedRectangle (growBarArea.toFloat().withWidth (juce::jmax (3.0f, frac * (float) growBarArea.getWidth())), 3.0f);
    }

    if (recording)
    {
        g.setColour (recordActiveBg.withAlpha (0.6f));
        g.drawRoundedRectangle (recordButton.getBounds().toFloat().expanded (3.0f), 6.0f, 2.0f);
    }
}

void RecordView::resized()
{
    auto area = getLocalBounds();
    const int contentWidth = juce::jmin (480, area.getWidth() - 48);
    auto content = area.withSizeKeepingCentre (contentWidth, 190);

    auto deviceRow = content.removeFromTop (26);
    inputLabel.setBounds (deviceRow.removeFromLeft (40));
    inputDeviceBox.setBounds (deviceRow.removeFromLeft (juce::jmin (240, deviceRow.getWidth() - 130)));
    deviceRow.removeFromLeft (8);
    channelPairBox.setBounds (deviceRow);

    content.removeFromTop (16);
    meterArea = content.removeFromTop (24);
    content.removeFromTop (16);

    auto recRow = content.removeFromTop (34);
    recordButton.setBounds (recRow.removeFromLeft (52));
    recRow.removeFromLeft (14);
    elapsedLabel.setBounds (recRow.removeFromLeft (110));

    content.removeFromTop (14);
    growBarArea = content.removeFromTop (6);
    content.removeFromTop (12);
    hintLabel.setBounds (content.removeFromTop (18).withWidth (getWidth()).withX (0));
}
