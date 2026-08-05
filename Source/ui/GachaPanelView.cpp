#include "GachaPanelView.h"

#include "../shared/ReferenceReport.h"
#include "../shared/ReferenceTools.h"
#include "AppLookAndFeel.h"
#include "Fonts.h"
#include "Theme.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }
}

// 選択行の右端に出すチップ。普段は「仮配置中」の状態表示、ホバーすると「残す」ボタンに変わる
// （状態とアクションを同じ場所に住まわせる。上段に独立の「残す」ボタンは置かない）。
// 行そのものは透過させたまま（選択・候補クリックは ListBox 側が担う）チップだけがクリックを受ける
// — AudioFileBrowserView::RowIconComponent と同じ構え
class GachaPanelView::KeepChipRow final : public juce::Component
{
public:
    KeepChipRow()
    {
        setInterceptsMouseClicks (false, true); // 自分は透過、チップだけ受ける
        addChildComponent (chip);
    }

    void update (bool showChip, std::function<void()> onKeepClick)
    {
        chip.onKeepClick = std::move (onKeepClick);
        chip.setVisible (showChip);
    }

    void resized() override
    {
        chip.setBounds (getWidth() - chipWidth - 8, getHeight() / 2 - chipHeight / 2,
                        chipWidth, chipHeight);
    }

private:
    static constexpr int chipWidth = 64;
    static constexpr int chipHeight = 18;

    struct Chip final : public juce::Component,
                        public juce::SettableTooltipClient
    {
        Chip()
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTooltip (jp (u8"この候補をプロジェクトに残す（確定）"));
        }

