#include "MixerOverlay.h"

#include <cmath>

#include "Fonts.h"
#include "StripParts.h"
#include "Theme.h"

namespace
{
constexpr int stripGap = 1;       // ストリップ間の区切り（背景色が見える隙間）
constexpr int sectionGap = 10;    // トラック群とバス群の間
constexpr int panelPad = 12;
} // namespace

// ---- MixerStrip -----------------------------------------------------------

MixerStrip::MixerStrip (Kind kindToUse, juce::String fxSlotNameToUse)
    : kind (kindToUse), fxSlotName (std::move (fxSlotNameToUse))
{
    const bool isTrack = kind == Kind::track;

    // スロットピル（トラック=3・バス/Master=1。中身の構成はbindで行う）
    for (int i = 0; i < (isTrack ? 3 : 1); ++i)
    {
        addAndMakeVisible (slotPills[i]);
        slotPills[i].onOpenEditor = [this, i] { if (onOpenSlot) onOpenSlot (i); };
        slotPills[i].onPowerToggled = [this] { if (onChanged) onChanged(); };
    }

    if (isTrack)
    {
        for (auto& row : sendRows)
        {
            addAndMakeVisible (row);
            row.onChanged = [this] { if (onChanged) onChanged(); };
        }
        setupKnob (panKnob, -1.0, 1.0, 0.0);
        panKnob.getProperties().set ("logicKnob", true); // FXパネルと同じLogic風ノブ（値はノブ中央に出る）
        panKnob.onValueChange = [this]
        {
            if (params != nullptr)
                params->pan.store ((float) panKnob.getValue());
            if (onChanged)
                onChanged();
        };
    }

    addAndMakeVisible (fader);
    addAndMakeVisible (meter);
    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setRange (0.0, 1.0);
    fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    fader.setDoubleClickReturnValue (true, isTrack ? 0.8 : 1.0); // バス/Masterの既定はユニティ
    fader.onValueChange = [this]
    {
        if (params != nullptr)
            params->gain.store ((float) fader.getValue());
        repaint (readoutArea); // 設定値dBの表示を追従させる
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

    // Space（再生/停止）を奪わせない（SendRowは自前で設定済み）
    for (auto* c : std::initializer_list<juce::Component*> {
             &fader, &panKnob, &muteButton, &soloButton })
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
            for (auto& row : sendRows)
                row.bind (params);
        }
    }

    // スロットピルの構成（enabled atomicの実体はTrackが所有するTrackParams。bindのたびに差し替える）
    if (kind == Kind::track)
    {
        slotPills[0].configure ("EQ", params != nullptr ? &params->eqEnabled : nullptr, false);
        slotPills[1].configure ("Comp", params != nullptr ? &params->compEnabled : nullptr, false);
        slotPills[2].configure ("Ext", nullptr, true);
    }
    else
        slotPills[0].configure (fxSlotName, nullptr, false);

    repaint();
}

void MixerStrip::updateMeter (const MeterFeed& feed)
{
    meter.update (feed.peak);
    if (! juce::approximatelyEqual (feed.maxSincePlay, peakMaxDisplay))
    {
        peakMaxDisplay = feed.maxSincePlay;
        repaint (readoutArea);
    }
}

void MixerStrip::paint (juce::Graphics& g)
{
    g.fillAll (selected ? Theme::headerSelectedBg : Theme::headerBg);
    if (selected)
    {
        g.setColour (Theme::accent);
        g.fillRect (0, 0, getWidth(), 2);
    }

    // EQサムネイル（スロットピルは子コンポーネントのSlotPillが描く）
    if (! eqThumbArea.isEmpty())
        StripParts::drawEqThumbnail (g, eqThumbArea.toFloat());

    // dB数値（左=フェーダー設定値、右=再生開始からのピーク保持）
    if (params != nullptr && ! readoutArea.isEmpty())
        Meters::drawDbReadout (g, readoutArea, params->gain.load(), peakMaxDisplay);

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

    // FXパネルと同じ縦並び（サムネイル→スロット→Sends行→Panノブ）
    if (kind == Kind::track)
    {
        eqThumbArea = area.removeFromTop (30);
        area.removeFromTop (6);
        for (auto& pill : slotPills)
        {
            pill.setBounds (area.removeFromTop (20));
            area.removeFromTop (3);
        }
        area.removeFromTop (3);
        for (auto& row : sendRows)
        {
            row.setBounds (area.removeFromTop (SendRow::preferredHeight));
            area.removeFromTop (3);
        }
        area.removeFromTop (3);
        panKnob.setBounds (area.removeFromTop (42).withSizeKeepingCentre (42, 42));
        area.removeFromTop (4);
    }
    else
    {
        eqThumbArea = {};
        slotPills[0].setBounds (area.removeFromTop (20));
        area.removeFromTop (6);
    }

    // dB数値の行（左=設定値、右=ピーク。Logicのストリップと同じくフェーダーの上）
    readoutArea = area.removeFromTop (16);
    area.removeFromTop (4);

    // フェーダー（目盛り込み）＋L/Rメーター（dB数字込み）。残り全部の高さ。
    // Logicのストリップと同じ分離配置
    const int faderW = 38;
    const int meterW = 26;
    const int totalW = faderW + 2 + meterW;
    auto faderArea = area.withSizeKeepingCentre (totalW, area.getHeight());
    fader.setBounds (faderArea.removeFromLeft (faderW));
    faderArea.removeFromLeft (2);
    // フェーダーと同じ高さで置く（井戸の上下揃えはStereoMeter::wellInsetYが行う）
    meter.setBounds (faderArea.withWidth (meterW));
}

