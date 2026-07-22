#include "FxEditorView.h"

#include "Fonts.h"
#include "Theme.h"

namespace
{
constexpr int titleHeight = 28;
constexpr int pad = 8;
constexpr int slotHeight = 30;
constexpr int slotGap = 3;
constexpr int sendsHeaderHeight = 20;
constexpr int sendsKnobRowHeight = 36 + SendKnob::labelHeight;
} // namespace

FxEditorView::FxEditorView()
{
    addChildComponent (closeButton);
    closeButton.getProperties().set ("flatButton", true);
    closeButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    closeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.55f));
    closeButton.onClick = [this] { if (onCloseRequested) onCloseRequested(); };

    // EQ/CompのON/OFF豆トグル（M/Sボタンと同じフラット角丸・点灯はアクセント）
    for (auto* b : std::initializer_list<juce::TextButton*> { &eqButton, &compButton })
    {
        addChildComponent (*b);
        b->setClickingTogglesState (true);
        b->getProperties().set ("flatButton", true);
        b->setColour (juce::TextButton::buttonColourId, Theme::controlBg);
        b->setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.4f));
        b->setColour (juce::TextButton::textColourOnId, Theme::controlTextOn);
        b->setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    }
    eqButton.onClick = [this]
    {
        if (auto params = targetParams())
            params->eqEnabled.store (eqButton.getToggleState());
        if (onFxEnabledChanged)
            onFxEnabledChanged();
    };
    compButton.onClick = [this]
    {
        if (auto params = targetParams())
            params->compEnabled.store (compButton.getToggleState());
        if (onFxEnabledChanged)
            onFxEnabledChanged();
    };

    for (auto& knob : sendKnobs)
    {
        addChildComponent (knob);
        knob.onChanged = [this] { if (onSendChanged) onSendChanged(); };
    }

    // Space（再生/停止）を奪わせない（SendKnobは自前で設定済み）
    for (auto* c : std::initializer_list<juce::Component*> { &closeButton, &eqButton, &compButton })
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

    eqButton.setVisible (isTrack);
    compButton.setVisible (isTrack);
    for (auto& knob : sendKnobs)
        knob.setVisible (isTrack);
    closeButton.setVisible (true);

    if (isTrack)
    {
        eqButton.setToggleState (params->eqEnabled.load(), juce::dontSendNotification);
        compButton.setToggleState (params->compEnabled.load(), juce::dontSendNotification);
        for (auto& knob : sendKnobs)
            knob.bind (params);
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
    sendsArea = {};
    area = area.reduced (pad, 0);

    auto addSlot = [this, &area] (const juce::String& name, bool grayed = false)
    {
        slots.push_back ({ area.removeFromTop (slotHeight), name, grayed });
        area.removeFromTop (slotGap);
    };

    if (target == Target::track)
    {
        addSlot ("EQ");
        addSlot ("Comp");
        addSlot ("Ext", true); // スライス6まで操作不可（グレーアウト）

        // ON/OFFはスロット行の右端（クリック領域はボタンが先に受けるので行クリックと共存できる）
        eqButton.setBounds (slots[0].bounds.withTrimmedRight (8).removeFromRight (34).withSizeKeepingCentre (34, 18));
        compButton.setBounds (slots[1].bounds.withTrimmedRight (8).removeFromRight (34).withSizeKeepingCentre (34, 18));

        // Sends区画（見出し＋ノブ行）
        area.removeFromTop (8);
        sendsArea = area.removeFromTop (sendsHeaderHeight + sendsKnobRowHeight + 10);
        auto knobRow = sendsArea.withTrimmedTop (sendsHeaderHeight).reduced (8, 0)
                           .removeFromTop (sendsKnobRowHeight);
        const int knobW = knobRow.getWidth() / numSendBuses;
        for (auto& knob : sendKnobs)
            knob.setBounds (knobRow.removeFromLeft (knobW));
    }
    else if (target == Target::bus || target == Target::master)
    {
        const bool isDelay = target == Target::bus && targetBus == 2;
        addSlot (target == Target::master ? "Limiter" : (isDelay ? "Delay" : "Reverb"));
    }
}

void FxEditorView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::timelineBg);

    // ヘッダー列との境界線＋タイトル（チャンネル名）
    g.setColour (Theme::panelBorder);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (Fonts::bodyStrong());
    g.drawText ("FX", 12, 0, 26, titleHeight, juce::Justification::centredLeft);
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.setFont (Fonts::forText (Fonts::body(), titleName));
    g.drawText (titleName, 38, 0, getWidth() - 74, titleHeight, juce::Justification::centredLeft);

    // スロット行（クリックで下部に詳細が開く。activeSlot = 詳細を開いている行）
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const auto& slot = slots[(size_t) i];
        const float alpha = slot.grayed ? 0.4f : 1.0f;
        const bool active = i == activeSlot;

        g.setColour ((active ? Theme::headerSelectedBg : Theme::headerBg).withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (slot.bounds.toFloat(), 5.0f);
        if (active)
        {
            g.setColour (Theme::accent);
            g.fillRoundedRectangle (slot.bounds.toFloat().removeFromLeft (3.0f), 1.5f);
        }
        g.setColour (juce::Colours::white.withAlpha ((active ? 0.9f : 0.7f) * alpha));
        g.setFont (Fonts::body());
        g.drawText (slot.name, slot.bounds.withTrimmedLeft (10), juce::Justification::centredLeft);
    }

    // Sends区画の見出し
    if (target == Target::track && ! sendsArea.isEmpty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.setFont (Fonts::small());
        g.drawText ("SENDS", sendsArea.withHeight (sendsHeaderHeight).withTrimmedLeft (10),
                    juce::Justification::centredLeft);
    }
}

void FxEditorView::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const auto& slot = slots[(size_t) i];
        if (slot.grayed || ! slot.bounds.contains (e.getPosition()))
            continue;
        if (onSlotClicked)
            onSlotClicked (i);
        return;
    }
}
