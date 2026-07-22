#include "ProjectChooserComponent.h"

#include <algorithm>
#include <map>

#include "../shared/Log.h"
#include "Fonts.h"
#include "Theme.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

// プロジェクトの識別色。名前のハッシュから色相を決める（同じ名前は常に同じ色）。
// 彩度・明度はテーマに合わせて控えめにし、カラーバーと波形の両方に使う
juce::Colour projectColour (const juce::String& name)
{
    const auto hue = (float) (name.hashCode() & 0xffff) / 65536.0f;
    return juce::Colour::fromHSV (hue, 0.45f, 0.8f, 1.0f);
}

// 新規プロジェクトの自動命名: 日付-ランダム英単語（例: 2026-07-22-ember）。
// 単語は呼びやすい短い名詞から選ぶ。同名が既にあれば別の単語を引き直す
juce::String defaultProjectName()
{
    static const char* words[] = {
        "amber",   "aurora",  "birch",   "breeze",  "canyon",  "cedar",   "cinder",
        "clover",  "cobalt",  "comet",   "coral",   "cove",    "dawn",    "drift",
        "dune",    "dusk",    "echo",    "ember",   "fern",    "flare",   "fog",
        "frost",   "garnet",  "glacier", "grove",   "harbor",  "haze",    "hollow",
        "indigo",  "iris",    "ivy",     "juniper", "lagoon",  "lantern", "lark",
        "lilac",   "lunar",   "maple",   "marble",  "meadow",  "mist",    "moss",
        "nebula",  "oasis",   "opal",    "orbit",   "pebble",  "pine",    "plume",
        "prairie", "prism",   "quartz",  "raven",   "reef",    "ridge",   "river",
        "sage",    "shore",   "sierra",  "slate",   "sparrow", "spruce",  "summit",
        "thistle", "tide",    "timber",  "topaz",   "tundra",  "velvet",  "violet",
        "willow",  "wren",    "zephyr",
    };
    const auto date = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");
    auto& random = juce::Random::getSystemRandom();

    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const auto name = date + "-" + words[random.nextInt (juce::numElementsInArray (words))];
        if (! Project::projectsRoot().getChildFile (name).exists())
            return name;
    }

    for (int i = 2;; ++i) // ほぼ到達しない保険（同日に単語が枯渇した場合）
    {
        const auto name = date + "-" + juce::String (i);
        if (! Project::projectsRoot().getChildFile (name).exists())
            return name;
    }
}

// ドロップされたパスからプロジェクトフォルダを解決する。
// フォルダ本体でも中の project.json でも受け付け、最初に見つかった有効なものを返す
juce::File resolveDroppedProject (const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        const juce::File f (path);
        if (f.isDirectory() && f.getChildFile ("project.json").existsAsFile())
            return f;
        if (f.getFileName() == "project.json" && f.existsAsFile())
            return f.getParentDirectory();
    }
    return {};
}

// メタ情報のサブテキスト（例: "1:23 · 120 BPM · 3tr"）。未着（bpm==0かつtracks==0）は空を返す
juce::String overviewMetaText (const ProjectOverview& o)
{
    if (o.bpm <= 0 && o.numTracks == 0)
        return {};

    juce::StringArray parts;
    if (o.lengthSeconds > 0.05)
    {
        const int total = (int) std::round (o.lengthSeconds);
        parts.add (juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2));
    }
    if (o.bpm > 0)
        parts.add (juce::String ((int) std::round (o.bpm)) + " BPM");
    parts.add (juce::String (o.numTracks) + "tr");
    return parts.joinIntoString (" · ");
}