        void mouseEnter (const juce::MouseEvent&) override { hovered = true; repaint(); }
        void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }
        void mouseDown (const juce::MouseEvent&) override
        {
            if (onKeepClick != nullptr)
                onKeepClick();
        }

        void paint (juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat();
            g.setColour (hovered ? Theme::playGreen.withAlpha (0.9f) : Theme::accent);
            g.fillRoundedRectangle (bounds, 4.0f);
            g.setColour (juce::Colours::white);
            g.setFont (Fonts::small());
            g.drawText (hovered ? jp (u8"残す") : jp (u8"仮配置中"), getLocalBounds(),
                        juce::Justification::centred);
        }

        std::function<void()> onKeepClick;
        bool hovered = false;
    };

    Chip chip;
};

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

    addAndMakeVisible (alignButton);
    alignButton.setButtonText (jp (u8"原曲を頭出し"));
    alignButton.onClick = [this]
    {
        if (onAlignReference)
            onAlignReference();
    };

    addAndMakeVisible (rollButton);
    rollButton.setButtonText (jp (u8"🎲 振り直す"));
    rollButton.onClick = [this]
    {
        if (onRoll)
            onRoll();
    };

    // レポート（カード行の下）。report.md 無し=書く／有り=開く（表示中はON色・再押下で閉じる）。
    // 分岐の判断は実行側 — report の有無とウィンドウの表示対象を知っているのは MainComponent
    addAndMakeVisible (reportButton);
    reportButton.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    reportButton.onClick = [this]
    {
        if (onReportAction)
            onReportAction();
    };
    reportButton.onRightClick = [this]
    {
        // 「書き直す」は report がある（=ボタンが「開く」の）ときだけ意味を持つ。
        // 生成中・report無しでは出さない
        const auto folder = selectedCardFolder();
        const auto generating = reportGeneratingFolder != nullptr ? reportGeneratingFolder()
                                                                  : juce::File();
        if (folder == juce::File() || generating != juce::File()
            || ! ReferenceReport::exists (folder) || ! ReferenceTools::reportAvailable())
            return;
        juce::PopupMenu menu;
        menu.addItem (1, jp (u8"レポートを書き直す（10〜15分・AI）"));
        juce::Component::SafePointer<GachaPanelView> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&reportButton),
                            [safe] (int result)
                            {
                                if (safe != nullptr && result == 1 && safe->onRewriteReport)
                                    safe->onRewriteReport();
                            });
    };

    // 生成中の経過行（ボタンと排他表示。テキストは MainComponent のタイマーが流し込む）
    addChildComponent (reportProgressLabel);
    reportProgressLabel.setFont (Fonts::small());
    reportProgressLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
    addChildComponent (reportCancelButton);
    reportCancelButton.setButtonText (jp (u8"キャンセル"));
    reportCancelButton.onClick = [this]
    {
        if (onCancelReport)
            onCancelReport();
    };

    addAndMakeVisible (lockCaption);
    lockCaption.setText (juce::String::fromUTF8 (u8"ロック"), juce::dontSendNotification);
    lockCaption.setFont (Fonts::small());
    lockCaption.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.4f));

    addAndMakeVisible (listBox);
    listBox.setRowHeight (36); // 3レーン×16分のドット譜が入る高さ
    listBox.setMultipleSelectionEnabled (false);
    listBox.setColour (juce::ListBox::backgroundColourId, Theme::timelineBg);
    listBox.setColour (juce::ListBox::outlineColourId, juce::Colours::white.withAlpha (0.07f));
    listBox.setOutlineThickness (1);

    addAndMakeVisible (infoLabel);
    infoLabel.setFont (Fonts::small());
    infoLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.45f));
    infoLabel.setJustificationType (juce::Justification::topLeft);

    // 下部の1行ガイド（次にやることを常に言う。ロックが押せない理由もここに出す）
    addAndMakeVisible (statusLabel);
    statusLabel.setFont (Fonts::small());
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.4f));
    statusLabel.setJustificationType (juce::Justification::topLeft);

    // Space（再生/停止）を奪わせない
    for (auto* c : std::initializer_list<juce::Component*> { &cardBox, &kickLock, &snareLock,
                                                            &hatLock, &rollButton, &alignButton,
                                                            &reportButton, &reportCancelButton,
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
        // ONにした瞬間の選択中候補から seed を確保する。未選択なら黙って戻さず理由を言う
        //（disabled で無反応にすると「押しても効かない」に見える）
        const int row = selectedCandidate();
        const auto laneName = &button == &kickLock ? "kick" : &button == &snareLock ? "snare" : "hat";
        if (row >= 0 && row < (int) items.size())
        {
            stored = &button == &kickLock    ? items[(size_t) row].kickSeed
                     : &button == &snareLock ? items[(size_t) row].snareSeed
                                             : items[(size_t) row].hatSeed;
        }
        else
        {
            button.setToggleState (false, juce::dontSendNotification);
            statusLabel.setText (jp (u8"ロックは候補を選んでから — どの候補の ") + laneName
                                     + jp (u8" を固定するかが決まらないためです"),
                                 juce::dontSendNotification);
            return; // updateControls で案内を上書きしない
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
    listBox.updateContent(); // チップ（仮配置中/残す）は選択行にだけ出す
    updateControls();        // ロックトグルの有効/無効は候補選択の有無で変わる
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
    // ロック中はボタン自身が対象を名乗る（「🎲 snare・hatを振り直す」— 押す前に効果が読める）。
    // 最長でも約170pt（パネル最小幅240ptのボタン212ptに収まる）
    juce::StringArray freeLanes;
    if (lockedKick.isEmpty())
        freeLanes.add ("kick");
    if (lockedSnare.isEmpty())
        freeLanes.add ("snare");
    if (lockedHat.isEmpty())
        freeLanes.add ("hat");
    rollButton.setButtonText (freeLanes.size() == 3 || freeLanes.isEmpty()
                                  ? jp (u8"🎲 振り直す")
                                  : jp (u8"🎲 ") + freeLanes.joinIntoString (jp (u8"・"))
                                        + jp (u8"を振り直す"));
    // カード選択はカードが在れば常に有効（ガチャツール drums.py の有無に相乗りしない —
    // レポートの閲覧/生成は独立判定なので、ガチャだけ欠けた環境でもカードは選べる必要がある）
    cardBox.setEnabled (true);

    // 頭出し: 可否と理由は実行側（MainComponent）が判定する（source.json・groove/gates・
    // クリップ同定・録音状態。ファイルI/Oを含むのでユーザー操作起点のここでだけ評価する）
    juce::String alignReason;
    if (! toolsAvailable || ! hasCard)
        alignReason = jp (u8"カードを選択してください");
    else if (alignUnavailableReason != nullptr)
        alignReason = alignUnavailableReason();
    alignButton.setEnabled (alignReason.isEmpty());
    alignButton.setTooltip (alignReason.isEmpty()
                                ? jp (u8"原曲クリップをBPM設定＋小節グリッドに合わせる（生成ドラムと重ねて聴ける）")
                                : alignReason);

    // レポート: report.md 無し=書く（AI・10〜15分）／有り=開く。選択中カードを生成中なら
    // ボタンの代わりに経過行を出す。存在チェックはファイルI/Oを含むが、updateControls は
    // ユーザー操作起点でしか呼ばれないので許容（alignと同じ扱い）
    {
        const auto cardFolder = selectedCardFolder();
        const bool hasReport = hasCard && ReferenceReport::exists (cardFolder);
        const auto generating = reportGeneratingFolder != nullptr ? reportGeneratingFolder()
                                                                  : juce::File();
        const bool generatingThis = generating != juce::File() && generating == cardFolder;

        reportButton.setVisible (! generatingThis);
        reportProgressLabel.setVisible (generatingThis);
        reportCancelButton.setVisible (generatingThis);

        if (hasReport)
        {
            reportButton.setButtonText (jp (u8"📄 レポートを開く"));
            reportButton.setEnabled (ReferenceTools::renderAvailable());
            reportButton.setTooltip (ReferenceTools::renderAvailable()
                                         ? jp (u8"分析レポートを別ウィンドウで読む（右クリックで書き直し）")
                                         : ReferenceTools::unavailableReason());
        }
        else
        {
            reportButton.setButtonText (jp (u8"📄 レポートを書く（10〜15分・AI）"));
            const bool canGenerate = hasCard && ReferenceTools::reportAvailable()
                                     && generating == juce::File();
            reportButton.setEnabled (canGenerate);
            reportButton.setTooltip (! hasCard ? jp (u8"カードを選択してください")
                                     : generating != juce::File()
                                         ? jp (u8"別のレポートを生成中です（同時に1件まで）")
                                     : ! ReferenceTools::reportAvailable()
                                         ? ReferenceTools::unavailableReason()
                                         : jp (u8"分析データから Claude Code がレポートを書く"
                                               u8"（Claude Code の利用枠を消費します）"));
        }
        const auto showing = reportWindowFolder != nullptr ? reportWindowFolder() : juce::File();
        reportButton.setToggleState (hasCard && showing != juce::File() && showing == cardFolder,
                                     juce::dontSendNotification);
    }

    // ロックは候補があれば常に押せる（未選択で押したときは handleLockToggle が理由を案内する）。
    // ON状態は 🔒 を付けて「固定されている」ことを名乗る
    const bool lanesUsable = toolsAvailable && hasCard && ! items.empty();
    kickLock.setEnabled (lanesUsable || kickLock.getToggleState());
    snareLock.setEnabled (lanesUsable || snareLock.getToggleState());
    hatLock.setEnabled (lanesUsable || hatLock.getToggleState());
    kickLock.setButtonText (lockedKick.isNotEmpty() ? jp (u8"🔒 kick") : juce::String ("kick"));
    snareLock.setButtonText (lockedSnare.isNotEmpty() ? jp (u8"🔒 snare") : juce::String ("snare"));
    hatLock.setButtonText (lockedHat.isNotEmpty() ? jp (u8"🔒 hat") : juce::String ("hat"));

    // 下部の1行ガイド（いまの状態で次にやることを言う）
    const int sel = selectedCandidate();
    if (! items.empty())
    {
        if (allLocked)
            statusLabel.setText (jp (u8"全レーンをロック中 — 振り直しても同じ候補しか出ません"),
                                 juce::dontSendNotification);
        else if (previewActive && sel >= 0)
            statusLabel.setText (jp (u8"候補") + juce::String (sel + 1)
                                     + jp (u8"を仮配置中 — 「仮配置中」をクリックで残す（確定）・"
                                            u8"他の候補クリックで差し替え"),
                                 juce::dontSendNotification);
        else
            statusLabel.setText (jp (u8"① 候補をクリックで仮配置 → ② 気に入ったレーンを🔒 → ③ 振り直す"),
                                 juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText ({}, juce::dontSendNotification);
    }

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

void GachaPanelView::showStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
}

void GachaPanelView::setKeepEnabled (bool enabled)
{
    previewActive = enabled;
    listBox.updateContent(); // 選択行のチップ（仮配置中/残す）を付け替える
    listBox.repaint();
    updateControls();
}

// 候補行 = 番号＋K/S/H のドット譜（1小節・16分×16）＋仮配置バッジ。
// hex の seed は表示しない（人が比べたいのはパターンそのもの。seed はロックの内部表現に徹する）
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

    // レーンタグ（K/S/H）
    const bool laneLocked[GachaSession::patternLanes] = {
        lockedKick.isNotEmpty(), lockedSnare.isNotEmpty(), lockedHat.isNotEmpty() };
    static const char* laneTags[GachaSession::patternLanes] = { "K", "S", "H" };
    const float laneHeight = 7.0f;
    const float laneGap = 3.0f;
    const float top = (height - (laneHeight * 3 + laneGap * 2)) / 2.0f;
    g.setFont (Fonts::mono (7.0f));
    for (int lane = 0; lane < GachaSession::patternLanes; ++lane)
    {
        const float y = top + lane * (laneHeight + laneGap);
        g.setColour (juce::Colours::white.withAlpha (0.3f));
        g.drawText (laneTags[lane], juce::Rectangle<float> (30.0f, y, 10.0f, laneHeight),
                    juce::Justification::centredLeft);
    }

    // ドット譜。骨格=濃・装飾（ゴースト）=淡。ロック中レーンはアクセント色で「固定」を示す
    if (candidate.hasPattern)
    {
        const float cellGap = 1.5f;
        const float gridX = 44.0f;
        const float gridRight = (float) width - 66.0f; // 右端はバッジ分を空ける
        const float cellW = juce::jmax (3.0f, (gridRight - gridX) / 16.0f - cellGap);
        for (int lane = 0; lane < GachaSession::patternLanes; ++lane)
        {
            const float y = top + lane * (laneHeight + laneGap);
            for (int slot = 0; slot < GachaSession::patternSlots; ++slot)
            {
                const int value = candidate.pattern[(size_t) lane][(size_t) slot];
                const auto base = laneLocked[lane] ? Theme::accent : juce::Colours::white;
                g.setColour (value == 2   ? base.withAlpha (0.80f)
                             : value == 1 ? base.withAlpha (0.34f)
                                          : juce::Colours::white.withAlpha (0.06f));
                g.fillRoundedRectangle (gridX + slot * (cellW + cellGap), y, cellW, laneHeight, 1.5f);
            }
        }
    }

    // 「仮配置中」バッジは KeepChipRow（refreshComponentForRow）が重ねる（ホバーで「残す」になるため）

    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawHorizontalLine (height - 1, 8.0f, (float) width);
}

juce::Component* GachaPanelView::refreshComponentForRow (int row, bool selected,
                                                         juce::Component* existing)
{
    std::unique_ptr<juce::Component> owned (existing);
    if (! juce::isPositiveAndBelow (row, getNumRows()))
        return nullptr;

    auto* chipRow = dynamic_cast<KeepChipRow*> (owned.get());
    if (chipRow == nullptr)
    {
        chipRow = new KeepChipRow();
        owned.reset (chipRow);
    }
    // チップは「仮配置中の選択行」にだけ出す。クリックは onKeep（確定）へ
    chipRow->update (selected && previewActive,
                     [this] { if (onKeep) onKeep(); });
    return owned.release();
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

    auto cardRow = area.removeFromTop (26);
    alignButton.setBounds (cardRow.removeFromRight (96));
    cardRow.removeFromRight (6);
    cardBox.setBounds (cardRow);
    area.removeFromTop (6);

    auto reportRow = area.removeFromTop (26);
    reportButton.setBounds (reportRow);
    // 生成中はボタンの代わりに経過行＋キャンセル（可視切り替えは updateControls）
    reportCancelButton.setBounds (reportRow.removeFromRight (72));
    reportRow.removeFromRight (6);
    reportProgressLabel.setBounds (reportRow);
    area.removeFromTop (8);

    auto lockRow = area.removeFromTop (24);
    lockCaption.setBounds (lockRow.removeFromLeft (40));
    const int lockWidth = (lockRow.getWidth() - 2 * 6) / 3;
    kickLock.setBounds (lockRow.removeFromLeft (lockWidth));
    lockRow.removeFromLeft (6);
    snareLock.setBounds (lockRow.removeFromLeft (lockWidth));
    lockRow.removeFromLeft (6);
    hatLock.setBounds (lockRow);
    area.removeFromTop (8);

    rollButton.setBounds (area.removeFromTop (28));
    area.removeFromTop (10);

    if (infoLabel.isVisible() && items.empty())
    {
        infoLabel.setBounds (area.removeFromTop (72));
        area.removeFromTop (4);
    }
    // 下部の1行ガイド（2行ぶんの高さ。候補がないときは空文字で場所だけ確保）
    statusLabel.setBounds (area.removeFromBottom (32));
    area.removeFromBottom (4);
    listBox.setBounds (area);
}
