#include "MainComponent.h"

#include <algorithm>
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

// バウンスの前回保存先（セッション内で記憶。プロジェクトを跨いでも引き継ぐ）
juce::File lastBounceDirectory;
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
    addChildComponent (fxEditor);  // 左のFXパネル（概要・基本常設・Iで開閉）
    addChildComponent (fxDetail);  // 下部のFX詳細（スロットクリックで開く・ピアノロールと排他）
    addChildComponent (bottomResizeBar); // 下部パネル表示中のみ可視（パネル群より後に追加＝前面）
    bottomResizeBar.onDragStart = [this] { bottomHeightAtDragStart = bottomPanelHeight; };
    bottomResizeBar.onDragged = [this] (int dy)
    {
        // 上へドラッグ＝パネルが広がる。上限はresized側のクランプに任せる
        bottomPanelHeight = juce::jmax (bottomPanelMinHeight, bottomHeightAtDragStart - dy);
        resized();
    };
    addAndMakeVisible (playButton);
    addAndMakeVisible (recordButton);
    addAndMakeVisible (addTrackButton);
    addAndMakeVisible (settingsButton);
    addAndMakeVisible (clickButton);
    addChildComponent (addTrackOverlay); // トラック追加メニュー表示中のみ可視
    addChildComponent (shortcutOverlay); // ⌘?表示中のみ可視
    addChildComponent (bounceOverlay);   // バウンス中のみ可視
    addChildComponent (mixerOverlay);    // X表示中のみ可視（bounceOverlayより背面に置く）
    addAndMakeVisible (lcd);
    addChildComponent (srWarningLabel); // 不一致時のみ表示

    timeline.setProject (project.get());
    headers.setProject (project.get());
    pianoRoll.setProject (project.get());
    mixerOverlay.setProject (project.get());
    mixerOverlay.onSelectTrack = [this] (int index) { selectTrackFromUser (index); };
    mixerOverlay.onChanged = [this]
    {
        setDirty (true);
        fxEditor.refreshValues(); // 同じsend/gain atomicを表示するエディタ側へ反映
        headers.refreshValues();  // 音量はヘッダーのスライダーにも表示される
    };
    // バス/Masterストリップのクリック → FXパネルでそのチャンネルのチェーンを表示
    mixerOverlay.onSelectBus = [this] (int bus)
    {
        openFxEditor();
        fxEditor.showBus (bus);
        syncFxDetail();
    };
    mixerOverlay.onSelectMaster = [this]
    {
        openFxEditor();
        fxEditor.showMaster();
        syncFxDetail();
    };
    mixerOverlay.onDismissed = [this]
    {
        if (fxEditor.isOpen())
        {
            fxEditor.showTrack (selectedTrack); // ミキサーを閉じたら選択トラック追従に戻す
            syncFxDetail();
        }
    };

    fxEditor.setProject (project.get());
    fxEditor.onCloseRequested = [this] { closeFxEditor(); };
    fxEditor.onSlotClicked = [this] (int slot) { toggleFxDetailSlot (slot); };
    fxEditor.onSendOrPanChanged = [this]
    {
        setDirty (true);
        mixerOverlay.refreshValues(); // send/panはミキサーと同じatomicの表示なので反映（非表示時はno-op）
    };
    fxEditor.onFxEnabledChanged = [this] { setDirty (true); }; // ON/OFFはミキサーに表示がないのでdirty化のみ
    fxEditor.onVolumeChanged = [this]
    {
        setDirty (true);
        mixerOverlay.refreshValues(); // 音量はミキサーのフェーダーと同じatomicの表示（非表示時はno-op）
        headers.refreshValues();      // ヘッダーのスライダーにも表示される
    };
    fxDetail.onCloseRequested = [this] { closeFxDetail(); };

    // ---- タイムライン・ヘッダの連携 ----
    timeline.onSeek = [this] (juce::int64 samplePos)
    {
        if (! engine.isRecording())
        {
            transport.seekRequest.store (samplePos);
            playStartSample = samplePos; // 再生中のクリックシークでも停止時の戻り先を更新する
        }
    };
    timeline.onTrackSelected = [this] (int index) { selectTrackFromUser (index); };
    timeline.onVerticalScroll = [this] (int y) { headers.setViewY (y); };
    timeline.onWillEditModel = [this] { undoStack.begin (*project); };
    timeline.onModelEdited = [this]
    {
        Log::info ("edit.timeline"); // 操作確定時（mouseUp等）に1回だけ来る
        pushSnapshot();
        setDirty (true);
        timeline.refresh();
    };
    // サイクル範囲はundo対象外（音量・ミュートと同じ扱い。Logicもサイクル操作はundoしない）なので
    // onWillEditModel/onModelEdited でなく専用コールバックで Transport同期とdirty化だけ行う
    timeline.onCycleChanged = [this]
    {
        syncCycleToTransport();
        setDirty (true);
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
    timeline.onExportItemRequested = [this] (int trackIndex, int itemIndex)
    { startRegionExportFlow (trackIndex, itemIndex); };
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
    headers.onSelect = [this] (int index) { selectTrackFromUser (index); };
    headers.onDeleteRequested = [this] (int index) { requestDeleteTrack (index); };
    headers.onChanged = [this]
    {
        setDirty (true);
        mixerOverlay.refreshValues(); // 音量はミキサーのフェーダーと同じatomicの表示（非表示時はno-op）
        if (fxEditor.isOpen())
        {
            fxEditor.refreshFromModel (selectedTrack); // リネームのタイトル反映等
            syncFxDetail();
        }
    };
    headers.onWillChangeStructure = [this] { undoStack.begin (*project); };
    headers.onInstrumentChanged = [this]
    {
        Log::info ("track.instrument");
        pushSnapshot(); // SynthBank が楽器変更を検知して音源を差し替える
        setDirty (true);
    };
    headers.onWheel = [this] (float deltaY) { timeline.scrollVertically (deltaY); };
    headers.canReorder = [this] { return ! engine.isRecording(); }; // 録音中は並び替え不可
    headers.onReorderRequested = [this] (int from, int to) { reorderTrack (from, to); };

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

    bounceOverlay.onCancel = [this]
    {
        Log::info ("bounce.cancel_requested", "source=overlay");
        bounceRenderer.cancel(); // 非同期。完了はpollBounce()が拾う
    };

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

    // FXパネルは基本常設（Iで開閉）
    fxEditor.openView();
    fxEditor.showTrack (selectedTrack);

    pushSnapshot();
    syncCycleToTransport(); // 保存済みサイクルを読み込んだ時点で反映（SR確定後はTimerが再同期する）
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

    pollBounce();

    // サイクル範囲のサンプル換算はBPM・サンプルレートに依存するため毎tick同期する
    // （BPM編集・デバイスSR確定・デバイス変更のどの経路でも取りこぼさない。atomic2本のstoreのみで安価）
    syncCycleToTransport();

    // メーター消費の一元化: peakL/peakR の exchange(0) はここでだけ行い、読み取った値を
    // ヘッダー・ミキサー・FXパネルへ配る（複数箇所でexchangeするとピークを取り合う）
    const bool playingNow = transport.isPlaying.load();
    if (playingNow && ! meterWasPlaying) // 再生開始でピーク保持をリセット（Logicの数値表示と同じ）
    {
        for (auto& feed : meterFeeds)
            feed.maxSincePlay = 0.0f;
        for (auto& feed : busFeeds)
            feed.maxSincePlay = 0.0f;
        masterFeed.maxSincePlay = 0.0f;
    }
    meterWasPlaying = playingNow;

    meterPeaks.resize (project->tracks.size());
    meterFeeds.resize (project->tracks.size());
    for (size_t i = 0; i < project->tracks.size(); ++i)
    {
        const StereoPeak p { project->tracks[i].params->peakL.exchange (0.0f),
                             project->tracks[i].params->peakR.exchange (0.0f) };
        meterPeaks[i] = p;
        meterFeeds[i].peak = p;
        meterFeeds[i].maxSincePlay = juce::jmax (meterFeeds[i].maxSincePlay, p[0], p[1]);
    }
    for (int b = 0; b < numSendBuses; ++b)
    {
        const StereoPeak p { project->busParams[b]->peakL.exchange (0.0f),
                             project->busParams[b]->peakR.exchange (0.0f) };
        busFeeds[b].peak = p;
        busFeeds[b].maxSincePlay = juce::jmax (busFeeds[b].maxSincePlay, p[0], p[1]);
    }
    {
        const StereoPeak p { project->masterParams->peakL.exchange (0.0f),
                             project->masterParams->peakR.exchange (0.0f) };
        masterFeed.peak = p;
        masterFeed.maxSincePlay = juce::jmax (masterFeed.maxSincePlay, p[0], p[1]);
    }
    headers.updateMeters (meterPeaks);
    mixerOverlay.updateMeters (meterFeeds, busFeeds, masterFeed);
    fxEditor.updateMeters (meterFeeds, busFeeds, masterFeed);

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

        // サイクルON時に範囲外（終端ちょうど含む）から再生を始めるときは範囲頭へジャンプ（Logic準拠）
        if (project->cycleEnabled && project->hasCycleRange())
        {
            const auto cycleStart = timeline.sixteenthStartSample (project->cycleStartSixteenths);
            const auto cycleEnd = timeline.sixteenthStartSample (project->cycleEndSixteenths);
            if (playStartSample < cycleStart || playStartSample >= cycleEnd)
            {
                playStartSample = cycleStart;
                transport.seekRequest.store (cycleStart);
            }
        }
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
            resetTrackPeakHolds();
            timeline.clearSelection();
            headers.rebuild();
            selectTrack (juce::jmin (index, (int) project->tracks.size() - 1));
            pushSnapshot();
            setDirty (true);
            timeline.refresh();
        });
}

