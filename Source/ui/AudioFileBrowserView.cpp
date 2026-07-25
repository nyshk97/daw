#include "AudioFileBrowserView.h"

#include "../shared/AudioFileTypes.h"
#include "Fonts.h"
#include "Theme.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }
}

// 行の左端アイコンのクリックだけを受ける当たり判定。行そのものは透過させたままにして、
// 選択・タイムラインへのドラッグ取り込み（ListBox内部の行コンポーネントが担う）を壊さない
class AudioFileBrowserView::RowIconComponent final : public juce::Component
{
public:
    RowIconComponent()
    {
        setInterceptsMouseClicks (false, true); // 自分は透過、子（アイコン領域）だけ受ける
        addChildComponent (hit);
    }

    // ListBoxは行コンポーネントをスクロール時に別の行へ使い回すので、呼ばれるたびに行番号を書き直す
    void update (int rowNumber, bool playable, std::function<void (int)> onClick)
    {
        hit.row = rowNumber;
        hit.onClick = std::move (onClick);
        hit.setVisible (playable); // フォルダ行はクリックを横取りしない
    }

    void resized() override { hit.setBounds (0, 0, iconHitWidth, getHeight()); }

private:
    struct HitArea final : public juce::Component
    {
        void mouseDown (const juce::MouseEvent&) override
        {
            if (onClick != nullptr)
                onClick (row);
        }

        std::function<void (int)> onClick;
        int row = -1;
    };

    HitArea hit;
};

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

    addAndMakeVisible (breadcrumb);
    breadcrumb.onNavigate = [this] (const juce::File& dir) { navigate (dir, true); };

    // オートプレビューのON/OFF。セッション内だけ保持し、起動時は常にON
    addAndMakeVisible (autoPreviewToggle);
    autoPreviewToggle.setBorderless (true);
    autoPreviewToggle.setWantsKeyboardFocus (false);
    autoPreviewToggle.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    autoPreviewToggle.setToggleIconColour (Theme::panelToggleOn);
    autoPreviewToggle.setToggleState (policy.isEnabled(), juce::dontSendNotification);
    autoPreviewToggle.setTooltip (jp (u8"選択したファイルを自動で試聴"));
    autoPreviewToggle.onClick = [this]
    {
        const bool on = ! policy.isEnabled();
        autoPreviewToggle.setToggleState (on, juce::dontSendNotification);
        apply (policy.setEnabled (on));
    };

    addAndMakeVisible (listBox);
    listBox.addMouseListener (this, true); // ホバー行の追跡（イベントは奪わず監視するだけ）
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
    stopTimer();
    contents.removeChangeListener (this);
    contents.clear();
    directoryThread.stopThread (2000);
}

void AudioFileBrowserView::navigate (const juce::File& directory, bool addToHistory)
{
    if (addToHistory)
        history.visit (directory);
    contents.setDirectory (directory, true, true);
    setHoveredRow (-1); // 移動先では行が総入れ替えになるので、古い行の▶を残さない
    listBox.deselectAllRows();
    breadcrumb.setPath (directory);
    refreshSelection();
}

bool AudioFileBrowserView::navigateHistory (int delta)
{
    const auto directory = history.move (delta);
    if (directory == juce::File())
        return false;
    navigate (directory, false);
    return true;
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

    const auto textColour = juce::Colours::white.withAlpha (selected ? 0.94f : 0.72f);
    const auto iconArea = juce::Rectangle<int> (iconAreaX, 0, iconAreaWidth, height);
    const auto file = contents.getFile (row);
    g.setFont (Fonts::body());

    if (info.isDirectory)
    {
        g.setColour (textColour);
        g.drawText (jp (u8"▸"), iconArea, juce::Justification::centred, false);
    }
    else if (policy.isActive (file))
    {
        // 試聴中（loading含む）の行は■。停止できることを示すためアクセント色で出す
        const float side = 9.0f;
        g.setColour (Theme::panelToggleOn);
        g.fillRoundedRectangle (juce::Rectangle<float> (side, side)
                                    .withCentre (iconArea.toFloat().getCentre()),
                                side * 0.14f);
    }
    else if (row == hoveredRow)
    {
        const float side = 10.0f;
        const auto box = juce::Rectangle<float> (side, side)
                             .withCentre (iconArea.toFloat().getCentre());
        juce::Path play; // 三角は左に重く見えるので少し右へ寄せる
        play.addTriangle (box.getX(), box.getY(), box.getX(), box.getBottom(),
                          box.getRight(), box.getCentreY());
        play.applyTransform (juce::AffineTransform::translation (side * 0.08f, 0.0f));
        g.setColour (juce::Colours::white.withAlpha (0.94f));
        g.fillPath (play);
    }
    else
    {
        g.setColour (textColour);
        g.drawText (jp (u8"♫"), iconArea, juce::Justification::centred, false);
    }

    g.setColour (textColour);
    const int textX = iconArea.getRight() + 6;
    g.drawText (info.filename, textX, 0, width - textX - 9, height,
                juce::Justification::centredLeft, true);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawHorizontalLine (height - 1, 8.0f, (float) width);
}

juce::Component* AudioFileBrowserView::refreshComponentForRow (int row, bool,
                                                               juce::Component* existing)
{
    std::unique_ptr<juce::Component> owned (existing);
    if (! juce::isPositiveAndBelow (row, getNumRows()))
        return nullptr;

    auto* iconRow = dynamic_cast<RowIconComponent*> (owned.get());
    if (iconRow == nullptr)
    {
        iconRow = new RowIconComponent();
        owned.reset (iconRow);
    }
    iconRow->update (row, isPlayable (contents.getFile (row)),
                     [this] (int clicked) { iconClicked (clicked); });
    return owned.release();
}

