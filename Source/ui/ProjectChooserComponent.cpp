#include "ProjectChooserComponent.h"

#include <algorithm>

#include "../shared/Log.h"
#include "Fonts.h"
#include "Theme.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

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

    refreshList();
    setSize (520, 480);
}

void ProjectChooserComponent::refreshList()
{
    entries.clear();
    for (auto& dir : Project::projectsRoot().findChildFiles (juce::File::findDirectories, false))
    {
        const auto json = dir.getChildFile ("project.json");
        if (json.existsAsFile())
            entries.push_back ({ dir, json.getLastModificationTime() });
    }

    // 最近触ったプロジェクトが先頭に来るよう更新日時の降順（同時刻は名前順で安定させる）
    std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b)
    {
        if (a.modified != b.modified)
            return b.modified < a.modified;
        return a.dir.getFileName().compareIgnoreCase (b.dir.getFileName()) < 0;
    });

    listBox.updateContent();
    if (! entries.empty())
        listBox.selectRow (0);

    emptyLabel.setVisible (entries.empty());
    nameEditor.setText (defaultProjectName(), juce::dontSendNotification);
}

int ProjectChooserComponent::getNumRows()
{
    return (int) entries.size();
}

void ProjectChooserComponent::paintListBoxItem (int rowNumber, juce::Graphics& g,
                                                int width, int height, bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= (int) entries.size())
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

    const auto& entry = entries[(size_t) rowNumber];
    const auto name = entry.dir.getFileName();
    const int textX = 18;
    const int textW = width - textX * 2;

    g.setColour (juce::Colours::white);
    // リスト行は主要コンテンツなのでbodyより一回り大きく。プロジェクト名は自由入力なのでCJK補正
    g.setFont (Fonts::forText (Fonts::body().withHeight (15.0f), name));
    g.drawText (name, textX, 7, textW, 18, juce::Justification::centredLeft);

    g.setColour (rowIsSelected ? juce::Colours::white.withAlpha (0.6f) : Theme::chooserMetaText);
    g.setFont (Fonts::small());
    g.drawText (formatModified (entry.modified), textX, 26, textW, 14,
                juce::Justification::centredLeft);
}

void ProjectChooserComponent::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    openRow (row);
}

void ProjectChooserComponent::returnKeyPressed (int lastRowSelected)
{
    openRow (lastRowSelected);
}

void ProjectChooserComponent::openRow (int row)
{
    if (row < 0 || row >= (int) entries.size())
        return;

    const auto dir = entries[(size_t) row].dir;
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

void ProjectChooserComponent::parentHierarchyChanged()
{
    // Return・矢印キーを最初から効かせるため、表示されたらリストにフォーカスを渡す
    if (isShowing())
        listBox.grabKeyboardFocus();
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

void ProjectChooserComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    // リストの入れ物。地を一段沈めてカードとして浮かせる
    const auto panel = listPanelArea.toFloat();
    g.setColour (Theme::chooserPanelBg);
    g.fillRoundedRectangle (panel, 8.0f);
    g.setColour (Theme::panelBorder);
    g.drawRoundedRectangle (panel.reduced (0.5f), 8.0f, 1.0f);
}

void ProjectChooserComponent::resized()
{
    auto area = getLocalBounds().reduced (24);

    titleLabel.setBounds (area.removeFromTop (22).withTrimmedLeft (2));
    area.removeFromTop (10);

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
