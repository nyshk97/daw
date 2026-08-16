#include "FxEditorView.h"

#include "Fonts.h"
#include "FxSlotLayout.h"
#include "StripParts.h"
#include "Theme.h"

namespace
{
constexpr int titleHeight = 28;
constexpr int pad = 14;
constexpr int thumbHeight = 44;
constexpr int slotHeight = 22;
constexpr int slotGap = 4;
constexpr int sendsHeaderHeight = 20;
constexpr int sendRowGap = 4;
constexpr int panKnobSize = 46;
} // namespace

FxEditorView::FxEditorView()
{
    addChildComponent (closeButton);
    closeButton.getProperties().set ("flatButton", true);
    closeButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    closeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.55f));
    closeButton.onClick = [this] { if (onCloseRequested) onCloseRequested(); };

    // スロットピル（対話込みの実体はSlotPill。ミキサーのストリップと共有部品）
    for (int i = 0; i < maxSlots; ++i)
    {
        addChildComponent (slotPills[i]);
        slotPills[i].onOpenEditor = [this, i] { if (onSlotClicked) onSlotClicked (i); };
        slotPills[i].onPowerToggled = [this]
        {
            repaintEqThumbnail(); // eqEnabledはサムネイルの実カーブ/フラット切替にも効く
            if (onFxEnabledChanged)
                onFxEnabledChanged();
        };
    }

    // Sendsの行（バス名ピル＋小ノブ。部品はミキサーのストリップと共有）
    for (auto& row : sendRows)
    {
        addChildComponent (row);
        row.onChanged = [this] { if (onSendOrPanChanged) onSendOrPanChanged(); };
    }

    // Panノブ（Logicのストリップと同じ「Sendsの下・フェーダーの上」。描画はAppLookAndFeel）
    addChildComponent (panKnob);
    panKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    panKnob.setRange (-1.0, 1.0);
    panKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.getProperties().set ("logicKnob", true);
    panKnob.onValueChange = [this]
    {
        if (auto params = targetParams())
            params->pan.store ((float) panKnob.getValue());
        if (onSendOrPanChanged)
            onSendOrPanChanged();
    };

    // 音量フェーダー＋L/Rメーター（Logicのチャンネルストリップと同じ分離配置。全チャンネル共通）
    addChildComponent (volumeSlider);
    addChildComponent (meter);
    volumeSlider.setSliderStyle (juce::Slider::LinearVertical);
    volumeSlider.setRange (0.0, 1.0);
    volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.onValueChange = [this]
    {
        if (auto params = targetParams())
            params->gain.store ((float) volumeSlider.getValue());
        repaint (volumeReadoutArea); // 設定値dBの表示を追従させる
        if (onVolumeChanged)
            onVolumeChanged();
    };

    // Space（再生/停止）を奪わせない（SendRowは自前で設定済み）
    for (auto* c : std::initializer_list<juce::Component*> { &closeButton, &panKnob,
                                                            &volumeSlider })
    {
        c->setWantsKeyboardFocus (false);
        c->setMouseClickGrabsKeyboardFocus (false);
    }

    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

void FxEditorView::openView()
{
    open = true;
    setVisible (true);
    rebind();
}

void FxEditorView::closeView()
{
    open = false;
    setVisible (false);
}

void FxEditorView::showTrack (int trackIndex)
{
    target = Target::track;
    targetTrack = trackIndex;
    rebind();
}

void FxEditorView::showBus (int busIndex)
{
    target = Target::bus;
    targetBus = juce::jlimit (0, numSendBuses - 1, busIndex);
    rebind();
}

void FxEditorView::showMaster()
{
    target = Target::master;
    rebind();
}

void FxEditorView::remapTrack (int newIndex)
{
    // 表示対象は同じトラックのままindexだけ引き直す（並び替えでは名前・paramsは変わらないので
    // rebind不要。newIndex < 0 = 対象が見つからない場合は refreshFromModel の防御に任せる）
    if (target == Target::track && newIndex >= 0)
        targetTrack = newIndex;
}