// 中心線対称のバーでミニ波形を描く。無音ビンは描かず曲の構造をそのまま見せる
void drawOverviewWaveform (juce::Graphics& g, juce::Rectangle<int> area,
                           const std::vector<float>& peaks, juce::Colour colour)
{
    if (peaks.empty() || area.getWidth() < 4)
        return;

    const float cy = area.toFloat().getCentreY();
    const float maxHalf = (float) area.getHeight() * 0.5f;
    g.setColour (colour);
    const int numCols = area.getWidth() / 2;
    for (int col = 0; col < numCols; ++col)
    {
        const auto peak = peaks[(size_t) (col * (int) peaks.size() / numCols)];
        if (peak < 0.004f)
            continue;
        const float half = juce::jmax (0.75f, peak * maxHalf);
        g.fillRect ((float) (area.getX() + col * 2), cy - half, 1.2f, half * 2.0f);
    }
}

// 更新日時のサブテキスト。直近は「Today/Yesterday HH:MM」、それ以前は日付だけで十分
juce::String formatModified (const juce::Time& t)
{
    const auto now = juce::Time::getCurrentTime();
    const auto sameDay = [] (const juce::Time& a, const juce::Time& b)
    {
        return a.getYear() == b.getYear() && a.getDayOfYear() == b.getDayOfYear();
    };

    if (sameDay (t, now))
        return "Today " + t.formatted ("%H:%M");
    if (sameDay (t, now - juce::RelativeTime::days (1)))
        return "Yesterday " + t.formatted ("%H:%M");
    return t.formatted ("%Y-%m-%d");
}
} // namespace

ProjectChooserComponent::ProjectChooserComponent()
{
    addAndMakeVisible (titleLabel);
    addChildComponent (hero); // 可視状態はrefreshList()がエントリ有無で決める
    addAndMakeVisible (listBox);
    addAndMakeVisible (emptyLabel);
    addAndMakeVisible (nameEditor);
    addAndMakeVisible (newButton);
    addAndMakeVisible (errorLabel);

    // セクションラベル風の小さめ大文字＋トラッキングで沈める（主役はリスト）
    titleLabel.setText ("PROJECTS", juce::dontSendNotification);
    titleLabel.setFont (Fonts::bodyStrong().withHeight (12.0f).withExtraKerningFactor (0.12f));
    titleLabel.setColour (juce::Label::textColourId, Theme::chooserTitleText);

    listBox.setRowHeight (48);
    listBox.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    listBox.addMouseListener (this, true); // ホバー行の追跡（paintListBoxItemにhover状態が無いため自前で持つ）
    // ↑↓Returnはヒーローを含めた選択遷移としてchooser自身のkeyPressedで処理する。
    // ListBoxにフォーカスを渡すと素のキー処理（ヒーローを知らない）に奪われるため無効化
    setWantsKeyboardFocus (true);
    listBox.setWantsKeyboardFocus (false);

    emptyLabel.setText ("No projects yet", juce::dontSendNotification);
    emptyLabel.setFont (Fonts::body());
    emptyLabel.setColour (juce::Label::textColourId, Theme::chooserMetaText);
    emptyLabel.setJustificationType (juce::Justification::centred);
    emptyLabel.setInterceptsMouseClicks (false, false);

    nameEditor.setFont (Fonts::body());
    nameEditor.setColour (juce::TextEditor::backgroundColourId, Theme::chooserPanelBg);
    nameEditor.setColour (juce::TextEditor::outlineColourId, Theme::panelBorder);
    nameEditor.setColour (juce::TextEditor::focusedOutlineColourId, Theme::accent);
    // 候補名をプリフィルし、フォーカスで全選択（macOSの保存ダイアログ方式）。
    // そのままEnter/Createで候補名、名前を考えたいときは打ち始めれば置き換わる
    nameEditor.setSelectAllWhenFocused (true);
    nameEditor.onReturnKey = [this] { createNewProject(); };

    newButton.setButtonText ("Create");
    newButton.onClick = [this] { createNewProject(); };

    errorLabel.setFont (Fonts::small());
    errorLabel.setColour (juce::Label::textColourId, Theme::warning);

    // ヒーローカード = 直近プロジェクト（entries[0]）のクイックオープン
    hero.onOpen = [this]
    {
        if (! entries.empty())
            openDirectory (entries[0].dir);
    };
    hero.onReveal = [this]
    {
        if (! entries.empty())
            showRevealMenu (entries[0].dir);
    };

    // オーバービューの到着（メッセージスレッド）。ジョブがこのラムダのコピーを持つため、
    // SafePointerで自身の破棄後は空振りさせる
    thumbnails.onLoaded = [safe = juce::Component::SafePointer<ProjectChooserComponent> (this)] (
                              const juce::File& dir, ProjectOverview overview)
    {
        if (safe != nullptr)
            safe->applyOverview (dir, std::move (overview));
    };

    refreshList();
    setSize (520, 584);
}

