#include "AudioFileBrowserView.h"

#include "../shared/AudioFileTypes.h"
#include "Fonts.h"
#include "Theme.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }
}

AudioFileBrowserView::AudioFilter::AudioFilter()
    : juce::FileFilter ("Audio files")
{
}

bool AudioFileBrowserView::AudioFilter::isFileSuitable (const juce::File& file) const
{
    return AudioFileTypes::isSupported (file);
}

bool AudioFileBrowserView::AudioFilter::isDirectorySuitable (const juce::File& file) const
{
    return ! file.isHidden();
}

AudioFileBrowserView::AudioFileBrowserView()
{
    formats.registerBasicFormats();
    directoryThread.startThread();
    contents.setIgnoresHiddenFiles (true);
    contents.addChangeListener (this);

    for (auto* button : { &backButton, &forwardButton, &parentButton, &previewButton })
    {
        addAndMakeVisible (*button);
        button->setWantsKeyboardFocus (false);
    }
    backButton.onClick = [this] { navigateHistory (-1); };
    forwardButton.onClick = [this] { navigateHistory (1); };
    parentButton.onClick = [this]
    {
        const auto parent = contents.getDirectory().getParentDirectory();
        if (parent != contents.getDirectory())
            navigate (parent, true);
    };
    previewButton.onClick = [this]
    {
        if (previewPlaying)
        {
            if (onPreviewStopRequested)
                onPreviewStopRequested();
            stopPreviewUi();
        }
        else if (const auto file = selectedFile(); file.existsAsFile())
        {
            if (onPreviewRequested)
                onPreviewRequested (file);
        }
    };

    addAndMakeVisible (pathLabel);
    pathLabel.setFont (Fonts::small());
    pathLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.48f));
    pathLabel.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (listBox);
    listBox.setRowHeight (30);
    listBox.setMultipleSelectionEnabled (false);
    listBox.setColour (juce::ListBox::backgroundColourId, Theme::timelineBg);
    listBox.setColour (juce::ListBox::outlineColourId, juce::Colours::white.withAlpha (0.07f));
    listBox.setOutlineThickness (1);

    for (auto* label : { &selectedNameLabel, &durationLabel, &statusLabel })
    {
        addAndMakeVisible (*label);
        label->setFont (Fonts::small());
        label->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.66f));
    }
    selectedNameLabel.setFont (Fonts::body());
    selectedNameLabel.setText (jp (u8"ファイルを選択"), juce::dontSendNotification);
    durationLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setColour (juce::Label::textColourId, Theme::warning);
    statusLabel.setJustificationType (juce::Justification::centredLeft);

    navigate (AudioBrowserNavigation::initialDirectory(), true);
}

AudioFileBrowserView::~AudioFileBrowserView()
{
    contents.removeChangeListener (this);
    contents.clear();
    directoryThread.stopThread (2000);
}

void AudioFileBrowserView::navigate (const juce::File& directory, bool addToHistory)
{
    if (addToHistory)
        history.visit (directory);
    contents.setDirectory (directory, true, true);
    listBox.deselectAllRows();
    pathLabel.setText (directory.getFullPathName(), juce::dontSendNotification);
    refreshSelection();
    updateButtons();
}

void AudioFileBrowserView::navigateHistory (int delta)
{
    if (const auto directory = history.move (delta); directory != juce::File())
        navigate (directory, false);
}

int AudioFileBrowserView::getNumRows()
{
    return contents.getNumFiles();
}

void AudioFileBrowserView::paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                                             bool selected)
{
    juce::DirectoryContentsList::FileInfo info;
    if (! contents.getFileInfo (row, info))
        return;
    if (selected)
    {
        g.setColour (Theme::chooserRowSelected);
        g.fillRect (0, 0, width, height);
    }
    g.setColour (juce::Colours::white.withAlpha (selected ? 0.94f : 0.72f));
    g.setFont (Fonts::body());
    const auto prefix = info.isDirectory ? juce::String::fromUTF8 (u8"▸  ") : juce::String::fromUTF8 (u8"♫  ");
    g.drawText (prefix + info.filename, 9, 0, width - 18, height,
                juce::Justification::centredLeft, true);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawHorizontalLine (height - 1, 8.0f, (float) width);
}