void FxEditorView::refreshFromModel (int selectedTrack)
{
    // 表示対象トラックが消えていたら選択トラック追従へ戻す（リネームはrebindで名前が更新される）
    if (target == Target::track
        && (project == nullptr || targetTrack < 0 || targetTrack >= (int) project->tracks.size()))
        targetTrack = selectedTrack;
    rebind();
}

void FxEditorView::refreshValues()
{
    if (! isVisible())
        return;
    for (auto& row : sendRows)
        row.refreshValue();
    for (auto& pill : slotPills)
        pill.repaint(); // EQ/CompのON/OFF（ミキサー側での電源トグル）を色に反映
    repaintEqThumbnail(); // eqEnabledトグルはサムネイルにも効く

    if (auto params = targetParams())
    {
        volumeSlider.setValue (params->gain.load(), juce::dontSendNotification);
        panKnob.setValue (params->pan.load(), juce::dontSendNotification);
    }
}

void FxEditorView::updateMeters (const std::vector<MeterFeed>& trackFeeds,
                                 const MeterFeed (&busFeeds)[numSendBuses], const MeterFeed& masterFeed)
{
    if (! isVisible() || ! meter.isVisible())
        return;

    static const MeterFeed silent;
    const MeterFeed* feed = &silent;
    switch (target)
    {
        case Target::track:
            if (targetTrack >= 0 && targetTrack < (int) trackFeeds.size())
                feed = &trackFeeds[(size_t) targetTrack];
            break;
        case Target::bus:    feed = &busFeeds[targetBus]; break;
        case Target::master: feed = &masterFeed; break;
        case Target::none:   break;
    }
    meter.update (feed->peak);
    if (target == Target::track)
        slotPills[FxSlots::grSlot].setGainReductionDb (feed->compGrDb); // Compピルのミニ GRバー
    if (target == Target::master)
        slotPills[0].setGainReductionDb (feed->limiterGrDb); // LimiterピルのミニGRバー
    if (! juce::approximatelyEqual (feed->maxSincePlay, peakMaxDisplay))
    {
        peakMaxDisplay = feed->maxSincePlay;
        repaint (volumeReadoutArea);
    }
}

juce::String FxEditorView::slotName (int slot) const
{
    if (slot >= 0 && slot < maxSlots)
        return slotNames[slot];
    return {};
}

juce::String FxEditorView::targetKey() const
{
    switch (target)
    {
        case Target::track:  return "track";
        case Target::bus:    return "bus" + juce::String (targetBus);
        case Target::master: return "master";
        case Target::none:   break;
    }
    return {};
}

void FxEditorView::setActiveSlot (int slot)
{
    if (activeSlot == slot)
        return;
    activeSlot = slot;
    for (int i = 0; i < maxSlots; ++i)
        slotPills[i].setActiveOutline (i == activeSlot);
}

std::shared_ptr<TrackParams> FxEditorView::targetParams() const
{
    if (project == nullptr)
        return nullptr;
    switch (target)
    {
        case Target::track:
            if (targetTrack >= 0 && targetTrack < (int) project->tracks.size())
                return project->tracks[(size_t) targetTrack].params;
            return nullptr;
        case Target::bus:    return project->busParams[targetBus];
        case Target::master: return project->masterParams;
        case Target::none:   break;
    }
    return nullptr;
}

