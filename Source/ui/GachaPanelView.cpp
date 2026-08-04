#include "GachaPanelView.h"

#include "../shared/ReferenceTools.h"
#include "AppLookAndFeel.h"
#include "Fonts.h"
#include "Theme.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }
}

GachaPanelView::GachaPanelView()
{
    addAndMakeVisible (cardBox);
    cardBox.setTextWhenNoChoicesAvailable (jp (u8"カードがありません"));
    cardBox.setTextWhenNothingSelected (jp (u8"カードを選択…"));
    cardBox.onChange = [this]
    {
        // 前カードの候補・ロックはこのカードでは意味を持たない（存在しない .mid を開く事故になる）
        setCandidates ({});
        clearLocks();
        updateControls();
        if (onCardChanged)
            onCardChanged();
    };

    // レーンロック（M/S と同じフラット角丸トグル）。ONにした瞬間に選択中候補のレーン seed を
    // 確保する（選択が外れてもONの間はその seed が --lock に渡り続ける）
    for (auto* b : { &kickLock, &snareLock, &hatLock })
    {
        addAndMakeVisible (*b);
        b->setClickingTogglesState (true);
        b->getProperties().set ("flatButton", true);
        b->setColour (juce::TextButton::buttonColourId, Theme::controlBg);
        b->setColour (juce::TextButton::buttonOnColourId, Theme::accent);
        b->setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.55f));
        b->setColour (juce::TextButton::textColourOnId, Theme::controlTextOn);
    }
    kickLock.onClick = [this] { handleLockToggle (kickLock, lockedKick); };
    snareLock.onClick = [this] { handleLockToggle (snareLock, lockedSnare); };
    hatLock.onClick = [this] { handleLockToggle (hatLock, lockedHat); };

    addAndMakeVisible (rollButton);
    rollButton.setButtonText (jp (u8"振り直す"));
    rollButton.onClick = [this]
    {
        if (onRoll)
            onRoll();
    };

    addAndMakeVisible (keepButton);
    keepButton.setButtonText (jp (u8"残す"));
    keepButton.setEnabled (false); // 候補を仮配置してから押せる
    keepButton.onClick = [this]
    {
        if (onKeep)
            onKeep();
    };

    addAndMakeVisible (listBox);
    listBox.setRowHeight (26);
    listBox.setMultipleSelectionEnabled (false);
    listBox.setColour (juce::ListBox::backgroundColourId, Theme::timelineBg);
    listBox.setColour (juce::ListBox::outlineColourId, juce::Colours::white.withAlpha (0.07f));
    listBox.setOutlineThickness (1);

    addAndMakeVisible (infoLabel);
    infoLabel.setFont (Fonts::small());
    infoLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.45f));
    infoLabel.setJustificationType (juce::Justification::topLeft);

    // Space（再生/停止）を奪わせない
    for (auto* c : std::initializer_list<juce::Component*> { &cardBox, &kickLock, &snareLock,
                                                            &hatLock, &rollButton, &keepButton,
                                                            &listBox })
    {
        c->setWantsKeyboardFocus (false);
        c->setMouseClickGrabsKeyboardFocus (false);
    }

    refreshAvailability();
}

void GachaPanelView::setProject (Project* projectToUse)
{
    project = projectToUse;
    items.clear();
    listBox.updateContent();
    refreshCards();
}

void GachaPanelView::refreshAvailability()
{
    toolsAvailable = ReferenceTools::gachaAvailable();
    updateControls();
}

void GachaPanelView::refreshCards()
{
    // 選択の維持: フォルダのフルパスで照合する（列挙し直しでIDが振り直されるため）
    const auto previous = selectedCardFolder();

    cardFolders.clear();
    cardBox.clear (juce::dontSendNotification);
    if (project != nullptr)
    {
        const auto references = project->directory.getChildFile ("references");
        auto folders = references.findChildFiles (juce::File::findDirectories, false);
        std::sort (folders.begin(), folders.end(),
                   [] (const juce::File& a, const juce::File& b)
                   { return a.getFileName().compareNatural (b.getFileName()) < 0; });
        for (const auto& folder : folders)
        {
            if (! folder.getChildFile ("card.json").existsAsFile())
                continue; // カード無し（分析失敗・ゲート落ち）のフォルダは出さない
            cardFolders.push_back (folder);
            cardBox.addItem (folder.getFileName(), (int) cardFolders.size());
            if (folder == previous)
                cardBox.setSelectedId ((int) cardFolders.size(), juce::dontSendNotification);
        }
    }
    // カードが1枚だけなら選んでおく（選択操作を省く）
    if (cardFolders.size() == 1 && cardBox.getSelectedId() == 0)
        cardBox.setSelectedId (1, juce::dontSendNotification);
    updateControls();
    resized(); // 案内文の出入りで一覧の上端が変わる
}

juce::File GachaPanelView::selectedCardFolder() const
{
    const int index = cardBox.getSelectedId() - 1;
    return index >= 0 && index < (int) cardFolders.size() ? cardFolders[(size_t) index]
                                                          : juce::File();
}

void GachaPanelView::setCandidates (std::vector<GachaSession::Candidate> list)
{
    items = std::move (list);
    listBox.deselectAllRows();
    listBox.updateContent();
    listBox.repaint();
    updateControls();
    resized(); // 案内文の出入りで一覧の上端が変わる
}

