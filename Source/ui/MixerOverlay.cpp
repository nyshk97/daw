#include "MixerOverlay.h"

#include <cmath>

#include "Fonts.h"
#include "Theme.h"

namespace
{
constexpr int stripGap = 1;       // ストリップ間の区切り（背景色が見える隙間）
constexpr int sectionGap = 10;    // トラック群とバス群の間
constexpr int panelPad = 12;
constexpr int titleBarHeight = 30; // MIXERタイトル＝ドラッグハンドル
constexpr int panelMaxHeight = 490;
constexpr int panelMinHeight = 340;

// メーターの表示値: -60dB..0dB を 0..1 に写す（ヘッダーのメーターと同じスケール）
float meterNorm (float level)
{
    if (level <= 0.001f)
        return 0.0f;
    const float db = 20.0f * std::log10 (level);
    return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
}
} // namespace

// ---- MixerStrip -----------------------------------------------------------

MixerStrip::MixerStrip (Kind kindToUse) : kind (kindToUse)
{
    const bool isTrack = kind == Kind::track;

    if (isTrack)
    {
        for (int b = 0; b < numSendBuses; ++b)
        {
            setupKnob (sendKnobs[b], 0.0, 1.0, 0.0);
            sendKnobs[b].onValueChange = [this, b]
            {
                if (params != nullptr)
                    params->sends[b].store ((float) sendKnobs[b].getValue());
                repaint (sendLabelAreas[b]); // ラベルの値表示を追従させる
                if (onChanged)
                    onChanged();
            };
            sendKnobs[b].onDragStart = [this, b] { sendDragging[b] = true; repaint (sendLabelAreas[b]); };
            sendKnobs[b].onDragEnd = [this, b] { sendDragging[b] = false; repaint (sendLabelAreas[b]); };
        }
        setupKnob (panKnob, -1.0, 1.0, 0.0);
        panKnob.onValueChange = [this]
        {
            if (params != nullptr)
                params->pan.store ((float) panKnob.getValue());
            repaint (panLabelArea); // ラベルの値表示を追従させる
            if (onChanged)
                onChanged();
        };
        panKnob.onDragStart = [this] { panDragging = true; repaint (panLabelArea); };
        panKnob.onDragEnd = [this] { panDragging = false; repaint (panLabelArea); };
    }

    addAndMakeVisible (fader);
    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setRange (0.0, 1.0);
    fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    fader.setDoubleClickReturnValue (true, isTrack ? 0.8 : 1.0); // バス/Masterの既定はユニティ
    fader.onValueChange = [this]
    {
        if (params != nullptr)
            params->gain.store ((float) fader.getValue());
        if (onChanged)
            onChanged();
    };

    // M/S はトラックヘッダーと同じフラット角丸トグル・同じ点灯色
    for (auto* b : std::initializer_list<juce::TextButton*> { &muteButton, &soloButton })
    {
        b->setClickingTogglesState (true);
        b->getProperties().set ("flatButton", true);
        b->setColour (juce::TextButton::buttonColourId, Theme::controlBg);
        b->setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.55f));
        b->setColour (juce::TextButton::textColourOnId, Theme::controlTextOn);
    }
    muteButton.setColour (juce::TextButton::buttonOnColourId, Theme::muteOn);
    soloButton.setColour (juce::TextButton::buttonOnColourId, Theme::soloOn);

    if (kind != Kind::master)
    {
        addAndMakeVisible (muteButton);
        muteButton.onClick = [this]
        {
            if (params != nullptr)
                params->mute.store (muteButton.getToggleState());
            if (onChanged)
                onChanged();
        };
    }
    if (isTrack)
    {
        addAndMakeVisible (soloButton);
        soloButton.onClick = [this]
        {
            if (params != nullptr)
                params->solo.store (soloButton.getToggleState());
            if (onChanged)
                onChanged();
        };
    }

    // Space（再生/停止）を奪わせない
    for (auto* c : std::initializer_list<juce::Component*> {
             &fader, &panKnob, &muteButton, &soloButton,
             &sendKnobs[0], &sendKnobs[1], &sendKnobs[2] })
    {
        c->setWantsKeyboardFocus (false);
        c->setMouseClickGrabsKeyboardFocus (false);
    }
}

