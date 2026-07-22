#include "MainComponent.h"

#include <cmath>

#include "../shared/Log.h"
#include "Fonts.h"
#include "Shortcuts.h"
#include "Theme.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

// ユーザーに見せるエラーは必ずログにも残す（ダイアログは閉じたら消えるため）
void showAlert (const juce::String& title, const juce::String& message)
{
    Log::error ("ui.alert", "title=" + title + " message=" + message.replace ("\n", " / "));
    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                 title, message);
}
}

MainComponent::MainComponent (std::unique_ptr<Project> projectToOpen)
    : project (std::move (projectToOpen))
{
    jassert (project != nullptr);
    Log::info ("project.open", "name=" + project->name()
                                   + " tracks=" + juce::String ((int) project->tracks.size())
                                   + " bpm=" + juce::String (project->bpm)
                                   + " sr=" + juce::String (project->sampleRate, 0));
    transport.bpm.store (project->bpm);

    addAndMakeVisible (timeline);
    addAndMakeVisible (headers);
    addChildComponent (pianoRoll); // リージョンを開いたときだけ表示
    addAndMakeVisible (playButton);
    addAndMakeVisible (recordButton);
    addAndMakeVisible (addTrackButton);
    addAndMakeVisible (settingsButton);
    addAndMakeVisible (clickButton);
    addChildComponent (addTrackOverlay); // トラック追加メニュー表示中のみ可視
    addChildComponent (shortcutOverlay); // ⌘?表示中のみ可視
    addAndMakeVisible (lcd);
    addChildComponent (srWarningLabel); // 不一致時のみ表示

    timeline.setProject (project.get());
    headers.setProject (project.get());
    pianoRoll.setProject (project.get());

    // ---- タイムライン・ヘッダの連携 ----
    timeline.onSeek = [this] (juce::int64 samplePos)
    {
        if (! engine.isRecording())
        {
            transport.seekRequest.store (samplePos);
            playStartSample = samplePos; // 再生中のクリックシークでも停止時の戻り先を更新する
        }
    };
    timeline.onTrackSelected = [this] (int index) { selectTrack (index); };
    timeline.onVerticalScroll = [this] (int y) { headers.setViewY (y); };
    timeline.onWillEditModel = [this] { undoStack.begin (*project); };
    timeline.onModelEdited = [this]
    {
        Log::info ("edit.timeline"); // 操作確定時（mouseUp等）に1回だけ来る
        pushSnapshot();
        setDirty (true);
        timeline.refresh();
    };
    timeline.onOpenRegion = [this] (int trackIndex, int regionIndex) { openPianoRoll (trackIndex, regionIndex); };
    timeline.onDeleteItemRequested = [this] (int trackIndex, int itemIndex)
    {
        if (trackIndex < 0 || trackIndex >= (int) project->tracks.size())
            return;
        if (project->tracks[(size_t) trackIndex].type == TrackType::midi)
            deleteRegionAt (trackIndex, itemIndex);
        else
            requestDeleteClipAt (trackIndex, itemIndex);
    };
    pianoRoll.onWillEditModel = [this] { undoStack.begin (*project); };
    pianoRoll.onModelEdited = [this]
    {
        Log::info ("edit.pianoroll"); // 操作確定時（mouseUp等）に1回だけ来る
        pushSnapshot();
        setDirty (true);
        timeline.refresh(); // リージョンのノートミニチュアを更新
    };
    pianoRoll.onPreviewNote = [this] (juce::uint64 trackId, int pitch, int velocity)
    {
        if (transport.isPlaying.load() || engine.isRecording())
            return;
        // 固定ピッチ打楽器（Kick等）はプレビューもその打楽器の音で鳴らす
        for (auto& track : project->tracks)
            if (track.id == trackId && track.type == TrackType::midi
                && track.drums && track.drumPitch >= 0)
                pitch = track.drumPitch;
        previewFifo.push ({ PreviewFifo::Command::Type::noteOn, trackId, pitch, velocity });
    };
    pianoRoll.onCloseRequested = [this] { closePianoRoll(); };
    headers.onSelect = [this] (int index) { selectTrack (index); };
    headers.onDeleteRequested = [this] (int index) { requestDeleteTrack (index); };
    headers.onChanged = [this] { setDirty (true); };
    headers.onWillChangeStructure = [this] { undoStack.begin (*project); };
    headers.onInstrumentChanged = [this]
    {
        Log::info ("track.instrument");
        pushSnapshot(); // SynthBank が楽器変更を検知して音源を差し替える
        setDirty (true);
    };
    headers.onWheel = [this] (float deltaY) { timeline.scrollVertically (deltaY); };

    // ---- トランスポートバー ----
    playButton.onClick = [this] { togglePlay(); };
    playButton.setTooltip (Shortcuts::tooltipText (Shortcuts::ID::playStop));

    recordButton.setIconColour (Theme::recordRed); // 待機中も録音ボタンと分かる赤
    recordButton.setColour (juce::TextButton::buttonOnColourId, Theme::recordActiveBg);
    recordButton.onClick = [this] { toggleRecord(); };
    recordButton.setTooltip (Shortcuts::tooltipText (Shortcuts::ID::record));

    addTrackButton.onClick = [this] { showAddTrackMenu(); };
    addTrackButton.setTooltip (jp (u8"トラックを追加"));
    addTrackOverlay.onPick = [this] (TrackType type) { addTrack (type); };

    settingsButton.onClick = [this] { showDeviceSettings(); };
    settingsButton.setTooltip (Shortcuts::tooltipText (Shortcuts::ID::audioSettings));
    settingsButton.setBorderless (true);

    clickButton.setClickingTogglesState (true); // ONで点灯（Logicのメトロノームボタン風）
    clickButton.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    clickButton.onClick = [this] { transport.clickEnabled.store (clickButton.getToggleState()); };
    clickButton.setTooltip (jp (u8"メトロノーム")); // ショートカットなし

    lcd.tempoLabel().setText (juce::String (project->bpm), juce::dontSendNotification);
    lcd.tempoLabel().onTextChange = [this] { applyBpmText(); };

    srWarningLabel.setFont (Fonts::body());
    srWarningLabel.setColour (juce::Label::textColourId, Theme::warning);
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
    Log::info ("project.close", "name=" + project->name() + " dirty=" + juce::String ((int) dirty));
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

    snapshots.deleteRetired(); // 旧スナップショット（＋退役したGM音源）の解放は必ずメッセージスレッドで

    // デバイスのサンプルレート確定・変更に追従してGM音源を作り直す（変更があったときだけ再push）
    if (synthBank.sync (*project, transport.sampleRate.load(), transport.blockSizeExpected.load()))
        pushSnapshot();

    // 音源生成の失敗をユーザーに通知（失敗はキャッシュされるので1件につき1回だけ出る）
    for (const auto& error : synthBank.takeCreateErrors())
        showAlert (jp (u8"ソフトウェア音源エラー"), error);

    // ,/. シークによる一時停止からの自動再開。いずれかのシークキーが押されている間（リピート中）は待ち続ける
    if (seekResumePending)
    {
        bool anyKeyDown = false;
        for (int i = 0; i < numSeekKeyCodes; ++i)
            anyKeyDown = anyKeyDown || juce::KeyPress::isKeyCurrentlyDown (seekKeyCodes[i]);

        if (anyKeyDown)
        {
            lastSeekKeyMs = juce::Time::getMillisecondCounter();
        }
        else if (juce::Time::getMillisecondCounter() - lastSeekKeyMs >= 200)
        {
            seekResumePending = false;
            numSeekKeyCodes = 0;
            Log::info ("transport.seek_resume", "pos=" + juce::String (transport.playheadSamplePos.load()));
            engine.play();
        }
    }

    headers.updateMeters();
    updateLcdTime();
    updateTransportButtons();
    applyProjectSampleRate();
    updateSampleRateWarning();
    logDeviceIfChanged();
    pollAudioAnomalies();
}

