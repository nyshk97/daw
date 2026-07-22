#include "TrackHeadersView.h"

#include "TimelineView.h"
#include "../shared/GmInstruments.h"
#include "Fonts.h"
#include "Shortcuts.h"
#include "Theme.h"
#include "TrackIcons.h"

// ---- TrackHeaderComponent -----------------------------------------------

TrackHeaderComponent::TrackHeaderComponent()
{
    addAndMakeVisible (nameLabel);
    addAndMakeVisible (muteButton);
    addAndMakeVisible (soloButton);
    addAndMakeVisible (volumeSlider);
    addChildComponent (instrumentBox); // MIDIトラックのみ bind() で表示

    nameLabel.setFont (Fonts::body());
    nameLabel.setEditable (false, true, false); // ダブルクリックでリネーム
    nameLabel.onTextChange = [this]
    {
        nameLabel.setFont (Fonts::forText (Fonts::body(), nameLabel.getText()));
        if (track != nullptr && nameLabel.getText() != track->name)
        {
            if (onWillChangeStructure)
                onWillChangeStructure(); // リネームはundo対象（変更前の状態を積む）
            track->name = nameLabel.getText();
            if (onChanged)
                onChanged();
        }
    };
    // ラベル上のシングルクリックでもトラック選択が効くように親へ流す
    nameLabel.addMouseListener (this, false);

    // M/Sはフラット角丸トグル（AppLookAndFeelの"flatButton"描画）。
    // 点灯色はLogic準拠でM=青・S=黄、点灯時は暗い文字にして読ませる
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

    muteButton.onClick = [this]
    {
        if (track != nullptr)
            track->params->mute.store (muteButton.getToggleState());
        if (onChanged)
            onChanged();
    };

    soloButton.onClick = [this]
    {
        if (track != nullptr)
            track->params->solo.store (soloButton.getToggleState());
        if (onChanged)
            onChanged();
    };

    volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    volumeSlider.setRange (0.0, 1.0);
    volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.setDoubleClickReturnValue (true, 0.8);
    volumeSlider.onValueChange = [this]
    {
        if (track != nullptr)
            track->params->gain.store ((float) volumeSlider.getValue());
        if (onChanged)
            onChanged();
    };

    for (int i = 0; i < numGmInstruments; ++i)
        instrumentBox.addItem (gmInstruments[i].name, i + 1); // idは楽器リストのindex+1
    instrumentBox.onChange = [this]
    {
        const int index = instrumentBox.getSelectedId() - 1;
        if (track == nullptr || index < 0 || index >= numGmInstruments)
            return;
        const auto& inst = gmInstruments[index];
        if (track->gmProgram == inst.program && track->drums == inst.drums
            && track->drumPitch == inst.fixedPitch)
            return; // bind() による表示同期では発火させない

        if (onWillChangeStructure)
            onWillChangeStructure(); // 楽器変更もundo対象
        track->gmProgram = inst.program;
        track->drums = inst.drums;
        track->drumPitch = inst.fixedPitch;
        if (onInstrumentChanged)
            onInstrumentChanged(); // pushSnapshot → SynthBank が音源を差し替える
    };

    // Space（再生/停止）を奪わせない
    for (auto* c : std::initializer_list<juce::Component*> { &muteButton, &soloButton,
                                                            &volumeSlider, &instrumentBox })
    {
        c->setWantsKeyboardFocus (false);
        c->setMouseClickGrabsKeyboardFocus (false);
    }
}

void TrackHeaderComponent::bind (Track* trackToBind, bool isSelected)
{
    track = trackToBind;
    selected = isSelected;

    if (track != nullptr)
    {
        nameLabel.setText (track->name, juce::dontSendNotification);
        nameLabel.setFont (Fonts::forText (Fonts::body(), track->name));
        muteButton.setToggleState (track->params->mute.load(), juce::dontSendNotification);
        soloButton.setToggleState (track->params->solo.load(), juce::dontSendNotification);
        volumeSlider.setValue (track->params->gain.load(), juce::dontSendNotification);

        instrumentBox.setVisible (track->type == TrackType::midi);
        if (track->type == TrackType::midi)
        {
            int matched = 0; // 一致が無ければ先頭（Piano）を表示
            for (int i = 0; i < numGmInstruments; ++i)
                if (gmInstruments[i].program == track->gmProgram && gmInstruments[i].drums == track->drums
                    && gmInstruments[i].fixedPitch == track->drumPitch)
                    { matched = i; break; }
            instrumentBox.setSelectedId (matched + 1, juce::dontSendNotification);
        }
    }
    repaint();
}

void TrackHeaderComponent::updateMeter()
{
    if (track == nullptr)
        return;

    // 読み取りリセット（audio側のCAS maxと組）。表示は「新しい値」か「前回の減衰」の大きい方
    const float incoming = track->params->peakLevel.exchange (0.0f);
    const float next = juce::jmax (incoming, meterDisplay * 0.8f);

    if (next < 0.005f)
    {
        if (meterDisplay > 0.0f) // 消える瞬間だけ描画し、無音トラックは再描画しない
        {
            meterDisplay = 0.0f;
            volumeSlider.getProperties().set ("meterLevel", 0.0);
            volumeSlider.repaint();
        }
        return;
    }

    if (juce::approximatelyEqual (next, meterDisplay))
        return; // 定常値（一定音量の持続音等）では再描画しない

    meterDisplay = next;
    volumeSlider.getProperties().set ("meterLevel", (double) meterDisplay);
    volumeSlider.repaint();
}