void FxEditorView::rebind()
{
    if (! isVisible())
        return;

    auto params = targetParams();
    const bool isTrack = target == Target::track && params != nullptr;

    switch (target)
    {
        case Target::track:
            titleName = isTrack ? project->tracks[(size_t) targetTrack].name : juce::String();
            break;
        case Target::bus:    titleName = SendBuses::names[targetBus]; break;
        case Target::master: titleName = "Master"; break;
        case Target::none:   titleName = {}; break;
    }

    // スロット構成（定義はFxSlotLayoutが単一の真実の源。enabled atomicの実体はTrackが所有する
    // TrackParams）。表示順もそちらの投影規則（panelOrder。Instrument=音源が一番上）に従う。
    // 配列index=スロット番号（意味ID）・存在は used。未使用スロットは名前を空にする
    //（isValidSlot＝履歴復元の判定材料）
    numOrderedSlots = 0;
    for (auto& name : slotNames)
        name = {};
    if (isTrack || params != nullptr)
    {
        const auto layout = isTrack ? FxSlots::trackPanelLayout (project->tracks[(size_t) targetTrack])
                            : target == Target::master ? FxSlots::masterLayout()
                                                       : FxSlots::busLayout (targetBus);
        for (int i = 0; i < maxSlots; ++i)
        {
            if (! layout.slots[i].used)
                continue;
            slotNames[i] = layout.slots[i].name;
            slotPills[i].configure (layout.slots[i].name, layout.slots[i].enabled,
                                    layout.slots[i].placeholder);
        }
        numOrderedSlots = isTrack ? FxSlots::panelOrder (layout, slotOrder)
                                  : (slotOrder[0] = 0, 1);
    }

    for (auto& pill : slotPills)
        pill.setVisible (false);
    for (int i = 0; i < numOrderedSlots; ++i)
        slotPills[slotOrder[i]].setVisible (true);

    for (auto& row : sendRows)
        row.setVisible (isTrack);
    panKnob.setVisible (isTrack);
    closeButton.setVisible (true);

    if (isTrack)
    {
        for (auto& row : sendRows)
            row.bind (params);
        panKnob.setValue (params->pan.load(), juce::dontSendNotification);
    }

    volumeSlider.setVisible (params != nullptr);
    meter.setVisible (params != nullptr);
    if (params != nullptr)
    {
        volumeSlider.setDoubleClickReturnValue (true, isTrack ? 0.8 : 1.0); // バス/Masterの既定はユニティ
        volumeSlider.setValue (params->gain.load(), juce::dontSendNotification);
    }

    resized();
    repaint();
}

void FxEditorView::resized()
{
    auto area = getLocalBounds();
    auto title = area.removeFromTop (titleHeight);
    closeButton.setBounds (title.removeFromRight (30).withSizeKeepingCentre (22, 20));

    eqThumbArea = {};
    sendsArea = {};
    area = area.reduced (pad, 0);

    // 配置は slotOrder の順（配列indexの並びとは別。Instrumentが先頭に来る）
    auto placeSlots = [this, &area]
    {
        for (int i = 0; i < numOrderedSlots; ++i)
        {
            slotPills[slotOrder[i]].setBounds (area.removeFromTop (slotHeight));
            area.removeFromTop (slotGap);
        }
    };

    if (target == Target::track)
    {
        // EQサムネイル（クリック=EQエディタを開くショートカット）
        area.removeFromTop (10);
        eqThumbArea = area.removeFromTop (thumbHeight);
        area.removeFromTop (10);

        placeSlots();

        // Sends区画（見出し＋「バス名ピル｜小ノブ」の行×3。Logicのsendスロット相当）
        area.removeFromTop (10);
        sendsArea = area.removeFromTop (sendsHeaderHeight
                                        + numSendBuses * SendRow::preferredHeight
                                        + (numSendBuses - 1) * sendRowGap + 10);
        auto rows = sendsArea.withTrimmedTop (sendsHeaderHeight);
        for (auto& row : sendRows)
        {
            row.setBounds (rows.removeFromTop (SendRow::preferredHeight));
            rows.removeFromTop (sendRowGap);
        }

        // Panノブ（Logicと同じく外付けの目印は持たない。リング・tickはAppLookAndFeelが描く）
        area.removeFromTop (8);
        panKnob.setBounds (area.removeFromTop (panKnobSize)
                               .withSizeKeepingCentre (panKnobSize, panKnobSize));
    }
    else if (target == Target::bus || target == Target::master)
    {
        area.removeFromTop (10);
        placeSlots();
    }

    // dB数値ボックス（左=設定値、右=ピーク。LogicのストリップどおりPanノブの下・フェーダーの上）
    area.removeFromTop (10);
    volumeReadoutArea = area.removeFromTop (16).withSizeKeepingCentre (84, 16);
    area.removeFromTop (6);
    area.removeFromBottom (12);

    // フェーダー（目盛り込み）＋L/Rメーター（dB数字込み）。残り全部の高さ
    const int faderW = 38;
    const int meterW = 26;
    auto pair = area.withSizeKeepingCentre (faderW + 2 + meterW, area.getHeight());
    volumeSlider.setBounds (pair.removeFromLeft (faderW));
    pair.removeFromLeft (2);
    // フェーダーと同じ高さで置く（井戸の上下揃えはStereoMeter::wellInsetYが行う）
    meter.setBounds (pair.withWidth (meterW));
}

