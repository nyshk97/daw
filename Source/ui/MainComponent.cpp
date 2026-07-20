#include "MainComponent.h"

#include <cmath>

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

void showAlert (const juce::String& title, const juce::String& message)
{
    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                 title, message);
}
}

MainComponent::MainComponent (std::unique_ptr<Project> projectToOpen)
    : project (std::move (projectToOpen))
{
    jassert (project != nullptr);
    transport.bpm.store (project->bpm);

    addAndMakeVisible (timeline);
    addAndMakeVisible (headers);
    addAndMakeVisible (playButton);
    addAndMakeVisible (recordButton);
    addAndMakeVisible (addTrackButton);
    addAndMakeVisible (settingsButton);
    addAndMakeVisible (clickButton);
    addAndMakeVisible (bpmCaption);
    addAndMakeVisible (bpmValue);
    addAndMakeVisible (positionLabel);
    addChildComponent (srWarningLabel); // 不一致時のみ表示

    timeline.setProject (project.get());
    headers.setProject (project.get());

    // ---- タイムライン・ヘッダの連携 ----
    timeline.onSeek = [this] (juce::int64 samplePos)
    {
        if (! engine.isRecording())
            transport.seekRequest.store (samplePos);
    };
    timeline.onTrackSelected = [this] (int index) { selectTrack (index); };
    timeline.onVerticalScroll = [this] (int y) { headers.setViewY (y); };
    headers.onSelect = [this] (int index) { selectTrack (index); };
    headers.onDeleteRequested = [this] (int index) { requestDeleteTrack (index); };
    headers.onChanged = [this] { setDirty (true); };
    headers.onWheel = [this] (float deltaY) { timeline.scrollVertically (deltaY); };

    // ---- トランスポートバー ----
    playButton.setButtonText (jp (u8"再生"));
    playButton.onClick = [this] { togglePlay(); };

    recordButton.setButtonText (jp (u8"録音"));
    recordButton.onClick = [this] { toggleRecord(); };

    addTrackButton.setButtonText (jp (u8"＋トラック"));
    addTrackButton.onClick = [this] { addTrack(); };

    settingsButton.setButtonText (jp (u8"デバイス設定"));
    settingsButton.onClick = [this] { showDeviceSettings(); };

    clickButton.setButtonText (jp (u8"クリック"));
    clickButton.onClick = [this] { transport.clickEnabled.store (clickButton.getToggleState()); };

    bpmCaption.setText ("BPM", juce::dontSendNotification);
    bpmCaption.setJustificationType (juce::Justification::centredRight);
    bpmValue.setEditable (true, false, false);
    bpmValue.setText (juce::String (project->bpm), juce::dontSendNotification);
    bpmValue.setJustificationType (juce::Justification::centred);
    bpmValue.setColour (juce::Label::outlineColourId, juce::Colour (0xff55555a));
    bpmValue.onTextChange = [this] { applyBpmText(); };

    positionLabel.setJustificationType (juce::Justification::centredLeft);

    srWarningLabel.setColour (juce::Label::textColourId, juce::Colours::orangered);
    srWarningLabel.setJustificationType (juce::Justification::centredLeft);

    // Space（再生/停止）をボタンに奪わせない
    for (auto* c : std::initializer_list<juce::Component*> {
             &playButton, &recordButton, &addTrackButton, &settingsButton, &clickButton })
    {
        c->setWantsKeyboardFocus (false);
        c->setMouseClickGrabsKeyboardFocus (false);
    }

    selectTrack (project->tracks.empty() ? -1 : 0);
    pushSnapshot();
    updateTransportButtons();

    setWantsKeyboardFocus (true);
    setSize (1100, 700);
    setAudioChannels (1, 2); // 入力1ch（マイク）・出力2ch
    startTimerHz (30);       // GOTCHAS.md: 通知はpush型でなくpull型（Timerポーリング）
}

MainComponent::~MainComponent()
{
    // engine・snapshots より先にオーディオコールバックを止める
    shutdownAudio();
}

// ---- オーディオコールバック（audio/ へ転送するだけ）----

void MainComponent::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    engine.prepareToPlay (samplesPerBlockExpected, sampleRate);
}

void MainComponent::getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill)
{
    engine.process (bufferToFill);
}

void MainComponent::releaseResources()
{
    engine.releaseResources();
}

// ---- 定期更新 ----

void MainComponent::timerCallback()
{
    if (! focusGrabbed && isShowing())
    {
        grabKeyboardFocus(); // Space等のキーを受けるため一度だけ取る
        focusGrabbed = true;
    }

    snapshots.deleteRetired(); // 旧スナップショットの解放は必ずメッセージスレッドで

    meterLevel = transport.inputPeakLevel.load();
    updatePositionLabel();
    updateTransportButtons();
    updateSampleRateWarning();
    repaint (meterArea);
}

