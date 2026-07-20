#include "TrackHeadersView.h"

#include "TimelineView.h"
#include "../shared/GmInstruments.h"

// ---- TrackHeaderComponent -----------------------------------------------

TrackHeaderComponent::TrackHeaderComponent()
{
    addAndMakeVisible (nameLabel);
    addAndMakeVisible (deleteButton);
    addAndMakeVisible (muteButton);
    addAndMakeVisible (soloButton);
    addAndMakeVisible (volumeSlider);
    addChildComponent (instrumentBox); // MIDIトラックのみ bind() で表示

    nameLabel.setEditable (false, true, false); // ダブルクリックでリネーム
    nameLabel.onTextChange = [this]
    {
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

    deleteButton.onClick = [this]
    {
        if (onDeleteClicked)
            onDeleteClicked();
    };

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    muteButton.onClick = [this]
    {
        if (track != nullptr)
            track->params->mute.store (muteButton.getToggleState());
        if (onChanged)
            onChanged();
    };

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::goldenrod);
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
    for (auto* c : std::initializer_list<juce::Component*> { &deleteButton, &muteButton, &soloButton,
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

void TrackHeaderComponent::paint (juce::Graphics& g)
{
    g.fillAll (selected ? juce::Colour (0xff33333c) : juce::Colour (0xff26262a));
    if (selected)
    {
        g.setColour (juce::Colour (0xff4a6ea9));
        g.fillRect (0, 0, 3, getHeight());
    }
    g.setColour (juce::Colour (0xff333338));
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void TrackHeaderComponent::resized()
{
    auto area = getLocalBounds().reduced (8, 4);

    auto row1 = area.removeFromTop (24);
    deleteButton.setBounds (row1.removeFromRight (24));
    row1.removeFromRight (4);
    nameLabel.setBounds (row1);

    area.removeFromTop (4);
    auto row2 = area.removeFromTop (22);
    muteButton.setBounds (row2.removeFromLeft (26));
    row2.removeFromLeft (4);
    soloButton.setBounds (row2.removeFromLeft (26));
    row2.removeFromLeft (6);
    volumeSlider.setBounds (row2);

    area.removeFromTop (4);
    instrumentBox.setBounds (area.removeFromTop (22)); // MIDIトラックのみ表示（3行目）
}

void TrackHeaderComponent::mouseDown (const juce::MouseEvent&)
{
    if (onSelect)
        onSelect();
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