void MixerStrip::mouseDown (const juce::MouseEvent& e)
{
    if (kind == Kind::track && ! eqThumbArea.isEmpty() && eqThumbArea.contains (e.getPosition()))
    {
        if (onOpenSlot)
            onOpenSlot (0); // サムネイル=EQエディタを開く（FXパネルと同じショートカット）
        return;
    }
    if (onSelect)
        onSelect();
}

// ---- MixerOverlay ---------------------------------------------------------

MixerOverlay::MixerOverlay()
{
    // ウィンドウ内で自分がフォーカスを受け、キーをonKey（MainComponentの集中ハンドラ）へ転送する
    setWantsKeyboardFocus (true);

    addAndMakeVisible (viewport);
    viewport.setViewedComponent (&stripRow, false);
    viewport.setScrollBarsShown (false, true, false, true);
    viewport.setScrollBarThickness (8);

    for (int b = 0; b < numSendBuses; ++b)
    {
        addAndMakeVisible (busStrips[b]);
        busStrips[b].onSelect = [this, b] { if (onSelectBus) onSelectBus (b); };
        busStrips[b].onOpenSlot = [this, b] (int) { if (onOpenBusSlot) onOpenBusSlot (b); };
        busStrips[b].onChanged = [this] { if (onChanged) onChanged(); }; // 電源トグルのdirty化（バスは常時ONだが将来用）
    }
    addAndMakeVisible (masterStrip);
    masterStrip.onSelect = [this] { if (onSelectMaster) onSelectMaster(); };
    masterStrip.onOpenSlot = [this] (int) { if (onOpenMasterSlot) onOpenMasterSlot(); };
    masterStrip.onChanged = [this] { if (onChanged) onChanged(); };
}

void MixerOverlay::setProject (Project* p)
{
    project = p;
}

void MixerOverlay::sync (int newSelectedTrack)
{
    selectedTrack = newSelectedTrack;
    if (! isShowing() || project == nullptr) // isVisibleでなくisShowing（ウィンドウごと非表示を含めて判定）
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
        strip->onOpenSlot = [this, i] (int slot) { if (onOpenTrackSlot) onOpenTrackSlot (i, slot); };
        stripRow.addAndMakeVisible (*strip);
        trackStrips.push_back (std::move (strip));
    }
}

void MixerOverlay::updateMeters (const std::vector<MeterFeed>& trackFeeds,
                                 const MeterFeed (&busFeeds)[numSendBuses], const MeterFeed& masterFeed)
{
    if (! isShowing())
        return;
    for (int i = 0; i < (int) trackStrips.size(); ++i)
        trackStrips[(size_t) i]->updateMeter (i < (int) trackFeeds.size() ? trackFeeds[(size_t) i]
                                                                          : MeterFeed());
    for (int b = 0; b < numSendBuses; ++b)
        busStrips[b].updateMeter (busFeeds[b]);
    masterStrip.updateMeter (masterFeed);
}

void MixerOverlay::paint (juce::Graphics& g)
{
    g.fillAll (Theme::windowBg);

    // トラック群とバス群の区切り線
    const int sepX = masterStrip.getX() - (MixerStrip::preferredWidth + stripGap) * numSendBuses
                     - sectionGap / 2 - stripGap;
    g.setColour (Theme::popupBorder);
    g.drawVerticalLine (sepX, 10.0f, (float) getHeight() - 10.0f);
}

void MixerOverlay::resized()
{
    auto area = getLocalBounds().reduced (panelPad);

    const int stripW = MixerStrip::preferredWidth;
    const int stripH = area.getHeight();

    // 右端から Master → バス3本（固定表示）
    masterStrip.setBounds (area.removeFromRight (stripW));
    for (int b = numSendBuses - 1; b >= 0; --b)
    {
        area.removeFromRight (stripGap);
        busStrips[b].setBounds (area.removeFromRight (stripW));
    }
    area.removeFromRight (sectionGap);

    // 残りがトラック群（横スクロール）
    viewport.setBounds (area);
    const int rowW = juce::jmax (area.getWidth(),
                                 (int) trackStrips.size() * (stripW + stripGap));
    stripRow.setSize (rowW, stripH);
    for (int i = 0; i < (int) trackStrips.size(); ++i)
        trackStrips[(size_t) i]->setBounds (i * (stripW + stripGap), 0, stripW, stripH);
}

bool MixerOverlay::keyPressed (const juce::KeyPress& key)
{
    return onKey ? onKey (key) : false;
}
