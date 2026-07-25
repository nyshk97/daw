#pragma once

#include <functional>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/AudioBrowserNavigation.h"

class AudioFileBrowserView final : public juce::Component,
                                   private juce::ListBoxModel,
                                   private juce::ChangeListener
{
public:
    AudioFileBrowserView();
    ~AudioFileBrowserView() override;

    std::function<void (const juce::File&)> onPreviewRequested;
    std::function<void()> onPreviewStopRequested;
    std::function<void (const juce::File&)> onImportRequested;

    void setPreviewState (bool loading, bool playing, const juce::String& error);
    void setImporting (bool importing);
    void stopPreviewUi();

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class AudioFilter final : public juce::FileFilter
    {
    public:
        AudioFilter();
        bool isFileSuitable (const juce::File& file) const override;
        bool isDirectorySuitable (const juce::File& file) const override;
    };

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                           bool selected) override;
    void selectedRowsChanged (int row) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;
    bool mayDragToExternalWindows() const override { return false; }
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void navigate (const juce::File& directory, bool addToHistory);
    void navigateHistory (int delta);
    void refreshSelection();
    juce::File selectedFile() const;
    juce::String durationText (const juce::File& file);
    void updateButtons();

    AudioFilter filter;
    juce::TimeSliceThread directoryThread { "Audio browser directory scan" };
    juce::DirectoryContentsList contents { &filter, directoryThread };
    juce::AudioFormatManager formats;
    AudioBrowserNavigation::History history;
    bool importInProgress = false;
    bool previewPlaying = false;

    juce::TextButton backButton { juce::String::fromUTF8 (u8"‹") };
    juce::TextButton forwardButton { juce::String::fromUTF8 (u8"›") };
    juce::TextButton parentButton { juce::String::fromUTF8 (u8"↑") };
    juce::Label pathLabel;
    juce::ListBox listBox { {}, this };
    juce::Label selectedNameLabel;
    juce::Label durationLabel;
    juce::TextButton previewButton { juce::String::fromUTF8 (u8"再生") };
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioFileBrowserView)
};