void ProjectChooserComponent::applyOverview (const juce::File& dir, ProjectOverview overview)
{
    for (int i = 0; i < (int) entries.size(); ++i)
    {
        if (entries[(size_t) i].dir == dir)
        {
            entries[(size_t) i].overview = std::move (overview);
            if (i == 0)
                updateHero();
            else
                listBox.repaintRow (i - 1);
            return;
        }
    }
}

void ProjectChooserComponent::updateHero()
{
    const bool show = ! entries.empty();
    if (show)
    {
        const auto& e = entries[0];
        auto meta = formatModified (e.modified);
        if (const auto info = overviewMetaText (e.overview); info.isNotEmpty())
            meta << " · " << info;
        hero.update (e.dir.getFileName(), meta, projectColour (e.dir.getFileName()),
                     e.overview.peaks);
    }
    hero.setSelected (heroSelected());
    if (hero.isVisible() != show)
    {
        hero.setVisible (show);
        resized(); // ヒーローの有無でリストの高さが変わる
    }
}

void ProjectChooserComponent::refreshList()
{
    const int previousRow = listBox.getSelectedRow();
    const auto selectedName = (previousRow >= 0 && previousRow < getNumRows())
                                  ? listEntry (previousRow).dir.getFileName()
                                  : juce::String();

    // 再読込でオーバービューが消えないよう、mtimeが変わっていないエントリは引き継ぐ
    // （同一(dir, mtime)の再依頼はローダー側で弾かれ、再到着しないため）
    std::map<juce::String, Entry> previous;
    for (auto& e : entries)
        previous.emplace (e.dir.getFullPathName(), std::move (e));

    entries.clear();
    for (auto& dir : Project::projectsRoot().findChildFiles (juce::File::findDirectories, false))
    {
        const auto json = dir.getChildFile ("project.json");
        if (! json.existsAsFile())
            continue;

        Entry entry { dir, json.getLastModificationTime(), {} };
        if (auto found = previous.find (dir.getFullPathName());
            found != previous.end() && found->second.modified == entry.modified)
            entry.overview = std::move (found->second.overview);
        entries.push_back (std::move (entry));
    }

    // 最近触ったプロジェクトが先頭に来るよう更新日時の降順（同時刻は名前順で安定させる）
    std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b)
    {
        if (a.modified != b.modified)
            return b.modified < a.modified;
        return a.dir.getFileName().compareIgnoreCase (b.dir.getFileName()) < 0;
    });

    for (const auto& e : entries)
        thumbnails.request (e.dir, e.modified);

    hoveredRow = -1;
    listBox.updateContent();
    // 選択は名前で追従（リストは entries[1..]）。見つからなければヒーロー選択（=リスト非選択）に戻す
    int rowToSelect = -1;
    for (int i = 1; i < (int) entries.size(); ++i)
        if (entries[(size_t) i].dir.getFileName() == selectedName)
            rowToSelect = i - 1;
    if (rowToSelect >= 0)
        listBox.selectRow (rowToSelect);
    else
        listBox.deselectAllRows();

    emptyLabel.setVisible (entries.empty());
    updateHero();

    // 候補名はユーザーが編集していなければ維持する（フォーカス復帰のたびに単語が変わらないように）。
    // 初回と、未編集のまま候補名が既存プロジェクトと衝突したときだけ引き直す
    const bool untouched = nameEditor.getText() == suggestedName;
    if (suggestedName.isEmpty()
        || (untouched && Project::projectsRoot().getChildFile (suggestedName).exists()))
    {
        suggestedName = defaultProjectName();
        if (untouched || nameEditor.isEmpty())
            nameEditor.setText (suggestedName, juce::dontSendNotification);
    }
}

