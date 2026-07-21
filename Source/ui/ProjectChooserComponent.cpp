#include "ProjectChooserComponent.h"

#include "../shared/Log.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }
}

ProjectChooserComponent::ProjectChooserComponent()
{
    addAndMakeVisible (titleLabel);
    addAndMakeVisible (listBox);
    addAndMakeVisible (openButton);
    addAndMakeVisible (newCaptionLabel);
    addAndMakeVisible (nameEditor);
    addAndMakeVisible (newButton);
    addAndMakeVisible (errorLabel);

    titleLabel.setText (jp (u8"プロジェクトを選択"), juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));

    listBox.setRowHeight (28);

    openButton.setButtonText (jp (u8"開く"));
    openButton.onClick = [this] { openRow (listBox.getSelectedRow()); };

    newCaptionLabel.setText (jp (u8"新規プロジェクト名"), juce::dontSendNotification);
    nameEditor.setTextToShowWhenEmpty (jp (u8"例: my-song"), juce::Colours::grey);
    nameEditor.onReturnKey = [this] { createNewProject(); };

    newButton.setButtonText (jp (u8"新規作成"));
    newButton.onClick = [this] { createNewProject(); };

    errorLabel.setColour (juce::Label::textColourId, juce::Colours::orangered);

    refreshList();
    setSize (520, 480);
}

void ProjectChooserComponent::refreshList()
{
    projectDirs.clear();
    for (auto& dir : Project::projectsRoot().findChildFiles (juce::File::findDirectories, false))
        if (dir.getChildFile ("project.json").existsAsFile())
            projectDirs.add (dir);

    struct Sorter
    {
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            return a.getFileName().compareIgnoreCase (b.getFileName());
        }
    };
    Sorter sorter;
    projectDirs.sort (sorter);

    listBox.updateContent();
    if (projectDirs.size() > 0)
        listBox.selectRow (0);
}

int ProjectChooserComponent::getNumRows()
{
    return projectDirs.size();
}

void ProjectChooserComponent::paintListBoxItem (int rowNumber, juce::Graphics& g,
                                                int width, int height, bool rowIsSelected)
{
    if (rowIsSelected)
        g.fillAll (juce::Colour (0xff39537d));

    if (rowNumber >= 0 && rowNumber < projectDirs.size())
    {
        g.setColour (juce::Colours::white);
        g.setFont (15.0f);
        g.drawText (projectDirs[rowNumber].getFileName(), 10, 0, width - 20, height,
                    juce::Justification::centredLeft);
    }
}

void ProjectChooserComponent::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    openRow (row);
}

void ProjectChooserComponent::openRow (int row)
{
    if (row < 0 || row >= projectDirs.size())
        return;

    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (projectDirs[row], warnings, error);

    if (project == nullptr)
    {
        Log::error ("project.load_failed", "dir=" + projectDirs[row].getFullPathName()
                                               + " error=" + error);
        errorLabel.setText (error, juce::dontSendNotification);
        return;
    }

    if (! warnings.isEmpty())
    {
        Log::warn ("project.load_warnings", "dir=" + projectDirs[row].getFullPathName()
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
    const auto name = juce::File::createLegalFileName (nameEditor.getText().trim());
    if (name.isEmpty())
    {
        errorLabel.setText (jp (u8"プロジェクト名を入力してください"), juce::dontSendNotification);
        return;
    }

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

void ProjectChooserComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void ProjectChooserComponent::resized()
{
    auto area = getLocalBounds().reduced (20);

    titleLabel.setBounds (area.removeFromTop (32));
    area.removeFromTop (10);

    auto bottom = area.removeFromBottom (96);
    listBox.setBounds (area);

    bottom.removeFromTop (8);
    auto openRowArea = bottom.removeFromTop (28);
    openButton.setBounds (openRowArea.removeFromRight (100));
    errorLabel.setBounds (openRowArea);

    bottom.removeFromTop (8);
    auto newRowArea = bottom.removeFromTop (28);
    newCaptionLabel.setBounds (newRowArea.removeFromLeft (140));
    newButton.setBounds (newRowArea.removeFromRight (100));
    newRowArea.removeFromRight (8);
    nameEditor.setBounds (newRowArea);
}
