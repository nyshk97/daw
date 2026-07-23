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

    // 減光表示の更新はここではしない（ソロは他トラックの表示にも効くため、
    // 全ヘッダ共通の30Hz pull（syncStateVisual）に一本化。遅延は最大1周期で知覚できない）
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

void TrackHeaderComponent::bind (Track* trackToBind, bool isSelected, bool anySolo)
{
    track = trackToBind;
    selected = isSelected;

    if (track != nullptr)
    {
        const bool mute = track->params->mute.load();
        const bool solo = track->params->solo.load();
        nameLabel.setText (track->name, juce::dontSendNotification);
        nameLabel.setFont (Fonts::forText (Fonts::body(), track->name));
        muteButton.setToggleState (mute, juce::dontSendNotification);
        soloButton.setToggleState (solo, juce::dontSendNotification);
        applyDimVisual (mute || (anySolo && ! solo));
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

void TrackHeaderComponent::updateMeter (StereoPeak incoming)
{
    if (track == nullptr)
        return;

    // 減衰・ピークホールドの計算は Meters::ChannelDisplay（ミキサー等のStereoMeterと同じ挙動）。
    // 描画はLookAndFeel側なので、値はプロパティで渡す
    bool changed = false;
    static const juce::Identifier levelProps[2] { "meterL", "meterR" };
    static const juce::Identifier holdProps[2] { "holdL", "holdR" };
    for (size_t ch = 0; ch < 2; ++ch)
    {
        if (! meterDisplay[ch].step (incoming[ch]))
            continue;
        volumeSlider.getProperties().set (levelProps[ch], (double) meterDisplay[ch].level);
        volumeSlider.getProperties().set (holdProps[ch], (double) meterDisplay[ch].hold);
        changed = true;
    }
    if (changed) // 定常値（一定音量の持続音等）では再描画しない
        volumeSlider.repaint();
}

void TrackHeaderComponent::applyDimVisual (bool dimmed)
{
    dimmedVisual = dimmed;
    const float alpha = dimmed ? 0.35f : 1.0f;
    nameLabel.setAlpha (alpha);
    volumeSlider.setAlpha (alpha);
    instrumentBox.setAlpha (alpha);
    repaint(); // 種別アイコン（paintで描く）の減光
}

void TrackHeaderComponent::syncStateVisual (bool anySolo)
{
    if (track == nullptr)
        return;
    const bool mute = track->params->mute.load();
    const bool solo = track->params->solo.load();
    if (muteButton.getToggleState() != mute)
        muteButton.setToggleState (mute, juce::dontSendNotification);
    if (soloButton.getToggleState() != solo)
        soloButton.setToggleState (solo, juce::dontSendNotification);

    // 減光＝聞こえないトラック（可聴判定は再生エンジンと同じ式）
    const bool dimmed = mute || (anySolo && ! solo);
    if (dimmed != dimmedVisual)
        applyDimVisual (dimmed);
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
        g.setColour (juce::Colours::white.withAlpha (dimmedVisual ? 0.25f : 0.65f));
        if (track->type == TrackType::midi)
            TrackIcons::drawKeyboard (g, typeIconArea());
        else
            TrackIcons::drawWaveform (g, typeIconArea());
    }
}

juce::Rectangle<float> TrackHeaderComponent::typeIconArea() const
{
    auto row1 = getLocalBounds().reduced (10, 8).removeFromTop (24);
    return juce::Rectangle<float> (16.0f, 16.0f)
               .withCentre (row1.removeFromLeft (iconSlotWidth).toFloat().getCentre());
}

void TrackHeaderComponent::resized()
{
    auto area = getLocalBounds().reduced (10, 8);

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

void TrackHeaderComponent::mouseDrag (const juce::MouseEvent& e)
{
    // 並び替えドラッグはヘッダ背景・種別アイコン領域から開始したときだけ。
    // nameLabelから転送されたイベント（addMouseListener経由）は originalComponent で除外し、
    // M/S・スライダー・楽器の上は子コンポーネントがイベントを取るのでそもそも届かない
    if (e.originalComponent != this || e.mods.isPopupMenu())
        return;
    if (! reorderDragging)
    {
        if (e.getDistanceFromDragStart() < 5)
            return;
        if (canReorder != nullptr && ! canReorder())
            return;
        reorderDragging = true;
    }
    if (onReorderDrag != nullptr && getParentComponent() != nullptr)
        onReorderDrag (e.getEventRelativeTo (getParentComponent()).y);
}

void TrackHeaderComponent::mouseUp (const juce::MouseEvent& e)
{
    if (! reorderDragging)
        return;
    reorderDragging = false;
    if (onReorderDrop != nullptr && getParentComponent() != nullptr)
        onReorderDrop (e.getEventRelativeTo (getParentComponent()).y);
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
            header->canReorder = [this] { return canReorder == nullptr || canReorder(); };
            header->onReorderDrag = [this] (int y) { updateReorderIndicator (y); };
            header->onReorderDrop = [this, i] (int y) { finishReorder (i, y); };
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

void TrackHeadersView::updateMeters (const std::vector<StereoPeak>& peaks)
{
    const bool anySolo = anySoloActive();
    for (int i = 0; i < (int) items.size(); ++i)
    {
        items[(size_t) i]->updateMeter (i < (int) peaks.size() ? peaks[(size_t) i]
                                                               : StereoPeak { 0.0f, 0.0f });
        items[(size_t) i]->syncStateVisual (anySolo);
    }
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
    const bool anySolo = anySoloActive();
    for (int i = 0; i < (int) items.size() && i < (int) project->tracks.size(); ++i)
        items[(size_t) i]->bind (&project->tracks[(size_t) i], i == selectedTrack, anySolo);
}

bool TrackHeadersView::anySoloActive() const
{
    if (project == nullptr)
        return false;
    for (auto& t : project->tracks)
        if (t.params->solo.load())
            return true;
    return false;
}

void TrackHeadersView::setViewY (int y)
{
    container.setTopLeftPosition (0, -y);
}

// ---- ドラッグ並び替え ----

int TrackHeadersView::gapForY (int containerY) const
{
    // ヘッダ境界（隙間）のうちYに最も近いもの。0 = 先頭の前、items.size() = 末尾の後
    return juce::jlimit (0, (int) items.size(),
                         juce::roundToInt ((float) containerY / (float) TimelineView::trackHeight));
}

void TrackHeadersView::updateReorderIndicator (int containerY)
{
    const int gap = gapForY (containerY);
    if (gap == reorderGap)
        return;
    reorderGap = gap;
    repaint();
}

void TrackHeadersView::finishReorder (int from, int containerY)
{
    const int gap = gapForY (containerY);
    reorderGap = -1;
    repaint();
    if (gap == from || gap == from + 1) // 自分の前後の隙間 = 順序が変わらない
        return;
    if (onReorderRequested)
        onReorderRequested (from, gap);
}

void TrackHeadersView::paintOverChildren (juce::Graphics& g)
{
    if (reorderGap < 0)
        return;
    // 挿入位置インジケータ（ヘッダ境界の横線）。containerのスクロール位置を足してビュー座標へ
    const int y = container.getY() + reorderGap * TimelineView::trackHeight;
    g.setColour (Theme::accent);
    g.fillRect (0, juce::jlimit (0, getHeight() - 3, y - 1), getWidth(), 3);
}

void TrackHeadersView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (onWheel)
        onWheel (wheel.deltaY);
}