void MainComponent::applyProjectSampleRate()
{
    if (projectRateApplied)
        return;

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return; // デバイス確定待ち。確定後のtickで再試行する

    if (engine.isRecording())
        return; // 録音中のデバイス再起動は避け、録音終了後のtickに回す

    projectRateApplied = true;

    // SR未確定の新規プロジェクトは最初の録音でデバイスレートに合わせて確定するので何もしない
    if (project->sampleRate <= 0.0)
        return;

    if (std::abs (device->getCurrentSampleRate() - project->sampleRate) <= 0.5)
        return;

    bool supported = false;
    for (auto rate : device->getAvailableSampleRates())
        supported = supported || std::abs (rate - project->sampleRate) <= 0.5;
    if (! supported)
    {
        // 合わせられないデバイスではSR不一致警告が出たままになる（従来挙動）
        Log::warn ("audio.device.rate_unsupported", "name=" + device->getName()
                                                        + " projectSr=" + juce::String (project->sampleRate, 0));
        return;
    }

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.sampleRate = project->sampleRate;
    const auto error = deviceManager.setAudioDeviceSetup (setup, true);
    if (error.isNotEmpty())
        Log::warn ("audio.device.rate_change_failed", "error=" + error);
    else
        Log::info ("audio.device.rate_change", "sr=" + juce::String (project->sampleRate, 0));
}