void MixerStrip::setupKnob (juce::Slider& knob, double min, double max, double returnValue)
{
    addAndMakeVisible (knob);
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setRange (min, max);
    knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    knob.setDoubleClickReturnValue (true, returnValue);
    knob.setColour (juce::Slider::rotarySliderFillColourId, Theme::accent);
    knob.setColour (juce::Slider::rotarySliderOutlineColourId, Theme::controlBg);
}

void MixerStrip::bind (const juce::String& name, std::shared_ptr<TrackParams> paramsToBind, bool isSelected)
{
    stripName = name;
    params = std::move (paramsToBind);
    selected = isSelected;

    if (params != nullptr)
    {
        fader.setValue (params->gain.load(), juce::dontSendNotification);
        muteButton.setToggleState (params->mute.load(), juce::dontSendNotification);
        soloButton.setToggleState (params->solo.load(), juce::dontSendNotification);
        if (kind == Kind::track)
        {
            panKnob.setValue (params->pan.load(), juce::dontSendNotification);
            for (int b = 0; b < numSendBuses; ++b)
                sendKnobs[b].setValue (params->sends[b].load(), juce::dontSendNotification);
        }
    }
    repaint();
}

void MixerStrip::updateMeter (float incoming)
{
    // 表示は「新しい値」か「前回の減衰」の大きい方（ヘッダーのメーターと同じ流儀）
    const float next = juce::jmax (incoming, meterDisplay * 0.8f);
    if (next < 0.005f)
    {
        if (meterDisplay > 0.0f)
        {
            meterDisplay = 0.0f;
            repaint (meterArea);
        }
        return;
    }
    if (juce::approximatelyEqual (next, meterDisplay))
        return;
    meterDisplay = next;
    repaint (meterArea);
}

void MixerStrip::paint (juce::Graphics& g)
{
    g.fillAll (selected ? Theme::headerSelectedBg : Theme::headerBg);
    if (selected)
    {
        g.setColour (Theme::accent);
        g.fillRect (0, 0, getWidth(), 2);
    }

    // sendノブの豆ラベル（通常はA/B/D、ドラッグ中は送り量0〜100）と
    // Panラベル（センターはPAN・振っているときはL35/R35、ドラッグ中はセンターでもC）
    if (kind == Kind::track && params != nullptr)
    {
        g.setFont (Fonts::small());
        for (int b = 0; b < numSendBuses; ++b)
        {
            const auto text = sendDragging[b]
                                  ? juce::String (juce::roundToInt (params->sends[b].load() * 100.0f))
                                  : juce::String (SendBuses::shortNames[b]);
            g.setColour (juce::Colours::white.withAlpha (sendDragging[b] ? 0.8f : 0.45f));
            g.drawText (text, sendLabelAreas[b], juce::Justification::centred);
        }

        const float pan = params->pan.load();
        const int amount = juce::roundToInt (std::abs (pan) * 100.0f);
        const auto panValue = (pan < 0.0f ? "L" : "R") + juce::String (amount);
        const auto panText = amount < 1 ? (panDragging ? juce::String ("C") : juce::String ("PAN"))
                                        : panValue;
        g.setColour (juce::Colours::white.withAlpha (panDragging || amount >= 1 ? 0.8f : 0.45f));
        g.drawText (panText, panLabelArea, juce::Justification::centred);
    }

    // メーター（縦バー。dBスケール・0.9超は赤）
    g.setColour (Theme::controlBg);
    g.fillRoundedRectangle (meterArea.toFloat(), 2.0f);
    const float norm = meterNorm (meterDisplay);
    if (norm > 0.0f)
    {
        auto bar = meterArea.toFloat();
        bar = bar.withTop (bar.getBottom() - bar.getHeight() * norm);
        g.setColour (meterDisplay > 0.9f ? Theme::recordRed : Theme::playGreen);
        g.fillRoundedRectangle (bar, 2.0f);
    }

    // 名前（下端）
    g.setColour (juce::Colours::white.withAlpha (kind == Kind::track ? 0.9f : 0.7f));
    g.setFont (Fonts::forText (Fonts::small(), stripName));
    g.drawText (stripName, nameArea, juce::Justification::centred);
}