void FxEditorView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::timelineBg);

    // ヘッダー列との境界線＋タイトル行（FX＋チャンネル名・下に区切り線）
    g.setColour (Theme::panelBorder);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
    g.drawHorizontalLine (titleHeight - 1, 0.0f, (float) getWidth() - 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (Fonts::bodyStrong());
    g.drawText ("FX", 12, 0, 26, titleHeight, juce::Justification::centredLeft);
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.setFont (Fonts::forText (Fonts::body(), titleName));
    g.drawText (titleName, 38, 0, getWidth() - 74, titleHeight, juce::Justification::centredLeft);

    // EQサムネイル（描画はミキサーと共有のStripParts）
    if (! eqThumbArea.isEmpty())
    {
        if (auto params = targetParams())
        {
            const double sr = getSampleRate ? getSampleRate() : 0.0;
            StripParts::drawEqThumbnail (g, eqThumbArea.toFloat(),
                                         Eq::loadAll (params->eqBands),
                                         params->eqEnabled.load(),
                                         sr > 0.0 ? sr : 48000.0);
        }
        if (hoverThumb)
        {
            g.setColour (Theme::accent.brighter (0.4f).withAlpha (0.7f));
            g.drawRoundedRectangle (eqThumbArea.toFloat().reduced (0.5f), 4.0f, 1.0f);
        }
    }

    // Sends区画の見出し（スロット・Sendsの行本体はSlotPill/SendRowが描く）
    if (target == Target::track && ! sendsArea.isEmpty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.setFont (Fonts::small());
        g.drawText ("SENDS", sendsArea.withHeight (sendsHeaderHeight), juce::Justification::centredLeft);
    }

    // dB数値（左=フェーダー設定値、右=再生開始からのピーク保持）
    if (volumeSlider.isVisible() && ! volumeReadoutArea.isEmpty())
        if (auto params = targetParams())
            Meters::drawDbReadout (g, volumeReadoutArea, params->gain.load(), peakMaxDisplay);
}

void FxEditorView::mouseDown (const juce::MouseEvent& e)
{
    if (! eqThumbArea.isEmpty() && eqThumbArea.contains (e.getPosition()))
        if (onSlotClicked)
            onSlotClicked (FxSlots::eq); // サムネイル=EQエディタを開く
}

void FxEditorView::mouseMove (const juce::MouseEvent& e)
{
    updateHover (e.getPosition());
}

void FxEditorView::mouseExit (const juce::MouseEvent&)
{
    updateHover ({ -1, -1 });
}

void FxEditorView::updateHover (juce::Point<int> pos)
{
    const bool newThumb = ! eqThumbArea.isEmpty() && eqThumbArea.contains (pos);
    if (newThumb != hoverThumb)
    {
        hoverThumb = newThumb;
        repaint (eqThumbArea);
    }
}