void MainComponent::logDeviceIfChanged()
{
    // 起動ヘッダの後段: デバイス情報は prepareToPlay 後にしか確定しないため、ここで確定/変化を拾う
    const double sr = transport.sampleRate.load();
    const int blockSize = transport.blockSizeExpected.load();
    auto* device = deviceManager.getCurrentAudioDevice();
    const auto name = device != nullptr ? device->getName() : juce::String();

    if (juce::approximatelyEqual (sr, loggedSampleRate)
        && blockSize == loggedBlockSize && name == loggedDeviceName)
        return;

    if (name != loggedDeviceName)
        projectRateApplied = false; // 別デバイスに替わったらプロジェクトSR合わせをやり直す

    loggedSampleRate = sr;
    loggedBlockSize = blockSize;
    loggedDeviceName = name;
    Log::info ("audio.device", "name=" + (name.isEmpty() ? "(none)" : name)
                                   + " sr=" + juce::String (sr, 0)
                                   + " blockSize=" + juce::String (blockSize));
}

void MainComponent::pollAudioAnomalies()
{
    // オーディオスレッドはログを書けないので、atomic経由で受け取りここで集約する。
    // 連続発生してもログは2秒に1回・件数付きの1行に抑える
    pendingMidiDrops += transport.midiDroppedNoteOns.exchange (0);
    pendingRecordDrops += transport.recordDroppedBlocks.exchange (0);

    if (++anomalyFlushTicks < 60) // 30Hz × 60 = 2秒
        return;
    anomalyFlushTicks = 0;

    if (pendingMidiDrops > 0)
    {
        Log::warn ("audio.midi_overflow", "droppedNoteOns=" + juce::String (pendingMidiDrops));
        pendingMidiDrops = 0;
    }
    if (pendingRecordDrops > 0)
    {
        Log::warn ("audio.record_fifo_drop", "blocks=" + juce::String (pendingRecordDrops));
        pendingRecordDrops = 0;
    }
}

// ---- 再生・録音 ----

void MainComponent::togglePlay()
{
    if (engine.isRecording())
    {
        finishRecording();
    }
    else if (seekResumePending)
    {
        // シーク後の再開待ち中は見かけ上「再生中」なので、spaceは停止として扱う
        seekResumePending = false;
        numSeekKeyCodes = 0;
        Log::info ("transport.stop", "pos=" + juce::String (transport.playheadSamplePos.load()));
    }
    else if (transport.isPlaying.load())
    {
        engine.stop();
        transport.seekRequest.store (playStartSample); // 停止したら再生開始位置に戻す（Logicのデフォルト挙動）
        Log::info ("transport.stop", "pos=" + juce::String (transport.playheadSamplePos.load())
                                         + " returnTo=" + juce::String (playStartSample));
    }
    else
    {
        // 直前のシークがまだオーディオスレッドに適用されていなければ、そちらが実際の開始位置
        const auto pendingSeek = transport.seekRequest.load();
        playStartSample = pendingSeek != TransportState::kNoSeek
                              ? pendingSeek
                              : juce::jmax ((juce::int64) 0, transport.playheadSamplePos.load());
        Log::info ("transport.play", "pos=" + juce::String (playStartSample));
        engine.play();
    }
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
    seekResumePending = false; // 録音はカウントイン込みで自前のトランスポート制御を行う
    numSeekKeyCodes = 0;

    if (selectedTrack < 0 || selectedTrack >= (int) project->tracks.size())
    {
        showAlert (jp (u8"録音できません"), jp (u8"録音先のトラックがありません。トラックを追加してください。"));
        return;
    }

    if (selectedTrackIsMidi())
        return; // MIDIトラックには録音できない（録音ボタンも無効化済み）

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
        // 失敗理由は Recorder::startRecording が record.start_failed としてログ済み
        showAlert (jp (u8"録音できません"), jp (u8"録音ファイルを作成できませんでした。"));
        return;
    }
    Log::info ("record.start", "file=" + pendingRecordFile.getFileName()
                                   + " track=" + juce::String (selectedTrack)
                                   + " punchIn=" + juce::String (punchIn)
                                   + " sr=" + juce::String (deviceRate, 0));
    updateTransportButtons();
}