void TrackHeaderComponent::paint (juce::Graphics& g)
{
    g.fillAll (selected ? Theme::headerSelectedBg : Theme::headerBg);
    if (selected)
    {
        g.setColour (Theme::accent);
        g.fillRect (0, 0, 3, getHeight());
    }
    g.setColour (Theme::panelBorder);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

    if (track != nullptr)
    {
        g.setColour (juce::Colours::white.withAlpha (0.65f));
        if (track->type == TrackType::midi)
            TrackIcons::drawKeyboard (g, typeIconArea());
        else
            TrackIcons::drawWaveform (g, typeIconArea());
    }
}

juce::Rectangle<float> TrackHeaderComponent::typeIconArea() const
{
    auto row1 = getLocalBounds().reduced (8, 4).removeFromTop (24);
    return juce::Rectangle<float> (16.0f, 16.0f)
               .withCentre (row1.removeFromLeft (iconSlotWidth).toFloat().getCentre());
}

void TrackHeaderComponent::resized()
{
    auto area = getLocalBounds().reduced (8, 4);

    auto row1 = area.removeFromTop (24);
    row1.removeFromLeft (iconSlotWidth); // トラック種別アイコン（paintで描く）
    nameLabel.setBounds (row1);

    area.removeFromTop (4);
    auto row2 = area.removeFromTop (22);
    muteButton.setBounds (row2.removeFromLeft (20).reduced (0, 2));
    row2.removeFromLeft (4);
    soloButton.setBounds (row2.removeFromLeft (20).reduced (0, 2));
    row2.removeFromLeft (6);
    volumeSlider.setBounds (row2);

    area.removeFromTop (4);
    instrumentBox.setBounds (area.removeFromTop (22)); // MIDIトラックのみ表示（3行目）
}

void TrackHeaderComponent::mouseDown (const juce::MouseEvent& e)
{
    if (onSelect)
        onSelect();

    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        juce::PopupMenu::Item deleteItem (juce::String::fromUTF8 (u8"トラックを削除"));
        deleteItem.itemID = 1;
        deleteItem.shortcutKeyDescription = Shortcuts::keyText (Shortcuts::ID::deleteTrack);
        menu.addItem (deleteItem);
        // メニュー表示中にrebuild()でヘッダが破棄されることがあるためSafePointerで守る
        juce::Component::SafePointer<TrackHeaderComponent> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safe] (int result)
                            {
                                if (result == 1 && safe != nullptr && safe->onDeleteClicked)
                                    safe->onDeleteClicked();
                            });
    }
}

// ---- TrackHeadersView ----------------------------------------------------

TrackHeadersView::TrackHeadersView()
{
    addAndMakeVisible (container);
}

void TrackHeadersView::setProject (Project* p)
{
    project = p;
    rebuild();
}

void TrackHeadersView::rebuild()
{
    items.clear();

    if (project != nullptr)
    {
        for (int i = 0; i < (int) project->tracks.size(); ++i)
        {
            auto header = std::make_unique<TrackHeaderComponent>();
            header->onSelect = [this, i] { if (onSelect) onSelect (i); };
            header->onDeleteClicked = [this, i] { if (onDeleteRequested) onDeleteRequested (i); };
            header->onChanged = [this] { if (onChanged) onChanged(); };
            header->onWillChangeStructure = [this] { if (onWillChangeStructure) onWillChangeStructure(); };
            header->onInstrumentChanged = [this] { if (onInstrumentChanged) onInstrumentChanged(); };
            header->setBounds (0, i * TimelineView::trackHeight,
                               preferredWidth, TimelineView::trackHeight);
            container.addAndMakeVisible (*header);
            items.push_back (std::move (header));
        }
    }

    container.setSize (preferredWidth,
                       juce::jmax (1, (int) items.size() * TimelineView::trackHeight));
    refreshBindings();
}

void TrackHeadersView::refreshValues()
{
    refreshBindings();
}

void TrackHeadersView::updateMeters()
{
    for (auto& item : items)
        item->updateMeter();
}

void TrackHeadersView::setSelectedTrack (int index)
{
    selectedTrack = index;
    refreshBindings();
}

void TrackHeadersView::refreshBindings()
{
    if (project == nullptr)
        return;
    for (int i = 0; i < (int) items.size() && i < (int) project->tracks.size(); ++i)
        items[(size_t) i]->bind (&project->tracks[(size_t) i], i == selectedTrack);
}

void TrackHeadersView::setViewY (int y)
{
    container.setTopLeftPosition (0, -y);
}

void TrackHeadersView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (onWheel)
        onWheel (wheel.deltaY);
}