void GachaPanelView::handleLockToggle (juce::TextButton& button, juce::String& stored)
{
    if (button.getToggleState())
    {
        // ONにした瞬間の選択中候補から seed を確保する。未選択ならONにできない
        //（何をロックするのか決まらないため。ボタン自体も未選択時はdisabledにしている）
        const int row = selectedCandidate();
        if (row >= 0 && row < (int) items.size())
        {
            stored = &button == &kickLock    ? items[(size_t) row].kickSeed
                     : &button == &snareLock ? items[(size_t) row].snareSeed
                                             : items[(size_t) row].hatSeed;
        }
        else
        {
            button.setToggleState (false, juce::dontSendNotification);
        }
    }
    else
    {
        stored.clear();
    }
    updateControls();
}

void GachaPanelView::clearLocks()
{
    lockedKick.clear();
    lockedSnare.clear();
    lockedHat.clear();
    for (auto* b : { &kickLock, &snareLock, &hatLock })
        b->setToggleState (false, juce::dontSendNotification);
}

void GachaPanelView::selectedRowsChanged (int)
{
    updateControls(); // ロックトグルの有効/無効は候補選択の有無で変わる
}

void GachaPanelView::updateControls()
{
    const bool hasCard = selectedCardFolder() != juce::File();
    const bool allLocked = lockedKick.isNotEmpty() && lockedSnare.isNotEmpty()
                           && lockedHat.isNotEmpty();
    // 全レーンロック時は振れない（CLI は重複排除して1件しか生成しない — 8件前提の一覧と矛盾する）
    rollButton.setEnabled (toolsAvailable && hasCard && ! allLocked);
    rollButton.setTooltip (allLocked ? jp (u8"全レーンをロックすると振り直せません（同じ候補しか出ないため）")
                                     : juce::String());
    cardBox.setEnabled (toolsAvailable);
    // ロックをONにするには「どの候補の目を固定するか」が要る → 未選択時はOFF側だけ操作可
    const bool canLock = toolsAvailable && hasCard && selectedCandidate() >= 0;
    kickLock.setEnabled (canLock || kickLock.getToggleState());
    snareLock.setEnabled (canLock || snareLock.getToggleState());
    hatLock.setEnabled (canLock || hatLock.getToggleState());

    if (! toolsAvailable)
        infoLabel.setText (ReferenceTools::unavailableReason(), juce::dontSendNotification);
    else if (! hasCard)
        infoLabel.setText (jp (u8"カードがありません。タイムラインのオーディオリージョンを"
                               u8"右クリック →「リファレンスとして分析…」で作成できます。"),
                           juce::dontSendNotification);
    else if (items.empty())
        infoLabel.setText (jp (u8"「振り直す」で候補を8件生成します。候補をクリックすると"
                               u8"再生ヘッドの小節頭に仮配置され、曲と一緒に鳴らせます。"),
                           juce::dontSendNotification);
    infoLabel.setVisible (! toolsAvailable || ! hasCard || items.empty());
    listBox.setVisible (! infoLabel.isVisible() || ! items.empty());
}

void GachaPanelView::paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                                       bool selected)
{
    if (row < 0 || row >= (int) items.size())
        return;
    if (selected)
    {
        g.setColour (Theme::accent.withAlpha (0.25f));
        g.fillRect (0, 0, width, height);
    }
    const auto& candidate = items[(size_t) row];
    g.setColour (juce::Colours::white.withAlpha (selected ? 0.94f : 0.72f));
    g.setFont (Fonts::body());
    g.drawText (juce::String (row + 1), 10, 0, 18, height, juce::Justification::centredLeft);
    // レーン seed（ロック対象を選ぶ手掛かり。ファイル名と同じ8桁hex）
    g.setFont (Fonts::mono (11.0f));
    g.setColour (juce::Colours::white.withAlpha (selected ? 0.80f : 0.55f));
    g.drawText ("k " + candidate.kickSeed + "  s " + candidate.snareSeed + "  h " + candidate.hatSeed,
                34, 0, width - 42, height, juce::Justification::centredLeft, true);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawHorizontalLine (height - 1, 8.0f, (float) width);
}

void GachaPanelView::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < (int) items.size() && onPick)
        onPick (row);
}

void GachaPanelView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::chooserPanelBg);
}

void GachaPanelView::resized()
{
    auto area = getLocalBounds().reduced (14, 10);

    cardBox.setBounds (area.removeFromTop (26));
    area.removeFromTop (8);

    auto lockRow = area.removeFromTop (24);
    const int lockWidth = (lockRow.getWidth() - 2 * 6) / 3;
    kickLock.setBounds (lockRow.removeFromLeft (lockWidth));
    lockRow.removeFromLeft (6);
    snareLock.setBounds (lockRow.removeFromLeft (lockWidth));
    lockRow.removeFromLeft (6);
    hatLock.setBounds (lockRow);
    area.removeFromTop (8);

    auto buttonRow = area.removeFromTop (28);
    rollButton.setBounds (buttonRow.removeFromLeft ((buttonRow.getWidth() - 8) / 2));
    buttonRow.removeFromLeft (8);
    keepButton.setBounds (buttonRow);
    area.removeFromTop (10);

    if (infoLabel.isVisible() && items.empty())
    {
        infoLabel.setBounds (area.removeFromTop (72));
        area.removeFromTop (4);
    }
    listBox.setBounds (area);
}