int ProjectChooserComponent::getNumRows()
{
    return juce::jmax (0, (int) entries.size() - 1);
}

bool ProjectChooserComponent::heroSelected() const
{
    return ! entries.empty() && listBox.getSelectedRow() < 0;
}

void ProjectChooserComponent::paintListBoxItem (int rowNumber, juce::Graphics& g,
                                                int width, int height, bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= getNumRows())
        return;

    const auto rowBounds = juce::Rectangle<float> (0, 0, (float) width, (float) height)
                               .reduced (6.0f, 2.0f);
    if (rowIsSelected)
    {
        g.setColour (Theme::chooserRowSelected);
        g.fillRoundedRectangle (rowBounds, 6.0f);
    }
    else if (rowNumber == hoveredRow)
    {
        g.setColour (Theme::chooserRowHover);
        g.fillRoundedRectangle (rowBounds, 6.0f);
    }

    const auto& entry = listEntry (rowNumber);
    const auto name = entry.dir.getFileName();
    const auto colour = projectColour (name);

    // 左端の識別カラーバー
    g.setColour (colour.withAlpha (rowIsSelected ? 1.0f : 0.85f));
    g.fillRoundedRectangle (14.0f, (float) height * 0.5f - 14.0f, 3.0f, 28.0f, 1.5f);

    // 右側のミニ波形
    const int waveWidth = 170;
    const int waveLeft = width - 18 - waveWidth;
    drawOverviewWaveform (g, { waveLeft, 8, waveWidth, height - 16 }, entry.overview.peaks,
                          rowIsSelected ? juce::Colours::white.withAlpha (0.7f)
                                        : colour.withAlpha (0.75f));

    const int textX = 26;
    const int textW = waveLeft - textX - 8;

    g.setColour (juce::Colours::white);
    // リスト行は主要コンテンツなのでbodyより一回り大きく。プロジェクト名は自由入力なのでCJK補正
    g.setFont (Fonts::forText (Fonts::body().withHeight (15.0f), name));
    g.drawText (name, textX, 7, textW, 18, juce::Justification::centredLeft);

    auto subText = formatModified (entry.modified);
    if (const auto info = overviewMetaText (entry.overview); info.isNotEmpty())
        subText << " · " << info;
    g.setColour (rowIsSelected ? juce::Colours::white.withAlpha (0.6f) : Theme::chooserMetaText);
    g.setFont (Fonts::small());
    g.drawText (subText, textX, 26, textW, 14, juce::Justification::centredLeft);
}

void ProjectChooserComponent::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() || row < 0 || row >= getNumRows())
        return;

    listBox.selectRow (row);
    showRevealMenu (listEntry (row).dir);
}

void ProjectChooserComponent::showRevealMenu (const juce::File& dir)
{
    juce::PopupMenu menu;
    menu.addItem (1, jp (u8"Finderで表示"));

    // コールバックは後から呼ばれるため、右クリック時点の対象を値で捕捉し寿命はSafePointerで確認する
    juce::Component::SafePointer<ProjectChooserComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options(), [safe, dir] (int result)
    {
        if (safe != nullptr && result == 1)
            dir.revealToUser();
    });
}

void ProjectChooserComponent::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    openRow (row);
}

void ProjectChooserComponent::selectedRowsChanged (int)
{
    hero.setSelected (heroSelected());
}

bool ProjectChooserComponent::keyPressed (const juce::KeyPress& key)
{
    if (entries.empty())
        return false;

    const int numListRows = getNumRows();
    const int selected = listBox.getSelectedRow();

    if (key.isKeyCode (juce::KeyPress::downKey))
    {
        if (heroSelected())
        {
            if (numListRows > 0)
                listBox.selectRow (0);
        }
        else
        {
            listBox.selectRow (juce::jmin (selected + 1, numListRows - 1));
        }
        return true;
    }
    if (key.isKeyCode (juce::KeyPress::upKey))
    {
        if (selected == 0)
            listBox.deselectAllRows(); // 先頭行からさらに上 = ヒーローへ戻る
        else if (selected > 0)
            listBox.selectRow (selected - 1);
        return true;
    }
    if (key.isKeyCode (juce::KeyPress::returnKey))
    {
        if (heroSelected())
            openDirectory (entries[0].dir);
        else
            openRow (selected);
        return true;
    }
    return false;
}