void AudioFileBrowserView::iconClicked (int row)
{
    const auto file = contents.getFile (row);
    if (! isPlayable (file))
        return;

    stopTimer();                 // 予約中のオートプレビューは捨てる
    suppressAutoPreview = true;  // 選択変更で「停止＋再予約」が走らないようにする
    listBox.selectRow (row);     // フッターのファイル名・長さを鳴らす行に合わせる
    suppressAutoPreview = false;
    apply (policy.iconClicked (file));
}

void AudioFileBrowserView::mouseMove (const juce::MouseEvent& event)
{
    updateHover (event);
}

void AudioFileBrowserView::mouseDrag (const juce::MouseEvent& event)
{
    updateHover (event);
}

void AudioFileBrowserView::mouseExit (const juce::MouseEvent& event)
{
    updateHover (event);
}

void AudioFileBrowserView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&)
{
    setHoveredRow (-1); // スクロールで行がずれるので一度消す（次のmouseMoveで付け直る）
}

void AudioFileBrowserView::updateHover (const juce::MouseEvent& event)
{
    const auto position = event.getEventRelativeTo (&listBox).getPosition();
    const bool inside = listBox.getLocalBounds().contains (position);
    setHoveredRow (inside ? listBox.getRowContainingPosition (position.x, position.y) : -1);
}

void AudioFileBrowserView::setHoveredRow (int row)
{
    if (row == hoveredRow)
        return;
    const int previous = hoveredRow;
    hoveredRow = row;
    if (previous >= 0)
        listBox.repaintRow (previous);
    if (hoveredRow >= 0)
        listBox.repaintRow (hoveredRow);
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

bool AudioFileBrowserView::isPlayable (const juce::File& file) const
{
    return ! importInProgress && file.existsAsFile() && AudioFileTypes::isSupported (file);
}

// 判定（PreviewPolicy）と副作用（再生・タイマー・再描画）の境目。停止は開始より先に流す
void AudioFileBrowserView::apply (const PreviewPolicy::Result& result)
{
    if (result.stopTimer)
        stopTimer();
    if (result.stopPreview)
    {
        if (onPreviewStopRequested)
            onPreviewStopRequested();
        stopPreviewUi();
    }
    if (result.startTimer)
        startTimer (autoPreviewDelayMs);
    if (result.startPreview && onPreviewRequested)
        onPreviewRequested (result.startFile);
    for (const auto& file : result.repaint)
        repaintRowFor (file);
}

void AudioFileBrowserView::repaintRowFor (const juce::File& file)
{
    for (int row = 0; row < contents.getNumFiles(); ++row)
        if (contents.getFile (row) == file)
        {
            listBox.repaintRow (row);
            return;
        }
}

void AudioFileBrowserView::timerCallback()
{
    stopTimer();
    apply (policy.takePending());
}

void AudioFileBrowserView::setTransportRunning (bool running)
{
    apply (policy.setTransportRunning (running));
}

void AudioFileBrowserView::cancelPreview()
{
    apply (policy.cancelAll());
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
    const auto file = selectedFile();
    const bool audio = isPlayable (file);
    selectedNameLabel.setText (audio ? file.getFileName() : jp (u8"ファイルを選択"),
                               juce::dontSendNotification);
    durationLabel.setText (audio ? durationText (file) : juce::String(),
                           juce::dontSendNotification);
    // 行アイコン由来の選択変更（iconClicked）では、そちらが開始・停止まで面倒を見る
    if (! suppressAutoPreview)
        apply (policy.selectionChanged (file, audio));
}

void AudioFileBrowserView::setPreviewState (bool loading, bool playing, const juce::String& error)
{
    // 末尾到達・失敗でAudioFilePreviewが自分でidleに戻るため、ここで試聴対象を解除する
    apply (policy.previewStateChanged (loading, playing));
    statusLabel.setText (loading ? jp (u8"読み込み中…") : error, juce::dontSendNotification);
}

void AudioFileBrowserView::stopPreviewUi()
{
    statusLabel.setText ({}, juce::dontSendNotification);
}

void AudioFileBrowserView::setImporting (bool importing)
{
    const bool changed = importing != importInProgress;
    importInProgress = importing;
    listBox.setEnabled (! importing);
    // 取り込みを始めたら予約中・再生中の試聴をまとめて畳む（毎フレーム呼ばれるので遷移時だけ）
    if (changed && importing)
        apply (policy.cancelAll());
}

void AudioFileBrowserView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::chooserPanelBg);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.drawHorizontalLine (getHeight() - footerHeight, 0.0f, (float) getWidth());
}

void AudioFileBrowserView::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop (32);
    autoPreviewToggle.setBounds (top.removeFromRight (30).reduced (2, 3));
    breadcrumb.setBounds (top.reduced (8, 0));

    auto footer = area.removeFromBottom (footerHeight).reduced (10, 6);
    auto line = footer.removeFromTop (24);
    durationLabel.setBounds (line.removeFromRight (52));
    selectedNameLabel.setBounds (line);
    footer.removeFromTop (2);
    statusLabel.setBounds (footer);
    listBox.setBounds (area.reduced (8, 4));
}
