#include "RecordView.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

const juce::Colour bg { 0xff17161a };
const juce::Colour textDim { 0xff97908a };
const juce::Colour cream { 0xfff2e8d5 };
const juce::Colour meterBg { 0xff111114 };
const juce::Colour meterGreen { 0xff7bc47b };
const juce::Colour meterYellow { 0xffdfae4a };
const juce::Colour meterRed { 0xffd94a43 };
const juce::Colour recordRed { 0xffd94a43 };
const juce::Colour recordRedBright { 0xffe85f57 };
const juce::Colour recordingInner { 0xff26252b };
const juce::Colour ringBg { 0xff2a292f };
const juce::Colour waveBg { 0xff101013 };
const juce::Colour waveBar { 0xffff7a2e }; // Vinyl Warmのアクセント（録音中＝保存される音）
const juce::Colour scaleText { 0xff6d675f };
const juce::Colour readyText { 0xff55504a };
const juce::Colour dotIdle { 0xff4a3532 };

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

//==============================================================================
// CircleRecordButton

void RecordView::CircleRecordButton::setRecordingState (bool nowRecording)
{
    if (recordingState == nowRecording)
        return;
    recordingState = nowRecording;
    repaint();
}

void RecordView::CircleRecordButton::setProgress (float newProgress)
{
    if (juce::approximatelyEqual (progress, newProgress))
        return;
    progress = newProgress;
    repaint();
}

void RecordView::CircleRecordButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto ring = bounds.reduced (3.0f);

    g.setColour (ringBg);
    g.drawEllipse (ring, 4.0f);

    // 経過リング（12時から時計回り。0 = 開始直後、1周 = 30分）
    if (recordingState && progress > 0.0f)
    {
        juce::Path arc;
        arc.addArc (ring.getX(), ring.getY(), ring.getWidth(), ring.getHeight(),
                    0.0f, juce::MathConstants<float>::twoPi * juce::jlimit (0.0f, 1.0f, progress), true);
        g.setColour (recordRed);
        g.strokePath (arc, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }

    auto inner = bounds.reduced (12.0f);
    if (down)
        inner = inner.reduced (2.0f);
    else if (highlighted && ! recordingState)
        inner = inner.expanded (1.5f);

    if (! recordingState)
    {
        // 待機中: 赤い丸（左上寄りのハイライトで立体感）
        juce::ColourGradient grad (recordRedBright,
                                   inner.getX() + inner.getWidth() * 0.35f,
                                   inner.getY() + inner.getHeight() * 0.30f,
                                   recordRed, inner.getCentreX(), inner.getBottom(), true);
        g.setGradientFill (grad);
        g.fillEllipse (inner);
    }
    else
    {
        // 録音中: 暗い円＋赤枠、中に停止の角丸四角（ボイスメモの文法）
        g.setColour (recordingInner);
        g.fillEllipse (inner);
        g.setColour (recordRed);
        g.drawEllipse (inner.reduced (1.0f), 2.0f);
        g.fillRoundedRectangle (juce::Rectangle<float> (25.0f, 25.0f).withCentre (inner.getCentre()), 5.0f);
    }
}

//==============================================================================
// RecordView

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

    // ゴースト＋描画シェブロン: 録音画面の主役は円形録音ボタンなので、ナビは気配を消す
    addAndMakeVisible (backButton);
    backButton.setButtonText (jp (u8"戻る"));
    backButton.getProperties().set ("fontSize", 12.5);
    backButton.getProperties().set ("style", "ghost");
    backButton.getProperties().set ("icon", "back");
    backButton.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    backButton.setWantsKeyboardFocus (false);
    backButton.onClick = [this]
    {
        if (onBack)
            onBack();
    };

    addAndMakeVisible (recordButton);
    recordButton.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    recordButton.onClick = [this]
    {
        if (onRecordToggle)
            onRecordToggle();
    };
    recordButton.setWantsKeyboardFocus (false);

    addAndMakeVisible (hintLabel);
    hintLabel.setFont (juce::FontOptions (11.0f));
    hintLabel.setColour (juce::Label::textColourId, textDim);
    hintLabel.setJustificationType (juce::Justification::centred);
    // 実際の保存先は setSaveFolderText で差し込む（既定文言は差し込み前のフォールバック）
    hintLabel.setText (jp (u8"自動命名で保存 → 停止でこのWAVを開く"), juce::dontSendNotification);

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