// ---- 再生・録音 ----

void MainComponent::togglePlay()
{
    if (engine.isRecording())
        finishRecording();
    else if (transport.isPlaying.load())
        engine.stop();
    else
        engine.play();
    updateTransportButtons();
}

void MainComponent::toggleRecord()
{
    if (engine.isRecording())
        finishRecording();
    else
        startRecordingFlow();
}

void MainComponent::startRecordingFlow()
{
    if (selectedTrack < 0 || selectedTrack >= (int) project->tracks.size())
    {
        showAlert (jp (u8"録音できません"), jp (u8"録音先のトラックがありません。トラックを追加してください。"));
        return;
    }

    const double deviceRate = transport.sampleRate.load();
    if (deviceRate <= 0.0)
    {
        showAlert (jp (u8"録音できません"), jp (u8"オーディオデバイスが準備できていません。"));
        return;
    }

    if (project->sampleRate <= 0.0)
    {
        project->sampleRate = deviceRate; // 最初の録音でプロジェクトのレートを確定
        setDirty (true);
    }
    else if (std::abs (project->sampleRate - deviceRate) > 0.5)
    {
        showAlert (jp (u8"サンプルレート不一致"),
                   jp (u8"プロジェクト ") + juce::String (project->sampleRate, 0) + " Hz / "
                       + jp (u8"デバイス ") + juce::String (deviceRate, 0) + " Hz\n"
                       + jp (u8"デバイス設定でサンプルレートを合わせてから録音してください。"));
        return;
    }

    // 録音開始位置 = 再生ヘッドがいる小節の頭（シークは小節スナップ済みなので通常は一致する）
    const double barLen = timeline.barLengthSamples();
    const auto playhead = juce::jmax ((juce::int64) 0, transport.playheadSamplePos.load());
    const auto bar = (juce::int64) std::floor ((double) playhead / barLen);
    const auto punchIn = (juce::int64) std::llround ((double) bar * barLen);

    pendingRecordFile = project->nextClipFile();
    pendingPunchIn = punchIn;
    pendingRecordTrack = selectedTrack;

    if (! engine.startRecording (pendingRecordFile, punchIn,
                                 (juce::int64) std::llround (barLen), deviceRate))
    {
        showAlert (jp (u8"録音できません"), jp (u8"録音ファイルを作成できませんでした。"));
        return;
    }
    updateTransportButtons();
}

void MainComponent::finishRecording()
{
    engine.stopRecording();
    engine.stop();

    const auto recordedLength = transport.recordedSamples.load();

    if (recordedLength <= 0)
    {
        // カウントイン中に止めた等、何も録れていない
        pendingRecordFile.deleteFile();
    }
    else
    {
        Clip clip;
        clip.fileName = pendingRecordFile.getFileName();
        clip.startSample = pendingPunchIn;
        clip.audio = Project::loadWavMono (pendingRecordFile);

        if (clip.audio != nullptr
            && pendingRecordTrack >= 0 && pendingRecordTrack < (int) project->tracks.size())
        {
            clip.buildPeakCache();
            project->tracks[(size_t) pendingRecordTrack].clips.push_back (std::move (clip));
            pushSnapshot();
            setDirty (true);
        }
        else
        {
            showAlert (jp (u8"録音エラー"), jp (u8"録音ファイルの読み込みに失敗しました。"));
        }
    }

    pendingRecordTrack = -1;
    timeline.refresh();
    updateTransportButtons();
}

// ---- 編集操作 ----

void MainComponent::requestDeleteSelectedClip()
{
    const auto sel = timeline.getSelection();
    if (! sel.isValid() || engine.isRecording())
        return;

    juce::NativeMessageBox::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle (jp (u8"クリップ削除"))
            .withMessage (jp (u8"選択中のクリップを削除しますか？"))
            .withButton (jp (u8"削除"))
            .withButton (jp (u8"キャンセル")),
        [this, sel] (int result)
        {
            if (result != 0)
                return;
            // 非同期ダイアログの間にモデルが変わっている可能性があるので再検証
            if (sel.track >= (int) project->tracks.size())
                return;
            auto& clips = project->tracks[(size_t) sel.track].clips;
            if (sel.clip >= (int) clips.size())
                return;

            clips.erase (clips.begin() + sel.clip);
            timeline.clearSelection();
            pushSnapshot();
            setDirty (true);
            timeline.refresh();
        });
}