void MixerStrip::resized()
{
    auto area = getLocalBounds().reduced (8, 8);

    nameArea = area.removeFromBottom (18);
    area.removeFromBottom (2);

    if (kind != Kind::master)
    {
        auto buttonRow = area.removeFromBottom (22);
        const int buttonW = kind == Kind::track ? (buttonRow.getWidth() - 4) / 2 : 24;
        if (kind == Kind::track)
        {
            muteButton.setBounds (buttonRow.removeFromLeft (buttonW).reduced (0, 1));
            buttonRow.removeFromLeft (4);
            soloButton.setBounds (buttonRow.reduced (0, 1));
        }
        else
        {
            muteButton.setBounds (buttonRow.withSizeKeepingCentre (buttonW, 20));
        }
        area.removeFromBottom (6);
    }

    if (kind == Kind::track)
    {
        // sendノブ3個（上端の横並び）＋豆ラベル
        auto sendRow = area.removeFromTop (28);
        auto labelRow = area.removeFromTop (12);
        const int knobW = sendRow.getWidth() / numSendBuses;
        for (int b = 0; b < numSendBuses; ++b)
        {
            sendKnobs[b].setBounds (sendRow.removeFromLeft (knobW).withSizeKeepingCentre (26, 26));
            sendLabelAreas[b] = labelRow.removeFromLeft (knobW);
        }
        area.removeFromTop (6);

        // Panノブ（send より一回り大きく・PANラベル付き）
        panKnob.setBounds (area.removeFromTop (40).withSizeKeepingCentre (38, 38));
        panLabelArea = area.removeFromTop (12);
        area.removeFromTop (4);
    }

    // フェーダー＋メーター（残り全部の高さ）
    const int faderW = 24;
    const int meterW = 6;
    const int totalW = faderW + 4 + meterW;
    auto faderArea = area.withSizeKeepingCentre (totalW, area.getHeight());
    fader.setBounds (faderArea.removeFromLeft (faderW));
    faderArea.removeFromLeft (4);
    meterArea = faderArea.withWidth (meterW).reduced (0, 6);
}

void MixerStrip::mouseDown (const juce::MouseEvent&)
{
    if (onSelect)
        onSelect();
}

// ---- MixerOverlay ---------------------------------------------------------

MixerOverlay::MixerOverlay()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);

    addAndMakeVisible (viewport);
    viewport.setViewedComponent (&stripRow, false);
    viewport.setScrollBarsShown (false, true, false, true);
    viewport.setScrollBarThickness (8);

    for (auto& strip : busStrips)
        addAndMakeVisible (strip);
    addAndMakeVisible (masterStrip);
}

void MixerOverlay::setProject (Project* p)
{
    project = p;
}

void MixerOverlay::showOver (juce::Rectangle<int> areaToCover, int newSelectedTrack)
{
    setBounds (areaToCover);
    setVisible (true);
    toFront (false);
    sync (newSelectedTrack);
}

void MixerOverlay::sync (int newSelectedTrack)
{
    selectedTrack = newSelectedTrack;
    if (! isVisible() || project == nullptr)
        return;

    if ((int) trackStrips.size() != (int) project->tracks.size())
        rebuildTrackStrips();

    for (int i = 0; i < (int) trackStrips.size(); ++i)
    {
        auto& track = project->tracks[(size_t) i];
        trackStrips[(size_t) i]->bind (track.name, track.params, i == selectedTrack);
    }
    for (int b = 0; b < numSendBuses; ++b)
        busStrips[b].bind (SendBuses::names[b], project->busParams[b], false);
    masterStrip.bind ("Master", project->masterParams, false);
    resized();
}

void MixerOverlay::rebuildTrackStrips()
{
    trackStrips.clear();
    for (int i = 0; i < (int) project->tracks.size(); ++i)
    {
        auto strip = std::make_unique<MixerStrip> (MixerStrip::Kind::track);
        strip->onSelect = [this, i] { if (onSelectTrack) onSelectTrack (i); };
        strip->onChanged = [this] { if (onChanged) onChanged(); };
        stripRow.addAndMakeVisible (*strip);
        trackStrips.push_back (std::move (strip));
    }
}