void RecordView::setSaveFolderText (const juce::String& folderDisplay)
{
    hintLabel.setText (folderDisplay + jp (u8" へ自動保存 → 停止でこのWAVを開く"),
                       juce::dontSendNotification);
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
    const float dt = 1.0f / 30.0f; // MainComponentのTimer周期

    // 立ち上がりは即・下りは減衰（読みやすいメーターの定番）
    levels[0] = juce::jmax (peakL, levels[0] * 0.85f);
    levels[1] = juce::jmax (peakR, levels[1] * 0.85f);

    // ピークホールド: 1.2秒保持してから緩やかに落とす
    for (int ch = 0; ch < 2; ++ch)
    {
        const float n = meterNorm (levels[ch]);
        if (n >= peakHold[ch])
        {
            peakHold[ch] = n;
            peakAge[ch] = 0.0f;
        }
        else
        {
            peakAge[ch] += dt;
            if (peakAge[ch] > 1.2f)
                peakHold[ch] = juce::jmax (n, peakHold[ch] - dt * 0.5f);
        }
    }

    if (recording != nowRecording)
    {
        recording = nowRecording;
        if (recording)
        {
            waveBars.clear();
            wavePeakAcc = 0.0f;
            waveTickCount = 0;
        }
        recordButton.setRecordingState (recording);
        // 録音中は戻れない（停止が先）。見た目も薄くして仕様と一致させる
        backButton.setEnabled (! recording);
        backButton.setAlpha (recording ? 0.25f : 1.0f);
        repaint();
    }

    if (recording)
    {
        wavePeakAcc = juce::jmax (wavePeakAcc, peakL, peakR);
        if (++waveTickCount >= 2)
        {
            waveBars.push_back (wavePeakAcc);
            wavePeakAcc = 0.0f;
            waveTickCount = 0;
            const auto maxBars = (size_t) juce::jmax (1, waveArea.getWidth() / 3);
            while (waveBars.size() > maxBars)
                waveBars.pop_front();
        }
    }

    elapsedText = formatElapsed (elapsedSeconds);
    recordButton.setProgress (recording ? (float) (elapsedSeconds / (30.0 * 60.0)) : 0.0f);
    repaint (meterArea);
    repaint (waveArea);
    repaint (timerArea);
}