void ProjectChooserComponent::openRow (int row)
{
    if (row >= 0 && row < getNumRows())
        openDirectory (listEntry (row).dir);
}

void ProjectChooserComponent::openDirectory (const juce::File& dir)
{
    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);

    if (project == nullptr)
    {
        Log::error ("project.load_failed", "dir=" + dir.getFullPathName() + " error=" + error);
        errorLabel.setText (error, juce::dontSendNotification);
        return;
    }

    if (! warnings.isEmpty())
    {
        Log::warn ("project.load_warnings", "dir=" + dir.getFullPathName()
                                                + " " + warnings.joinIntoString (" / "));
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                     jp (u8"読み込み時の警告"),
                                                     warnings.joinIntoString ("\n"));
    }

    if (onProjectOpened)
        onProjectOpened (std::move (project));
}

void ProjectChooserComponent::createNewProject()
{
    const auto typed = nameEditor.getText().trim();
    const auto name = juce::File::createLegalFileName (typed.isNotEmpty() ? typed
                                                                          : defaultProjectName());
    juce::String error;
    auto project = Project::createNew (Project::projectsRoot().getChildFile (name), error);
    if (project == nullptr)
    {
        Log::error ("project.create_failed", "name=" + name + " error=" + error);
        errorLabel.setText (error, juce::dontSendNotification);
        return;
    }
    Log::info ("project.create", "name=" + name);

    if (onProjectOpened)
        onProjectOpened (std::move (project));
}

bool ProjectChooserComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    return resolveDroppedProject (files) != juce::File();
}

void ProjectChooserComponent::fileDragEnter (const juce::StringArray&, int, int)
{
    dragHover = true;
    repaint();
}

void ProjectChooserComponent::fileDragExit (const juce::StringArray&)
{
    dragHover = false;
    repaint();
}

void ProjectChooserComponent::filesDropped (const juce::StringArray& files, int, int)
{
    dragHover = false;
    repaint();

    const auto dir = resolveDroppedProject (files);
    if (dir != juce::File())
        openDirectory (dir);
}

void ProjectChooserComponent::parentHierarchyChanged()
{
    // Return・矢印キーを最初から効かせるため、表示されたらリストにフォーカスを渡す。
    // 初回起動時は setContentOwned がウィンドウの setVisible(true) より先に走り
    // isShowing() が偽になるため、コールスタックを抜けてから（表示後に）再試行する
    if (isShowing())
    {
        grabKeyboardFocus();
        return;
    }

    juce::Component::SafePointer<ProjectChooserComponent> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr && safe->isShowing())
            safe->grabKeyboardFocus();
    });
}

void ProjectChooserComponent::mouseMove (const juce::MouseEvent& e)
{
    updateHoveredRow (e);
}

void ProjectChooserComponent::mouseExit (const juce::MouseEvent& e)
{
    updateHoveredRow (e);
}

void ProjectChooserComponent::updateHoveredRow (const juce::MouseEvent& e)
{
    const auto pos = e.getEventRelativeTo (&listBox).getPosition();
    const int row = listBox.getLocalBounds().contains (pos)
                        ? listBox.getRowContainingPosition (pos.x, pos.y)
                        : -1;
    if (row == hoveredRow)
        return;

    const int previous = std::exchange (hoveredRow, row);
    if (previous >= 0)
        listBox.repaintRow (previous);
    if (hoveredRow >= 0)
        listBox.repaintRow (hoveredRow);
}

void ProjectChooserComponent::HeroCard::update (const juce::String& newName,
                                                const juce::String& newMeta,
                                                juce::Colour newColour,
                                                std::vector<float> newPeaks)
{
    name = newName;
    meta = newMeta;
    colour = newColour;
    peaks = std::move (newPeaks);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    repaint();
}

