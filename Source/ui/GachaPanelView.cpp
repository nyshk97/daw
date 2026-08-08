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

// 選択行の右端に出すチップ。普段は「仮配置中」の状態表示、ホバーすると「ビートを残す」ボタンに
// 変わる（状態とアクションを同じ場所に住まわせる。上段に独立の「残す」ボタンは置かない）。
// 「残す」は全パーツ一括確定なので、ラベルは行単位でなくビート全体を名乗る。
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
    static constexpr int chipWidth = 84; // 「ビートを残す」が収まる幅
    static constexpr int chipHeight = 18;

    struct Chip final : public juce::Component,
                        public juce::SettableTooltipClient
    {
        Chip()
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTooltip (jp (u8"仮配置中の全パーツをプロジェクトに残す（確定）"));
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
            g.drawText (hovered ? jp (u8"ビートを残す") : jp (u8"仮配置中"), getLocalBounds(),
                        juce::Justification::centred);
        }

        std::function<void()> onKeepClick;
        bool hovered = false;
    };

    Chip chip;
};

// Loops 行の右端の ✓（タイムラインに敷く）。行クリック＝試聴と役割を分離する（モック確定仕様）。
// 仮配置中の選択行には KeepChipRow と同じ「仮配置中/ビートを残す」チップも並べる
class GachaPanelView::LoopAdoptRow final : public juce::Component
{
public:
    LoopAdoptRow()
    {
        setInterceptsMouseClicks (false, true); // 行は透過（試聴クリックは ListBox 側が受ける）
        addAndMakeVisible (adopt);
        addChildComponent (keepChip);
    }

    void update (bool showKeepChip, std::function<void()> onAdoptClick,
                 std::function<void()> onKeepClick)
    {
        adopt.onAdoptClick = std::move (onAdoptClick);
        keepChip.onKeepClick = std::move (onKeepClick);
        keepChip.setVisible (showKeepChip);
    }

    void resized() override
    {
        adopt.setBounds (getWidth() - adoptSize - 8, getHeight() / 2 - adoptSize / 2,
                         adoptSize, adoptSize);
        keepChip.setBounds (adopt.getX() - 84 - 6, getHeight() / 2 - 9, 84, 18);
    }

private:
    static constexpr int adoptSize = 24;

    struct AdoptButton final : public juce::Component,
                               public juce::SettableTooltipClient
    {
        AdoptButton()
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTooltip (juce::String::fromUTF8 (u8"採用してタイムラインへ（キー/BPM の設定を確認します）"));
        }
        void mouseEnter (const juce::MouseEvent&) override { hovered = true; repaint(); }
        void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }
        void mouseDown (const juce::MouseEvent&) override
        {
            if (onAdoptClick != nullptr)
                onAdoptClick();
        }
        void paint (juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat();
            g.setColour (hovered ? Theme::accent.brighter (0.15f) : Theme::accent);
            g.fillRoundedRectangle (bounds, 5.0f);
            g.setColour (juce::Colours::white);
            g.setFont (Fonts::small());
            g.drawText (juce::CharPointer_UTF8 ("\xe2\x9c\x93"), getLocalBounds(),
                        juce::Justification::centred);
        }
        std::function<void()> onAdoptClick;
        bool hovered = false;
    };

    struct KeepChip final : public juce::Component,
                            public juce::SettableTooltipClient
    {
        KeepChip()
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTooltip (juce::String::fromUTF8 (u8"仮配置中の全パーツをプロジェクトに残す（確定）"));
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
            g.drawText (hovered ? jp (u8"ビートを残す") : jp (u8"仮配置中"), getLocalBounds(),
                        juce::Justification::centred);
        }
        std::function<void()> onKeepClick;
        bool hovered = false;
    };

    AdoptButton adopt;
    KeepChip keepChip;
};