void MainComponent::requestDeleteTrack (int index)
{
    if (engine.isRecording() || index < 0 || index >= (int) project->tracks.size())
        return;

    juce::NativeMessageBox::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle (jp (u8"トラック削除"))
            .withMessage (jp (u8"「") + project->tracks[(size_t) index].name
                          + jp (u8"」を削除しますか？トラック上のクリップも削除されます。"))
            .withButton (jp (u8"削除"))
            .withButton (jp (u8"キャンセル")),
        [this, index] (int result)
        {
            if (result != 0 || index >= (int) project->tracks.size())
                return;

            project->tracks.erase (project->tracks.begin() + index);
            timeline.clearSelection();
            headers.rebuild();
            selectTrack (juce::jmin (index, (int) project->tracks.size() - 1));
            pushSnapshot();
            setDirty (true);
            timeline.refresh();
        });
}

void MainComponent::addTrack()
{
    if (engine.isRecording())
        return;

    Track track;
    track.name = jp (u8"トラック ") + juce::String ((int) project->tracks.size() + 1);
    project->tracks.push_back (std::move (track));

    headers.rebuild();
    selectTrack ((int) project->tracks.size() - 1);
    pushSnapshot();
    setDirty (true);
    timeline.refresh();
}

void MainComponent::selectTrack (int index)
{
    selectedTrack = project->tracks.empty()
        ? -1
        : juce::jlimit (0, (int) project->tracks.size() - 1, index);
    headers.setSelectedTrack (selectedTrack);
    timeline.setSelectedTrack (selectedTrack);
}

void MainComponent::showDeviceSettings()
{
    if (engine.isRecording())
        return;

    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        deviceManager, 1, 2, 2, 2, false, false, true, false);
    selector->setSize (500, 400);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector.release());
    options.dialogTitle = jp (u8"オーディオデバイス設定");
    options.dialogBackgroundColour = getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void MainComponent::applyBpmText()
{
    const double value = bpmValue.getText().getDoubleValue();
    if (value < 30.0 || value > 300.0)
    {
        bpmValue.setText (juce::String (transport.bpm.load()), juce::dontSendNotification);
        return;
    }
    transport.bpm.store (value);
    project->bpm = value;
    setDirty (true);
    timeline.refresh(); // 小節幅（サンプル換算）が変わる
}

// ---- 保存 ----

bool MainComponent::trySave()
{
    if (engine.isRecording())
        return false;

    project->bpm = transport.bpm.load();
    juce::String error;
    if (! project->save (error))
    {
        showAlert (jp (u8"保存に失敗しました"), error);
        return false;
    }
    setDirty (false);
    return true;
}

juce::String MainComponent::windowTitle() const
{
    return jp (u8"daw — ") + project->name() + (dirty ? jp (u8" ●") : juce::String());
}

void MainComponent::setDirty (bool nowDirty)
{
    if (dirty == nowDirty)
        return;
    dirty = nowDirty;
    if (onTitleChanged)
        onTitleChanged (windowTitle());
}

void MainComponent::pushSnapshot()
{
    snapshots.push (project->buildSnapshot());
}

// ---- キーボード ----

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        togglePlay();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::deleteKey
        || key.getKeyCode() == juce::KeyPress::backspaceKey)
    {
        requestDeleteSelectedClip();
        return true;
    }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0))
    {
        trySave();
        return true;
    }
    if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0))
    {
        addTrack();
        return true;
    }
    // Logic準拠: ⌘←/→ = 横ズームアウト/イン
    if (key == juce::KeyPress (juce::KeyPress::leftKey, juce::ModifierKeys::commandModifier, 0))
    {
        timeline.zoomBy (1.0 / juce::MathConstants<double>::sqrt2);
        return true;
    }
    if (key == juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::commandModifier, 0))
    {
        timeline.zoomBy (juce::MathConstants<double>::sqrt2);
        return true;
    }

    // 修飾キーなしの1文字ショートカット（Logic準拠: ,/.でシーク、mでミュート、rで録音）
    if (! key.getModifiers().testFlags (juce::ModifierKeys::commandModifier
                                        | juce::ModifierKeys::ctrlModifier
                                        | juce::ModifierKeys::altModifier))
    {
        switch (key.getTextCharacter())
        {
            case ',': seekByBar (-1); return true;
            case '.': seekByBar (1); return true;
            case 'm': toggleMuteSelectedTrack(); return true;
            case 'r': toggleRecord(); return true;
            default: break;
        }
    }
    return false;
}