void MixerOverlay::updateMeters (const std::vector<float>& trackPeaks,
                                 const float (&busPeaks)[numSendBuses], float masterPeak)
{
    if (! isVisible())
        return;
    for (int i = 0; i < (int) trackStrips.size(); ++i)
        trackStrips[(size_t) i]->updateMeter (i < (int) trackPeaks.size() ? trackPeaks[(size_t) i] : 0.0f);
    for (int b = 0; b < numSendBuses; ++b)
        busStrips[b].updateMeter (busPeaks[b]);
    masterStrip.updateMeter (masterPeak);
}

juce::Rectangle<int> MixerOverlay::panelBounds() const
{
    // 固定部（バス3＋Master）＋トラック部（最大でトラック数分、最低2本分は確保）
    const int stripW = MixerStrip::preferredWidth + stripGap;
    const int fixedW = stripW * (numSendBuses + 1);
    const int wantTracksW = stripW * juce::jmax (2, (int) trackStrips.size());
    const auto available = getLocalBounds().reduced (20);

    const int w = juce::jmin (available.getWidth(),
                              panelPad * 2 + wantTracksW + sectionGap + fixedW);
    const int h = juce::jlimit (juce::jmin (panelMinHeight, available.getHeight()),
                                panelMaxHeight, available.getHeight());
    // ドラッグ移動のoffsetを効かせつつ、必ず領域内に収める
    return juce::Rectangle<int> (w, h)
        .withCentre (available.getCentre() + panelOffset)
        .constrainedWithin (available);
}

void MixerOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.45f)); // 背後を暗くしてミキサーに集中させる

    const auto panel = panelBounds();
    g.setColour (Theme::popupBg);
    g.fillRoundedRectangle (panel.toFloat(), 8.0f);
    g.setColour (Theme::popupBorder);
    g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), 8.0f, 1.0f);

    // タイトルバー（＝ドラッグハンドル）。選択画面のPROJECTS見出しと同じトーン
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (Fonts::bodyStrong());
    g.drawText ("MIXER", panel.withHeight (titleBarHeight).withTrimmedLeft (14),
                juce::Justification::centredLeft);

    // トラック群とバス群の区切り線
    const int sepX = masterStrip.getX() - (MixerStrip::preferredWidth + stripGap) * numSendBuses
                     - sectionGap / 2 - stripGap;
    g.setColour (Theme::popupBorder);
    g.drawVerticalLine (sepX, (float) panel.getY() + 10.0f, (float) panel.getBottom() - 10.0f);
}

void MixerOverlay::resized()
{
    auto panel = panelBounds().withTrimmedTop (titleBarHeight).reduced (panelPad, 0);
    panel.removeFromBottom (panelPad);

    const int stripW = MixerStrip::preferredWidth;
    const int stripH = panel.getHeight();

    // 右端から Master → バス3本（固定表示）
    masterStrip.setBounds (panel.removeFromRight (stripW));
    for (int b = numSendBuses - 1; b >= 0; --b)
    {
        panel.removeFromRight (stripGap);
        busStrips[b].setBounds (panel.removeFromRight (stripW));
    }
    panel.removeFromRight (sectionGap);

    // 残りがトラック群（横スクロール）
    viewport.setBounds (panel);
    const int rowW = juce::jmax (panel.getWidth(),
                                 (int) trackStrips.size() * (stripW + stripGap));
    stripRow.setSize (rowW, stripH);
    for (int i = 0; i < (int) trackStrips.size(); ++i)
        trackStrips[(size_t) i]->setBounds (i * (stripW + stripGap), 0, stripW, stripH);
}

void MixerOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (! panelBounds().contains (e.getPosition()))
    {
        dismiss();
        return;
    }
    // パネル内の地の部分（タイトルバー・隙間。子コンポーネント上のクリックはここへ来ない）はドラッグ開始
    draggingPanel = true;
    dragAnchor = e.getPosition() - panelOffset;
}

void MixerOverlay::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingPanel)
        return;
    panelOffset = e.getPosition() - dragAnchor;
    resized(); // 子のレイアウトをパネルに追従させる（panelBoundsがクランプする）
    repaint();
}