GachaPanelView::GachaPanelView()
{
    // パーツタブ（ラジオ動作）。将来 Keys 等が +1 されるだけの構造
    for (auto* tab : { &drumsTab, &bassTab, &loopsTab })
    {
        addAndMakeVisible (*tab);
        tab->setClickingTogglesState (false);
        tab->getProperties().set ("flatButton", true);
        tab->setColour (juce::TextButton::buttonColourId, Theme::controlBg);
        tab->setColour (juce::TextButton::buttonOnColourId, Theme::accent);
        tab->setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.55f));
        tab->setColour (juce::TextButton::textColourOnId, Theme::controlTextOn);
    }
    drumsTab.onClick = [this] { switchPart (Part::drums); };
    bassTab.onClick = [this] { switchPart (Part::bass); };
    loopsTab.onClick = [this] { switchPart (Part::loops); };
    drumsTab.setToggleState (true, juce::dontSendNotification);

    // ---- アンカーカード（全タブ共通・タブ直下。可視切り替えは updateControls / resized）----
    addChildComponent (anchorCard);
    addChildComponent (anchorReleaseButton); // カードの上に重ねるので card より後に add（Z順）
    anchorReleaseButton.setTooltip (jp (u8"アンカーを解除する（クリップは残る。ベースガチャは"
                                        u8"カード駆動に戻る・⌘Zで戻せる）"));
    anchorReleaseButton.onClick = [this]
    {
        if (onAnchorRelease)
            onAnchorRelease();
    };
    addChildComponent (prevPageButton);
    addChildComponent (nextPageButton);
    addChildComponent (pageInfoLabel);
    prevPageButton.setButtonText (jp (u8"← 前の5本"));
    nextPageButton.setButtonText (jp (u8"次の5本 →"));
    pageInfoLabel.setJustificationType (juce::Justification::centred);
    pageInfoLabel.setFont (Fonts::small());
    pageInfoLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));
    prevPageButton.onClick = [this]
    {
        if (onLoopPageRequested && loopRecommendation.page > 1)
            onLoopPageRequested (loopRecommendation.page - 1);
    };
    nextPageButton.onClick = [this]
    {
        if (onLoopPageRequested)
            onLoopPageRequested (loopRecommendation.page + 1);
    };

    addAndMakeVisible (cardBox);
    cardBox.setTextWhenNoChoicesAvailable (jp (u8"カードがありません"));
    cardBox.setTextWhenNothingSelected (jp (u8"カードを選択…"));
    cardBox.onChange = [this]
    {
        auto& state = parts[(size_t) currentPart];
        // 新しい選択は ComboBox から解決する（selectedCardFolder() は state 側＝変更前の値を返す）
        const int index = cardBox.getSelectedId() - 1;
        const auto folder = index >= 0 && index < (int) cardFolders.size()
                                ? cardFolders[(size_t) index]
                                : juce::File();
        if (folder == state.cardFolder)
            return; // switchPart の復元などで発火した同値変更は無視
        state.cardFolder = folder;
        // 前カードの候補・ロックはこのカードでは意味を持たない（存在しない .mid を開く事故になる）
        if (currentPart == Part::loops)
            setLoopPage ({}); // Loops の候補は loopRecommendation 側（旧カードのおすすめを残さない）
        else
            setCandidates ({});
        clearLocks();
        updateControls();
        if (onCardChanged)
            onCardChanged (currentPart);
    };

    // レーンロック（M/S と同じフラット角丸トグル）。ONにした瞬間に選択中候補のレーン seed を
    // 確保する（選択が外れてもONの間はその seed が --lock に渡り続ける）
    for (int i = 0; i < 3; ++i)
    {
        auto& b = lockButtons[i];
        addAndMakeVisible (b);
        b.setClickingTogglesState (true);
        b.getProperties().set ("flatButton", true);
        b.setColour (juce::TextButton::buttonColourId, Theme::controlBg);
        b.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
        b.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.55f));
        b.setColour (juce::TextButton::textColourOnId, Theme::controlTextOn);
        b.onClick = [this, i] { handleLockToggle (i); };
    }

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
        menu.addItem (1, jp (u8"レポートを書き直す（約8分）"));
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
    listBox.setRowHeight (36); // 3レーン×16分のドット譜（bass は音高ストリップ）が入る高さ
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
    for (auto* c : std::initializer_list<juce::Component*> { &drumsTab, &bassTab, &cardBox,
                                                            &lockButtons[0], &lockButtons[1],
                                                            &lockButtons[2], &rollButton,
                                                            &alignButton, &reportButton,
                                                            &reportCancelButton, &listBox })
    {
        c->setWantsKeyboardFocus (false);
        c->setMouseClickGrabsKeyboardFocus (false);
    }

    refreshAvailability();
}

const char* GachaPanelView::laneName (int laneIndex) const
{
    static const char* drumLanes[3] = { "kick", "snare", "hat" };
    static const char* bassLanes[2] = { "prog", "rhythm" };
    return currentPart == Part::drums ? drumLanes[laneIndex] : bassLanes[laneIndex];
}

void GachaPanelView::setProject (Project* projectToUse)
{
    project = projectToUse;
    for (auto& state : parts)
    {
        state.items.clear();
        state.cardFolder = juce::File();
        for (auto& lock : state.locks)
            lock.clear();
        state.previewActive = false;
    }
    listBox.updateContent();
    refreshCards();
}

void GachaPanelView::refreshAvailability()
{
    toolsAvailable = ReferenceTools::gachaAvailable();
    bassToolAvailable = ReferenceTools::bassGachaAvailable();
    loopToolAvailable = ReferenceTools::loopGachaAvailable();
    updateControls();
}