void MainComponent::finishRecording()
{
    engine.stopRecording();
    engine.stop();
    playStartSample = pendingPunchIn;
    transport.seekRequest.store (pendingPunchIn); // 停止で録音開始小節の頭に戻し、テイクをすぐ聴き直せるようにする

    const auto recordedLength = transport.recordedSamples.load();

    if (recordedLength <= 0)
    {
        // カウントイン中に止めた等、何も録れていない
        Log::info ("record.discard", "file=" + pendingRecordFile.getFileName());
        pendingRecordFile.deleteFile();
    }
    else
    {
        // timelineSamples = タイムライン上の録音区間長。FIFO drop があると実WAVはこれより短い
        Log::info ("record.stop", "file=" + pendingRecordFile.getFileName()
                                      + " timelineSamples=" + juce::String (recordedLength));
        Clip clip;
        clip.fileName = pendingRecordFile.getFileName();
        clip.startSample = pendingPunchIn;
        clip.audio = Project::loadWavMono (pendingRecordFile);
        if (clip.audio != nullptr)
            clip.lengthSamples = clip.audio->getNumSamples(); // 録音直後はWAV全長を参照

        if (clip.audio != nullptr
            && pendingRecordTrack >= 0 && pendingRecordTrack < (int) project->tracks.size())
        {
            clip.buildPeakCache();
            undoStack.begin (*project); // 録音＝クリップ追加もundo対象
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

void MainComponent::finishRecordingForClose()
{
    if (engine.isRecording())
        finishRecording();
}

// ---- 編集操作 ----

void MainComponent::requestDeleteSelectedClip()
{
    const auto sel = timeline.getSelection();
    if (sel.isValid())
        requestDeleteClipAt (sel.track, sel.clip);
}

void MainComponent::requestDeleteClipAt (int trackIndex, int clipIndex)
{
    if (engine.isRecording())
        return;
    if (trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    if (clipIndex < 0 || clipIndex >= (int) project->tracks[(size_t) trackIndex].clips.size())
        return;

    juce::NativeMessageBox::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle (jp (u8"クリップ削除"))
            .withMessage (jp (u8"選択中のクリップを削除しますか？"))
            .withButton (jp (u8"削除"))
            .withButton (jp (u8"キャンセル")),
        [this, trackIndex, clipIndex] (int result)
        {
            if (result != 0)
                return;
            // 非同期ダイアログの間にモデルが変わっている可能性があるので再検証
            if (trackIndex >= (int) project->tracks.size())
                return;
            auto& clips = project->tracks[(size_t) trackIndex].clips;
            if (clipIndex >= (int) clips.size())
                return;

            Log::info ("clip.delete", "track=" + juce::String (trackIndex)
                                          + " file=" + clips[(size_t) clipIndex].fileName);
            undoStack.begin (*project);
            clips.erase (clips.begin() + clipIndex);
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

            Log::info ("track.delete", "name=" + project->tracks[(size_t) index].name);
            undoStack.begin (*project);
            project->tracks.erase (project->tracks.begin() + index);
            timeline.clearSelection();
            headers.rebuild();
            selectTrack (juce::jmin (index, (int) project->tracks.size() - 1));
            pushSnapshot();
            setDirty (true);
            timeline.refresh();
        });
}

void MainComponent::showAddTrackMenu()
{
    if (engine.isRecording())
        return;

    addTrackOverlay.setBounds (getLocalBounds());
    addTrackOverlay.show (addTrackButton.getBounds());
}

void MainComponent::addTrack (TrackType type)
{
    if (engine.isRecording())
        return;

    Log::info ("track.add", juce::String ("type=") + (type == TrackType::midi ? "midi" : "audio"));
    undoStack.begin (*project);

    Track track;
    track.id = project->allocateId();
    track.type = type;
    track.name = (type == TrackType::midi ? jp (u8"MIDI ") : jp (u8"トラック "))
                     + juce::String ((int) project->tracks.size() + 1);
    project->tracks.push_back (std::move (track));

    headers.rebuild();
    selectTrack ((int) project->tracks.size() - 1);
    pushSnapshot();
    setDirty (true);
    timeline.refresh();
}

bool MainComponent::selectedTrackIsMidi() const
{
    return selectedTrack >= 0 && selectedTrack < (int) project->tracks.size()
           && project->tracks[(size_t) selectedTrack].type == TrackType::midi;
}

void MainComponent::deleteSelectedRegion()
{
    const auto sel = timeline.getRegionSelection();
    if (sel.isValid())
        deleteRegionAt (sel.track, sel.region);
}

void MainComponent::deleteRegionAt (int trackIndex, int regionIndex)
{
    if (engine.isRecording())
        return;
    if (trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& regions = project->tracks[(size_t) trackIndex].midiRegions;
    if (regionIndex < 0 || regionIndex >= (int) regions.size())
        return;

    Log::info ("region.delete", "track=" + juce::String (trackIndex));
    undoStack.begin (*project);
    regions.erase (regions.begin() + regionIndex);
    timeline.clearSelection();
    pushSnapshot();
    setDirty (true);
    timeline.refresh();
}

void MainComponent::performUndo()
{
    if (engine.isRecording())
        return;
    if (undoStack.undo (*project))
    {
        Log::info ("edit.undo");
        afterHistoryRestore();
    }
}

void MainComponent::performRedo()
{
    if (engine.isRecording())
        return;
    if (undoStack.redo (*project))
    {
        Log::info ("edit.redo");
        afterHistoryRestore();
    }
}

void MainComponent::afterHistoryRestore()
{
    timeline.clearSelection();
    headers.rebuild();
    selectTrack (selectedTrack); // 範囲内にクランプし直す
    pushSnapshot();              // SynthBank も復元後のトラック構成に同期される
    setDirty (true);
    timeline.refresh();
}

void MainComponent::openPianoRoll (int trackIndex, int regionIndex)
{
    if (trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    if (regionIndex < 0 || regionIndex >= (int) track.midiRegions.size())
        return;
    auto& region = track.midiRegions[(size_t) regionIndex];

    // 同じリージョンを再ダブルクリック → 閉じる（トグル）
    if (pianoRoll.isShowingRegion (track.id, region.id))
    {
        closePianoRoll();
        return;
    }
    pianoRoll.openRegion (track.id, region.id);
    resized();
}

void MainComponent::closePianoRoll()
{
    if (! pianoRoll.isOpen())
        return;
    const auto trackId = pianoRoll.currentTrackId();
    pianoRoll.close();
    // プレビュー発音の残りを打ち消す（停止中のみ有効なコマンド）
    if (! transport.isPlaying.load())
        previewFifo.push ({ PreviewFifo::Command::Type::allNotesOff, trackId, 0, 0 });
    resized();
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
    const double value = lcd.tempoLabel().getText().getDoubleValue();
    if (value < 30.0 || value > 300.0)
    {
        lcd.tempoLabel().setText (juce::String (transport.bpm.load()), juce::dontSendNotification);
        return;
    }
    Log::info ("project.bpm", "value=" + juce::String (value));
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
    // undo/redo履歴が参照するWAVはGCから保護する（redoでの復元に備える）
    if (! project->save (error, undoStack.referencedWavs()))
    {
        showAlert (jp (u8"保存に失敗しました"), error);
        return false;
    }
    Log::info ("project.save", "name=" + project->name());
    setDirty (false);
    return true;
}

juce::String MainComponent::windowTitle() const
{
    return juce::String (DAW_APP_NAME) + jp (u8" — ") + project->name() + (dirty ? jp (u8" ●") : juce::String());
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
    // MIDIトラックの音源を先に用意してから、スナップショットに参照を埋めて渡す。
    // sampleRate 未確定の間は synth が null のまま（timerCallback の sync が確定後に再pushする）
    synthBank.sync (*project, transport.sampleRate.load(), transport.blockSizeExpected.load());

    auto snapshot = project->buildSnapshot();
    for (size_t i = 0; i < project->tracks.size() && i < snapshot->tracks.size(); ++i)
        if (project->tracks[i].type == TrackType::midi)
            snapshot->tracks[i].synth = synthBank.get (project->tracks[i].id);

    snapshots.push (std::move (snapshot));

    // モデルが変わった可能性があるのでピアノロールも同期（対象リージョンが消えていれば閉じる）
    pianoRoll.refreshFromModel();
}

// ---- キーボード ----

// キー判定は Shortcuts.h のテーブル（matches）を必ず経由する（KeyPress直書き禁止）。
// 各分岐の実処理と「ピアノロール表示中のみ」等の有効条件はここに手書きする
bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    using SC = Shortcuts::ID;
    const auto is = [&key] (SC id) { return Shortcuts::matches (key, id); };
    const bool escape = key == juce::KeyPress (juce::KeyPress::escapeKey);

    // ショートカット一覧表示中はモーダル: 閉じる操作（Esc/⌘?）以外のキーは全て消費する
    if (shortcutOverlay.isVisible())
    {
        if (escape || is (SC::shortcutList))
            shortcutOverlay.dismiss();
        return true;
    }
    if (addTrackOverlay.isVisible())
    {
        if (escape)
        {
            addTrackOverlay.dismiss();
            return true;
        }
        if (is (SC::shortcutList))
            return true; // オーバーレイは高々1枚: AddTrack表示中の⌘?は無視
    }
    if (is (SC::shortcutList))
    {
        shortcutOverlay.setBounds (getLocalBounds());
        shortcutOverlay.show();
        return true;
    }
    if (is (SC::playStop))
    {
        togglePlay();
        return true;
    }
    // Logic準拠: ⌘Delete = 選択トラックを削除
    if (is (SC::deleteTrack))
    {
        requestDeleteTrack (selectedTrack);
        return true;
    }
    if (is (SC::deleteItem))
    {
        if (pianoRoll.isOpen() && pianoRoll.deleteSelectedNotes())
            return true;
        if (timeline.getRegionSelection().isValid())
            deleteSelectedRegion();
        else
            requestDeleteSelectedClip();
        return true;
    }
    // ピアノロールの選択ノートへのキー操作（Logic準拠: ↑↓=半音・⌥↑↓=オクターブ、⌘C/⌘V）
    if (pianoRoll.isOpen() && ! engine.isRecording())
    {
        const bool up = key.isKeyCode (juce::KeyPress::upKey);
        if (is (SC::noteOctave))
            return pianoRoll.transposeSelection (up ? 12 : -12);
        if (is (SC::noteSemitone))
            return pianoRoll.transposeSelection (up ? 1 : -1);
        if (is (SC::noteCopy))
            return pianoRoll.copySelection();
        if (is (SC::notePaste))
            return pianoRoll.pasteAtPlayhead();
    }
    // Undo/Redo（構造編集のみ対象）
    if (is (SC::redo))
    {
        performRedo();
        return true;
    }
    if (is (SC::undo))
    {
        performUndo();
        return true;
    }
    if (is (SC::save))
    {
        trySave();
        return true;
    }
    if (is (SC::openChooser))
    {
        // 未保存確認と画面遷移はMainWindow側。遷移はcallAsyncで遅延されるので
        // このkeyPressed実行中にthisが破棄されることはない
        if (onOpenChooserRequested != nullptr)
            onOpenChooserRequested();
        return true;
    }
    if (is (SC::addAudioTrack))
    {
        addTrack (TrackType::audio);
        return true;
    }
    if (is (SC::addMidiTrack))
    {
        addTrack (TrackType::midi);
        return true;
    }
    // Logic準拠: ⌘←/→ = 横ズームアウト/イン
    if (is (SC::zoomHorizontal))
    {
        const bool out = key.isKeyCode (juce::KeyPress::leftKey);
        timeline.zoomBy (out ? 1.0 / juce::MathConstants<double>::sqrt2
                             : juce::MathConstants<double>::sqrt2);
        return true;
    }
    if (is (SC::audioSettings))
    {
        showDeviceSettings();
        return true;
    }
    // Logic準拠: Ctrl+M = 選択中のリージョン/クリップをミュート/ミュート解除
    if (is (SC::muteRegion))
    {
        toggleMuteSelectedItem();
        return true;
    }
    // Logic準拠: ⌘T = 選択中のリージョン/クリップを再生ヘッド位置で分割
    if (is (SC::split))
    {
        splitSelectedItemAtPlayhead();
        return true;
    }
    // ,/.=1拍シーク、Shift+,/.（レイアウトにより<>）=1小節シーク
    if (is (SC::seekBeat) || is (SC::seekBar))
    {
        const auto tc = key.getTextCharacter();
        const int direction = (tc == ',' || tc == '<') ? -1 : 1;
        seekByStep (direction, is (SC::seekBar), key.getKeyCode());
        return true;
    }
    // ⌥,/. = 前/次のセクション頭へシーク（textCharacterは⌥で記号に化けるのでkeyCodeで方向判定）
    if (is (SC::seekSection))
    {
        seekToSection (key.getKeyCode() == ',' ? -1 : 1, key.getKeyCode());
        return true;
    }
    if (is (SC::muteTrack))
    {
        toggleMuteSelectedTrack();
        return true;
    }
    if (is (SC::record))
    {
        toggleRecord();
        return true;
    }
    return false;
}

// 再生中のキーシークは一時停止し、キー入力が止まってから自動再開する（シーク戻しと再生進行が同時に走るのを防ぐ）
void MainComponent::pauseForKeySeek (int keyCode)
{
    if (transport.isPlaying.load())
    {
        engine.stop();
        seekResumePending = true;
        Log::info ("transport.seek_pause", "pos=" + juce::String (transport.playheadSamplePos.load()));
    }
    if (seekResumePending)
    {
        lastSeekKeyMs = juce::Time::getMillisecondCounter();
        bool known = false;
        for (int i = 0; i < numSeekKeyCodes; ++i)
            known = known || seekKeyCodes[i] == keyCode;
        if (! known && numSeekKeyCodes < maxSeekKeyCodes)
            seekKeyCodes[numSeekKeyCodes++] = keyCode;
    }
}

void MainComponent::seekByStep (int direction, bool wholeBar, int keyCode)
{
    if (engine.isRecording())
        return;

    pauseForKeySeek (keyCode);

    const double stepLen = timeline.barLengthSamples() / (wholeBar ? 1.0 : 4.0);
    const auto pos = juce::jmax ((juce::int64) 0, transport.playheadSamplePos.load());
    auto step = (juce::int64) std::floor ((double) pos / stepLen);

    if (direction > 0)
    {
        ++step;
    }
    else
    {
        // 区切りの途中なら区切り頭へ、すでに頭なら前の区切りへ
        const auto stepStart = (juce::int64) std::llround ((double) step * stepLen);
        if ((double) (pos - stepStart) < stepLen * 0.01)
            --step;
        step = juce::jmax ((juce::int64) 0, step);
    }

    const auto target = (juce::int64) std::llround ((double) step * stepLen);
    transport.seekRequest.store (target);
    playStartSample = target; // シーク先が新しい戻り先になる（自動再開後の停止でもここへ戻る）
}

void MainComponent::seekToSection (int direction, int keyCode)
{
    if (engine.isRecording() || project->markers.empty())
        return;

    // 前=厳密に前の境界、次=厳密に次の境界。境界ちょうどに居るときも1つ進む/戻る（1拍/1小節シークと同じ流儀）
    const auto pos = juce::jmax ((juce::int64) 0, transport.playheadSamplePos.load());
    juce::int64 target = -1;
    for (const auto& marker : project->markers)
    {
        const auto sample = timeline.beatStartSample (marker.startBeats);
        if (direction > 0)
        {
            if (sample > pos)
            {
                target = sample;
                break;
            }
        }
        else
        {
            if (sample >= pos)
                break;
            target = sample;
        }
    }
    if (target < 0)
        return; // 前/次のセクションがなければno-op

    pauseForKeySeek (keyCode);
    transport.seekRequest.store (target);
    playStartSample = target;
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

void MainComponent::toggleMuteSelectedItem()
{
    if (engine.isRecording())
        return;
    if (const auto sel = timeline.getSelection(); sel.isValid())
        timeline.toggleMuteAt (sel.track, sel.clip);
    else if (const auto rsel = timeline.getRegionSelection(); rsel.isValid())
        timeline.toggleMuteAt (rsel.track, rsel.region);
}

void MainComponent::splitSelectedItemAtPlayhead()
{
    if (engine.isRecording())
        return;
    if (const auto sel = timeline.getSelection(); sel.isValid())
        timeline.splitAtPlayhead (sel.track, sel.clip);
    else if (const auto rsel = timeline.getRegionSelection(); rsel.isValid())
        timeline.splitAtPlayhead (rsel.track, rsel.region);
}

// ---- 表示更新 ----

void MainComponent::updateTransportButtons()
{
    const bool recording = engine.isRecording();
    // シーク後の再開待ち中も見かけ上は「再生中」として表示する
    const bool playing = transport.isPlaying.load() || seekResumePending;
    playButton.setIcon (playing ? IconButton::Icon::stop : IconButton::Icon::play);
    playButton.setIconColour (playing ? Theme::playGreen  // 再生中は緑（メーターと同色）
                                      : juce::Colours::white.withAlpha (0.85f));
    recordButton.setToggleState (recording, juce::dontSendNotification); // 録音中は赤点灯
    recordButton.setIconColour (recording ? juce::Colours::white : Theme::recordRed);
    if (recording)
    {
        // 録音中はアイコンの周りに赤いハローをゆっくり明滅させる（Timer 30Hzから毎tick呼ばれる）。
        // グロー色は録音中の暗赤背景(recordActiveBg)に埋もれないよう明るめの赤にする
        const float phase = (float) (juce::Time::getMillisecondCounter() % 1600) / 1600.0f;
        const float wave = 0.5f + 0.5f * std::sin (phase * juce::MathConstants<float>::twoPi);
        recordButton.setGlow (0.30f + 0.70f * wave, Theme::recordGlow);
    }
    else
        recordButton.setGlow (0.0f, Theme::recordGlow);
    // MIDIトラック選択中は録音ボタン無効（録音停止としては常に押せる）
    recordButton.setEnabled (recording || ! selectedTrackIsMidi());
}

void MainComponent::updateLcdTime()
{
    const double sr = timeline.effectiveSampleRate();
    const auto playhead = juce::jmax ((juce::int64) 0, transport.playheadSamplePos.load());
    const double seconds = (double) playhead / sr;
    const int minutes = (int) (seconds / 60.0);

    // 秒はゼロ埋め（"0:05.3"）にして再生中の桁揺れを抑える
    lcd.setTimeText (juce::String (minutes) + ":"
                     + juce::String::formatted ("%04.1f", seconds - minutes * 60.0));
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

    // 上部バー: 上端の1pxハイライトのみ。下との区切りは境界線でなく背景の明度差に任せる
    auto bar = getLocalBounds().removeFromTop (topBarHeight).toFloat();
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.fillRect (bar.removeFromTop (1.0f));
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    auto topRow = area.removeFromTop (topBarHeight).reduced (12, 8);
    // トランスポートボタンは行の高さいっぱいだと大きすぎるので一回り小さくして縦中央に置く
    auto transportButton = [&topRow] { return topRow.removeFromLeft (38).withSizeKeepingCentre (38, 26); };
    playButton.setBounds (transportButton());
    topRow.removeFromLeft (6);
    recordButton.setBounds (transportButton());
    topRow.removeFromLeft (14);
    clickButton.setBounds (transportButton());
    topRow.removeFromLeft (10);

    // 歯車は補助機能なので控えめに（枠を絞るとアイコンもホバー範囲も一回り小さくなる）
    settingsButton.setBounds (topRow.removeFromRight (44).withSizeKeepingCentre (32, 32));
    topRow.removeFromRight (10);

    // LCDはウィンドウ中央に置く（Logicの配置）。狭いときは左のボタン群を優先して右へ逃がす
    auto lcdArea = juce::Rectangle<int> (TransportLcd::preferredWidth, topRow.getHeight())
                       .withCentre ({ getWidth() / 2, topRow.getCentreY() });
    lcdArea.setX (juce::jlimit (topRow.getX(),
                                juce::jmax (topRow.getX(),
                                            topRow.getRight() - TransportLcd::preferredWidth),
                                lcdArea.getX()));
    lcd.setBounds (lcdArea);

    auto warnArea = topRow;
    warnArea.setLeft (juce::jmin (warnArea.getRight(), lcdArea.getRight() + 10));
    srWarningLabel.setBounds (warnArea);

    if (pianoRoll.isOpen())
        pianoRoll.setBounds (area.removeFromBottom (PianoRollView::preferredHeight));

    // ＋ボタンの帯はヘッダー列の中だけに置く（全幅に取るとタイムライン下に死にスペースができる）
    auto headerColumn = area.removeFromLeft (TrackHeadersView::preferredWidth);
    headerColumn.removeFromTop (TimelineView::topHeight); // ルーラー＋マーカーレーン分の高さを合わせる
    addTrackButton.setBounds (headerColumn.removeFromBottom (32).reduced (8, 4));
    headers.setBounds (headerColumn);
    timeline.setBounds (area);

    if (addTrackOverlay.isVisible())
    {
        addTrackOverlay.setBounds (getLocalBounds());
        addTrackOverlay.setAnchor (addTrackButton.getBounds());
    }

    if (shortcutOverlay.isVisible())
        shortcutOverlay.setBounds (getLocalBounds());
}