void ProjectChooserComponent::HeroCard::setSelected (bool nowSelected)
{
    if (selected == nowSelected)
        return;
    selected = nowSelected;
    repaint();
}

void ProjectChooserComponent::HeroCard::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    // 選択中はリスト行と同じ塗りで「Returnで開く対象」を示す
    auto bg = selected ? Theme::chooserRowSelected : Theme::chooserPanelBg;
    if (hover)
        bg = bg.brighter (selected ? 0.05f : 0.12f);
    g.setColour (bg);
    g.fillRoundedRectangle (bounds, 8.0f);
    if (! selected)
    {
        g.setColour (Theme::panelBorder);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);
    }

    const float cy = bounds.getCentreY();
    g.setColour (colour);
    g.fillRoundedRectangle (16.0f, cy - 18.0f, 3.5f, 36.0f, 1.75f);

    const int waveWidth = 220;
    const int waveLeft = getWidth() - 16 - waveWidth;
    drawOverviewWaveform (g, { waveLeft, 14, waveWidth, getHeight() - 28 }, peaks,
                          selected ? juce::Colours::white.withAlpha (0.75f)
                                   : colour.withAlpha (0.8f));

    const int textX = 30;
    const int textW = (peaks.empty() ? getWidth() - 16 : waveLeft) - textX - 10;

    g.setColour (juce::Colours::white);
    // 直近プロジェクトの「顔」なのでリスト行よりさらに一回り大きく。名前は自由入力なのでCJK補正
    g.setFont (Fonts::forText (Fonts::bodyStrong().withHeight (17.0f), name));
    g.drawText (name, textX, getHeight() / 2 - 21, textW, 22, juce::Justification::centredLeft);

    g.setColour (selected ? juce::Colours::white.withAlpha (0.6f) : Theme::chooserMetaText);
    g.setFont (Fonts::small());
    g.drawText (meta, textX, getHeight() / 2 + 3, textW, 14, juce::Justification::centredLeft);
}

void ProjectChooserComponent::HeroCard::mouseEnter (const juce::MouseEvent&)
{
    hover = true;
    repaint();
}

void ProjectChooserComponent::HeroCard::mouseExit (const juce::MouseEvent&)
{
    hover = false;
    repaint();
}

void ProjectChooserComponent::HeroCard::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && onReveal)
        onReveal();
}

void ProjectChooserComponent::HeroCard::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() && contains (e.getPosition()) && onOpen)
        onOpen();
}

void ProjectChooserComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    // リストの入れ物。地を一段沈めてカードとして浮かせる。
    // プロジェクトフォルダをドラッグで重ねている間はアクセント枠で受け入れ可能を示す
    const auto panel = listPanelArea.toFloat();
    g.setColour (Theme::chooserPanelBg);
    g.fillRoundedRectangle (panel, 8.0f);
    g.setColour (dragHover ? Theme::accent : Theme::panelBorder);
    g.drawRoundedRectangle (panel.reduced (dragHover ? 1.0f : 0.5f), 8.0f,
                            dragHover ? 2.0f : 1.0f);
}

void ProjectChooserComponent::resized()
{
    auto area = getLocalBounds().reduced (24);

    titleLabel.setBounds (area.removeFromTop (22).withTrimmedLeft (2));
    area.removeFromTop (10);

    if (hero.isVisible())
    {
        hero.setBounds (area.removeFromTop (84));
        area.removeFromTop (12);
    }

    auto newRowArea = area.removeFromBottom (32);
    area.removeFromBottom (6);
    errorLabel.setBounds (area.removeFromBottom (16));
    area.removeFromBottom (6);

    listPanelArea = area;
    listBox.setBounds (area.reduced (2, 6));
    emptyLabel.setBounds (area);

    newButton.setBounds (newRowArea.removeFromRight (96));
    newRowArea.removeFromRight (8);
    nameEditor.setBounds (newRowArea);
}