void MainComponent::seekByBar (int direction)
{
    if (engine.isRecording())
        return;

    const double barLen = timeline.barLengthSamples();
    const auto pos = juce::jmax ((juce::int64) 0, transport.playheadSamplePos.load());
    auto bar = (juce::int64) std::floor ((double) pos / barLen);

    if (direction > 0)
    {
        ++bar;
    }
    else
    {
        // 小節の途中なら小節頭へ、すでに小節頭なら前の小節へ
        const auto barStart = (juce::int64) std::llround ((double) bar * barLen);
        if ((double) (pos - barStart) < barLen * 0.01)
            --bar;
        bar = juce::jmax ((juce::int64) 0, bar);
    }

    transport.seekRequest.store ((juce::int64) std::llround ((double) bar * barLen));
}

void MainComponent::toggleMuteSelectedTrack()
{
    if (selectedTrack < 0 || selectedTrack >= (int) project->tracks.size())
        return;

    auto& params = *project->tracks[(size_t) selectedTrack].params;
    params.mute.store (! params.mute.load());
    headers.refreshValues();
    setDirty (true);
}

// ---- 表示更新 ----

void MainComponent::updateTransportButtons()
{
    const bool recording = engine.isRecording();
    playButton.setButtonText (transport.isPlaying.load() ? jp (u8"停止") : jp (u8"再生"));
    recordButton.setButtonText (recording ? jp (u8"録音停止") : jp (u8"録音"));
    recordButton.setColour (juce::TextButton::buttonColourId,
                            recording ? juce::Colours::darkred
                                      : getLookAndFeel().findColour (juce::TextButton::buttonColourId));
}

void MainComponent::updatePositionLabel()
{
    const double barLen = timeline.barLengthSamples();
    const double beatLen = barLen / 4.0;
    const double sr = timeline.effectiveSampleRate();
    const auto playhead = juce::jmax ((juce::int64) 0, transport.playheadSamplePos.load());

    const int bar = (int) std::floor ((double) playhead / barLen) + 1;
    const int beat = (int) std::floor (std::fmod ((double) playhead, barLen) / beatLen) + 1;
    const double seconds = (double) playhead / sr;
    const int minutes = (int) (seconds / 60.0);

    positionLabel.setText (juce::String (bar) + "." + juce::String (beat)
                               + "  |  " + juce::String (minutes) + ":"
                               + juce::String (seconds - minutes * 60.0, 1),
                           juce::dontSendNotification);
}

void MainComponent::updateSampleRateWarning()
{
    const double deviceRate = transport.sampleRate.load();
    const bool mismatch = project->sampleRate > 0.0 && deviceRate > 0.0
                          && std::abs (project->sampleRate - deviceRate) > 0.5;
    if (mismatch)
        srWarningLabel.setText (jp (u8"SR不一致: プロジェクト ") + juce::String (project->sampleRate, 0)
                                    + jp (u8" / デバイス ") + juce::String (deviceRate, 0),
                                juce::dontSendNotification);
    srWarningLabel.setVisible (mismatch);
}

// ---- 描画・レイアウト ----

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    // 入力レベルメーター（録音のゲイン確認用に1本だけ）
    g.setColour (juce::Colours::darkgrey);
    g.fillRect (meterArea);
    g.setColour (meterLevel > 0.9f ? juce::Colours::red : juce::Colours::limegreen);
    auto bar = meterArea;
    g.fillRect (bar.removeFromLeft ((int) (juce::jmin (1.0f, meterLevel) * (float) meterArea.getWidth())));
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    auto topRow = area.removeFromTop (44).reduced (8, 7);
    playButton.setBounds (topRow.removeFromLeft (80));
    topRow.removeFromLeft (6);
    recordButton.setBounds (topRow.removeFromLeft (80));
    topRow.removeFromLeft (14);
    clickButton.setBounds (topRow.removeFromLeft (86));
    topRow.removeFromLeft (6);
    bpmCaption.setBounds (topRow.removeFromLeft (40));
    topRow.removeFromLeft (4);
    bpmValue.setBounds (topRow.removeFromLeft (56));
    topRow.removeFromLeft (14);
    positionLabel.setBounds (topRow.removeFromLeft (140));

    settingsButton.setBounds (topRow.removeFromRight (110));
    topRow.removeFromRight (10);
    meterArea = topRow.removeFromRight (120).reduced (0, 6);
    topRow.removeFromRight (10);
    srWarningLabel.setBounds (topRow);

    auto bottomRow = area.removeFromBottom (36).reduced (8, 4);
    addTrackButton.setBounds (bottomRow.removeFromLeft (TrackHeadersView::preferredWidth - 8));

    auto headerColumn = area.removeFromLeft (TrackHeadersView::preferredWidth);
    headerColumn.removeFromTop (TimelineView::rulerHeight); // ルーラー分の高さを合わせる
    headers.setBounds (headerColumn);
    timeline.setBounds (area);
}