// ヘッダのドラッグ＆ドロップ並び替え。to は挿入先の隙間番号（0..tracks.size()）。
// vector順 = 表示順・保存順・再生順なので入れ替えるだけでよいが、index保持の参照
// （選択トラック・Timelineの両選択・FXパネル・ミキサー）はIDで退避して引き直す
void MainComponent::reorderTrack (int from, int to)
{
    if (engine.isRecording())
        return;
    const int numTracks = (int) project->tracks.size();
    if (from < 0 || from >= numTracks || to < 0 || to > numTracks || to == from || to == from + 1)
        return;

    const auto trackIdAt = [this] (int index) -> juce::uint64
    {
        return index >= 0 && index < (int) project->tracks.size()
                   ? project->tracks[(size_t) index].id
                   : 0;
    };
    const auto selectedId = trackIdAt (selectedTrack);
    const auto clipSelId = trackIdAt (timeline.getSelection().track);
    const auto regionSelId = trackIdAt (timeline.getRegionSelection().track);
    const auto fxTrackId = trackIdAt (fxEditor.shownTrack());

    Log::info ("track.reorder", "from=" + juce::String (from) + " to=" + juce::String (to)
                                    + " name=" + project->tracks[(size_t) from].name);
    undoStack.begin (*project);

    auto& tracks = project->tracks;
    if (to > from)
        std::rotate (tracks.begin() + from, tracks.begin() + from + 1, tracks.begin() + to);
    else
        std::rotate (tracks.begin() + to, tracks.begin() + from, tracks.begin() + from + 1);
    resetTrackPeakHolds();

    const auto indexOfId = [this] (juce::uint64 id) -> int
    {
        if (id != 0)
            for (int i = 0; i < (int) project->tracks.size(); ++i)
                if (project->tracks[(size_t) i].id == id)
                    return i;
        return -1;
    };

    headers.rebuild();
    timeline.remapSelectionTracks (indexOfId (clipSelId), indexOfId (regionSelId));
    fxEditor.remapTrack (indexOfId (fxTrackId));
    selectTrack (indexOfId (selectedId)); // ヘッダ・タイムライン・ミキサー・FXパネルの選択表示も同期
    pushSnapshot();
    setDirty (true);
    timeline.refresh();
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
    selectTrackFromUser ((int) project->tracks.size() - 1); // トラック追加はユーザー操作＝エディタも追従
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

// ピーク保持（dB数値表示）はトラックindexに紐づくため、構造変更（削除・並び替え・undo/redo）で
// 別トラックの値を引き継いでしまう。表示専用の値なので全リセットが最も単純で安全
void MainComponent::resetTrackPeakHolds()
{
    for (auto& feed : meterFeeds)
        feed.maxSincePlay = 0.0f;
}

void MainComponent::afterHistoryRestore()
{
    resetTrackPeakHolds();
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
    closeFxDetail(); // 下部スロットはFX詳細と排他（後勝ち）
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

// ---- 左のFXパネル（基本常設・Iで開閉。ピアノロールとは独立に共存）----

void MainComponent::toggleFxEditor()
{
    if (fxEditor.isOpen())
        closeFxEditor();
    else
    {
        openFxEditor();
        fxEditor.showTrack (selectedTrack);
    }
}

void MainComponent::openFxEditor()
{
    if (fxEditor.isOpen())
        return;
    Log::info ("fxeditor.open");
    fxEditor.openView();
    resized();
}

void MainComponent::closeFxEditor()
{
    if (! fxEditor.isOpen())
        return;
    Log::info ("fxeditor.close");
    closeFxDetail(); // 概要が消えたら詳細も道連れ（詳細だけ残ると対象の手掛かりを失う）
    fxEditor.closeView();
    resized();
}

void MainComponent::toggleFxDetailSlot (int slot)
{
    if (fxDetail.isOpen() && fxDetailSlot == slot && fxDetailKey == fxEditor.targetKey())
    {
        closeFxDetail(); // 同じスロットの再クリックは閉じる
        return;
    }
    closePianoRoll(); // 下部スロットはピアノロールと排他（後勝ち）
    fxDetailSlot = slot;
    fxDetailKey = fxEditor.targetKey();
    Log::info ("fxdetail.open", "fx=" + fxEditor.slotName (slot)
                                    + " channel=" + fxEditor.channelName());
    fxDetail.show (fxEditor.slotName (slot), fxEditor.channelName());
    fxEditor.setActiveSlot (slot);
    resized();
}

void MainComponent::closeFxDetail()
{
    if (! fxDetail.isOpen())
        return;
    Log::info ("fxdetail.close");
    fxDetail.close();
    fxDetailSlot = -1;
    fxDetailKey.clear();
    fxEditor.setActiveSlot (-1);
    resized();
}

void MainComponent::syncFxDetail()
{
    if (! fxDetail.isOpen())
        return;

    // トラック→トラックは同じスロット（EQ/Comp）のまま追従、同一バス/Masterはタイトル更新のみ。
    // トラック⇄バス等はチェーン構成が変わるので閉じる
    const auto key = fxEditor.targetKey();
    const bool followable = (key == "track" && fxDetailKey == "track") || key == fxDetailKey;
    if (followable && fxDetailSlot >= 0 && fxDetailSlot < fxEditor.numSlots())
    {
        fxDetailKey = key;
        fxDetail.show (fxEditor.slotName (fxDetailSlot), fxEditor.channelName());
        fxEditor.setActiveSlot (fxDetailSlot);
        return;
    }
    closeFxDetail();
}

void MainComponent::selectTrack (int index)
{
    selectedTrack = project->tracks.empty()
        ? -1
        : juce::jlimit (0, (int) project->tracks.size() - 1, index);
    headers.setSelectedTrack (selectedTrack);
    timeline.setSelectedTrack (selectedTrack);
    mixerOverlay.sync (selectedTrack); // トラック増減・選択変更をストリップに反映（非表示中はno-op）
    if (fxEditor.isOpen())
    {
        fxEditor.refreshFromModel (selectedTrack); // バス/Master表示は維持し、対象消滅時だけ追従に戻す
        syncFxDetail();
    }
}

void MainComponent::selectTrackFromUser (int index)
{
    selectTrack (index);
    if (fxEditor.isOpen())
    {
        fxEditor.showTrack (selectedTrack); // ユーザーのトラック選択はパネルも追従（バス/Master表示から戻る）
        syncFxDetail();
    }
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

// ---- バウンス（書き出し）----

void MainComponent::startBounceFlow()
{
    if (bounceActive || engine.isRecording())
        return;

    // 素材が何も無ければ入口で弾く（mute/soloを踏まえた正確な判定はbeginBounceで行う）
    bool hasContent = false;
    for (auto& track : project->tracks)
        hasContent = hasContent || ! track.clips.empty() || ! track.midiRegions.empty();
    if (! hasContent)
    {
        showAlert (jp (u8"書き出せません"), jp (u8"書き出す内容がありません。"));
        return;
    }

    if (project->sampleRate <= 0.0 && transport.sampleRate.load() <= 0.0)
    {
        showAlert (jp (u8"書き出せません"), jp (u8"オーディオデバイスが準備できていません。"));
        return;
    }

    stopPlaybackForBounce();

    const auto dir = lastBounceDirectory.isDirectory()
                         ? lastBounceDirectory
                         : juce::File::getSpecialLocation (juce::File::userDesktopDirectory);
    bounceChooser = std::make_unique<juce::FileChooser> (
        jp (u8"書き出し"), dir.getChildFile (project->name() + ".wav"), "*.wav");

    const auto flags = juce::FileBrowserComponent::saveMode
                       | juce::FileBrowserComponent::canSelectFiles
                       | juce::FileBrowserComponent::warnAboutOverwriting;
    bounceChooser->launchAsync (flags, [this] (const juce::FileChooser& chooser)
    {
        // thisの生存: bounceChooserはthisのメンバーで、this破棄時にダイアログごと片付く
        const auto chosen = chooser.getResult();
        if (chosen == juce::File())
            return; // キャンセル
        const auto target = chosen.withFileExtension ("wav");
        lastBounceDirectory = target.getParentDirectory();
        beginBounce (target);
    });
}

void MainComponent::beginBounce (const juce::File& target)
{
    if (bounceActive || engine.isRecording())
        return;

    const double sr = project->sampleRate > 0.0 ? project->sampleRate : transport.sampleRate.load();
    if (sr <= 0.0)
    {
        showAlert (jp (u8"書き出せません"), jp (u8"オーディオデバイスが準備できていません。"));
        return;
    }

    BounceRenderer::Request request;
    request.sampleRate = sr;
    request.bpm = juce::jlimit (20.0, 400.0, transport.bpm.load());
    request.targetFile = target;

    // バス・Masterも開始時の値をプレーン値へ固定する（トラックのmute/solo/gainと同じ扱い）
    for (int b = 0; b < numSendBuses; ++b)
    {
        request.busGain[b] = project->busParams[b]->gain.load();
        request.busMute[b] = project->busParams[b]->mute.load();
    }
    request.masterGain = project->masterParams->gain.load();

    // 開始時点のmute/solo/gainをプレーン値へ固定する（共有atomicのTrackParamsはワーカーへ渡さない。
    // 保存ダイアログ表示中に変えられた値もここで確定する）
    bool anySolo = false;
    for (auto& track : project->tracks)
        anySolo = anySolo || track.params->solo.load();

    const double tps = Ppq::ticksPerSample (request.bpm, sr);
    auto snapshot = project->buildSnapshot(); // クリップ参照とノートのフラット化を再利用（synthは空のまま）
    juce::int64 endSample = 0;

    for (size_t i = 0; i < snapshot->tracks.size() && i < project->tracks.size(); ++i)
    {
        auto& model = project->tracks[i];
        auto& params = *model.params;
        const bool audible = ! params.mute.load() && (! anySolo || params.solo.load());
        const float gain = params.gain.load();
        if (! audible || gain <= 0.0f)
            continue; // 非可聴トラックはリクエストに入れない（RTのprocessと同じ規則）

        BounceRenderer::TrackRender trackRender;
        trackRender.gain = gain;
        trackRender.pan = params.pan.load();
        for (int b = 0; b < numSendBuses; ++b)
            trackRender.sends[b] = params.sends[b].load();
        trackRender.clips = std::move (snapshot->tracks[i].clips);
        trackRender.notes = std::move (snapshot->tracks[i].notes);

        // 終端 = 最後のクリップ終端 / MIDIリージョン終端。リージョンは最後のノートの後の
        // 余白も範囲に含めるため、ノート終端でなくモデル側のリージョン境界から算出する
        // （スナップショットには境界情報が残らない）。ミュートリージョンは含めない
        for (auto& clip : trackRender.clips)
            endSample = juce::jmax (endSample, clip.startSample + clip.lengthSamples);
        if (model.type == TrackType::midi)
            for (auto& region : model.midiRegions)
                if (! region.muted)
                    endSample = juce::jmax (endSample, (juce::int64) std::llround (
                                                (double) (region.startPpq + region.lengthPpq) / tps));

        // ノートも音声もないトラックはレンダリング対象にしない（終端への寄与は上で済んでいる。
        // ノートのないリージョンだけのプロジェクトはリージョン終端までの無音が書き出される）
        if (trackRender.clips.empty() && trackRender.notes.empty())
            continue;

        if (model.type == TrackType::midi)
        {
            // RT側の共有インスタンスとはprocessBlockが並走するため共有不可。専用に生成する
            trackRender.synth = synthBank.createIndependent (model.gmProgram, model.drums,
                                                             sr, BounceRenderer::renderBlockSize);
            if (trackRender.synth == nullptr)
            {
                auto errors = synthBank.takeCreateErrors();
                showAlert (jp (u8"書き出しを中止しました"),
                           jp (u8"ソフトウェア音源を作成できませんでした。\n")
                               + errors.joinIntoString ("\n"));
                return;
            }
            request.wantTail = true; // 可聴なMIDIトラックがあるときだけ余韻テールを付ける
        }

        request.tracks.push_back (std::move (trackRender));
    }

    if (endSample <= 0)
    {
        showAlert (jp (u8"書き出せません"),
                   jp (u8"書き出す内容がありません（全トラックがミュート、または空です）。"));
        return;
    }
    request.endSample = endSample;

    // サイクルON時はその範囲を書き出す（Logicのサイクル書き出しと同じ）。
    // ループ素材用途なのでMIDIがあってもテールを付けず、出力長＝範囲サンプル長ちょうどにする
    if (project->cycleEnabled && project->hasCycleRange())
    {
        const double sixteenthLen = sr * 60.0 / request.bpm / 4.0;
        request.startSample = (juce::int64) std::llround ((double) project->cycleStartSixteenths * sixteenthLen);
        request.endSample = (juce::int64) std::llround ((double) project->cycleEndSixteenths * sixteenthLen);
        request.wantTail = false;
    }

    Log::info ("bounce.start", "target=" + target.getFullPathName()
                                   + " sr=" + juce::String (sr, 0)
                                   + " startSample=" + juce::String (request.startSample)
                                   + " endSample=" + juce::String (request.endSample)
                                   + " tracks=" + juce::String ((int) request.tracks.size())
                                   + " tail=" + juce::String ((int) request.wantTail));

    startBounceRequest (std::move (request));
}

// 書き出し前の再生停止（状態を単純に保つ。⌘B/⌘E共通）
void MainComponent::stopPlaybackForBounce()
{
    if (transport.isPlaying.load() || seekResumePending)
    {
        seekResumePending = false;
        numSeekKeyCodes = 0;
        engine.stop();
        transport.seekRequest.store (playStartSample);
        updateTransportButtons();
        Log::info ("transport.stop", "reason=bounce pos=" + juce::String (playStartSample));
    }
}

// レンダラー起動＋進捗オーバーレイ表示（⌘B/⌘E共通の尻尾）。完了はpollBounce()が拾う
bool MainComponent::startBounceRequest (BounceRenderer::Request&& request)
{
    if (! bounceRenderer.start (std::move (request)))
    {
        showAlert (jp (u8"書き出せません"), jp (u8"前回の書き出しが終了していません。"));
        return false;
    }

    bounceActive = true;
    bounceDoneTicks = 0;
    bounceOverlay.setBounds (getLocalBounds());
    bounceOverlay.show();
    refreshMacMenu(); // バウンス中はFileメニューをdisabledにする
    return true;
}

void MainComponent::exportSelectedItem()
{
    const auto& regionSel = timeline.getRegionSelection();
    if (regionSel.isValid())
    {
        startRegionExportFlow (regionSel.track, regionSel.region);
        return;
    }
    const auto& clipSel = timeline.getSelection();
    if (clipSel.isValid())
        startRegionExportFlow (clipSel.track, clipSel.clip);
}

void MainComponent::startRegionExportFlow (int trackIndex, int itemIndex)
{
    if (bounceActive || engine.isRecording())
        return;
    if (trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    const auto& track = project->tracks[(size_t) trackIndex];
    const bool isMidi = track.type == TrackType::midi;
    if (itemIndex < 0 || itemIndex >= (int) (isMidi ? track.midiRegions.size() : track.clips.size()))
        return;

    // 中身のないアイテムは入口で弾く（⌘Bの空プロジェクト弾きと同じ流儀）
    if (isMidi && track.midiRegions[(size_t) itemIndex].notes.empty())
    {
        showAlert (jp (u8"書き出せません"), jp (u8"リージョンにノートがありません。"));
        return;
    }

    if (project->sampleRate <= 0.0 && transport.sampleRate.load() <= 0.0)
    {
        showAlert (jp (u8"書き出せません"), jp (u8"オーディオデバイスが準備できていません。"));
        return;
    }

    stopPlaybackForBounce();

    const auto dir = lastBounceDirectory.isDirectory()
                         ? lastBounceDirectory
                         : juce::File::getSpecialLocation (juce::File::userDesktopDirectory);
    const auto defaultName = juce::File::createLegalFileName (project->name() + "-" + track.name + ".wav");
    bounceChooser = std::make_unique<juce::FileChooser> (
        jp (u8"リージョンを書き出し"), dir.getChildFile (defaultName), "*.wav");

    const auto flags = juce::FileBrowserComponent::saveMode
                       | juce::FileBrowserComponent::canSelectFiles
                       | juce::FileBrowserComponent::warnAboutOverwriting;
    bounceChooser->launchAsync (flags, [this, trackIndex, itemIndex] (const juce::FileChooser& chooser)
    {
        // thisの生存: bounceChooserはthisのメンバーで、this破棄時にダイアログごと片付く。
        // ダイアログはモーダルで表示中に編集は起きないが、indexの範囲はbeginRegionBounce側でも再検証する
        const auto chosen = chooser.getResult();
        if (chosen == juce::File())
            return; // キャンセル
        const auto target = chosen.withFileExtension ("wav");
        lastBounceDirectory = target.getParentDirectory();
        beginRegionBounce (target, trackIndex, itemIndex);
    });
}

void MainComponent::beginRegionBounce (const juce::File& target, int trackIndex, int itemIndex)
{
    if (bounceActive || engine.isRecording())
        return;
    if (trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    const auto& track = project->tracks[(size_t) trackIndex];

    const double sr = project->sampleRate > 0.0 ? project->sampleRate : transport.sampleRate.load();
    if (sr <= 0.0)
    {
        showAlert (jp (u8"書き出せません"), jp (u8"オーディオデバイスが準備できていません。"));
        return;
    }

    BounceRenderer::Request request;
    request.sampleRate = sr;
    request.bpm = juce::jlimit (20.0, 400.0, transport.bpm.load());
    request.targetFile = target;
    request.wantTail = false; // リージョン厳密長（サイクル範囲書き出しと同じ規則）

    // バス・Masterは⌘Bと同じく開始時の値を焼き込む（=聞こえたままの経路）。
    // トラックのmute/solo・リージョン自身のmutedは見ない（明示選択が優先）
    for (int b = 0; b < numSendBuses; ++b)
    {
        request.busGain[b] = project->busParams[b]->gain.load();
        request.busMute[b] = project->busParams[b]->mute.load();
    }
    request.masterGain = project->masterParams->gain.load();

    BounceRenderer::TrackRender trackRender;
    if (! BounceRenderer::buildItemRender (track, itemIndex, request.bpm, sr,
                                           trackRender, request.startSample, request.endSample)
        || (track.type == TrackType::midi && trackRender.notes.empty()))
    {
        showAlert (jp (u8"書き出せません"), jp (u8"書き出す内容がありません。"));
        return;
    }

    if (track.type == TrackType::midi)
    {
        // RT側の共有インスタンスとはprocessBlockが並走するため共有不可。専用に生成する
        trackRender.synth = synthBank.createIndependent (track.gmProgram, track.drums,
                                                         sr, BounceRenderer::renderBlockSize);
        if (trackRender.synth == nullptr)
        {
            auto errors = synthBank.takeCreateErrors();
            showAlert (jp (u8"書き出しを中止しました"),
                       jp (u8"ソフトウェア音源を作成できませんでした。\n")
                           + errors.joinIntoString ("\n"));
            return;
        }
    }
    request.tracks.push_back (std::move (trackRender));

    Log::info ("bounce.start", "target=" + target.getFullPathName()
                                   + " source=region track=" + juce::String (trackIndex)
                                   + " item=" + juce::String (itemIndex)
                                   + " sr=" + juce::String (sr, 0)
                                   + " startSample=" + juce::String (request.startSample)
                                   + " endSample=" + juce::String (request.endSample));

    startBounceRequest (std::move (request));
}

void MainComponent::pollBounce()
{
    // 完了表示の自動クローズ
    if (bounceDoneTicks > 0 && --bounceDoneTicks == 0)
        bounceOverlay.dismiss();

    if (! bounceActive)
        return;

    bounceOverlay.setProgress (bounceRenderer.progress());
    if (bounceRenderer.status() == BounceRenderer::Status::running)
        return;

    bounceActive = false;
    const auto result = bounceRenderer.takeResult();
    switch (result.status)
    {
        case BounceRenderer::Status::success:
            Log::info ("bounce.done", "samples=" + juce::String (result.writtenSamples)
                                          + " peak=" + juce::String (result.peak, 3)
                                          + " scaled=" + juce::String ((int) result.scaled));
            bounceOverlay.showDone();
            bounceDoneTicks = 40; // 30Hz × 40 ≈ 1.3秒表示して自動で消える
            break;

        case BounceRenderer::Status::cancelled:
            Log::info ("bounce.cancelled");
            bounceOverlay.dismiss();
            break;

        default:
            Log::error ("bounce.failed", "message=" + result.errorMessage.replace ("\n", " / "));
            bounceOverlay.dismiss();
            showAlert (jp (u8"書き出しに失敗しました"), result.errorMessage);
            break;
    }
    refreshMacMenu();
}

void MainComponent::cancelBounceForClose()
{
    if (! bounceActive)
        return;

    Log::info ("bounce.cancel_requested", "source=close");
    bounceRenderer.cancelAndWait(); // ワーカーが一時ファイルを削除してから戻る（数十ms想定）
    bounceActive = false;
    (void) bounceRenderer.takeResult();
    bounceOverlay.dismiss();
    refreshMacMenu();
    Log::info ("bounce.cancelled", "reason=close");
}

void MainComponent::refreshMacMenu()
{
    // Fileメニューのenable状態はメニュー再構築時にgetCommandInfoから引き直される（Main.cpp側）
    if (auto* model = juce::MenuBarModel::getMacMainMenu())
        model->menuItemsChanged();
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

    // バウンス中はモーダル: Esc（キャンセル要求）以外のキーは全て消費する
    if (bounceActive)
    {
        if (escape)
        {
            Log::info ("bounce.cancel_requested", "source=escape");
            bounceRenderer.cancel(); // 非同期。完了はpollBounce()が拾う
        }
        return true;
    }

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
        if (is (SC::shortcutList) || is (SC::toggleMixer) || is (SC::toggleFxEditor))
            return true; // オーバーレイは高々1枚: AddTrack表示中の⌘?/X/Bは無視
    }
    // Logic準拠: X = ミキサー。表示中もモーダルにしない（Space再生・シーク等はそのまま効く）
    if (is (SC::toggleMixer))
    {
        if (mixerOverlay.isVisible())
        {
            Log::info ("mixer.close");
            mixerOverlay.dismiss();
        }
        else
        {
            Log::info ("mixer.open");
            mixerOverlay.showOver (mixerArea, selectedTrack);
        }
        return true;
    }
    if (mixerOverlay.isVisible() && escape)
    {
        Log::info ("mixer.close", "source=escape");
        mixerOverlay.dismiss();
        return true;
    }
    // Logic準拠: B = 下部FXエディタ（Smart Controls相当）
    if (is (SC::toggleFxEditor))
    {
        toggleFxEditor();
        return true;
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
    if (is (SC::bounce))
    {
        // 通常はネイティブメニューのkeyEquivalent（⌘B）が先にイベントを取るため、
        // ここはメニューが効かない状況のフォールバック
        startBounceFlow();
        return true;
    }
    // Logic準拠: ⌘E = 選択中のリージョン/クリップをオーディオファイルとして書き出す
    if (is (SC::exportRegion))
    {
        exportSelectedItem();
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
    if (is (SC::toggleSolo))
    {
        toggleSoloTracks();
        return true;
    }
    if (is (SC::record))
    {
        toggleRecord();
        return true;
    }
    // Logic準拠: C = サイクル（ループ範囲）の入/切
    if (is (SC::toggleCycle))
    {
        toggleCycle();
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

void MainComponent::toggleCycle()
{
    if (! project->hasCycleRange())
        return; // トグルする範囲がない（範囲はルーラーのドラッグで作る）

    project->cycleEnabled = ! project->cycleEnabled;
    Log::info ("cycle.toggle", "enabled=" + juce::String ((int) project->cycleEnabled)
                                   + " start=" + juce::String (project->cycleStartSixteenths)
                                   + " end=" + juce::String (project->cycleEndSixteenths));
    syncCycleToTransport();
    setDirty (true);
    timeline.refresh(); // 帯の黄/グレー切り替え
}

void MainComponent::syncCycleToTransport()
{
    if (project->hasCycleRange())
    {
        // 順序が重要: 範囲を書いてから enabled を立てる（有効化の瞬間に不整合な範囲を読ませない）
        transport.cycleRange.store (TransportState::packCycle (
            timeline.sixteenthStartSample (project->cycleStartSixteenths),
            timeline.sixteenthStartSample (project->cycleEndSixteenths)));
        transport.cycleEnabled.store (project->cycleEnabled);
    }
    else
    {
        // 逆順: 先に無効化してから範囲を消す
        transport.cycleEnabled.store (false);
        transport.cycleRange.store (0);
    }
}

void MainComponent::toggleMuteSelectedTrack()
{
    if (selectedTrack < 0 || selectedTrack >= (int) project->tracks.size())
        return;

    auto& params = *project->tracks[(size_t) selectedTrack].params;
    params.mute.store (! params.mute.load());
    headers.refreshValues();
    mixerOverlay.sync (selectedTrack); // ミキサー表示中のmキーでもM点灯を同期する
    setDirty (true);
}

void MainComponent::toggleSoloTracks()
{
    // Logic準拠のsキー: どれかがソロ中なら全解除（構成をlastSoloIdsに記憶）、
    // ソロなしなら直近の構成を再適用。記憶が現存トラックに1本も残っていなければ選択トラックをソロにする
    std::vector<juce::uint64> active;
    for (auto& track : project->tracks)
        if (track.params->solo.load())
            active.push_back (track.id);

    if (! active.empty())
    {
        lastSoloIds = active;
        for (auto& track : project->tracks)
            track.params->solo.store (false);
        Log::info ("track.solo_all_off", "remembered=" + juce::String ((int) active.size()));
    }
    else
    {
        auto isRemembered = [this] (juce::uint64 id)
        {
            for (auto rememberedId : lastSoloIds)
                if (rememberedId == id)
                    return true;
            return false;
        };

        int applied = 0;
        for (auto& track : project->tracks)
        {
            if (isRemembered (track.id))
            {
                track.params->solo.store (true);
                ++applied;
            }
        }
        if (applied == 0)
        {
            if (selectedTrack < 0 || selectedTrack >= (int) project->tracks.size())
                return;
            auto& track = project->tracks[(size_t) selectedTrack];
            track.params->solo.store (true);
            lastSoloIds = { track.id };
            applied = 1;
        }
        Log::info ("track.solo_on", "tracks=" + juce::String (applied));
    }

    headers.refreshValues();
    mixerOverlay.sync (selectedTrack); // ミキサー表示中のsキーでもS点灯を同期する（全ストリップ再バインド）
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

    // 下部スロット: ピアノロール⇄FX詳細の排他（後勝ち）。FXパネルより先に取り、横幅フルを使わせる
    // （EQカーブ等の詳細UIに横幅を与えるのがこの配置の目的）。
    // 高さは両パネル共通で、上端のドラッグハンドルで可変（タイムラインの最低高は確保）
    const bool bottomOpen = pianoRoll.isOpen() || fxDetail.isOpen();
    bottomResizeBar.setVisible (bottomOpen);
    if (bottomOpen)
    {
        const int h = juce::jlimit (bottomPanelMinHeight,
                                    juce::jmax (bottomPanelMinHeight, area.getHeight() - 200),
                                    bottomPanelHeight);
        auto panelArea = area.removeFromBottom (h);
        if (pianoRoll.isOpen())
            pianoRoll.setBounds (panelArea);
        else
            fxDetail.setBounds (panelArea);
        // 境界をまたぐ8pxの帯（掴みやすさ優先。パネル群より前面）
        bottomResizeBar.setBounds (panelArea.getX(), panelArea.getY() - 4, panelArea.getWidth(), 8);
    }

    // FXパネル（概要）はヘッダー列のさらに左（基本常設）
    if (fxEditor.isOpen())
        fxEditor.setBounds (area.removeFromLeft (FxEditorView::preferredWidth));

    // ミキサーはヘッダー＋タイムライン領域だけを覆う（上部バー・下部・FXパネルは操作可能なまま。
    // バスストリップをクリックしてFXパネルの表示を切り替える動線を塞がない）
    mixerArea = area;
    if (mixerOverlay.isVisible())
        mixerOverlay.setBounds (mixerArea);

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

    if (bounceOverlay.isVisible())
        bounceOverlay.setBounds (getLocalBounds());
}