void GachaPanelView::switchPart (Part part)
{
    if (part == currentPart)
        return;
    // Loops を離れるとき試聴を止める（同じ行の再クリック＝停止のトグルをそのまま使う）
    if (currentPart == Part::loops && auditioningRow >= 0 && onLoopAudition)
        onLoopAudition (auditioningRow);
    currentPart = part;
    drumsTab.setToggleState (part == Part::drums, juce::dontSendNotification);
    bassTab.setToggleState (part == Part::bass, juce::dontSendNotification);
    loopsTab.setToggleState (part == Part::loops, juce::dontSendNotification);

    // 状態はすべて parts[] にあるので、ウィジェットへ反映し直すだけ（退避は不要 —
    // cardBox.onChange / handleLockToggle が都度 parts[] を更新している）
    auto& state = parts[(size_t) part];
    applyCardSelection (state.cardFolder);
    listBox.deselectAllRows();
    listBox.updateContent();
    listBox.repaint();
    for (int i = 0; i < 3; ++i)
        lockButtons[i].setToggleState (state.locks[i].isNotEmpty(), juce::dontSendNotification);
    updateControls();
    resized(); // ロック行のボタン数・案内文の出入りが変わる
}

void GachaPanelView::applyCardSelection (const juce::File& folder)
{
    cardBox.setSelectedId (0, juce::dontSendNotification);
    for (size_t i = 0; i < cardFolders.size(); ++i)
        if (cardFolders[i] == folder)
            cardBox.setSelectedId ((int) i + 1, juce::dontSendNotification);
}

void GachaPanelView::refreshCards()
{
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
        }
    }
    // 列挙し直しで選択が消えたパーツは畳む（フォルダ削除・カード消滅）
    for (auto& state : parts)
        if (state.cardFolder != juce::File()
            && std::find (cardFolders.begin(), cardFolders.end(), state.cardFolder)
                   == cardFolders.end())
            state.cardFolder = juce::File();
    // カードが1枚だけなら未選択の**全パーツ**で選んでおく（選択操作を省く。表示中パーツ
    // だけに絞ると、他タブへ切り替えたとき「カードを選択…」に戻って見える。パーツ別参照が
    // 意味を持つのはカードが複数あるときだけなので、1枚なら迷わず埋めてよい）
    if (cardFolders.size() == 1)
        for (auto& state : parts)
            if (state.cardFolder == juce::File())
                state.cardFolder = cardFolders.front();
    applyCardSelection (parts[(size_t) currentPart].cardFolder);
    updateControls();
    resized(); // 案内文の出入りで一覧の上端が変わる
}

juce::File GachaPanelView::selectedCardFolder() const
{
    return selectedCardFolder (currentPart);
}

juce::File GachaPanelView::selectedCardFolder (Part part) const
{
    return parts[(size_t) part].cardFolder;
}

void GachaPanelView::setCandidates (std::vector<GachaSession::Candidate> list)
{
    parts[(size_t) currentPart].items = std::move (list);
    listBox.deselectAllRows();
    listBox.updateContent();
    listBox.repaint();
    updateControls();
    resized(); // 案内文の出入りで一覧の上端が変わる
}

void GachaPanelView::setLoopPage (GachaSession::LoopRecommendation page)
{
    loopRecommendation = std::move (page);
    auditioningRow = -1; // ページが変われば行の意味が変わる（試聴の停止は呼び出し側が行う）
    listBox.deselectAllRows();
    listBox.updateContent();
    listBox.repaint();
    updateControls();
    resized();
}

void GachaPanelView::setAuditioningRow (int row)
{
    auditioningRow = row;
    listBox.repaint();
}