void AudioFileBrowserView::selectedRowsChanged (int)
{
    refreshSelection();
}

void AudioFileBrowserView::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (importInProgress)
        return;
    const auto file = contents.getFile (row);
    if (file.isDirectory())
        navigate (file, true);
    else if (AudioFileTypes::isSupported (file) && onImportRequested)
        onImportRequested (file);
}

juce::var AudioFileBrowserView::getDragSourceDescription (const juce::SparseSet<int>& rows)
{
    if (importInProgress || rows.size() != 1)
        return {};
    const auto file = contents.getFile (rows[0]);
    return file.existsAsFile() && AudioFileTypes::isSupported (file)
               ? juce::var (file.getFullPathName())
               : juce::var();
}

void AudioFileBrowserView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    listBox.updateContent();
    listBox.repaint();
}

juce::File AudioFileBrowserView::selectedFile() const
{
    const int row = listBox.getSelectedRow();
    return row >= 0 ? contents.getFile (row) : juce::File();
}

juce::String AudioFileBrowserView::durationText (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return {};
    const auto seconds = (int) std::llround ((double) reader->lengthInSamples / reader->sampleRate);
    return juce::String::formatted ("%d:%02d", seconds / 60, seconds % 60);
}

void AudioFileBrowserView::refreshSelection()
{
    stopPreviewUi();
    if (onPreviewStopRequested)
        onPreviewStopRequested();
    const auto file = selectedFile();
    const bool audio = file.existsAsFile() && AudioFileTypes::isSupported (file);
    selectedNameLabel.setText (audio ? file.getFileName() : jp (u8"ファイルを選択"),
                               juce::dontSendNotification);
    durationLabel.setText (audio ? durationText (file) : juce::String(),
                           juce::dontSendNotification);
    previewButton.setEnabled (audio && ! importInProgress);
}

void AudioFileBrowserView::setPreviewState (bool loading, bool playing, const juce::String& error)
{
    previewPlaying = loading || playing;
    previewButton.setButtonText (previewPlaying ? jp (u8"停止") : jp (u8"再生"));
    statusLabel.setText (loading ? jp (u8"読み込み中…") : error, juce::dontSendNotification);
}

void AudioFileBrowserView::stopPreviewUi()
{
    previewPlaying = false;
    previewButton.setButtonText (jp (u8"再生"));
    statusLabel.setText ({}, juce::dontSendNotification);
}

void AudioFileBrowserView::setImporting (bool importing)
{
    importInProgress = importing;
    listBox.setEnabled (! importing);
    previewButton.setEnabled (! importing && selectedFile().existsAsFile());
}

void AudioFileBrowserView::updateButtons()
{
    backButton.setEnabled (history.canMove (-1));
    forwardButton.setEnabled (history.canMove (1));
    parentButton.setEnabled (contents.getDirectory().getParentDirectory() != contents.getDirectory());
}

void AudioFileBrowserView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::chooserPanelBg);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.drawHorizontalLine (getHeight() - 78, 0.0f, (float) getWidth());
}

void AudioFileBrowserView::resized()
{
    auto area = getLocalBounds();
    auto nav = area.removeFromTop (32);
    backButton.setBounds (nav.removeFromLeft (30).reduced (2));
    forwardButton.setBounds (nav.removeFromLeft (30).reduced (2));
    parentButton.setBounds (nav.removeFromLeft (30).reduced (2));
    pathLabel.setBounds (nav.reduced (6, 0));

    auto preview = area.removeFromBottom (78).reduced (10, 7);
    auto line = preview.removeFromTop (25);
    durationLabel.setBounds (line.removeFromRight (52));
    selectedNameLabel.setBounds (line);
    preview.removeFromTop (4);
    previewButton.setBounds (preview.removeFromRight (64).reduced (0, 2));
    statusLabel.setBounds (preview);
    listBox.setBounds (area.reduced (8, 4));
}