void RecordView::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    // L/Rメーター（-60〜0dBFSの固定スケール・緑→黄→赤・ピークホールド付き）
    auto drawMeter = [&g] (juce::Rectangle<int> r, float level, float hold)
    {
        g.setColour (meterBg);
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
        const float w = meterNorm (level) * (float) r.getWidth();
        if (w > 0.0f)
        {
            juce::ColourGradient grad (meterGreen, r.toFloat().getTopLeft(),
                                       meterRed, r.toFloat().getTopRight(), false);
            grad.addColour (0.75, meterGreen);
            grad.addColour (0.85, meterYellow);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (r.toFloat().withWidth (w), 3.0f);
        }
        if (hold > 0.01f)
        {
            g.setColour (cream.withAlpha (0.85f));
            const float x = (float) r.getX() + hold * (float) r.getWidth() - 2.0f;
            g.fillRect (juce::Rectangle<float> (x, (float) r.getY(), 2.0f, (float) r.getHeight()));
        }
    };
    auto meters = meterArea;
    drawMeter (meters.removeFromTop (12), levels[0], peakHold[0]);
    meters.removeFromTop (4);
    drawMeter (meters.removeFromTop (12), levels[1], peakHold[1]);

    // dB目盛り（-12dB前後で振れていれば健康、の読み方がつく）
    g.setFont (juce::FontOptions (9.0f));
    g.setColour (scaleText);
    for (const int db : { -60, -40, -20, -12, -6, 0 })
    {
        const float x = (float) scaleArea.getX() + (float) (db + 60) / 60.0f * (float) scaleArea.getWidth();
        const auto label = db == 0 ? juce::String ("0dB") : juce::String (db);
        const auto just = db == -60 ? juce::Justification::topLeft
                        : db == 0   ? juce::Justification::topRight
                                    : juce::Justification::centredTop;
        const auto textArea = db == -60 ? scaleArea.withLeft ((int) x)
                            : db == 0   ? scaleArea.withRight ((int) x)
                                        : scaleArea.withWidth (40).withCentre ({ (int) x, scaleArea.getCentreY() });
        g.drawText (label, textArea, just, false);
    }

    // 波形ロール（録音中だけ流れる＝録れている証拠。待機中はREADY）
    g.setColour (waveBg);
    g.fillRoundedRectangle (waveArea.toFloat(), 5.0f);
    const int waveRight = waveArea.getRight() - 4;
    if (! recording && waveBars.empty())
    {
        g.setFont (juce::Font (juce::FontOptions (10.0f)).withExtraKerningFactor (0.15f));
        g.setColour (readyText);
        g.drawText ("READY", waveArea, juce::Justification::centred, false);
    }
    else
    {
        g.setColour (juce::Colour (0xff26252b));
        g.fillRect (waveArea.getX() + 4, waveArea.getCentreY(), waveRight - waveArea.getX() - 4, 1);
        const float waveH = (float) waveArea.getHeight();
        int i = 0;
        for (auto it = waveBars.rbegin(); it != waveBars.rend(); ++it, ++i)
        {
            const int x = waveRight - (i + 1) * 3;
            if (x < waveArea.getX() + 2)
                break;
            const float h = juce::jmax (1.0f, meterNorm (*it) * waveH * 0.9f);
            g.setColour (waveBar);
            g.fillRect ((float) x, (float) waveArea.getCentreY() - h * 0.5f, 2.0f, h);
        }
        if (recording)
        {
            g.setColour (cream.withAlpha (0.7f));
            g.fillRect (waveRight, waveArea.getY() + 2, 1, waveArea.getHeight() - 4);
        }
    }

    // タイマー（ドット＋数字を1組で中央揃え。録音中はドットが脈打つ）
    const juce::Font timerFont { juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 44.0f, juce::Font::plain) };
    const float textW = juce::GlyphArrangement::getStringWidth (timerFont, elapsedText);
    const float dotD = 13.0f, dotGap = 14.0f;
    const float startX = (float) timerArea.getCentreX() - (dotD + dotGap + textW) * 0.5f;
    if (recording)
    {
        const float phase = (float) (juce::Time::getMillisecondCounter() % 1200u) / 1200.0f;
        const float alpha = 0.625f + 0.375f * std::cos (phase * juce::MathConstants<float>::twoPi);
        g.setColour (recordRed.withAlpha (alpha));
    }
    else
    {
        g.setColour (dotIdle);
    }
    g.fillEllipse (startX, (float) timerArea.getCentreY() - dotD * 0.5f, dotD, dotD);
    g.setColour (cream);
    g.setFont (timerFont);
    g.drawText (elapsedText,
                juce::Rectangle<float> (startX + dotD + dotGap, (float) timerArea.getY(),
                                        textW + 4.0f, (float) timerArea.getHeight()),
                juce::Justification::centredLeft, false);
}

void RecordView::resized()
{
    auto area = getLocalBounds();
    backButton.setBounds (14, 14, 76, 30);
    const int contentWidth = juce::jmin (440, area.getWidth() - 48);
    auto content = area.withSizeKeepingCentre (contentWidth, 360);

    auto deviceRow = content.removeFromTop (26);
    inputLabel.setBounds (deviceRow.removeFromLeft (40));
    inputDeviceBox.setBounds (deviceRow.removeFromLeft (juce::jmin (240, deviceRow.getWidth() - 130)));
    deviceRow.removeFromLeft (8);
    channelPairBox.setBounds (deviceRow);

    content.removeFromTop (16);
    meterArea = content.removeFromTop (28);
    scaleArea = content.removeFromTop (15).withTrimmedTop (2);
    content.removeFromTop (14);
    waveArea = content.removeFromTop (58);
    content.removeFromTop (12);
    timerArea = content.removeFromTop (48);
    content.removeFromTop (10);
    recordButton.setBounds (content.removeFromTop (104).withSizeKeepingCentre (104, 104));
    content.removeFromTop (10);
    hintLabel.setBounds (content.removeFromTop (18).withWidth (getWidth()).withX (0));
}