void GachaPanelView::handleLockToggle (int laneIndex)
{
    auto& state = parts[(size_t) currentPart];
    auto& button = lockButtons[laneIndex];
    auto& stored = state.locks[laneIndex];
    if (button.getToggleState())
    {
        // ONにした瞬間の選択中候補から seed を確保する。未選択なら黙って戻さず理由を言う
        //（disabled で無反応にすると「押しても効かない」に見える）
        const int row = selectedCandidate();
        const auto& items = state.items;
        if (row >= 0 && row < (int) items.size())
        {
            const auto& candidate = items[(size_t) row];
            if (currentPart == Part::drums)
                stored = laneIndex == 0 ? candidate.kickSeed
                         : laneIndex == 1 ? candidate.snareSeed
                                          : candidate.hatSeed;
            else
                stored = laneIndex == 0 ? candidate.progSeed : candidate.rhythmSeed;
        }
        else
        {
            button.setToggleState (false, juce::dontSendNotification);
            statusLabel.setText (jp (u8"ロックは候補を選んでから — どの候補の ")
                                     + laneName (laneIndex)
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
    auto& state = parts[(size_t) currentPart];
    for (auto& lock : state.locks)
        lock.clear();
    for (auto& b : lockButtons)
        b.setToggleState (false, juce::dontSendNotification);
}

void GachaPanelView::selectedRowsChanged (int)
{
    listBox.updateContent(); // チップ（仮配置中/残す）は選択行にだけ出す
    updateControls();        // ロックトグルの有効/無効は候補選択の有無で変わる
}

void GachaPanelView::updateControls()
{
    auto& state = parts[(size_t) currentPart];
    const bool isDrums = currentPart == Part::drums;
    const bool isLoops = currentPart == Part::loops;
    const bool partToolAvailable = isDrums ? toolsAvailable
                                 : isLoops ? loopToolAvailable
                                           : bassToolAvailable;
    const bool hasCard = selectedCardFolder() != juce::File();
    const int lanes = numLanes();
    bool allLocked = lanes > 0; // loops はレーンを持たない（決定的ランキング）
    for (int i = 0; i < lanes; ++i)
        allLocked = allLocked && state.locks[i].isNotEmpty();

    bassTab.setEnabled (bassToolAvailable);
    bassTab.setTooltip (bassToolAvailable ? juce::String() : ReferenceTools::unavailableReason());
    // Loops タブは**常に開ける**（アンカーの解除 UI がこのタブにしか無い — ツールや index が
    // 無い別マシンでも、保存済みアンカーによるベース追従を解除できる必要がある）。
    // ツール可用性で無効になるのは「おすすめを出す」だけ
    loopsTab.setEnabled (true);
    loopsTab.setTooltip (loopToolAvailable ? juce::String()
                                           : ReferenceTools::loopGachaUnavailableReason());

    if (isLoops)
    {
        // おすすめは決定的なので「振り直す」でなく「出す」（同じ入力→同じ5本。探索はページング）
        rollButton.setButtonText (jp (u8"✨ おすすめを出す"));
        rollButton.setEnabled (partToolAvailable && hasCard);
        rollButton.setTooltip ({});
    }
    else
    {
        // 全レーンロック時は振れない（CLI は重複排除して1件しか生成しない — 8件前提の一覧と矛盾する）
        rollButton.setEnabled (partToolAvailable && hasCard && ! allLocked);
        rollButton.setTooltip (allLocked ? jp (u8"全レーンをロックすると振り直せません（同じ候補しか出ないため）")
                                         : juce::String());
        // ロック中はボタン自身が対象を名乗る（「🎲 snare・hatを振り直す」— 押す前に効果が読める）
        juce::StringArray freeLanes;
        for (int i = 0; i < lanes; ++i)
            if (state.locks[i].isEmpty())
                freeLanes.add (laneName (i));
        rollButton.setButtonText (freeLanes.size() == lanes || freeLanes.isEmpty()
                                      ? jp (u8"🎲 振り直す")
                                      : jp (u8"🎲 ") + freeLanes.joinIntoString (jp (u8"・"))
                                            + jp (u8"を振り直す"));
    }
    // カード選択はカードが在れば常に有効（ガチャツール drums.py の有無に相乗りしない —
    // レポートの閲覧/生成は独立判定なので、ガチャだけ欠けた環境でもカードは選べる必要がある）
    cardBox.setEnabled (true);

    // 頭出し: 可否と理由は実行側（MainComponent）が判定する（source.json・groove/gates・
    // クリップ同定・録音状態。ファイルI/Oを含むのでユーザー操作起点のここでだけ評価する）
    juce::String alignReason;
    if (! hasCard)
        alignReason = jp (u8"カードを選択してください");
    else if (alignUnavailableReason != nullptr)
        alignReason = alignUnavailableReason();
    alignButton.setEnabled (alignReason.isEmpty());
    alignButton.setTooltip (alignReason.isEmpty()
                                ? jp (u8"原曲クリップをBPM設定＋小節グリッドに合わせる（生成物と重ねて聴ける）")
                                : alignReason);

    // レポート: report.md 無し=書く（AI・約8分）／有り=開く。選択中カードを生成中なら
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

        reportButton.placeholder = ! hasReport;
        if (hasReport)
        {
            reportButton.removeColour (juce::TextButton::textColourOffId);
            reportButton.setButtonText (jp (u8"📄 レポートを開く"));
            reportButton.setEnabled (ReferenceTools::renderAvailable());
            reportButton.setTooltip (ReferenceTools::renderAvailable()
                                         ? jp (u8"分析レポートを別ウィンドウで読む（右クリックで書き直し）")
                                         : ReferenceTools::unavailableReason());
        }
        else
        {
            reportButton.setColour (juce::TextButton::textColourOffId,
                                    juce::Colours::white.withAlpha (0.55f));
            reportButton.setButtonText (jp (u8"＋ レポートを書く（約8分）"));
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
    const bool lanesUsable = partToolAvailable && hasCard && ! state.items.empty();
    for (int i = 0; i < 3; ++i)
    {
        const bool inUse = i < lanes;
        lockButtons[i].setVisible (inUse);
        if (! inUse)
            continue;
        lockButtons[i].setEnabled (lanesUsable || lockButtons[i].getToggleState());
        lockButtons[i].setButtonText (state.locks[i].isNotEmpty()
                                          ? jp (u8"🔒 ") + laneName (i)
                                          : juce::String (laneName (i)));
    }

    // 下部の1行ガイド（いまの状態で次にやることを言う）
    const int sel = selectedCandidate();
    const bool rowsEmpty = isLoops ? loopRecommendation.candidates.empty() : state.items.empty();
    if (isLoops)
    {
        if (! rowsEmpty)
            statusLabel.setText (state.previewActive
                                     ? jp (u8"ループを仮配置中 — 「仮配置中」をクリックでビート全体を残す（確定）・"
                                            u8"別の ✓ で差し替え")
                                     : jp (u8"行クリックで試聴 → 気に入ったら右端の ✓ で採用"),
                                 juce::dontSendNotification);
        else
            statusLabel.setText ({}, juce::dontSendNotification);
    }
    else if (! state.items.empty())
    {
        if (allLocked)
            statusLabel.setText (jp (u8"全レーンをロック中 — 振り直しても同じ候補しか出ません"),
                                 juce::dontSendNotification);
        else if (state.previewActive && sel >= 0)
            statusLabel.setText (jp (u8"候補") + juce::String (sel + 1)
                                     + jp (u8"を仮配置中 — 「仮配置中」をクリックでビート全体を残す（確定）・"
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

    if (! partToolAvailable)
        infoLabel.setText (isLoops ? ReferenceTools::loopGachaUnavailableReason()
                                   : ReferenceTools::unavailableReason(),
                           juce::dontSendNotification);
    else if (! hasCard)
        infoLabel.setText (jp (u8"カードがありません。タイムラインのオーディオリージョンを"
                               u8"右クリック →「リファレンスとして分析…」で作成できます。"),
                           juce::dontSendNotification);
    else if (rowsEmpty)
        infoLabel.setText (isLoops
                               ? jp (u8"「おすすめを出す」で、リファレンスに雰囲気が近い上モノループを"
                                     u8"ライブラリから選びます。行クリックで試聴、✓ で採用します。")
                           : isDrums
                               ? jp (u8"「振り直す」で候補を8件生成します。候補をクリックすると"
                                     u8"再生ヘッドの小節頭に仮配置され、曲と一緒に鳴らせます。")
                               : jp (u8"「振り直す」でベース候補を8件生成します。ドラムの仮配置が"
                                     u8"あれば同じ位置に重なり、キックに噛み合った候補になります。"),
                           juce::dontSendNotification);
    infoLabel.setVisible (! partToolAvailable || ! hasCard || rowsEmpty);
    listBox.setVisible (! infoLabel.isVisible() || ! rowsEmpty);

    // ---- アンカーカード（全タブ共通）----
    const bool showAnchor = project != nullptr && project->loopAnchor.has_value();
    anchorCard.setVisible (showAnchor);
    anchorReleaseButton.setVisible (showAnchor);
    if (showAnchor)
    {
        const auto& anchor = *project->loopAnchor;
        const auto bpmText = juce::String (anchor.bpm,
                                           anchor.bpm == std::floor (anchor.bpm) ? 0 : 1);
        anchorCard.name = juce::File::createFileWithoutCheckingPath (anchor.libraryPath)
                              .getFileNameWithoutExtension();
        anchorCard.chips.clearQuick();
        anchorCard.chips.add (ProjectKeys::displayName (anchor.key));
        anchorCard.chips.add (bpmText + "bpm");
        anchorCard.chips.add (juce::String (anchor.loopBars) + jp (u8"小節"));
        // 警告チップは追従チップより前（幅に入り切らないとき後ろから落ちるため）。
        // 追従チップの文言はタブ共通の短縮形 — Bass タブでは色の強調で「生成が実際に
        // 読んでいる」ことを示し、詳細な説明はツールチップに任せる
        if (anchor.degraded)
            anchorCard.chips.add (jp (u8"検出低信頼"));
        anchorCard.chips.add (jp (u8"ベース追従"));
        anchorCard.emphasizeChip = currentPart == Part::bass ? anchorCard.chips.size() - 1 : -1;

        // ツールチップ = 全情報（進行のルート列・信頼度）。進行は小節ごとに | で区切る
        juce::String tip = juce::File::createFileWithoutCheckingPath (anchor.libraryPath).getFileName()
                           + "\n" + ProjectKeys::displayName (anchor.key) + " / " + bpmText + "bpm / "
                           + juce::String (anchor.loopBars) + jp (u8"小節（1小節 ")
                           + juce::String (anchor.slotsPerBar) + jp (u8"スロット）");
        if (! anchor.roots.empty() && anchor.slotsPerBar > 0)
        {
            juce::StringArray bars;
            for (int bar = 0; bar * anchor.slotsPerBar < (int) anchor.roots.size(); ++bar)
            {
                juce::StringArray slots;
                for (int s = 0; s < anchor.slotsPerBar
                                && bar * anchor.slotsPerBar + s < (int) anchor.roots.size(); ++s)
                    slots.add (ProjectKeys::rootName (anchor.roots[(size_t) (bar * anchor.slotsPerBar + s)]));
                bars.add (slots.joinIntoString (" "));
            }
            tip += jp (u8"\n進行: ") + bars.joinIntoString (" | ");
        }
        if (! anchor.confidence.empty())
        {
            float sum = 0.0f, low = 1.0f;
            for (const auto c : anchor.confidence)
            {
                sum += c;
                low = juce::jmin (low, c);
            }
            tip += jp (u8"\n検出信頼度: 平均 ")
                   + juce::String (sum / (float) anchor.confidence.size(), 2)
                   + jp (u8" / 最低 ") + juce::String (low, 2);
        }
        anchorCard.setTooltip (tip);
        anchorCard.repaint();
    }
    const bool showPaging = isLoops && ! loopRecommendation.candidates.empty();
    prevPageButton.setVisible (showPaging);
    nextPageButton.setVisible (showPaging);
    pageInfoLabel.setVisible (showPaging);
    if (showPaging)
    {
        const int start = (loopRecommendation.page - 1) * loopRecommendation.pageSize + 1;
        const int end = start + (int) loopRecommendation.candidates.size() - 1;
        pageInfoLabel.setText (jp (u8"候補 ") + juce::String (loopRecommendation.total)
                                   + jp (u8"本中 ") + juce::String (start) + jp (u8"〜")
                                   + juce::String (end) + jp (u8"位"),
                               juce::dontSendNotification);
        prevPageButton.setButtonText (jp (u8"← 前の") + juce::String (loopRecommendation.pageSize) + jp (u8"本"));
        nextPageButton.setButtonText (jp (u8"次の") + juce::String (loopRecommendation.pageSize) + jp (u8"本 →"));
        prevPageButton.setEnabled (loopRecommendation.page > 1);
        nextPageButton.setEnabled (end < loopRecommendation.total);
    }
}

void GachaPanelView::showStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
}

void GachaPanelView::setPreviewActive (Part part, bool active)
{
    parts[(size_t) part].previewActive = active;
    if (part == currentPart)
    {
        listBox.updateContent(); // 選択行のチップ（仮配置中/残す）を付け替える
        listBox.repaint();
        updateControls();
    }
}

void GachaPanelView::clearAllPreviewBadges()
{
    for (auto& state : parts)
        state.previewActive = false;
    listBox.deselectAllRows();
    listBox.updateContent();
    listBox.repaint();
    updateControls();
}

// 候補行 = 番号＋ミニチュア＋仮配置バッジ。drums は K/S/H のドット譜（1小節・16分×16）、
// bass は音高ストリップ（パターン1周の x=位置 / y=音高）。
// hex の seed は表示しない（人が比べたいのはパターンそのもの。seed はロックの内部表現に徹する）
void GachaPanelView::paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                                       bool selected)
{
    // ---- Loops 行: 名前＋メタ（キー/BPM・移調・理由）の2行。試聴中は左端にアクセント帯 ----
    if (currentPart == Part::loops)
    {
        if (row < 0 || row >= (int) loopRecommendation.candidates.size())
            return;
        const auto& c = loopRecommendation.candidates[(size_t) row];
        if (selected)
        {
            g.setColour (Theme::accent.withAlpha (0.25f));
            g.fillRect (0, 0, width, height);
        }
        if (row == auditioningRow)
        {
            g.setColour (Theme::accent);
            g.fillRect (0, 0, 3, height);
        }
        const int rank = (loopRecommendation.page - 1) * loopRecommendation.pageSize + row + 1;
        const auto name = juce::File::createFileWithoutCheckingPath (c.path)
                              .getFileNameWithoutExtension();
        g.setColour (juce::Colours::white.withAlpha (selected ? 0.94f : 0.78f));
        g.setFont (Fonts::body());
        g.drawText ((row == auditioningRow ? jp (u8"▮▮ ") : jp (u8"▶ "))
                        + juce::String (rank) + ". " + name,
                    10, 2, width - 48, height / 2, juce::Justification::centredLeft);

        const auto keyText = ProjectKeys::displayName (ProjectKey { c.keyRoot, c.keyMode });
        const auto semis = c.transposeSemitones;
        const auto keyPart = semis == 0 ? jp (u8"キーそのまま")
                                        : jp (u8"移調 ") + juce::String (semis > 0 ? "+" : "")
                                              + juce::String (semis) + jp (u8" 半音");
        const int pct = (int) std::lround ((c.bpmRatio - 1.0) * 100.0);
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (Fonts::small());
        g.drawText (keyText + " " + juce::String (c.bpm, c.bpm == std::floor (c.bpm) ? 0 : 1)
                        + jp (u8"bpm（") + keyPart + jp (u8"・BPM ")
                        + juce::String (pct >= 0 ? "+" : "") + juce::String (pct) + jp (u8"%） — ")
                        + c.reason,
                    10, height / 2, width - 48, height / 2 - 2, juce::Justification::centredLeft);
        return;
    }

    const auto& state = parts[(size_t) currentPart];
    if (row < 0 || row >= (int) state.items.size())
        return;
    if (selected)
    {
        g.setColour (Theme::accent.withAlpha (0.25f));
        g.fillRect (0, 0, width, height);
    }
    const auto& candidate = state.items[(size_t) row];
    g.setColour (juce::Colours::white.withAlpha (selected ? 0.94f : 0.72f));
    g.setFont (Fonts::body());
    g.drawText (juce::String (row + 1), 10, 0, 18, height, juce::Justification::centredLeft);

    const float gridX = 44.0f;
    const float gridRight = (float) width - 66.0f; // 右端はバッジ分を空ける

    if (currentPart == Part::drums)
    {
        // レーンタグ（K/S/H）
        const bool laneLocked[GachaSession::patternLanes] = {
            state.locks[0].isNotEmpty(), state.locks[1].isNotEmpty(), state.locks[2].isNotEmpty() };
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
    }
    else
    {
        // ベースの音高ストリップ。進行ロック中はアクセント色（音高＝進行の見た目のため）
        const float top = 5.0f;
        const float stripHeight = (float) height - 10.0f;
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (gridX, top, juce::jmax (0.0f, gridRight - gridX), stripHeight, 2.0f);
        const auto base = state.locks[0].isNotEmpty() ? Theme::accent : juce::Colours::white;
        for (const auto& dot : candidate.bassDots)
        {
            const float x = gridX + dot.x * (gridRight - gridX - 6.0f);
            const float y = top + (1.0f - dot.y) * (stripHeight - 3.0f);
            g.setColour (base.withAlpha (0.75f));
            g.fillRoundedRectangle (x, y, 6.0f, 3.0f, 1.0f);
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

    if (currentPart == Part::loops)
    {
        // Loops 行は全行に ✓（敷く）ボタン。仮配置中の選択行には keep チップも並ぶ
        auto* loopRow = dynamic_cast<LoopAdoptRow*> (owned.get());
        if (loopRow == nullptr)
        {
            loopRow = new LoopAdoptRow();
            owned.reset (loopRow);
        }
        loopRow->update (selected && parts[(size_t) Part::loops].previewActive,
                         [this, row]
                         {
                             // ✓ は子コンポーネントで ListBox の行選択を通らない — 選択を採用行へ
                             // 揃えてから採用する（「仮配置中」チップが別の行に出るのを防ぐ）
                             listBox.selectRow (row);
                             if (onLoopAdopt)
                                 onLoopAdopt (row);
                         },
                         [this] { if (onKeep) onKeep(); });
        return owned.release();
    }

    auto* chipRow = dynamic_cast<KeepChipRow*> (owned.get());
    if (chipRow == nullptr)
    {
        chipRow = new KeepChipRow();
        owned.reset (chipRow);
    }
    // チップは「仮配置中の選択行」にだけ出す。クリックは onKeep（全パーツ一括確定）へ
    chipRow->update (selected && parts[(size_t) currentPart].previewActive,
                     [this] { if (onKeep) onKeep(); });
    return owned.release();
}

void GachaPanelView::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (currentPart == Part::loops)
    {
        // 行クリック＝試聴のトグル（採用は右端の ✓ だけ — モック確定仕様）
        if (row >= 0 && row < (int) loopRecommendation.candidates.size() && onLoopAudition)
            onLoopAudition (row);
        return;
    }
    if (row >= 0 && row < (int) parts[(size_t) currentPart].items.size() && onPick)
        onPick (row);
}

void GachaPanelView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::chooserPanelBg);
}

void GachaPanelView::AnchorCard::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff1f2530));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (juce::Colour (0xff3a455a));
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

    auto inner = getLocalBounds().reduced (10, 7);
    inner.removeFromRight (60); // 解除ボタンの席（ボタン本体は親が同じ矩形に重ねて置く）

    g.setColour (juce::Colour (0xff7fa3e8));
    g.setFont (Fonts::body());
    g.drawText (juce::String::fromUTF8 (u8"⚓"), inner.removeFromLeft (16),
                juce::Justification::centredLeft);
    inner.removeFromLeft (5);

    const auto font = Fonts::small();
    g.setFont (font);
    g.setColour (juce::Colour (0xffdfe6f2));
    g.drawText (name, inner.removeFromTop (15), juce::Justification::centredLeft, true);
    inner.removeFromTop (4);

    auto chipRow = inner.removeFromTop (15);
    int x = chipRow.getX();
    for (int i = 0; i < chips.size(); ++i)
    {
        const int w = juce::GlyphArrangement::getStringWidthInt (font, chips[i]) + 14;
        if (x + w > chipRow.getRight())
            break; // 入り切らないチップは落とす（全情報はツールチップ側にある）
        const juce::Rectangle<int> chip (x, chipRow.getY(), w, chipRow.getHeight());
        g.setColour (juce::Colour (i == emphasizeChip ? 0xff3d5a8c : 0xff32405c));
        g.fillRoundedRectangle (chip.toFloat(), 4.0f);
        g.setColour (juce::Colour (0xffaecbff));
        g.drawText (chips[i], chip, juce::Justification::centred);
        x += w + 5;
    }
}

void GachaPanelView::resized()
{
    auto area = getLocalBounds().reduced (14, 10);

    // パーツタブ（Drums / Bass / Loops）
    auto tabRow = area.removeFromTop (24);
    const int tabWidth = (tabRow.getWidth() - 12) / 3;
    drumsTab.setBounds (tabRow.removeFromLeft (tabWidth));
    tabRow.removeFromLeft (6);
    bassTab.setBounds (tabRow.removeFromLeft (tabWidth));
    tabRow.removeFromLeft (6);
    loopsTab.setBounds (tabRow);
    area.removeFromTop (8);

    // アンカーカード（全タブ共通・タブ直下。採用中のみ）
    if (anchorCard.isVisible())
    {
        const auto cardArea = area.removeFromTop (46);
        anchorCard.setBounds (cardArea);
        anchorReleaseButton.setBounds (cardArea.reduced (8, 0)
                                           .removeFromRight (52)
                                           .withSizeKeepingCentre (52, 24));
        area.removeFromTop (8);
    }

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

    const int lanes = numLanes();
    lockCaption.setVisible (lanes > 0);
    if (lanes > 0) // loops はロック行を持たない
    {
        auto lockRow = area.removeFromTop (24);
        lockCaption.setBounds (lockRow.removeFromLeft (40));
        const int lockWidth = (lockRow.getWidth() - (lanes - 1) * 6) / lanes;
        for (int i = 0; i < lanes; ++i)
        {
            if (i > 0)
                lockRow.removeFromLeft (6);
            lockButtons[i].setBounds (i == lanes - 1 ? lockRow : lockRow.removeFromLeft (lockWidth));
        }
        area.removeFromTop (8);
    }

    rollButton.setBounds (area.removeFromTop (28));
    area.removeFromTop (10);

    const bool rowsEmpty = currentPart == Part::loops ? loopRecommendation.candidates.empty()
                                                      : parts[(size_t) currentPart].items.empty();
    if (infoLabel.isVisible() && rowsEmpty)
    {
        infoLabel.setBounds (area.removeFromTop (72));
        area.removeFromTop (4);
    }
    // 下部の1行ガイド（2行ぶんの高さ。候補がないときは空文字で場所だけ確保）
    statusLabel.setBounds (area.removeFromBottom (32));
    area.removeFromBottom (4);
    if (prevPageButton.isVisible()) // Loops のページング行（一覧とガイドの間）
    {
        auto pageRow = area.removeFromBottom (24);
        prevPageButton.setBounds (pageRow.removeFromLeft (96));
        nextPageButton.setBounds (pageRow.removeFromRight (96));
        pageInfoLabel.setBounds (pageRow);
        area.removeFromBottom (4);
    }
    listBox.setBounds (area);
}
