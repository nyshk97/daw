#include "FxEditorView.h"

#include "Fonts.h"
#include "Theme.h"

namespace
{
constexpr int titleHeight = 28;
constexpr int pad = 14;
constexpr int thumbHeight = 44;
constexpr int slotHeight = 22;
constexpr int slotGap = 4;
constexpr int sendsHeaderHeight = 20;
constexpr int sendsKnobRowHeight = 36 + SendKnob::labelHeight;
constexpr int panKnobSize = 46;

// 電源アイコン（円弧＋上の縦線）。areaの中心に描く
void drawPowerIcon (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto c = area.getCentre();
    const float r = 4.5f;
    juce::Path p;
    p.addCentredArc (c.x, c.y + 1.0f, r, r, 0.0f, 0.7f,
                     juce::MathConstants<float>::twoPi - 0.7f, true);
    p.startNewSubPath (c.x, c.y - r - 1.0f);
    p.lineTo (c.x, c.y - 0.5f);
    g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded));
}

// エディタアイコン（スライダー2本: 横線＋つまみの丸）。areaの中心に描く
void drawEditIcon (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto c = area.getCentre();
    for (int i = 0; i < 2; ++i)
    {
        const float ly = c.y + (i == 0 ? -3.0f : 3.0f);
        const float thumbX = c.x + (i == 0 ? -2.0f : 2.0f);
        g.fillRect (juce::Rectangle<float> (c.x - 5.5f, ly - 0.7f, 11.0f, 1.4f));
        g.fillEllipse (juce::Rectangle<float> (3.6f, 3.6f).withCentre ({ thumbX, ly }));
    }
}
} // namespace

FxEditorView::FxEditorView()
{
    addChildComponent (closeButton);
    closeButton.getProperties().set ("flatButton", true);
    closeButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    closeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.55f));
    closeButton.onClick = [this] { if (onCloseRequested) onCloseRequested(); };

    for (auto& knob : sendKnobs)
    {
        addChildComponent (knob);
        knob.onChanged = [this] { if (onSendOrPanChanged) onSendOrPanChanged(); };
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

    // Space（再生/停止）を奪わせない（SendKnobは自前で設定済み）
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
    for (auto& knob : sendKnobs)
        knob.refreshValue();
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
    if (! juce::approximatelyEqual (feed->maxSincePlay, peakMaxDisplay))
    {
        peakMaxDisplay = feed->maxSincePlay;
        repaint (volumeReadoutArea);
    }
}

juce::String FxEditorView::slotName (int slot) const
{
    if (slot >= 0 && slot < (int) slots.size())
        return slots[(size_t) slot].name;
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
    repaint();
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

std::atomic<bool>* FxEditorView::slotEnabledAtomic (int slot) const
{
    // ON/OFFを持つのはトラックのEQ(0)・Comp(1)のみ（バス/MasterのFXは常在でバイパスなし）
    if (target != Target::track)
        return nullptr;
    auto params = targetParams(); // 実体はTrackが所有しているので返り値の寿命に依存しない
    if (params == nullptr)
        return nullptr;
    if (slot == 0)
        return &params->eqEnabled;
    if (slot == 1)
        return &params->compEnabled;
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

    for (auto& knob : sendKnobs)
        knob.setVisible (isTrack);
    panKnob.setVisible (isTrack);
    closeButton.setVisible (true);

    if (isTrack)
    {
        for (auto& knob : sendKnobs)
            knob.bind (params);
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

    slots.clear();
    eqThumbArea = {};
    sendsArea = {};
    area = area.reduced (pad, 0);

    auto addSlot = [this, &area] (const juce::String& name, bool grayed = false)
    {
        slots.push_back ({ area.removeFromTop (slotHeight), name, grayed });
        area.removeFromTop (slotGap);
    };

    if (target == Target::track)
    {
        // EQサムネイル（クリック=EQエディタを開くショートカット）
        area.removeFromTop (10);
        eqThumbArea = area.removeFromTop (thumbHeight);
        area.removeFromTop (10);

        addSlot ("EQ");
        addSlot ("Comp");
        addSlot ("Ext", true); // スライス6まで操作不可（空きスロット表示）

        // Sends区画（見出し＋ノブ行）
        area.removeFromTop (10);
        sendsArea = area.removeFromTop (sendsHeaderHeight + sendsKnobRowHeight + 10);
        auto knobRow = sendsArea.withTrimmedTop (sendsHeaderHeight).reduced (8, 0)
                           .removeFromTop (sendsKnobRowHeight);
        const int knobW = knobRow.getWidth() / numSendBuses;
        for (auto& knob : sendKnobs)
            knob.setBounds (knobRow.removeFromLeft (knobW));

        // Panノブ（Logicと同じく外付けの目印は持たない。リング・tickはAppLookAndFeelが描く）
        area.removeFromTop (8);
        panKnob.setBounds (area.removeFromTop (panKnobSize)
                               .withSizeKeepingCentre (panKnobSize, panKnobSize));
    }
    else if (target == Target::bus || target == Target::master)
    {
        area.removeFromTop (10);
        const bool isDelay = target == Target::bus && targetBus == 2;
        addSlot (target == Target::master ? "Limiter" : (isDelay ? "Delay" : "Reverb"));
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

    // EQサムネイル（EQ DSP実装まではフラットカーブのプレースホルダ）
    if (! eqThumbArea.isEmpty())
    {
        const auto thumb = eqThumbArea.toFloat();
        g.setColour (Theme::faderSlotBg);
        g.fillRoundedRectangle (thumb, 4.0f);
        {
            juce::Graphics::ScopedSaveState save (g);
            juce::Path clip;
            clip.addRoundedRectangle (thumb, 4.0f);
            g.reduceClipRegion (clip);

            const float curveY = thumb.getY() + thumb.getHeight() * 0.55f;
            g.setColour (Theme::eqThumbCurve.withAlpha (0.14f)); // カーブ下の淡い塗り
            g.fillRect (thumb.withTop (curveY));
            g.setColour (Theme::eqThumbCurve);
            g.fillRect (juce::Rectangle<float> (thumb.getX(), curveY - 0.9f,
                                                thumb.getWidth(), 1.8f));
        }
        if (hoverThumb)
        {
            g.setColour (Theme::accent.brighter (0.4f).withAlpha (0.7f));
            g.drawRoundedRectangle (thumb.reduced (0.5f), 4.0f, 1.0f);
        }
    }

    // スロット（Logic風ピル。ON=青・OFF=グレー・空き=暗い枠。
    // hover中の行は「電源｜エディタ」の2分割コントロールに変わる）
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const auto& slot = slots[(size_t) i];
        const auto bounds = slot.bounds.toFloat();

        if (slot.grayed)
        {
            g.setColour (Theme::faderSlotBg);
            g.fillRoundedRectangle (bounds, 5.0f);
            g.setColour (juce::Colours::white.withAlpha (0.25f));
            g.setFont (Fonts::small());
            g.drawText (slot.name, slot.bounds, juce::Justification::centred);
            continue;
        }

        auto* enabled = slotEnabledAtomic (i);
        const bool isOn = enabled == nullptr || enabled->load();
        const auto base = isOn ? Theme::accent : Theme::controlBg;
        const bool hovered = i == hoverSlot;

        if (enabled != nullptr && hovered)
        {
            // 2分割（Logicの電源｜エディタ｜差し替え矢印から、固定スロットに不要な矢印を除いた形）
            juce::Graphics::ScopedSaveState save (g);
            juce::Path clip;
            clip.addRoundedRectangle (bounds, 5.0f);
            g.reduceClipRegion (clip);

            auto left = bounds;
            const auto right = left.removeFromRight (bounds.getWidth() * 0.5f);
            g.setColour (hoverPower ? base.brighter (0.18f) : base);
            g.fillRect (left);
            g.setColour (hoverPower ? base : base.brighter (0.18f));
            g.fillRect (right);
            g.setColour (juce::Colours::black.withAlpha (0.35f)); // 分割線
            g.fillRect (juce::Rectangle<float> (right.getX() - 0.5f, bounds.getY(),
                                                1.0f, bounds.getHeight()));

            g.setColour (juce::Colours::white.withAlpha (0.95f));
            drawPowerIcon (g, left);
            drawEditIcon (g, right);
        }
        else
        {
            g.setColour (hovered ? base.brighter (0.1f) : base);
            g.fillRoundedRectangle (bounds, 5.0f);
            g.setColour (juce::Colours::white.withAlpha (isOn ? 0.95f : 0.75f));
            g.setFont (Fonts::smallStrong());
            g.drawText (slot.name, slot.bounds, juce::Justification::centred);
        }

        if (i == activeSlot) // 下部エディタで開いている行（Logicのリンク枠相当）
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawRoundedRectangle (bounds.reduced (0.75f), 5.0f, 1.5f);
        }
    }

    // Sends区画の見出し
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
    {
        if (onSlotClicked)
            onSlotClicked (0); // トラックのスロット0=EQ
        return;
    }

    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const auto& slot = slots[(size_t) i];
        if (slot.grayed || ! slot.bounds.contains (e.getPosition()))
            continue;
        if (auto* enabled = slotEnabledAtomic (i); enabled != nullptr
                                                   && e.x < slot.bounds.getCentreX())
        {
            enabled->store (! enabled->load()); // 左半分=電源トグル
            repaint();
            if (onFxEnabledChanged)
                onFxEnabledChanged();
            return;
        }
        if (onSlotClicked)
            onSlotClicked (i);
        return;
    }
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
    int newSlot = -1;
    bool newPower = false;
    const bool newThumb = ! eqThumbArea.isEmpty() && eqThumbArea.contains (pos);

    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const auto& slot = slots[(size_t) i];
        if (slot.grayed || ! slot.bounds.contains (pos))
            continue;
        newSlot = i;
        newPower = slotEnabledAtomic (i) != nullptr && pos.x < slot.bounds.getCentreX();
        break;
    }

    if (newSlot != hoverSlot || newPower != hoverPower || newThumb != hoverThumb)
    {
        hoverSlot = newSlot;
        hoverPower = newPower;
        hoverThumb = newThumb;
        repaint();
    }
}
