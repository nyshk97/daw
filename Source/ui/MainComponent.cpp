#include "MainComponent.h"

#include <algorithm>
#include <cmath>
#include <errno.h>
#include <signal.h> // kill(pid, 0) による一時ディレクトリの持ち主判定

#include "../shared/Log.h"
#include "../shared/MidiFileTypes.h"
#include "../shared/MidiImport.h"
#include "../shared/ReferenceExport.h"
#include "../shared/ReferenceTools.h"
#include "../shared/SpawnedProcess.h"
#include "../shared/TempDirSweep.h"
#include "Fonts.h"
#include "Shortcuts.h"
#include "Theme.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

// URL取り込みの進捗バーの内訳。前半をダウンロード、残りを取り込み（AudioImporter）に割り当てる。
// オーバーレイは1枚を通しで使うので、切り替わりでバーが戻らない
constexpr float downloadProgressShare = 0.7f;

// ユーザーに見せるエラーは必ずログにも残す（ダイアログは閉じたら消えるため）
void showAlert (const juce::String& title, const juce::String& message)
{
    Log::error ("ui.alert", "title=" + title + " message=" + message.replace ("\n", " / "));
    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                 title, message);
}

// バウンスの前回保存先（セッション内で記憶。プロジェクトを跨いでも引き継ぐ）
juce::File lastBounceDirectory;

// 初期名（「トラック 5」「MIDI 3」）かどうか。手動で命名済みのトラックは自動リネームしない
bool isDefaultTrackName (const juce::String& name)
{
    for (const char* prefix : { u8"トラック ", u8"MIDI " })
    {
        const auto head = juce::String::fromUTF8 (prefix);
        if (name.startsWith (head) && name.length() > head.length()
            && name.substring (head.length()).containsOnly ("0123456789"))
            return true;
    }
    return false;
}

// サンプルの頭の無音カット位置の自動検出。
// 「ピーク絶対値の1%（かつ下限 -60dBFS）を最初に超える位置の1ms手前」。全編が閾値未満なら0
juce::int64 detectSampleStartOffset (const juce::AudioBuffer<float>& audio, double sourceRate)
{
    const int numSamples = audio.getNumSamples();
    const int numChannels = juce::jmin (2, audio.getNumChannels());
    if (numSamples <= 0 || numChannels <= 0)
        return 0;

    float peak = 0.0f;
    for (int ch = 0; ch < numChannels; ++ch)
        peak = juce::jmax (peak, audio.getMagnitude (ch, 0, numSamples));
    const float threshold = juce::jmax (peak * 0.01f, 0.001f);

    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (std::abs (audio.getReadPointer (ch)[i]) <= threshold)
                continue;
            const auto backoff = (juce::int64) (sourceRate * 0.001); // 打点を削らないよう1ms手前から
            return juce::jlimit ((juce::int64) 0, (juce::int64) numSamples - 1,
                                 (juce::int64) i - backoff);
        }
    }
    return 0;
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
    addChildComponent (fxEditor);  // 左のFXパネル（概要・基本常設・Iで開閉）
    addChildComponent (fxDetail);  // 下部のFX詳細（スロットクリックで開く・ピアノロールと排他）
    addChildComponent (bottomResizeBar); // 下部パネル表示中のみ可視（パネル群より後に追加＝前面）
    addChildComponent (rightPanel);      // 右ドック（初期状態は閉じる）
    addChildComponent (rightResizeBar);  // 右ドック表示中のみ可視
    bottomResizeBar.onDragStart = [this] { bottomHeightAtDragStart = bottomPanelHeight; };
    bottomResizeBar.onDragged = [this] (int dy)
    {
        // 上へドラッグ＝パネルが広がる。上限はresized側のクランプに任せる
        bottomPanelHeight = juce::jmax (bottomPanelMinHeight, bottomHeightAtDragStart - dy);
        resized();
    };
    rightResizeBar.onDragStart = [this] { rightWidthAtDragStart = rightPanelWidth; };
    rightResizeBar.onDragged = [this] (int dx)
    {
        rightPanelWidth = juce::jlimit (rightPanelMinWidth, rightPanelMaxWidth,
                                        rightWidthAtDragStart - dx);
        resized();
    };
    addAndMakeVisible (playButton);
    addAndMakeVisible (recordButton);
    addAndMakeVisible (addTrackButton);
    addAndMakeVisible (settingsButton);
    addAndMakeVisible (notesButton);
    addAndMakeVisible (filesButton);
    addAndMakeVisible (gachaButton);
    addAndMakeVisible (clickButton);
    addChildComponent (addTrackOverlay); // トラック追加メニュー表示中のみ可視
    addChildComponent (shortcutOverlay); // ⌘?表示中のみ可視
    addChildComponent (bounceOverlay);   // バウンス中のみ可視
    addChildComponent (importOverlay);   // 取り込み中のみ可視
    addChildComponent (urlOverlay);      // URL入力中のみ可視
    addChildComponent (referenceOverlay); // リファレンス分析中のみ可視
    addAndMakeVisible (lcd);
    addChildComponent (srWarningLabel); // 不一致時のみ表示

    timeline.setProject (project.get());
    headers.setProject (project.get());
    pianoRoll.setProject (project.get());
    rightPanel.setProject (project.get());
    rightPanel.onMemoChanged = [this] { setDirty (true); };
    rightPanel.fileBrowser().onPreviewRequested = [this] (const juce::File& file)
    {
        previewError.clear();
        Log::info ("file_preview.start", "source=" + file.getFullPathName());
        filePreview.start (file);
    };
    rightPanel.fileBrowser().onPreviewStopRequested = [this]
    {
        if (filePreview.status() != AudioFilePreview::Status::idle)
            Log::info ("file_preview.stop");
        filePreview.stop();
        previewError.clear();
    };
    rightPanel.fileBrowser().onImportRequested = [this] (const juce::File& file)
    {
        startImport (file, -1, // 試聴の停止・予約の取り消しは startImport 側で畳む
                     timeline.snapSampleToVisibleGrid (timeline.editPositionSample()));
    };
    // .mid のダブルクリック＝再生ヘッドの小節頭へ取り込み（オーディオと違い配置グリッドは小節固定）
    rightPanel.fileBrowser().onMidiImportRequested = [this] (const juce::File& file)
    {
        importMidiFile (file, playheadBarStartPpq());
    };
    mixerWindow.content().setProject (project.get());
    mixerWindow.content().onSelectTrack = [this] (int index) { selectTrackFromUser (index); };
    mixerWindow.content().onChanged = [this]
    {
        setDirty (true);
        fxEditor.refreshValues(); // 同じsend/gain atomicを表示するエディタ側へ反映
        headers.refreshValues();  // 音量はヘッダーのスライダーにも表示される
    };
    // バス/Masterストリップのクリック → FXパネルでそのチャンネルのチェーンを表示
    mixerWindow.content().onSelectBus = [this] (int bus)
    {
        openFxEditor();
        fxEditor.showBus (bus);
        syncFxDetail();
    };
    mixerWindow.content().onSelectMaster = [this]
    {
        openFxEditor();
        fxEditor.showMaster();
        syncFxDetail();
    };
    // ミキサーのスロット（エディタ側クリック・EQサムネイル）→ チャンネル選択＋下部詳細エディタを開く
    // （FXパネルのスロットクリックと同じトグル挙動。FXパネルが閉じていても対象を確定してから開く）
    mixerWindow.content().onOpenTrackSlot = [this] (int index, int slot)
    {
        selectTrackFromUser (index);
        openFxEditor();
        fxEditor.showTrack (selectedTrack);
        toggleFxDetailSlot (slot);
    };
    mixerWindow.content().onOpenBusSlot = [this] (int bus)
    {
        openFxEditor();
        fxEditor.showBus (bus);
        toggleFxDetailSlot (0);
    };
    mixerWindow.content().onOpenMasterSlot = [this]
    {
        openFxEditor();
        fxEditor.showMaster();
        toggleFxDetailSlot (0);
    };
    // ミキサーウィンドウにフォーカスがあるときのキーは集中ハンドラへ転送（Space再生・X/Esc等がそのまま効く）
    mixerWindow.content().onKey = [this] (const juce::KeyPress& key) { return keyPressed (key); };
    mixerWindow.onDismissed = [this]
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
        mixerWindow.content().refreshValues(); // send/panはミキサーと同じatomicの表示なので反映（非表示時はno-op）
    };
    fxEditor.onFxEnabledChanged = [this]
    {
        setDirty (true);
        mixerWindow.content().refreshValues(); // ミキサーのスロットピルもeqEnabled/compEnabledを表示する（非表示時はno-op）
    };
    fxEditor.onVolumeChanged = [this]
    {
        setDirty (true);
        mixerWindow.content().refreshValues(); // 音量はミキサーのフェーダーと同じatomicの表示（非表示時はno-op）
        headers.refreshValues();      // ヘッダーのスライダーにも表示される
    };
    fxDetail.onCloseRequested = [this] { closeFxDetail(); };

    // 下部詳細に載せる Instrument エディタ。モデルの書き換えはビュー側が行い、
    // undoスナップショットと確定通知（音源への反映）はここで受ける
    instrumentDetail.onWillEdit = [this] (bool valueOnly)
    {
        // サンプル値だけの編集は種別を分けて積む（undo/redoでも音を切らずに戻すため）
        undoStack.begin (*project, valueOnly ? UndoStack::EditKind::sampleValue
                                             : UndoStack::EditKind::structure);
    };
    // 音量・ルート音・頭カット: どれも「次の発音から効く」値なので、スナップショットは再pushせず
    // SamplerEngine の atomic ミラーだけ更新する。再pushすると差し替え検出により
    // 全ノートオフ＋跨ぎノート再発音が走り、追従モードで鳴っている音が頭から鳴り直してしまう
    instrumentDetail.onValueEdited = [this]
    {
        // sync() はサンプル設定のミラー更新だけを行い false を返す（音源の作り直しは起きない）。
        // 万一 true（構成変更を検知）なら、その時だけスナップショットを追従させる
        if (synthBank.sync (*project, transport.sampleRate.load(), transport.blockSizeExpected.load()))
            pushSnapshot();
        setDirty (true);
        pianoRoll.refreshFromModel(); // ルート音の変更で固定行（forcedPitch）と鍵盤ラベルが動く
    };
    instrumentDetail.onPitchModeEdited = [this]
    {
        // pushSnapshot → SynthBank::sync() が音程モードの変化を検知して requestStopAll() を立て、
        // スナップショットのポインタが変わることで resound（跨ぎノートの復元）も走る。
        // ここから sync()/requestStopAll() を直接呼ばないのは、undo/redo（UIを通らない復元）と
        // 経路を一本にするため
        pushSnapshot();
        setDirty (true);
        // ピアノロールの固定行（forcedPitch）が切り替わる。pushSnapshot内のrefreshFromModelが描き直す
        timeline.refresh();
    };
    instrumentDetail.onPreview = [this] (int pitch)
    {
        if (transport.isPlaying.load() || engine.isRecording())
            return;
        // 宛先はエディタが表示しているトラック（選択が動いていても取り違えない）
        if (auto* target = instrumentDetail.shownTrack())
            previewFifo.push ({ PreviewFifo::Command::Type::noteOn, target->id, pitch, 100 });
    };

    // ---- タイムライン・ヘッダの連携 ----
    timeline.onSeek = [this] (juce::int64 samplePos)
    {
        if (! engine.isRecording())
            locate (samplePos); // クリック＝「ここから聴きたい」なのでヘッドと開始位置を揃える
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
    // クリップのオーディオ値（リージョンゲイン・フェード）は経路を分ける: ドラッグ中も音へ
    // 反映したいが、通常の pushSnapshot だと差し替え検出で鳴っているMIDIが消音＋再発音される
    // （頭から鳴り直す）。MIDI構成は変わらないので、世代を据え置く push を使う。
    // undoは種別 clipValue で積み、復元時（afterHistoryRestore）も同じ据え置きpushが選ばれる
    timeline.onWillEditClipValue = [this]
    { undoStack.begin (*project, UndoStack::EditKind::clipValue); };
    timeline.onClipValueEdited = [this]
    {
        pushAudioValueSnapshot();
        setDirty (true);
    };
    // サイクル範囲はundo対象外（音量・ミュートと同じ扱い。Logicもサイクル操作はundoしない）なので
    // onWillEditModel/onModelEdited でなく専用コールバックで Transport同期とdirty化だけ行う
    timeline.onCycleChanged = [this]
    {
        syncCycleToTransport();
        setDirty (true);
    };
    // 曲末フェード（ルーラー右クリック）。開始点はTimelineView側でグリッドへ丸め済み
    timeline.onSongFadeRequested = [this] (int startSixteenths) { setSongFadeFrom (startSixteenths); };
    timeline.onSongFadeClearRequested = [this] { clearSongFade(); };
    // フェード帯の出入りでレーン上端が変わる → トラックヘッダ側の上端も合わせ直す
    timeline.onLaneTopChanged = [this] { resized(); };
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
    timeline.onAnalyzeItemRequested = [this] (int trackIndex, int itemIndex)
    { startReferenceAnalysis (trackIndex, itemIndex); };
    // 録音中は構造編集を止める。キー経由（⌘T/⌃M/⌘R）はMainComponent側でも弾いているが、
    // 右クリックメニュー経由はTimelineViewが直接モデルを書くのでここを通す
    timeline.canEdit = [this] { return ! engine.isRecording(); };
    timeline.onImportFilesDropped = [this] (const juce::StringArray& files, int trackIndex,
                                            juce::int64 startSample)
    {
        // 複数ファイルは先頭のみ処理（残りは完了表示の文言で知らせる）
        startImport (juce::File (files[0]), trackIndex, startSample, files.size() > 1);
    };
    // MIDIトラックの行／ヘッダーへのドロップ = サンプル音源の割り当て（同じ受け口）
    timeline.onAssignInstrumentDropped = [this] (const juce::StringArray& files, int trackIndex)
    {
        startInstrumentImport (juce::File (files[0]), trackIndex, files.size() > 1);
    };
    headers.onAssignInstrumentDropped = [this] (const juce::StringArray& files, int trackIndex)
    {
        startInstrumentImport (juce::File (files[0]), trackIndex, files.size() > 1);
    };
    // .mid のD&D。タイムライン＝ドロップ位置の小節頭 / ヘッダー＝再生ヘッドの小節頭
    // （ヘッダーには横座標が無い）。複数ファイルは先頭のみ処理（オーディオD&Dと同じ規則）
    timeline.onImportMidiDropped = [this] (const juce::StringArray& files, juce::int64 startPpq)
    {
        importMidiFile (juce::File (files[0]), startPpq, files.size() > 1);
    };
    headers.onMidiFilesDropped = [this] (const juce::StringArray& files)
    {
        importMidiFile (juce::File (files[0]), playheadBarStartPpq(), files.size() > 1);
    };

    // ---- ガチャ（右パネル第3モード。Drums / Bass のパーツ切り替え型）----
    rightPanel.gachaPanel().onRoll = [this] { performGachaRoll(); };
    rightPanel.gachaPanel().onPick = [this] (int index) { pickGachaCandidate (index); };
    rightPanel.gachaPanel().onKeep = [this] { keepGachaCandidate(); };
    rightPanel.gachaPanel().onLoopAudition = [this] (int index) { toggleLoopAudition (index); };
    rightPanel.gachaPanel().onLoopAdopt = [this] (int index) { adoptLoopCandidate (index); };
    rightPanel.gachaPanel().onLoopPageRequested = [this] (int page) { performLoopRecommend (page); };
    rightPanel.gachaPanel().onAnchorRelease = [this] { releaseLoopAnchor(); };
    rightPanel.gachaPanel().onCardChanged = [this] (GachaSession::Part part)
    {
        // カード変更は**そのパーツの仮配置だけ**撤去する（他パーツ維持 —
        // 「Drums 仮配置 → Bass のカード選択 → Drums が消える」でコア導線を壊さない）
        cancelGachaPart (part);
        gachaSession.setCandidates (part, {}); // 前カードの候補一覧はパネル側でもクリア済み
        if (part == GachaSession::Part::loops)
        {
            // 旧カードのおすすめ・試聴を残さない（残すと新カードの顔で旧候補を採用できてしまう）
            filePreview.stop();
            rightPanel.gachaPanel().setAuditioningRow (-1);
        }
    };
    // 「原曲を頭出し」の可否（空文字=可）。ファイルI/Oを含むためパネルの updateControls 経由でのみ呼ばれる
    rightPanel.gachaPanel().alignUnavailableReason = [this]() -> juce::String
    {
        if (engine.isRecording())
            return jp (u8"録音中は使えません");
        const auto folder = rightPanel.gachaPanel().selectedCardFolder();
        if (folder == juce::File())
            return jp (u8"カードを選択してください");
        const auto descriptor = ReferenceAlign::readSourceDescriptor (folder);
        if (! descriptor.isValid())
            return jp (u8"元クリップの記録がありません（このリージョンを再分析すると付きます）");
        const auto info = ReferenceAlign::readInfo (folder);
        if (! info.available)
            return info.reason;
        const auto located = ReferenceAlign::locateClip (*project, descriptor);
        if (located.matches == 0)
            return jp (u8"分析時のクリップが見つかりません（削除・トリムされた可能性）");
        if (located.matches > 1)
            return jp (u8"同じ内容のクリップが複数あり特定できません（複製されている可能性）");
        return {};
    };
    rightPanel.gachaPanel().onAlignReference = [this] { performGachaAlign(); };
    rightPanel.gachaPanel().onReportAction = [this] { handleReportAction(); };
    rightPanel.gachaPanel().onRewriteReport = [this] { confirmRewriteReport(); };
    rightPanel.gachaPanel().onCancelReport = [this]
    {
        Log::info ("report.generate.cancel_requested", "source=panel");
        reportGenerator.cancel(); // 非同期。完了は pollReportGeneration() が拾う
    };
    rightPanel.gachaPanel().reportWindowFolder = [this] { return reportWindow.showingFolder(); };
    rightPanel.gachaPanel().reportGeneratingFolder = [this]
    {
        return reportGenerator.status() == ReferenceReportGenerator::Status::running
                   ? reportGenerator.targetFolder()
                   : juce::File();
    };
    reportWindow.onDismissed = [this] { rightPanel.gachaPanel().refreshReportButton(); };
    addChildComponent (toast);
    // 編集開始（undoスナップショットの直前）に仮配置を撤去する。begin が状態を保存する前に
    // 呼ばれるため、仮リージョンが undo 履歴に混入しない
    undoStack.willBegin = [this] { cancelGachaPreview(); };
    // タイムライン上の仮オブジェクト（仮リージョン・自動作成トラック）への操作は「撤去して中止」
    timeline.onPreviewObjectGesture = [this] (juce::uint64 trackId, juce::uint64 regionId)
    {
        if (! gachaSession.isPreviewObject (trackId, regionId))
            return false;
        cancelGachaPreview();
        return true;
    };
    // ループ採用の仮クリップ（音声）への操作も「撤去して中止」（クリップは ID を持たないため
    // fileName で判定する）
    timeline.onPreviewClipGesture = [this] (juce::uint64 trackId, const juce::String& fileName)
    {
        if (! gachaSession.isPreviewClip (trackId, fileName))
            return false;
        cancelGachaPreview();
        return true;
    };
    // 自動作成トラックのリネーム・楽器変更も「撤去して中止」（ヘッダのコールバック内からの
    // 呼び出しになるため、cancelGachaPreview のヘッダ rebuild は非同期で行われる）
    headers.onTrackEditBlocked = [this] (int index)
    {
        if (index < 0 || index >= (int) project->tracks.size()
            || ! gachaSession.trackIsPreviewOwned (project->tracks[(size_t) index].id))
            return false;
        cancelGachaPreview();
        return true;
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
        // 編集ピッチが1行に固定される音源（GMの固定ピッチ打楽器・サンプルの固定モード）は
        // プレビューもその1音で鳴らす（PianoRollView::forcedPitch と同じ規則）
        for (auto& track : project->tracks)
        {
            if (track.id != trackId || track.type != TrackType::midi)
                continue;
            if (track.usesSampler())
            {
                if (! track.samplePitchFollow)
                    pitch = track.sampleRootNote;
            }
            else if (track.drums && track.drumPitch >= 0)
            {
                pitch = track.drumPitch;
            }
        }
        previewFifo.push ({ PreviewFifo::Command::Type::noteOn, trackId, pitch, velocity });
    };
    pianoRoll.onCloseRequested = [this] { closePianoRoll(); };
    headers.onSelect = [this] (int index) { selectTrackFromUser (index); };
    headers.onDeleteRequested = [this] (int index) { requestDeleteTrack (index); };
    headers.onChanged = [this]
    {
        setDirty (true);
        mixerWindow.content().refreshValues(); // 音量はミキサーのフェーダーと同じatomicの表示（非表示時はno-op）
        if (fxEditor.isOpen())
        {
            fxEditor.refreshFromModel (selectedTrack); // リネームのタイトル反映等
            syncFxDetail();
        }
    };
    headers.onWillChangeStructure = [this] { undoStack.begin (*project); };
    headers.onInstrumentChanged = [this] (int index)
    {
        if (index >= 0 && index < (int) project->tracks.size())
        {
            const auto& track = project->tracks[(size_t) index];
            if (track.instrument == InstrumentKind::sample)
                Log::info ("instrument.assign", "source=menu track=" + juce::String (index)
                                                    + " name=" + track.sampleName);
            else
                Log::info ("instrument.revert_gm", "track=" + juce::String (index)
                                                       + " program=" + juce::String (track.gmProgram)
                                                       + " drums=" + juce::String ((int) track.drums));
        }
        pushSnapshot(); // SynthBank が楽器変更を検知して音源を差し替える
        setDirty (true);
        // FXパネルのInstrumentスロット（表示名・点灯/グレー）を更新し、下部エディタも追従させる。
        // GMへ戻したときは syncFxDetail → updateFxDetailBody が中身のないエディタを閉じる
        if (fxEditor.isOpen())
        {
            fxEditor.refreshFromModel (selectedTrack);
            syncFxDetail();
        }
        // 固定ピッチ行・鍵盤ラベルが変わるのでピアノロールも描き直す（pushSnapshot内のrefreshFromModel）
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

    settingsButton.onClick = [this] { toggleDeviceSettings ("button"); };
    settingsButton.setTooltip (Shortcuts::tooltipText (Shortcuts::ID::audioSettings));
    settingsButton.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    settingsButton.setToggleIconColour (Theme::panelToggleOn);
    settingsButton.setBorderless (true);

    notesButton.onClick = [this] { toggleRightPanel (RightPanel::Mode::notes); };
    notesButton.setTooltip (Shortcuts::tooltipText (Shortcuts::ID::toggleNotes));
    notesButton.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    notesButton.setToggleIconColour (Theme::panelToggleOn);
    notesButton.setBorderless (true);

    filesButton.onClick = [this] { toggleRightPanel (RightPanel::Mode::files); };
    filesButton.setTooltip (Shortcuts::tooltipText (Shortcuts::ID::toggleFiles));
    filesButton.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    filesButton.setToggleIconColour (Theme::panelToggleOn);
    filesButton.setBorderless (true);

    gachaButton.onClick = [this] { toggleRightPanel (RightPanel::Mode::gacha); };
    gachaButton.setTooltip (jp (u8"ドラムガチャ")); // ショートカットなし（ボタンのみ）
    gachaButton.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    gachaButton.setToggleIconColour (Theme::panelToggleOn);
    gachaButton.setBorderless (true);

    bounceOverlay.onCancel = [this]
    {
        Log::info ("bounce.cancel_requested", "source=overlay");
        bounceRenderer.cancel(); // 非同期。完了はpollBounce()が拾う
    };

    importOverlay.setLabels (jp (u8"取り込み中…"), jp (u8"取り込みが完了しました"));
    importOverlay.onCancel = [this]
    {
        Log::info ("import.cancel_requested", "source=overlay");
        audioImporter.cancel(); // 非同期。完了はpollImport()が拾う
    };

    referenceOverlay.onCancel = [this]
    {
        Log::info ("reference.analyze.cancel_requested", "source=overlay");
        referenceAnalyzer.cancel(); // 非同期。完了はpollReferenceAnalysis()が拾う
    };

    clickButton.setClickingTogglesState (true); // ONで点灯（Logicのメトロノームボタン風）
    clickButton.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    clickButton.onClick = [this] { transport.clickEnabled.store (clickButton.getToggleState()); };
    clickButton.setTooltip (jp (u8"メトロノーム")); // ショートカットなし

    lcd.tempoLabel().setText (juce::String (project->bpm), juce::dontSendNotification);
    lcd.tempoLabel().onTextChange = [this] { applyBpmText(); };
    refreshKeyDisplay();
    lcd.onKeyClick = [this] { showKeyMenu(); };

    srWarningLabel.setFont (Fonts::body());
    srWarningLabel.setColour (juce::Label::textColourId, Theme::warning);
    srWarningLabel.setJustificationType (juce::Justification::centredLeft);

    // Space（再生/停止）をボタンに奪わせない
    for (auto* c : std::initializer_list<juce::Component*> {
             &playButton, &recordButton, &addTrackButton, &settingsButton, &notesButton,
             &filesButton, &gachaButton, &clickButton })
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
    engine.setFilePreview (&filePreview);
    setAudioChannels (1, 2); // 入力1ch（マイク）・出力2ch
    startTimerHz (30);       // GOTCHAS.md: 通知はpush型でなくpull型（Timerポーリング）
}

MainComponent::~MainComponent()
{
    Log::info ("project.close", "name=" + project->name() + " dirty=" + juce::String ((int) dirty));
    // engine・snapshots より先にオーディオコールバックを止める
    shutdownAudio();
    filePreview.cancelAndWait();
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
    filePreview.deleteRetired();
    if (auto error = filePreview.takeError(); error.isNotEmpty())
    {
        previewError = error;
        Log::error ("file_preview.failed", "message=" + error.replace ("\n", " / "));
    }
    const auto previewStatus = filePreview.status();
    rightPanel.fileBrowser().setPreviewState (previewStatus == AudioFilePreview::Status::loading,
                                              previewStatus == AudioFilePreview::Status::playing,
                                              previewError);
    rightPanel.fileBrowser().setImporting (importActive);
    rightPanel.fileBrowser().setTransportRunning (transport.isPlaying.load());

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
    pollUrlImport(); // ダウンロード完了時にそのまま pollImport 側の取り込みへ引き渡す
    pollImport();
    pollReferenceAnalysis();
    pollReportGeneration();

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
    mixerWindow.content().updateMeters (meterFeeds, busFeeds, masterFeed);
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

// ---- 再生位置 ----

// ヘッドと開始位置を同時に動かす。「ここから聴きたい」という意思表示（クリック・キーシーク）は
// 必ずこちらを通す。片方だけ動かすと、マーカーの位置と実際に鳴る位置が食い違う
void MainComponent::locate (juce::int64 samplePos)
{
    transport.seekRequest.store (samplePos);
    setPlayStart (samplePos);
}

// 開始位置だけを動かす。録音まわり専用で、ヘッドは PlaybackEngine 側が動かす
// （録音開始はカウントイン分だけ手前へシークするので、ここでヘッドに触ると1小節分が消える）
void MainComponent::setPlayStart (juce::int64 samplePos)
{
    playStartSample = samplePos;
    timeline.setPlayStartSample (samplePos);
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
        // ヘッドは止めた場所に残す（そこが⌘T分割・⌘V貼り付けの基準になる）。
        // 次にどこから鳴るかは開始位置マーカーが示しているので、ここで巻き戻さない
        engine.stop();
        Log::info ("transport.stop", "pos=" + juce::String (transport.playheadSamplePos.load())
                                         + " startPos=" + juce::String (playStartSample));
    }
    else
    {
        // 開始位置マーカーの場所から鳴らす。停止中はヘッドがマーカーから離れていることがあるので、
        // ヘッドの位置ではなくマーカーを信じてそこへ飛ばす
        transport.seekRequest.store (playStartSample);

        // サイクルON時に範囲外（終端ちょうど含む）から再生を始めるときは範囲頭へジャンプ（Logic準拠）
        if (project->cycleEnabled && project->hasCycleRange())
        {
            const auto cycleStart = timeline.sixteenthStartSample (project->cycleStartSixteenths);
            const auto cycleEnd = timeline.sixteenthStartSample (project->cycleEndSixteenths);
            if (playStartSample < cycleStart || playStartSample >= cycleEnd)
                locate (cycleStart);
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
    cancelGachaPreview(); // 未確定の仮リージョンを鳴らしながら録らない
    seekResumePending = false; // 録音はカウントイン込みで自前のトランスポート制御を行う
    numSeekKeyCodes = 0;
    // 録音の終了でクリップが増える＝リージョンゲインの吹き出しが保持しているindexが失効するため、
    // 開きっぱなしにしない（吹き出し表示中はモーダルなので、ここへ来るのは⌘.等の経路）
    timeline.dismissGainCallout();

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

    // 録音開始位置 = 開始位置マーカーがいる小節の頭（シークは小節スナップ済みなので通常は一致する）。
    // ヘッドではなくマーカー基準にすることで、「聴いて止めた場所」ではなく
    // 「さっきと同じ場所」から録り直せる（テイクを重ねるときの前提）
    const double barLen = timeline.barLengthSamples();
    const auto bar = (juce::int64) std::floor ((double) playStartSample / barLen);
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
    // 小節頭へ丸めた結果をマーカーへ反映（マーカーと実際の録音開始をズラさない）。
    // ヘッドは engine.startRecording がカウントイン分だけ手前へ飛ばしているので、ここでは触らない
    setPlayStart (punchIn);

    Log::info ("record.start", "file=" + pendingRecordFile.getFileName()
                                   + " track=" + juce::String (selectedTrack)
                                   + " punchIn=" + juce::String (punchIn)
                                   + " countInStart=" + juce::String (punchIn - (juce::int64) std::llround (barLen))
                                   + " sr=" + juce::String (deviceRate, 0));
    closeDeviceSettings(); // デバイス設定は非モーダルなので、録音中に触られないよう閉じる
    updateTransportButtons();
    // 録音中は「原曲を頭出し」を disabled にする（BPM変更とクリップ移動を伴うため。
    // 録音終了側は finishRecording のクリップ追加 → pushSnapshot 経由で引き直される）
    if (rightPanel.isOpen() && rightPanel.mode() == RightPanel::Mode::gacha)
        rightPanel.gachaPanel().refreshAlignAvailability();
}

void MainComponent::finishRecording()
{
    engine.stopRecording();
    engine.stop();
    // 開始位置は録音開始小節の頭のまま＝Spaceでテイクを頭から聴き直せる／rで同じ場所へ録り直せる。
    // ヘッドは録り終わった場所に残す（そこで切る・貼るができる）
    setPlayStart (pendingPunchIn);

    const auto recordedLength = transport.recordedSamples.load();

    // カウントイン中に止めた（ヘッドがpunchInより手前＝負のこともある）／何も録れていない場合は
    // ヘッドを置き去りにせず録音開始位置へ寄せる
    if (recordedLength <= 0 || transport.playheadSamplePos.load() < pendingPunchIn)
        transport.seekRequest.store (pendingPunchIn);

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
        clip.audio = Project::loadWav (pendingRecordFile);
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
    // 録音終了で「原曲を頭出し」の disabled を解く（破棄で終えると pushSnapshot を通らないため）
    if (rightPanel.isOpen() && rightPanel.mode() == RightPanel::Mode::gacha)
        rightPanel.gachaPanel().refreshAlignAvailability();
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
            if (clipIndex >= (int) project->tracks[(size_t) trackIndex].clips.size())
                return;
            // ループ採用の仮クリップは「撤去して中止」（削除でなくキャンセル扱い）
            const auto& target = project->tracks[(size_t) trackIndex].clips[(size_t) clipIndex];
            if (gachaSession.isPreviewClip (project->tracks[(size_t) trackIndex].id, target.fileName))
            {
                cancelGachaPreview();
                return;
            }
            const auto expectedFile = target.fileName;
            const auto expectedStart = target.startSample;

            // begin の willBegin と同じ撤去を**先に**行い、index ずれをここで吸収する
            // （begin 後の再検証だと、中止したとき空の undo 履歴が1件残る）。
            // 撤去後は**同一性まで**再検証する — 別のクリップを消さないため
            cancelGachaPreview();
            if (trackIndex >= (int) project->tracks.size())
                return;
            auto& clips = project->tracks[(size_t) trackIndex].clips;
            if (clipIndex >= (int) clips.size()
                || clips[(size_t) clipIndex].fileName != expectedFile
                || clips[(size_t) clipIndex].startSample != expectedStart)
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
    // ガチャの自動作成トラックは「撤去して中止」（確認ダイアログも出さない）
    if (gachaSession.trackIsPreviewOwned (project->tracks[(size_t) index].id))
    {
        cancelGachaPreview();
        return;
    }

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
    // 仮配置中の並び替えは撤去して中止（from/to は仮トラック込みの並びで計算されているため）
    if (gachaSession.hasPreview())
    {
        cancelGachaPreview();
        return;
    }
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
    // undo/redo は begin を通らない（willBegin フックが効かない）ので、仮配置をここで撤去する
    cancelGachaPreview();
    auto kind = UndoStack::EditKind::structure;
    if (undoStack.undo (*project, kind))
    {
        Log::info ("edit.undo");
        afterHistoryRestore (kind);
    }
}

void MainComponent::performRedo()
{
    if (engine.isRecording())
        return;
    cancelGachaPreview(); // undo/redo は begin を通らない（willBegin フックが効かない）
    auto kind = UndoStack::EditKind::structure;
    if (undoStack.redo (*project, kind))
    {
        Log::info ("edit.redo");
        afterHistoryRestore (kind);
    }
}

// ピーク保持（dB数値表示）はトラックindexに紐づくため、構造変更（削除・並び替え・undo/redo）で
// 別トラックの値を引き継いでしまう。表示専用の値なので全リセットが最も単純で安全
void MainComponent::resetTrackPeakHolds()
{
    for (auto& feed : meterFeeds)
        feed.maxSincePlay = 0.0f;
}

void MainComponent::afterHistoryRestore (UndoStack::EditKind kind)
{
    resetTrackPeakHolds();
    timeline.clearSelection();
    headers.rebuild();
    selectTrack (selectedTrack); // 範囲内にクランプし直す（下部エディタの対象もここで張り替わる）

    // BPMもStateに含まれる。transport（再生換算）とLCD表示を復元値に追従させる
    // （末尾の timeline.refresh() が小節幅を描き直す）
    if (std::abs (transport.bpm.load() - project->bpm) > 1e-9)
    {
        transport.bpm.store (project->bpm);
        lcd.tempoLabel().setText (juce::String (project->bpm), juce::dontSendNotification);
    }
    refreshKeyDisplay(); // キーもStateに含まれる
    rightPanel.gachaPanel().refreshAnchorRow(); // アンカーもStateに含まれる（採用・解除のundo/redo）

    if (kind == UndoStack::EditKind::sampleValue)
    {
        // サンプル値（音量・ルート音・頭カット）だけの復元はスナップショットを再pushしない:
        // 差し替え検出で全ノートオフ＋跨ぎノート再発音が走り、鳴っている音が頭から鳴り直すため。
        // ノート・クリップ・トラック構成は変わっていないので、SamplerEngineのatomicミラー更新で足りる
        if (synthBank.sync (*project, transport.sampleRate.load(), transport.blockSizeExpected.load()))
            pushSnapshot(); // 想定外に構成変更が検知されたときだけ追従
        pianoRoll.refreshFromModel(); // ルート音の復元で固定行（forcedPitch）が動く
    }
    else if (kind == UndoStack::EditKind::clipValue)
    {
        // クリップ値（リージョンゲイン）の復元。スナップショット経由で鳴る値なので再pushは必要だが、
        // MIDI構成は変わっていないので世代を据え置いて発音を乱さない（ドラッグ中の更新と同じ経路）
        pushAudioValueSnapshot();
    }
    else
    {
        pushSnapshot(); // SynthBank も復元後のトラック構成に同期される
    }

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

    // ガチャの仮リージョンは「撤去して中止」（撤去後の古いindexで開くとクラッシュ・誤対象になる。
    // 通常はタイムラインのmouseDownガードで先に撤去されるため、ここは最終防衛線）
    if (gachaSession.isPreviewObject (track.id, region.id))
    {
        cancelGachaPreview();
        return;
    }

    // 同じリージョンを再ダブルクリック → 閉じる（トグル）
    if (pianoRoll.isShowingRegion (track.id, region.id))
    {
        closePianoRoll();
        return;
    }
    closeFxDetail(); // 下部スロットはFX詳細と排他（後勝ち）
    pianoRoll.openRegion (track.id, region.id);
    resized();                      // ここで初めてピアノロールにboundsが付く（閉じている間は更新されない）
    pianoRoll.applyPendingScroll(); // 確定した高さでスクロール位置を決める
    if (! suppressHistoryPush)
        bottomHistory.push (currentPianoRollEntry());
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

// ---- 右パネル（メモ／ファイル。初期状態は閉じる）----

void MainComponent::toggleRightPanel (RightPanel::Mode mode)
{
    if (rightPanel.isOpen() && rightPanel.mode() == mode)
    {
        closeRightPanel();
        return;
    }

    if (rightPanel.isOpen() && rightPanel.mode() == RightPanel::Mode::files
        && mode != RightPanel::Mode::files)
        rightPanel.fileBrowser().cancelPreview(); // 予約中のオートプレビューも取り消す
    if (rightPanel.isOpen() && rightPanel.mode() == RightPanel::Mode::gacha
        && mode != RightPanel::Mode::gacha)
        cancelGachaPreview(); // ガチャモードを離れる＝未確定の仮配置を撤去
    rightPanel.open (mode);
    notesButton.setToggleState (mode == RightPanel::Mode::notes, juce::dontSendNotification);
    filesButton.setToggleState (mode == RightPanel::Mode::files, juce::dontSendNotification);
    gachaButton.setToggleState (mode == RightPanel::Mode::gacha, juce::dontSendNotification);
    resized();
    if (mode == RightPanel::Mode::notes)
        rightPanel.focusNotesEditor();
}

void MainComponent::closeRightPanel()
{
    if (! rightPanel.isOpen())
        return;
    if (rightPanel.mode() == RightPanel::Mode::files)
        rightPanel.fileBrowser().cancelPreview(); // 予約中のオートプレビューも取り消す
    if (rightPanel.mode() == RightPanel::Mode::gacha)
        cancelGachaPreview(); // モードを離れる＝未確定の仮配置を撤去
    rightPanel.close();
    notesButton.setToggleState (false, juce::dontSendNotification);
    filesButton.setToggleState (false, juce::dontSendNotification);
    gachaButton.setToggleState (false, juce::dontSendNotification);
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
    updateFxDetailBody();
    resized();
    // updateFxDetailBody は中身のないエディタを閉じることがあるので、開けたときだけ積む
    if (! suppressHistoryPush && fxDetail.isOpen())
        bottomHistory.push (currentFxDetailEntry (slot));
}

void MainComponent::closeFxDetail()
{
    if (! fxDetail.isOpen())
        return;
    Log::info ("fxdetail.close");
    fxDetail.close(); // 中身（body）の参照はclose側で外れる
    instrumentDetail.setTrack (nullptr);
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
        updateFxDetailBody();
        // 追従は「ユーザーが下部エリアを開く操作をした」わけではないので履歴を1件消費させない。
        // ただし現在地の内容は実表示に合わせて差し替える（放置すると E で復元したときに
        // 追従前のトラックが戻ってしまう）
        if (fxDetail.isOpen())
            bottomHistory.replaceCurrent (currentFxDetailEntry (fxDetailSlot));
        return;
    }
    closeFxDetail();
}

// 下部詳細の中身の載せ替え。Instrumentスロットのときだけ InstrumentDetailView を載せる
// （他のFXは未実装なので空パネルのまま。EQ/Compのエディタも将来ここに足す）。
// サンプルを持たないトラックのInstrumentスロットはクリック不可なので、通常ここには来ない
void MainComponent::updateFxDetailBody()
{
    if (! fxDetail.isOpen())
    {
        fxDetail.setBody (nullptr);
        return;
    }

    if (fxEditor.isInstrumentSlot (fxDetailSlot))
    {
        // 対象はFXパネルが表示しているトラック（選択トラックとは別に追従判定されるため shownTrack を見る）
        const int index = fxEditor.shownTrack();
        const bool valid = index >= 0 && index < (int) project->tracks.size()
                           && project->tracks[(size_t) index].usesSampler();
        if (! valid)
        {
            // undoで割り当てが取り消された・GMへ戻した等。中身のないInstrumentエディタは残さず閉じる
            instrumentDetail.setTrack (nullptr);
            closeFxDetail();
            return;
        }
        instrumentDetail.setTrack (&project->tracks[(size_t) index]);
        fxDetail.setBody (&instrumentDetail);
        return;
    }

    instrumentDetail.setTrack (nullptr);
    fxDetail.setBody (nullptr);
}

// ---- 下部エリアの表示トグル・履歴（E / [ / ]）----
//
// 復元は openPianoRoll / toggleFxDetailSlot をそのまま再利用する。両者が排他close・状態設定・
// resized・applyPendingScroll をすべて含んでいるので、ここで同じ処理を書き直すと二重管理になる。
// 積み直しは suppressHistoryPush で止める。
//
// カーソル操作は「findValid で非破壊に探す → 復元 → 成功したら commit」の順で行う。
// 先にcommitすると、復元に失敗したとき表示だけ据え置きでカーソルが進むズレが残る。

int MainComponent::trackIndexForId (juce::uint64 trackId) const
{
    for (size_t i = 0; i < project->tracks.size(); ++i)
        if (project->tracks[i].id == trackId)
            return (int) i;
    return -1;
}

BottomPanelHistory::Entry MainComponent::currentPianoRollEntry() const
{
    BottomPanelHistory::Entry entry;
    entry.kind = BottomPanelHistory::Entry::Kind::pianoRoll;
    entry.trackId = pianoRoll.currentTrackId();
    entry.regionId = pianoRoll.currentRegionId();
    return entry;
}

BottomPanelHistory::Entry MainComponent::currentFxDetailEntry (int slot) const
{
    BottomPanelHistory::Entry entry;
    entry.kind = BottomPanelHistory::Entry::Kind::fxDetail;
    entry.channelKey = fxEditor.targetKey();
    entry.slot = slot;
    const int index = fxEditor.shownTrack();
    if (index >= 0 && index < (int) project->tracks.size())
        entry.trackId = project->tracks[(size_t) index].id;
    return entry;
}

bool MainComponent::bottomEntryIsValid (const BottomPanelHistory::Entry& entry) const
{
    const int trackIndex = trackIndexForId (entry.trackId);

    if (entry.kind == BottomPanelHistory::Entry::Kind::pianoRoll)
    {
        if (trackIndex < 0)
            return false;
        for (const auto& region : project->tracks[(size_t) trackIndex].midiRegions)
            if (region.id == entry.regionId)
                return true;
        return false; // リージョンが削除された
    }

    if (entry.channelKey == "track")
    {
        if (trackIndex < 0)
            return false; // トラックが削除された
        // Instrumentスロットはサンプルを持つトラックにしか無い（GMへ戻した・undoで割り当てが
        // 消えた場合はここで弾いてスキップさせる）。fxEditor.isInstrumentSlot は「いま表示中の
        // チャンネル」基準なので使えず、スロット番号そのもので判定する
        if (entry.slot == FxEditorView::instrumentSlot
            && ! project->tracks[(size_t) trackIndex].usesSampler())
            return false;
        return true;
    }

    // バス/Masterのチャンネル自体は常に存在する。スロット番号がそのチャンネルに実在するかは
    // 表示対象を切り替えないと分からないので、復元側（restoreBottomEntry）で確認する
    return entry.slot >= 0 && entry.slot < FxEditorView::maxSlots;
}

bool MainComponent::restoreBottomEntry (const BottomPanelHistory::Entry& entry)
{
    if (! bottomEntryIsValid (entry))
        return false;

    const juce::ScopedValueSetter<bool> guard (suppressHistoryPush, true);

    if (entry.kind == BottomPanelHistory::Entry::Kind::pianoRoll)
    {
        // すでに同じリージョンを表示中なら呼ばない（openPianoRoll は同一リージョンで閉じるトグル）
        if (! pianoRoll.isShowingRegion (entry.trackId, entry.regionId))
        {
            const int trackIndex = trackIndexForId (entry.trackId);
            const auto& regions = project->tracks[(size_t) trackIndex].midiRegions;
            int regionIndex = -1;
            for (size_t i = 0; i < regions.size(); ++i)
                if (regions[i].id == entry.regionId)
                    regionIndex = (int) i;
            if (regionIndex < 0)
                return false;
            openPianoRoll (trackIndex, regionIndex);
        }
        return pianoRoll.isOpen();
    }

    // FX詳細は「左パネルの表示対象を先に合わせてから」スロットを開く。
    // 逆にすると toggleFxDetailSlot が拾う fxEditor.targetKey() が古い対象のままになる
    openFxEditor();
    if (entry.channelKey == "track")
        fxEditor.showTrack (trackIndexForId (entry.trackId));
    else if (entry.channelKey.startsWith ("bus"))
        fxEditor.showBus (entry.channelKey.substring (3).getIntValue());
    else
        fxEditor.showMaster();

    if (entry.slot >= fxEditor.numSlots())
        return false; // そのチャンネルには無いスロット（構成が変わった場合の保険）

    // すでに同じチャンネル・同じスロットを表示中なら呼ばない（同上のトグル対策）
    if (! (fxDetail.isOpen() && fxDetailSlot == entry.slot && fxDetailKey == fxEditor.targetKey()))
        toggleFxDetailSlot (entry.slot);
    return fxDetail.isOpen();
}

void MainComponent::toggleBottomPanel()
{
    if (pianoRoll.isOpen() || fxDetail.isOpen())
    {
        Log::info ("bottom.toggle", "action=close");
        if (pianoRoll.isOpen())
            closePianoRoll();
        else
            closeFxDetail();
        return;
    }

    // 閉じている: まず現在地を復元する（カーソルは元からそこなのでcommit不要）
    if (bottomHistory.hasCurrent() && restoreBottomEntry (bottomHistory.current()))
    {
        Log::info ("bottom.toggle", "action=restore pos="
                                        + juce::String (bottomHistory.currentPosition()));
        return;
    }

    // 現在地が無効（対象が消えた）なら後ろ方向に有効なものを探す
    const int position = bottomHistory.findValid (-1, [this] (const BottomPanelHistory::Entry& e)
                                                  { return bottomEntryIsValid (e); });
    if (position >= 0 && restoreBottomEntry (bottomHistory.entryAt (position)))
    {
        bottomHistory.commit (position);
        Log::info ("bottom.toggle", "action=restore pos=" + juce::String (position));
        return;
    }
    Log::info ("bottom.toggle", "action=none");
}

void MainComponent::navigateBottomHistory (int direction)
{
    const int position = bottomHistory.findValid (direction, [this] (const BottomPanelHistory::Entry& e)
                                                  { return bottomEntryIsValid (e); });
    if (position < 0)
    {
        Log::info ("bottom.history", "dir=" + juce::String (direction) + " result=none");
        return;
    }
    if (! restoreBottomEntry (bottomHistory.entryAt (position)))
    {
        Log::info ("bottom.history", "dir=" + juce::String (direction) + " result=failed");
        return;
    }
    bottomHistory.commit (position);
    Log::info ("bottom.history", "dir=" + juce::String (direction)
                                     + " pos=" + juce::String (position));
}

void MainComponent::selectTrack (int index)
{
    selectedTrack = project->tracks.empty()
        ? -1
        : juce::jlimit (0, (int) project->tracks.size() - 1, index);
    headers.setSelectedTrack (selectedTrack);
    timeline.setSelectedTrack (selectedTrack);
    mixerWindow.content().sync (selectedTrack); // トラック増減・選択変更をストリップに反映（非表示中はno-op）
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

void MainComponent::toggleDeviceSettings (const char* source)
{
    if (deviceSettingsWindow != nullptr)
    {
        Log::info ("settings.close", juce::String ("source=") + source);
        closeDeviceSettings();
        return;
    }

    if (engine.isRecording())
        return;

    Log::info ("settings.open", juce::String ("source=") + source);
    auto window = std::make_unique<DeviceSettingsWindow> (deviceManager);
    // Esc/クローズボタンからの通知はウィンドウ自身のコールスタック上で来るので、破棄は次のメッセージへ回す
    window->onDismissed = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->closeDeviceSettings();
        });
    };
    window->openOver (getTopLevelComponent());
    deviceSettingsWindow = std::move (window);
    settingsButton.setToggleState (true, juce::dontSendNotification);
}

void MainComponent::closeDeviceSettings()
{
    if (deviceSettingsWindow == nullptr)
        return;
    deviceSettingsWindow.reset();
    settingsButton.setToggleState (false, juce::dontSendNotification);
}

void MainComponent::applyBpmText()
{
    const double value = lcd.tempoLabel().getText().getDoubleValue();
    if (value < 30.0 || value > 300.0)
    {
        lcd.tempoLabel().setText (juce::String (transport.bpm.load()), juce::dontSendNotification);
        return;
    }
    setProjectBpm (value);
}

// begin しない適用部（transport・project->bpm・LCD の同期を1箇所に保つ）。
// 単独のBPM変更（setProjectBpm）と頭出しの複合操作（performReferenceAlign）の両方から呼ぶ
void MainComponent::applyProjectBpm (double value)
{
    transport.bpm.store (value);
    project->bpm = value;
    lcd.tempoLabel().setText (juce::String (value), juce::dontSendNotification);
}

// BPM変更の一本化（LCD・「BPMをプロジェクトに設定」の両経路）。undo対象
void MainComponent::setProjectBpm (double value)
{
    if (std::abs (project->bpm - value) < 1e-9)
    {
        // 同値なら何もしない（undo履歴にno-opを積まない）。LCDの表示だけ正規化する
        lcd.tempoLabel().setText (juce::String (project->bpm), juce::dontSendNotification);
        return;
    }
    Log::info ("project.bpm", "value=" + juce::String (value));
    undoStack.begin (*project);
    applyProjectBpm (value);
    setDirty (true);
    timeline.refresh(); // 小節幅（サンプル換算）が変わる
}

// キー変更の一本化（LCDのKEYメニュー経路）。undo対象。
// undo粒度は「ユーザーから見た1操作＝1件」— 編集経路はこのメニューのみ（経路を増やさない）
void MainComponent::setProjectKey (const ProjectKey& value)
{
    if (project->key.has_value() && *project->key == value)
        return; // 同値なら何もしない（undo履歴にno-opを積まない）
    Log::info ("project.key", "value=" + ProjectKeys::cliText (value));
    undoStack.begin (*project);
    project->key = value;
    setDirty (true);
    refreshKeyDisplay();
}

void MainComponent::refreshKeyDisplay()
{
    lcd.setKeyText (project->key.has_value() ? ProjectKeys::displayName (*project->key)
                                             : juce::String());
}

// KEYセクションのクリック → フラット24項目・2列（メジャー列/マイナー列）のメニュー。
// 「F♯m」の形で直接選ぶ（サブメニューの1段を省く。モック案Cで確定）。
// 列の割り付け: 見出し込みで各列13項目になるよう [見出し+12メジャー][見出し+12マイナー] の順で足す
void MainComponent::showKeyMenu()
{
    juce::PopupMenu menu;
    const auto current = project->key;
    for (const auto mode : { KeyMode::major, KeyMode::minor })
    {
        menu.addSectionHeader (mode == KeyMode::major ? jp (u8"メジャー") : jp (u8"マイナー"));
        for (int root = 0; root < 12; ++root)
        {
            const ProjectKey candidate { root, mode };
            menu.addItem (ProjectKeys::displayName (candidate),
                          true, current.has_value() && *current == candidate,
                          [this, candidate] { setProjectKey (candidate); });
        }
    }
    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (lcd.localAreaToGlobal (lcd.keySectionBounds()))
                            .withMinimumNumColumns (2)
                            .withMaximumNumColumns (2));
}

// キーあり・頭出し不可の経路（BPM＋キーを undo 1件で）。判断は ReferenceAlign::applyBpmAndKey
void MainComponent::performBpmAndKey (double bpm, const std::optional<ProjectKey>& key)
{
    if (engine.isRecording())
        return;

    switch (ReferenceAlign::applyBpmAndKey (*project, undoStack, bpm, key))
    {
        case ReferenceAlign::ApplyResult::applied:
            applyProjectBpm (project->bpm);
            refreshKeyDisplay();
            setDirty (true);
            timeline.refresh(); // 小節幅（サンプル換算）が変わる
            Log::info ("project.bpm_key",
                       "bpm=" + juce::String (bpm, 3)
                           + " key=" + (key.has_value() ? ProjectKeys::cliText (*key) : "none"));
            break;

        case ReferenceAlign::ApplyResult::noChange:
        case ReferenceAlign::ApplyResult::notFound: // BPM範囲外（カード検証済みの値では来ない）
            break;
    }
}

// 原曲クリップの頭出し。モデル変更（no-op判定・begin 1回・BPM＋キー＋クリップ移動）は
// ReferenceAlign::apply が担当し、ここは録音ガードとUI同期だけを行う
void MainComponent::performReferenceAlign (const ReferenceAlign::ClipDescriptor& descriptor,
                                           double bpm, double firstDownbeatSec,
                                           const std::optional<ProjectKey>& key)
{
    if (engine.isRecording())
        return; // 最終防衛線（パネル側はボタンも disabled にする）

    switch (ReferenceAlign::apply (*project, undoStack, descriptor, bpm, firstDownbeatSec, key))
    {
        case ReferenceAlign::ApplyResult::applied:
            applyProjectBpm (project->bpm); // transport・LCD を適用後の値に同期
            refreshKeyDisplay();
            pushSnapshot();                 // クリップ位置の変更を再生へ反映
            setDirty (true);
            timeline.refresh();
            Log::info ("reference.align", "bpm=" + juce::String (bpm, 3)
                                              + " file=" + descriptor.fileName
                                              + " fdSec=" + juce::String (firstDownbeatSec, 4)
                                              + " key=" + (key.has_value() ? ProjectKeys::cliText (*key)
                                                                           : juce::String ("none")));
            break;

        case ReferenceAlign::ApplyResult::noChange:
            Log::info ("reference.align", "result=no_change"); // 頭出し済み（undoは積まれていない）
            break;

        case ReferenceAlign::ApplyResult::notFound:
            showAlert (jp (u8"頭出しできません"),
                       jp (u8"分析時のクリップが特定できません（削除・トリム・複製の可能性）。"));
            break;
    }
}

// ---- バウンス（書き出し）----

void MainComponent::startBounceFlow()
{
    if (bounceActive || engine.isRecording() || importActive || isUrlImporting())
        return;
    cancelGachaPreview(); // 未確定の仮リージョンを書き出しに混入させない

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
    // クリップ参照とノートのフラット化を再利用（synthは空のまま）。エンジンへは渡さないので
    // MIDI世代は進めない（進めると、この後のリージョンゲイン調整でエンジンが世代変更と誤認して
    // 鳴っているMIDIを消音＋再発音してしまう）
    auto snapshot = project->buildSnapshot (Project::SnapshotChange::offlineRender);
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
        // （スナップショットには境界情報が残らない）。ミュートリージョンは含めない。
        // ループはここに効く: オーディオは展開済みのクリップ実体から取れるが、MIDIは
        // モデル側から算出するので totalLengthPpq（ループ終端）を見ないと本体終端で切れる
        for (auto& clip : trackRender.clips)
            endSample = juce::jmax (endSample, clip.startSample + clip.lengthSamples);
        if (model.type == TrackType::midi)
            for (auto& region : model.midiRegions)
                if (! region.muted)
                    endSample = juce::jmax (endSample, (juce::int64) std::llround (
                                                (double) (region.startPpq + region.totalLengthPpq()) / tps));

        // 固定モード（One Shot）のサンプルはノート長でもリージョン長でもなく「サンプル全長」鳴る。
        // テールの上限（5秒・-60dB打ち切り）に依存させず、範囲自体を末尾まで延ばす
        // （サイクルON時はこの後で範囲を上書きするので、指定範囲どおりに切れる）
        endSample = juce::jmax (endSample, BounceRenderer::oneShotEndSample (model, trackRender.notes,
                                                                            request.bpm, sr));

        // ノートも音声もないトラックはレンダリング対象にしない（終端への寄与は上で済んでいる。
        // ノートのないリージョンだけのプロジェクトはリージョン終端までの無音が書き出される）
        if (trackRender.clips.empty() && trackRender.notes.empty())
            continue;

        if (model.type == TrackType::midi)
        {
            // RT側の共有インスタンスとはprocessBlockが並走するため共有不可。専用に生成する
            trackRender.synth = synthBank.createIndependent (model, sr, BounceRenderer::renderBlockSize);
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

    // 曲末フェード。位置は16分音符単位のまま渡し、サンプル換算はレンダラ側が SongFade で行う
    request.fadeOutStartSixteenths = project->fadeOutStartSixteenths;
    request.fadeOutEndSixteenths = project->fadeOutEndSixteenths;

    // サイクルON時はその範囲を書き出す（Logicのサイクル書き出しと同じ）。
    // ループ素材用途なのでMIDIがあってもテールを付けず、出力長＝範囲サンプル長ちょうどにする
    if (project->cycleEnabled && project->hasCycleRange())
    {
        const double sixteenthLen = sr * 60.0 / request.bpm / 4.0;
        request.startSample = (juce::int64) std::llround ((double) project->cycleStartSixteenths * sixteenthLen);
        request.endSample = (juce::int64) std::llround ((double) project->cycleEndSixteenths * sixteenthLen);
        request.wantTail = false;
    }
    else
    {
        // 曲末フェードの終端＝曲そのものの終端。終端の置き換えとテールの落とし方は
        // Request 側に閉じてある（テスト可能にするため。規則はそちらのコメント参照）
        request.applySongFadeToRange();
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
        engine.stop(); // ヘッドは止めた場所に残す（再生停止と同じ扱い）
        updateTransportButtons();
        Log::info ("transport.stop", "reason=bounce pos=" + juce::String (transport.playheadSamplePos.load())
                                         + " startPos=" + juce::String (playStartSample));
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
    // 書き出し対象が仮オブジェクトなら撤去して中止。他対象でも先に撤去する
    //（仮リージョンは対象トラック/リージョン列の末尾に居るため、撤去で index はずれない）
    if (trackIndex >= 0 && trackIndex < (int) project->tracks.size())
    {
        const auto& t = project->tracks[(size_t) trackIndex];
        if (t.type == TrackType::midi && itemIndex >= 0 && itemIndex < (int) t.midiRegions.size()
            && gachaSession.isPreviewObject (t.id, t.midiRegions[(size_t) itemIndex].id))
        {
            cancelGachaPreview();
            return;
        }
    }
    cancelGachaPreview();
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
        trackRender.synth = synthBank.createIndependent (track, sr, BounceRenderer::renderBlockSize);
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

// ---- オーディオファイルの取り込み ----

void MainComponent::startImportFlow()
{
    if (importActive || bounceActive || engine.isRecording())
        return;

    importChooser = std::make_unique<juce::FileChooser> (
        jp (u8"オーディオ/MIDIを読み込む"),
        juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads"),
        "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.mid;*.midi");

    const auto flags = juce::FileBrowserComponent::openMode
                       | juce::FileBrowserComponent::canSelectFiles;
    importChooser->launchAsync (flags, [this] (const juce::FileChooser& chooser)
    {
        // thisの生存: importChooserはthisのメンバーで、this破棄時にダイアログごと片付く
        const auto chosen = chooser.getResult();
        if (chosen == juce::File())
            return; // キャンセル
        if (MidiFileTypes::isSupported (chosen))
            importMidiFile (chosen, 0); // メニュー経由は曲頭（小節1）へ。オーディオと同じ規則
        else
            startImport (chosen, -1, 0); // メニュー経由は常に新規トラックの小節1（曲頭）へ
    });
}

// プロジェクトSRの確定（最初の録音「または取り込み」時にデバイスレートで確定。
// SR確定はundo対象外 — UndoStackが戻すのはtracks/markersのみで、録音による確定と同じ扱い）。
// URL取り込みでも同じ確定を通すため切り出してある（呼ぶのは「URLが確定してから」で、
// 入力オーバーレイを開いてキャンセルしただけでdirtyにならないようにする）
bool MainComponent::ensureProjectSampleRate (double& targetRate)
{
    targetRate = project->sampleRate;
    if (targetRate > 0.0)
        return true;

    const double deviceRate = transport.sampleRate.load();
    if (deviceRate <= 0.0)
    {
        showAlert (jp (u8"取り込めません"), jp (u8"オーディオデバイスが準備できていません。"));
        return false;
    }

    project->sampleRate = deviceRate;
    setDirty (true);
    targetRate = deviceRate;
    return true;
}

bool MainComponent::startImport (const juce::File& source, int targetTrack, juce::int64 startSample,
                                 bool othersSkipped, const juce::String& displayName)
{
    // URL取り込みからの引き渡しでは、呼ぶ前に urlStage を idle に戻してある
    //（そうしないと自分の状態で自分を弾いて必ず false になる）
    if (importActive || bounceActive || engine.isRecording() || isUrlImporting())
        return false;
    rightPanel.fileBrowser().cancelPreview(); // 予約中のオートプレビューごと畳む

    double targetRate = 0.0;
    if (! ensureProjectSampleRate (targetRate))
        return false;

    importIsInstrument = false;
    importTargetTrack = targetTrack;
    importStartSample = juce::jmax ((juce::int64) 0, startSample);
    return beginImportWorker (source, targetRate, othersSkipped, displayName);
}

// ブラウザ／FinderからMIDIトラックへのドロップ = サンプル音源の割り当て。
// クリップ取り込みと同じ一時ファイル→リネーム方式（排他も importActive を共用）
void MainComponent::startInstrumentImport (const juce::File& source, int trackIndex, bool othersSkipped)
{
    if (importActive || bounceActive || engine.isRecording() || isUrlImporting())
        return;
    if (trackIndex < 0 || trackIndex >= (int) project->tracks.size()
        || project->tracks[(size_t) trackIndex].type != TrackType::midi)
        return;
    // ガチャの自動作成トラックへの音源割り当ては「撤去して中止」（完了処理の begin フックで
    // 仮トラックが消え、保持した trackIndex が範囲外になるため。他トラックへの割り当ては
    // 仮オブジェクトが末尾に居る不変条件により index がずれないのでそのまま進めてよい）
    if (gachaSession.trackIsPreviewOwned (project->tracks[(size_t) trackIndex].id))
    {
        cancelGachaPreview();
        return;
    }
    rightPanel.fileBrowser().cancelPreview(); // 予約中のオートプレビューごと畳む

    importIsInstrument = true;
    importTargetTrack = trackIndex;
    importStartSample = 0;
    // targetSampleRate = 0 = 元のSRを保つ。サンプルは再生時に sourceRate/deviceRate の比で
    // 読み進めるので変換が不要で、デバイスSRを変えても追従できる（プロジェクトSRも確定させない）
    beginImportWorker (source, 0.0, othersSkipped, {});
}

// 取り込みワーカーの起動（クリップ・サンプル共通の尻尾）。完了はpollImport()が拾う
bool MainComponent::beginImportWorker (const juce::File& source, double targetRate, bool othersSkipped,
                                       const juce::String& displayName)
{
    // 一時名はGCパターン（clip-*.wav / instr-*.wav）に掛からない名前。最終名の採番は
    // 完了処理のリネーム時に行う（nextClipFile()は予約できないため、録音完了との採番競合を構造的に防ぐ）
    importTempFile = project->directory.getChildFile (".import-" + juce::Uuid().toString() + ".wav.tmp");
    // URL取り込みでは動画タイトルが渡る。空ならこれまで通り元ファイル名から作る
    //（一時ファイル名が新規トラック名になってしまうのを防ぐ）
    importDisplayName = displayName.isNotEmpty() ? displayName
                                                 : source.getFileNameWithoutExtension();

    AudioImporter::Request request;
    request.sourceFile = source;
    request.tempFile = importTempFile;
    request.targetSampleRate = targetRate;
    if (! audioImporter.start (std::move (request)))
    {
        showAlert (jp (u8"取り込めません"), jp (u8"前回の取り込みが終了していません。"));
        return false;
    }

    Log::info ("import.start", "source=" + source.getFullPathName()
                                   + " kind=" + juce::String (importIsInstrument ? "instrument" : "clip")
                                   + " track=" + juce::String (importTargetTrack)
                                   + " startSample=" + juce::String (importStartSample)
                                   + " sr=" + juce::String (targetRate, 0));
    importActive = true;
    importDoneTicks = 0;
    importOverlay.setLabels (importIsInstrument ? jp (u8"音源を読み込み中…") : jp (u8"取り込み中…"),
                             importIsInstrument
                                 ? (othersSkipped ? jp (u8"先頭の1ファイルのみ割り当てました")
                                                  : jp (u8"音源を割り当てました"))
                                 : (othersSkipped ? jp (u8"先頭の1ファイルのみ取り込みました")
                                                  : jp (u8"取り込みが完了しました")));
    importOverlay.setBounds (getLocalBounds());
    importOverlay.show();
    refreshMacMenu(); // 取り込み中はFileメニューをdisabledにする
    return true;
}

void MainComponent::pollImport()
{
    // 完了表示の自動クローズ
    if (importDoneTicks > 0 && --importDoneTicks == 0)
        importOverlay.dismiss();

    if (! importActive)
        return;

    // URL取り込みではダウンロード分を前半に取っているので、後半の帯に写像する
    importOverlay.setProgress (importProgressBase + importProgressSpan * audioImporter.progress());
    if (audioImporter.status() == AudioImporter::Status::running)
        return;

    importActive = false;
    const auto result = audioImporter.takeResult();
    switch (result.status)
    {
        case AudioImporter::Status::success:
            if (importIsInstrument)
                finishInstrumentImport (result);
            else
                finishImport (result);
            break;

        case AudioImporter::Status::cancelled:
            Log::info ("import.cancelled");
            importOverlay.dismiss();
            break;

        default:
            Log::error ("import.failed", "message=" + result.errorMessage.replace ("\n", " / "));
            importOverlay.dismiss();
            showAlert (jp (u8"取り込みに失敗しました"), result.errorMessage);
            break;
    }

    // 成功・失敗・キャンセルのいずれでも、URLからの取り込みならここで一時ディレクトリを畳む
    // （finishImport の中で return する経路も通るので、分岐の外側に置く）
    cleanupUrlTempDir();
    importProgressBase = 0.0f;
    importProgressSpan = 1.0f;
    refreshMacMenu();
}

// 成功時の確定処理。リネーム→モデル反映→undo登録→保存までメッセージスレッドで一続きに行う
// （モーダル排他により、開始時に保持した配置先はこの時点でも有効）
void MainComponent::finishImport (const AudioImporter::Result& result)
{
    const auto finalFile = project->nextClipFile();
    if (! importTempFile.moveFileTo (finalFile))
    {
        importOverlay.dismiss();
        importTempFile.deleteFile();
        Log::error ("import.failed", "message=rename_failed target=" + finalFile.getFileName());
        showAlert (jp (u8"取り込みに失敗しました"), jp (u8"変換結果の配置に失敗しました。"));
        return;
    }

    Clip clip;
    clip.fileName = finalFile.getFileName();
    clip.name = importDisplayName;
    clip.startSample = importStartSample;
    clip.audio = Project::loadWav (finalFile);
    if (clip.audio == nullptr)
    {
        importOverlay.dismiss();
        finalFile.deleteFile();
        Log::error ("import.failed", "message=readback_failed file=" + finalFile.getFileName());
        showAlert (jp (u8"取り込みに失敗しました"), jp (u8"変換結果の読み込みに失敗しました。"));
        return;
    }
    clip.lengthSamples = clip.audio->getNumSamples();
    clip.buildPeakCache();

    undoStack.begin (*project); // 取り込み＝クリップ/トラック追加もundo対象（SR確定は対象外）

    int trackIndex = importTargetTrack;
    if (trackIndex < 0 || trackIndex >= (int) project->tracks.size()
        || project->tracks[(size_t) trackIndex].type != TrackType::audio)
    {
        Track track;
        track.id = project->allocateId();
        track.type = TrackType::audio;
        track.name = importDisplayName; // 新規トラック名 = 元ファイル名（拡張子なし）
        project->tracks.push_back (std::move (track));
        trackIndex = (int) project->tracks.size() - 1;
        headers.rebuild();
    }
    project->tracks[(size_t) trackIndex].clips.push_back (std::move (clip));

    selectTrackFromUser (trackIndex); // 取り込んだリージョンのトラックを選択状態にする
    pushSnapshot();
    setDirty (true);
    trySave(); // 確定処理の一部として保存まで行う（クリップWAVがGC対象から即座に外れる）
    timeline.refresh();

    Log::info ("import.done", "file=" + finalFile.getFileName()
                                  + " name=" + importDisplayName
                                  + " track=" + juce::String (trackIndex)
                                  + " frames=" + juce::String (result.outputFrames)
                                  + " ch=" + juce::String (result.numChannels)
                                  + " sourceSr=" + juce::String (result.sourceSampleRate, 0));
    importOverlay.showDone();
    importDoneTicks = 40; // 30Hz × 40 ≈ 1.3秒表示して自動で消える
}

// サンプル音源の割り当て確定。リネーム→自動検出→モデル反映→音源生成→保存までを
// メッセージスレッドで一続きに行う（モーダル排他により、開始時の割り当て先はこの時点でも有効）
void MainComponent::finishInstrumentImport (const AudioImporter::Result& result)
{
    const int trackIndex = importTargetTrack;
    if (trackIndex < 0 || trackIndex >= (int) project->tracks.size()
        || project->tracks[(size_t) trackIndex].type != TrackType::midi)
    {
        importOverlay.dismiss();
        importTempFile.deleteFile();
        Log::error ("instrument.load_fail", "message=track_missing track=" + juce::String (trackIndex));
        showAlert (jp (u8"割り当てに失敗しました"), jp (u8"割り当て先のトラックが見つかりません。"));
        return;
    }

    const auto finalFile = project->nextInstrumentFile();
    if (! importTempFile.moveFileTo (finalFile))
    {
        importOverlay.dismiss();
        importTempFile.deleteFile();
        Log::error ("instrument.load_fail", "message=rename_failed target=" + finalFile.getFileName());
        showAlert (jp (u8"割り当てに失敗しました"), jp (u8"変換結果の配置に失敗しました。"));
        return;
    }

    // 元SRのまま保存しているので、再生比率の計算に使うSRをここで確定する
    double sourceRate = 0.0;
    auto audio = Project::loadWav (finalFile, &sourceRate);
    if (audio == nullptr)
    {
        importOverlay.dismiss();
        finalFile.deleteFile();
        Log::error ("instrument.load_fail", "message=readback_failed file=" + finalFile.getFileName());
        showAlert (jp (u8"割り当てに失敗しました"), jp (u8"変換結果の読み込みに失敗しました。"));
        return;
    }

    undoStack.begin (*project); // 音源の割り当てはundo対象

    auto& track = project->tracks[(size_t) trackIndex];
    track.instrument = InstrumentKind::sample;
    track.sampleFile = finalFile.getFileName();
    track.sampleName = importDisplayName;
    track.samplePitchFollow = false; // 落とした直後は常に「固定」（One Shot）
    track.sampleRootNote = 60;       // C3
    track.sampleGain = 1.0f;
    track.sampleAudio = audio;
    track.sampleSourceRate = sourceRate;
    track.samplePeakCache = buildFullPeakCache (*audio);
    track.sampleStartOffset = detectSampleStartOffset (*audio, sourceRate);
    // 初期名のトラックだけサンプル名へ寄せる（手動で命名済みなら触らない）
    if (isDefaultTrackName (track.name))
        track.name = importDisplayName;

    headers.rebuild(); // 楽器プルダウンの項目・トラック名の反映
    selectTrackFromUser (trackIndex);
    pushSnapshot();    // SynthBank が SamplerEngine を生成する
    setDirty (true);
    trySave();         // 確定処理の一部として保存まで行う（サンプルWAVがGC対象から即座に外れる）
    timeline.refresh();

    Log::info ("instrument.assign", "file=" + finalFile.getFileName()
                                        + " name=" + importDisplayName
                                        + " track=" + juce::String (trackIndex)
                                        + " frames=" + juce::String (result.outputFrames)
                                        + " ch=" + juce::String (result.numChannels)
                                        + " sourceSr=" + juce::String (sourceRate, 0)
                                        + " startOffset=" + juce::String (track.sampleStartOffset));
    importOverlay.showDone();
    importDoneTicks = 40; // 30Hz × 40 ≈ 1.3秒表示して自動で消える
}

// ---- MIDIファイル（.mid）の取り込み ----
// オーディオと違いデコード・リサンプルが無いので同期で完結する（ワーカー・オーバーレイなし）。
// プロジェクトのサンプルレートは確定させない（音を持たないため）

juce::int64 MainComponent::playheadBarStartPpq() const
{
    const double barLen = timeline.barLengthSamples();
    const auto bar = (juce::int64) std::floor ((double) timeline.editPositionSample() / barLen);
    return juce::jmax ((juce::int64) 0, bar) * Ppq::ticksPerBar;
}

void MainComponent::importMidiFile (const juce::File& source, juce::int64 startPpq, bool othersSkipped)
{
    if (importActive || bounceActive || engine.isRecording() || isUrlImporting())
        return;
    rightPanel.fileBrowser().cancelPreview(); // 予約中のオートプレビューごと畳む

    MidiImport::Result parsed;
    juce::String error;
    if (! MidiImport::parseFile (source, parsed, error))
    {
        Log::error ("midi_import.failed", "source=" + source.getFullPathName()
                                              + " message=" + error.replace ("\n", " / "));
        showAlert (jp (u8"MIDIを取り込めません"), error);
        return;
    }

    const auto outcome = MidiImport::apply (*project, undoStack, parsed,
                                            source.getFileNameWithoutExtension(), startPpq);
    if (outcome.numTracksCreated == 0)
        return;

    headers.rebuild();
    selectTrackFromUser (outcome.firstTrackIndex); // 取り込んだトラックを選択状態にする
    pushSnapshot();
    setDirty (true);
    trySave();
    timeline.refresh();

    Log::info ("midi_import.done", "source=" + source.getFullPathName()
                                       + " tracks=" + juce::String (outcome.numTracksCreated)
                                       + " drumNotes=" + juce::String ((int) parsed.drumNotes.size())
                                       + " otherNotes=" + juce::String ((int) parsed.otherNotes.size())
                                       + " startPpq=" + juce::String (startPpq)
                                       + (othersSkipped ? " othersSkipped=1" : ""));
}

// ---- リファレンス分析（リージョン右クリック）----

void MainComponent::startReferenceAnalysis (int trackIndex, int itemIndex)
{
    if (importActive || bounceActive || engine.isRecording() || isUrlImporting() || analysisActive)
        return;
    if (trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    if (track.type != TrackType::audio || itemIndex < 0 || itemIndex >= (int) track.clips.size())
        return;
    if (! ReferenceTools::analyzeAvailable())
    {
        showAlert (jp (u8"分析できません"), ReferenceTools::unavailableReason());
        return;
    }

    // 分析は数分かかり、その間はモーダル（キーもクリックも塞ぐ）。再生しっぱなしで
    // 止められない状態を作らないよう、バウンスと同じく開始時に再生を止める
    stopPlaybackForBounce();

    const auto& clip = track.clips[(size_t) itemIndex];
    // 完了ダイアログの「頭出し」用にクリップの同定情報を控える（index は保持しない —
    // 完了時とボタン押下時に記述子から再解決する）
    analysisSourceClip = { clip.fileName, clip.offsetSamples, clip.lengthSamples };
    // 名前はクリップ名から（録音クリップは表示名が空なのでトラック名で代替）
    const auto rawName = clip.name.isNotEmpty() ? clip.name : track.name;
    const auto folder = ReferenceExport::allocateFolder (project->directory, rawName);

    juce::String error;
    if (! ReferenceExport::exportClipRange (project->directory, clip, folder, error))
    {
        folder.deleteRecursively();
        Log::error ("reference.analyze.fail", "stage=export message=" + error.replace ("\n", " / "));
        showAlert (jp (u8"分析できません"), error);
        return;
    }

    ReferenceAnalyzer::Request request;
    request.script = ReferenceTools::analyzeScript();
    request.referenceFolder = folder;
    if (! referenceAnalyzer.start (std::move (request)))
    {
        folder.deleteRecursively();
        showAlert (jp (u8"分析できません"), jp (u8"前回の分析が終了していません。"));
        return;
    }

    analysisActive = true;
    analysisFolder = folder;
    Log::info ("reference.analyze.start", "name=" + folder.getFileName()
                                              + " track=" + juce::String (trackIndex)
                                              + " clip=" + juce::String (itemIndex));
    referenceOverlay.setBounds (getLocalBounds());
    referenceOverlay.show (folder.getFileName());
    refreshMacMenu(); // 分析中はFileメニューをdisabledにする
}

void MainComponent::pollReferenceAnalysis()
{
    if (! analysisActive)
        return;

    referenceOverlay.setStatusLine (referenceAnalyzer.currentLine());
    referenceOverlay.setProgress (referenceAnalyzer.progress());
    if (referenceAnalyzer.status() == ReferenceAnalyzer::Status::running)
        return;

    analysisActive = false;
    const auto result = referenceAnalyzer.takeResult();
    referenceOverlay.dismiss();
    const auto folder = analysisFolder;
    analysisFolder = juce::File();

    switch (result.status)
    {
        case ReferenceAnalyzer::Status::success:
        {
            Log::info ("reference.analyze.done",
                       "name=" + folder.getFileName()
                           + " hasCard=" + juce::String ((int) result.hasCard)
                           + (result.hasCard ? " bpm=" + juce::String (result.bpm, 3) : juce::String())
                           + (result.keyText.isNotEmpty() ? " key=" + result.keyText : juce::String()));
            if (result.hasCard)
            {
                // パネルを開いたまま分析したケースを拾う（一覧はモードを開くたびにしか
                // 列挙し直さないため、ここで足さないと「カードがありません」のまま残る）
                rightPanel.gachaPanel().refreshCards();
                // 「BPM 112.9 / D major」。キーは card.json でゲート落ち省略されることがある
                auto message = jp (u8"BPM ") + juce::String (result.bpm, 1)
                               + (result.keyText.isNotEmpty() ? " / " + result.keyText : juce::String());
                const double cardBpm = result.bpm;
                // 頭出しを提供できるか: groove.json の補正済み位相＋gates.downbeat.ok＋
                // 元クリップが（fileName＋offset＋length で）ちょうど1件同定できること
                const auto alignInfo = ReferenceAlign::readInfo (folder);
                const auto descriptor = analysisSourceClip;
                const bool canAlign = alignInfo.available && descriptor.isValid()
                                      && ReferenceAlign::locateClip (*project, descriptor).matches == 1;
                const double fdSec = alignInfo.firstDownbeatSec;
                const auto title = jp (u8"分析が完了しました（") + folder.getFileName() + jp (u8"）");

                // カードにキーがあれば、ルート/モードを確認・変更できるダイアログへ
                // （ルート変更=声域合わせ・モード変更=平行調取り違えの保険）。無ければ現行のBPMのみ
                if (const auto cardKey = ProjectKeys::fromCardText (result.keyText); cardKey.has_value())
                {
                    showAnalysisKeyDialog (title, message, cardBpm, canAlign, descriptor, fdSec, *cardKey);
                }
                else
                {
                    juce::Component::SafePointer<MainComponent> safe (this);
                    juce::NativeMessageBox::showAsync (
                        juce::MessageBoxOptions()
                            .withIconType (juce::MessageBoxIconType::InfoIcon)
                            .withTitle (title)
                            .withMessage (message)
                            .withButton (canAlign ? jp (u8"BPM を設定して原曲を頭出し")
                                                  : jp (u8"BPM をプロジェクトに設定"))
                            .withButton (jp (u8"閉じる")),
                        [safe, cardBpm, canAlign, descriptor, fdSec] (int button)
                        {
                            if (button != 0 || safe == nullptr)
                                return;
                            if (canAlign)
                                safe->performReferenceAlign (descriptor, cardBpm, fdSec);
                            else
                                safe->setProjectBpm (cardBpm); // undo対応済みの経路（⌘Zで戻せる）
                        });
                }
            }
            else
            {
                // card.json 自体が生成されなかった（BPM/テンポのゲート落ち）。分析結果は残っている
                showAlert (jp (u8"分析は完了しましたが制約カードは生成されませんでした"),
                           result.noCardReason.isNotEmpty()
                               ? result.noCardReason
                               : jp (u8"BPM・テンポのゲートが落ちています（analysis/ 内の結果は参照できます）。"));
            }
            break;
        }

        case ReferenceAnalyzer::Status::cancelled:
            // 不完全な残骸は次回実行を壊す（analyze.sh はステム出力の存在で分離をスキップする）ため
            // 今回のフォルダを丸ごと消す。コピー元リージョンは残っているのでやり直しは効く
            folder.deleteRecursively();
            Log::info ("reference.analyze.cancel", "name=" + folder.getFileName());
            break;

        default:
            folder.deleteRecursively();
            Log::error ("reference.analyze.fail",
                        "name=" + folder.getFileName()
                            + " message=" + result.errorMessage.replace ("\n", " / "));
            showAlert (jp (u8"分析に失敗しました"), result.errorMessage);
            break;
    }
    refreshMacMenu();
}

// 分析完了ダイアログ（カードにキーがある場合）。NativeMessageBox はコンボボックスを
// 載せられないため AlertWindow を使う。初期値はカードの検出値で、適用前にルート
// （声域に合わせて移調する）とモード（キー検出器の平行調取り違えの保険）を変更できる
void MainComponent::showAnalysisKeyDialog (const juce::String& title, const juce::String& message,
                                           double cardBpm, bool canAlign,
                                           const ReferenceAlign::ClipDescriptor& descriptor,
                                           double fdSec, const ProjectKey& cardKey)
{
    auto* window = new juce::AlertWindow (title, message, juce::MessageBoxIconType::InfoIcon, this);
    juce::StringArray rootItems;
    for (int root = 0; root < 12; ++root)
        rootItems.add (ProjectKeys::rootName (root));
    window->addComboBox ("root", rootItems, jp (u8"ルート"));
    window->addComboBox ("mode", { jp (u8"メジャー"), jp (u8"マイナー") }, jp (u8"モード"));
    window->getComboBoxComponent ("root")->setSelectedItemIndex (cardKey.root, juce::dontSendNotification);
    window->getComboBoxComponent ("mode")->setSelectedItemIndex (
        cardKey.mode == KeyMode::minor ? 1 : 0, juce::dontSendNotification);
    window->addButton (canAlign ? jp (u8"BPM とキーを設定して原曲を頭出し")
                                : jp (u8"BPM とキーを設定"),
                       1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton (jp (u8"閉じる"), 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> safe (this);
    window->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safe, window, cardBpm, canAlign, descriptor, fdSec] (int button)
            {
                // window はコールバック後に deleteWhenDismissed=true で破棄される。
                // コンボの読み出しは破棄前のここで行う
                if (button != 1 || safe == nullptr)
                    return;
                const ProjectKey key {
                    juce::jlimit (0, 11, window->getComboBoxComponent ("root")->getSelectedItemIndex()),
                    window->getComboBoxComponent ("mode")->getSelectedItemIndex() == 1 ? KeyMode::minor
                                                                                       : KeyMode::major
                };
                // undo は経路別に1件: 頭出し可 → BPM＋キー＋クリップ移動 / 不可 → BPM＋キー
                if (canAlign)
                    safe->performReferenceAlign (descriptor, cardBpm, fdSec, key);
                else
                    safe->performBpmAndKey (cardBpm, key);
            }),
        true);
}

// ---- ドラムガチャ（右パネル第3モード）----

// 「原曲を頭出し」。保存済み index は使わず、クリック時に source.json の記述子から再解決する
// ガチャパネルのレポートボタン。report.md 有→ウィンドウのトグル、無→生成の開始
void MainComponent::handleReportAction()
{
    const auto folder = rightPanel.gachaPanel().selectedCardFolder();
    if (folder == juce::File())
        return;
    if (ReferenceReport::exists (folder))
        toggleReportWindow();
    else
        startReportGeneration (folder);
}

// トグルの判定は「ウィンドウの表示対象 == 選択中カード」
// （別カード表示中に押したら閉じずに内容を入れ替える）
void MainComponent::toggleReportWindow()
{
    const auto folder = rightPanel.gachaPanel().selectedCardFolder();
    if (folder == juce::File())
        return;
    if (reportWindow.showingFolder() == folder)
    {
        Log::info ("report.close", "source=button");
        reportWindow.dismiss(); // onDismissed がボタン表示を戻す
        return;
    }
    reportWindow.openFor (folder, this);
    rightPanel.gachaPanel().refreshReportButton();
}

// 右クリック「書き直す」。既存 report.md が上書きされる旨を確認してから始める
// （生成自体は report.md.next へのトランザクションなので、失敗・中断では旧レポートが残る）
void MainComponent::confirmRewriteReport()
{
    const auto folder = rightPanel.gachaPanel().selectedCardFolder();
    if (folder == juce::File() || ! ReferenceReport::exists (folder))
        return;
    juce::NativeMessageBox::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle (jp (u8"レポートを書き直す"))
            .withMessage (folder.getFileName() + jp (u8" の既存レポートを書き直します（約8分）。"
                                                     u8"完成すると現在の report.md は置き換わります。"))
            .withButton (jp (u8"書き直す"))
            .withButton (jp (u8"キャンセル")),
        [this, folder] (int result)
        {
            if (result == 0)
                startReportGeneration (folder);
        });
}

void MainComponent::startReportGeneration (const juce::File& folder)
{
    if (! ReferenceTools::reportAvailable())
        return;
    // idle 以外は開始しない（running の多重起動に加え、完了〜poll 回収前の上書きも防ぐ。
    // ボタン側でも無効化しているが、書き直し確認など非同期ダイアログ経由に備える）
    if (reportGenerator.status() != ReferenceReportGenerator::Status::idle)
        return;

    if (! reportGenerator.start ({ ReferenceTools::reportScript(), folder }))
        return;
    Log::info ("report.generate.start", "folder=" + folder.getFileName());
    rightPanel.gachaPanel().setReportProgress (jp (u8"レポートを書いています…"));
    rightPanel.gachaPanel().refreshReportButton(); // ボタン→経過行へ切り替え
}

void MainComponent::pollReportGeneration()
{
    const auto status = reportGenerator.status();
    if (status == ReferenceReportGenerator::Status::idle)
        return;

    if (status == ReferenceReportGenerator::Status::running)
    {
        // 経過行: 「3:24 ｜ ==> グルーヴ」。テキスト更新のみ（updateControls は呼ばない）
        const auto elapsed = juce::Time::getCurrentTime() - reportGenerator.startTime();
        const int totalSeconds = (int) elapsed.inSeconds();
        const auto clock = juce::String (totalSeconds / 60) + ":"
                           + juce::String (totalSeconds % 60).paddedLeft ('0', 2);
        const auto line = reportGenerator.currentLine();
        rightPanel.gachaPanel().setReportProgress (
            clock + (line.isEmpty() ? juce::String() : jp (u8" ｜ ") + line));
        return;
    }

    // 完了（success / cancelled / failed）
    const auto folder = reportGenerator.targetFolder();
    const auto result = reportGenerator.takeResult();
    rightPanel.gachaPanel().refreshReportButton(); // 経過行→ボタンへ戻す

    if (result.status == ReferenceReportGenerator::Status::success)
    {
        Log::info ("report.generate.success", "folder=" + folder.getFileName());
        // ウィンドウが同じフォルダを表示中なら自動再読込（別フォルダ表示中は切り替えない）
        reportWindow.reloadIfShowing (folder);
        juce::Component::SafePointer<MainComponent> safe (this);
        toast.show (folder.getFileName() + jp (u8" のレポートができました — クリックで開く"),
                    false,
                    [safe, folder]
                    {
                        if (safe == nullptr)
                            return;
                        safe->reportWindow.openFor (folder, safe.getComponent());
                        safe->rightPanel.gachaPanel().refreshReportButton();
                    });
    }
    else if (result.status == ReferenceReportGenerator::Status::failed)
    {
        // 全文（exit code＋stderr の理由行）はログへ、トーストには実際の理由（2行目）を優先して出す
        //（1行目は「失敗しました（exit N)」の汎用文で、ロック競合や claude 認証エラーが隠れるため）
        Log::error ("report.generate.fail", "folder=" + folder.getFileName()
                                                + " message=" + result.errorMessage.replace ("\n", " / "));
        const auto detail = result.errorMessage.fromFirstOccurrenceOf ("\n", false, false).trim();
        toast.show (jp (u8"レポート生成に失敗: ")
                        + (detail.isNotEmpty()
                               ? detail
                               : result.errorMessage.upToFirstOccurrenceOf ("\n", false, false)),
                    true, nullptr);
    }
    else
    {
        Log::info ("report.generate.cancelled", "folder=" + folder.getFileName());
    }
}

bool MainComponent::isReportGenerationRunning() const
{
    return reportGenerator.status() == ReferenceReportGenerator::Status::running;
}

void MainComponent::cancelReportForClose()
{
    if (! isReportGenerationRunning())
        return;
    Log::info ("report.generate.cancel_requested", "source=close");
    reportGenerator.cancelAndWait(); // プロセスグループ終了→join→残骸 .next 掃除（ワーカー内）まで
    reportGenerator.takeResult();
    Log::info ("report.generate.cancel", "reason=close");
}

void MainComponent::performGachaAlign()
{
    if (engine.isRecording())
        return; // 最終防衛線（ボタンも disabled にしているが、状態の隙間を塞ぐ）
    auto& panel = rightPanel.gachaPanel();
    const auto folder = panel.selectedCardFolder();
    if (folder == juce::File())
        return;

    const auto descriptor = ReferenceAlign::readSourceDescriptor (folder);
    const auto info = ReferenceAlign::readInfo (folder);
    const auto card = juce::JSON::parse (folder.getChildFile ("card.json").loadFileAsString());
    const auto cardBpm = card.getProperty ("global", {}).getProperty ("bpm", {});
    if (! descriptor.isValid() || ! info.available || ! (cardBpm.isDouble() || cardBpm.isInt()))
    {
        // ボタン有効化後に状態が変わった（ファイル削除等）。理由を出して有効状態を引き直す
        panel.showStatus (jp (u8"頭出しできません — リファレンスの情報を読めませんでした"));
        panel.refreshAlignAvailability();
        return;
    }

    performReferenceAlign (descriptor, (double) cardBpm, info.firstDownbeatSec);
    panel.refreshAlignAvailability();
}

namespace
{
// ガチャCLI（drums.py / bass.py）の同期実行。実測0.5秒程度なので同期で読み切る。
// venv破損等で固まらないよう既定10秒で見切る。失敗時は detail に stderr の最終行
bool runGachaTool (const juce::StringArray& argv, juce::StringArray& stdoutLines,
                   juce::String& detail, int timeoutMs = 10000)
{
    SpawnedProcess proc;
    if (! proc.start (argv))
        return false;
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    juce::StringArray stderrLines;
    const bool finished = proc.readUntilFinished (
        [deadline] { return juce::Time::getMillisecondCounter() > deadline; },
        [&stdoutLines] (const juce::String& line) { stdoutLines.add (line); },
        [&stderrLines] (const juce::String& line) { stderrLines.add (line); });
    if (finished && proc.exitCode() == 0)
        return true;
    for (int i = stderrLines.size(); --i >= 0 && detail.isEmpty();)
        detail = stderrLines[i].trim();
    if (detail.isEmpty())
        detail = "exit=" + juce::String (proc.exitCode());
    return false;
}
}

void MainComponent::performGachaRoll()
{
    switch (rightPanel.gachaPanel().selectedPart())
    {
        case GachaSession::Part::bass:  performBassRoll(); break;
        case GachaSession::Part::loops: performLoopRecommend (1); break;
        case GachaSession::Part::drums: performDrumsRoll(); break;
    }
}

void MainComponent::performDrumsRoll()
{
    auto& panel = rightPanel.gachaPanel();
    const auto cardFolder = panel.selectedCardFolder (GachaSession::Part::drums);
    if (cardFolder == juce::File() || ! ReferenceTools::gachaAvailable())
        return;

    // ロック: パネルがトグルON時に確保した seed をそのまま渡す（現在の選択には依存しない —
    // 「点灯しているのに振り直しで変わる」を作らないため）
    juce::StringArray lockParts;
    if (panel.lockedKickSeed().isNotEmpty())
        lockParts.add ("kick=" + panel.lockedKickSeed());
    if (panel.lockedSnareSeed().isNotEmpty())
        lockParts.add ("snare=" + panel.lockedSnareSeed());
    if (panel.lockedHatSeed().isNotEmpty())
        lockParts.add ("hat=" + panel.lockedHatSeed());

    juce::StringArray argv { ReferenceTools::venvPython().getFullPathName(),
                             ReferenceTools::drumsScript().getFullPathName(),
                             cardFolder.getFullPathName(),
                             "--count", "8", "--porcelain" };
    if (! lockParts.isEmpty())
    {
        argv.add ("--lock");
        argv.add (lockParts.joinIntoString (","));
    }

    juce::StringArray stdoutLines;
    juce::String detail;
    if (! runGachaTool (argv, stdoutLines, detail))
    {
        Log::error ("gacha.roll_failed", "part=drums detail=" + detail);
        showAlert (jp (u8"ガチャに失敗しました"),
                   detail.isNotEmpty() ? detail : jp (u8"drums.py がエラーで終了しました。"));
        return;
    }

    // 候補一覧は「今回の実行で申告されたファイル名」だけから作る（gacha/ の全列挙だと
    // 過去の振り直し分が混ざる）。JSON でない行は読み飛ばさずエラーにする（契約の破れを隠さない）
    std::vector<GachaSession::Candidate> candidates;
    for (const auto& line : stdoutLines)
    {
        if (line.trim().isEmpty())
            continue;
        GachaSession::Candidate candidate;
        if (! GachaSession::parsePorcelainLine (line, candidate))
        {
            Log::error ("gacha.roll_failed", "message=porcelain_parse line=" + line);
            showAlert (jp (u8"ガチャに失敗しました"), jp (u8"drums.py の出力を解釈できませんでした。"));
            return;
        }
        candidates.push_back (std::move (candidate));
    }

    // 一覧のパターン・ミニチュア用に各候補の .mid を読む（8件×数KBなので同期で足りる）。
    // 読めなくても一覧からは外さない（hasPattern=false の行はドットなしで出る）
    for (auto& candidate : candidates)
    {
        MidiImport::Result parsed;
        juce::String parseError;
        const auto midFile = cardFolder.getChildFile ("gacha").getChildFile (candidate.base + ".mid");
        if (MidiImport::parseFile (midFile, parsed, parseError))
        {
            candidate.pattern = GachaSession::patternFromDrumNotes (parsed.drumNotes);
            candidate.hasPattern = true;
        }
    }

    Log::info ("gacha.roll", "part=drums count=" + juce::String ((int) candidates.size())
                                 + " locks=" + (lockParts.isEmpty() ? juce::String ("none")
                                                                    : lockParts.joinIntoString (",")));
    gachaSession.setCandidates (GachaSession::Part::drums, candidates);
    panel.setCandidates (std::move (candidates));
}

MainComponent::DrumsSource MainComponent::resolveDrumsSource() const
{
    DrumsSource source;

    // 1. 仮配置中の Drums（ガチャ産＝seed 持ちで延長再生成できる）
    if (gachaSession.hasPreview (GachaSession::Part::drums))
    {
        const auto trackId = gachaSession.previewTrackId (GachaSession::Part::drums);
        const auto regionId = gachaSession.previewRegionId (GachaSession::Part::drums);
        for (int t = 0; t < (int) project->tracks.size(); ++t)
        {
            if (project->tracks[(size_t) t].id != trackId)
                continue;
            const auto& regions = project->tracks[(size_t) t].midiRegions;
            for (int r = 0; r < (int) regions.size(); ++r)
                if (regions[(size_t) r].id == regionId)
                    return { t, r, true };
        }
    }

    // 2. 選択中の Drum Kit リージョン（手直し済み・手打ち。延長対象外）
    const auto& sel = timeline.getRegionSelection();
    if (sel.isValid() && sel.track < (int) project->tracks.size())
    {
        const auto& track = project->tracks[(size_t) sel.track];
        if (track.type == TrackType::midi && track.drums
            && sel.region < (int) track.midiRegions.size())
            source = { sel.track, sel.region, false };
    }
    return source; // 3. 無し（キックブーストなしで生成する）
}

// 仮配置中のガチャ産ドラムを同一 seed・--bars 変更で延長再生成する（1小節ベース＋
// 4小節ドラムの逆= 8小節ベース＋4小節ドラムで後半が無音にならないための処置）。
// 失敗しても致命ではない（そのまま= 後半ドラム無しで鳴る）ので false を返すだけ
bool MainComponent::extendDrumsPreview (int bars)
{
    const auto& source = gachaSession.previewSource (GachaSession::Part::drums);
    if (! source.isValid() || source.laneSeeds.size() != 3)
        return false;

    juce::StringArray argv { ReferenceTools::venvPython().getFullPathName(),
                             ReferenceTools::drumsScript().getFullPathName(),
                             source.cardFolder.getFullPathName(),
                             "--count", "1", "--bars", juce::String (bars),
                             "--lock",
                             "kick=" + source.laneSeeds[0] + ",snare=" + source.laneSeeds[1]
                                 + ",hat=" + source.laneSeeds[2],
                             "--porcelain" };
    juce::StringArray stdoutLines;
    juce::String detail;
    if (! runGachaTool (argv, stdoutLines, detail))
    {
        Log::error ("gacha.extend_failed", "detail=" + detail);
        return false;
    }
    GachaSession::Candidate candidate;
    juce::String firstLine;
    for (const auto& line : stdoutLines)
        if (firstLine.isEmpty() && line.trim().isNotEmpty())
            firstLine = line;
    if (! GachaSession::parsePorcelainLine (firstLine, candidate))
    {
        Log::error ("gacha.extend_failed", "message=porcelain_parse line=" + firstLine);
        return false;
    }

    MidiImport::Result parsed;
    juce::String error;
    const auto midFile = source.cardFolder.getChildFile ("gacha").getChildFile (candidate.base + ".mid");
    if (! MidiImport::parseFile (midFile, parsed, error))
    {
        Log::error ("gacha.extend_failed", "file=" + midFile.getFileName()
                                               + " message=" + error.replace ("\n", " / "));
        return false;
    }
    // 差し替え（仮配置は active のまま同じ場所に置き直される）
    if (! gachaSession.previewCandidate (GachaSession::Part::drums, *project, parsed, 0))
    {
        // 対象トラック消失等でセッションが畳まれた可能性がある（他の失敗経路と同じ共通同期）
        syncTransportAfterGachaRestore();
        return false;
    }
    auto extended = source;
    extended.bars = bars;
    gachaSession.setPreviewSource (GachaSession::Part::drums, std::move (extended));
    Log::info ("gacha.extend", "bars=" + juce::String (bars) + " candidate=" + candidate.base);
    return true;
}

void MainComponent::performBassRoll()
{
    auto& panel = rightPanel.gachaPanel();
    const auto cardFolder = panel.selectedCardFolder (GachaSession::Part::bass);
    if (cardFolder == juce::File() || ! ReferenceTools::bassGachaAvailable())
        return;

    // 1. 生成キーの解決。**アンカー（採用ループ）があればアンカーのキー**で生成する —
    //    ルート列はループの絶対音高なので、プロジェクトキーが違う（「敷くだけ」・手動変更）と
    //    装飾音のスケールフィルタが進行と噛み合わず、候補が空になり得る（実測: A–F–G の
    //    アンカー＋C# major で bass.py が ValueError）。
    //    アンカーが無いときだけ従来の「生成は常にプロジェクトのキー」＋未設定ガード
    const bool followAnchor = project->loopAnchor.has_value();
    if (! followAnchor && ! project->key.has_value())
    {
        panel.showStatus (jp (u8"キーが未設定です — ヘッダーの KEY をクリックして設定するか、"
                              u8"リージョンの分析完了時に「BPM とキーを設定」でコピーしてください"));
        Log::info ("gacha.bass_roll_blocked", "reason=no_key");
        return;
    }
    const ProjectKey generationKey = followAnchor ? project->loopAnchor->key : *project->key;

    // 2. パターン長を決める。追従時は進行がループのルート列に固定される（--roots）—
    //    パターン長もカードの chords でなくアンカーのループ長が勝つ
    //    （カード=曲A・進行=採用ループ、のパーツ別参照が普通のため）。
    //    アンカーが無ければ従来どおりカード駆動（chords 無し・欠損は 1 = ルート連打）
    const auto card = juce::JSON::parse (cardFolder.getChildFile ("card.json").loadFileAsString());
    int loopBars = 1;
    if (followAnchor)
        loopBars = project->loopAnchor->loopBars;
    else if (const auto chords = card.getProperty ("chords", {}); chords.isObject())
        loopBars = juce::jlimit (1, 16, (int) chords.getProperty ("loop_bars", 1));

    // 3. ドラムソースを解決し、試聴長・キック抽出を計画する（判断は GachaSession::planBassRoll —
    //    試聴長 = max(ドラム長, loop_bars) の loop_bars 倍数切り上げ・PPQ 960→480 換算込み）
    auto source = resolveDrumsSource();
    auto plan = GachaSession::planBassRoll (*project, source.trackIndex, source.regionIndex, loopBars);

    // 4. ベースが長ければ seed 持ちのガチャ産仮配置ドラムのみ延長再生成
    //    （手直し済み・手打ちリージョンは v1 では延長対象外＝そのまま）
    if (source.fromPreview && plan.drumsBars > 0 && plan.previewBars > plan.drumsBars)
    {
        // 延長の失敗は2種類ある: (a) 単なる再生成失敗（セッション健在 → 元の長さのドラムで
        // 続行してよい）と (b) セッション中断（対象トラック消失等 → baseline 復元でキーが
        // 未設定へ戻り得る）。(b) は続行すると空の optional 参照・候補と違うキーでの生成になる
        if (! extendDrumsPreview (plan.previewBars)
            && ! gachaSession.hasPreview (GachaSession::Part::drums))
        {
            panel.showStatus (jp (u8"ドラムの延長に失敗したため、ベースの振り直しを中止しました"));
            Log::error ("gacha.bass_roll_aborted", "reason=extend_terminated_session");
            return;
        }
        source = resolveDrumsSource(); // 延長でリージョンが差し替わったので引き直す
        pushSnapshot();                // 延長後のドラムを再生へ反映
        timeline.refresh();
        // 5. 延長後のドラムリージョンからキックを抽出し直す
        plan = GachaSession::planBassRoll (*project, source.trackIndex, source.regionIndex, loopBars);
    }
    const int previewBars = plan.previewBars;
    const auto& kickTicks = plan.kickTicks;

    // 6. bass.py 呼び出し（--bpm にプロジェクト BPM を必ず渡す — カード BPM とプロジェクトの
    //    再生テンポのずれを作らない。BPM の契約は plan/bass.py 参照）
    juce::StringArray lockParts;
    // 追従時は進行がループ由来で固定されるため prog ロックは送らない（bass.py 側は併用を拒否する）
    if (! followAnchor && panel.lockedProgSeed().isNotEmpty())
        lockParts.add ("prog=" + panel.lockedProgSeed());
    if (panel.lockedRhythmSeed().isNotEmpty())
        lockParts.add ("rhythm=" + panel.lockedRhythmSeed());

    juce::StringArray argv { ReferenceTools::venvPython().getFullPathName(),
                             ReferenceTools::bassScript().getFullPathName(),
                             cardFolder.getFullPathName(),
                             "--key", ProjectKeys::cliText (generationKey),
                             "--bpm", juce::String (project->bpm),
                             "--count", "8", "--porcelain" };
    if (previewBars != loopBars)
    {
        argv.add ("--bars");
        argv.add (juce::String (previewBars));
    }
    if (! kickTicks.isEmpty())
    {
        argv.add ("--kick-ticks");
        argv.add (kickTicks.joinIntoString (","));
    }
    if (! lockParts.isEmpty())
    {
        argv.add ("--lock");
        argv.add (lockParts.joinIntoString (","));
    }

    // ループ追従: アンカーの契約 JSON を一時ファイルで渡す（runGachaTool は同期なので実行後に消す）
    juce::File rootsFile;
    if (followAnchor)
    {
        rootsFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("lala-roots-" + juce::Uuid().toString() + ".json");
        if (! rootsFile.replaceWithText (LoopAnchors::rootsContractJson (*project->loopAnchor)))
        {
            Log::error ("gacha.roll_failed", "part=bass detail=roots_write file="
                                                 + rootsFile.getFullPathName());
            showAlert (jp (u8"ガチャに失敗しました"), jp (u8"ルート列の一時ファイルを書けませんでした。"));
            return;
        }
        argv.add ("--roots");
        argv.add (rootsFile.getFullPathName());
    }

    juce::StringArray stdoutLines;
    juce::String detail;
    const bool toolOk = runGachaTool (argv, stdoutLines, detail);
    if (rootsFile != juce::File())
        rootsFile.deleteFile(); // 出力は取り込み済み。契約は project.json のアンカーが持ち続ける
    if (! toolOk)
    {
        Log::error ("gacha.roll_failed", "part=bass detail=" + detail);
        showAlert (jp (u8"ガチャに失敗しました"),
                   detail.isNotEmpty() ? detail : jp (u8"bass.py がエラーで終了しました。"));
        return;
    }

    std::vector<GachaSession::Candidate> candidates;
    for (const auto& line : stdoutLines)
    {
        if (line.trim().isEmpty())
            continue;
        GachaSession::Candidate candidate;
        if (! GachaSession::parseBassPorcelainLine (line, candidate))
        {
            Log::error ("gacha.roll_failed", "message=porcelain_parse line=" + line);
            showAlert (jp (u8"ガチャに失敗しました"), jp (u8"bass.py の出力を解釈できませんでした。"));
            return;
        }
        candidates.push_back (std::move (candidate));
    }

    // ミニチュア（音高ストリップ）用に各候補の .mid を読む。パターン1周ぶんだけ描く
    for (auto& candidate : candidates)
    {
        MidiImport::Result parsed;
        juce::String parseError;
        const auto midFile = cardFolder.getChildFile ("gacha").getChildFile (candidate.base + ".mid");
        if (MidiImport::parseFile (midFile, parsed, parseError))
            candidate.bassDots = GachaSession::bassDotsFromNotes (
                parsed.otherNotes, (juce::int64) loopBars * Ppq::ticksPerBar);
    }

    Log::info ("gacha.roll", "part=bass count=" + juce::String ((int) candidates.size())
                                 + " key=" + ProjectKeys::cliText (generationKey)
                                 + " bars=" + juce::String (previewBars)
                                 + " roots=" + (followAnchor ? "anchor" : "card")
                                 + " kicks=" + juce::String (kickTicks.size())
                                 + " locks=" + (lockParts.isEmpty() ? juce::String ("none")
                                                                    : lockParts.joinIntoString (",")));
    gachaSession.setCandidates (GachaSession::Part::bass, candidates);
    panel.setCandidates (std::move (candidates));
}

void MainComponent::pickGachaCandidate (int index)
{
    if (engine.isRecording())
        return;
    auto& panel = rightPanel.gachaPanel();
    const auto part = panel.selectedPart();
    if (index < 0 || index >= (int) panel.candidates().size())
        return;
    const auto cardFolder = panel.selectedCardFolder();
    if (cardFolder == juce::File())
        return;
    const auto candidate = panel.candidates()[(size_t) index];
    const auto midFile = cardFolder.getChildFile ("gacha").getChildFile (candidate.base + ".mid");

    MidiImport::Result parsed;
    juce::String error;
    if (! MidiImport::parseFile (midFile, parsed, error))
    {
        Log::error ("gacha.pick_failed", "file=" + midFile.getFileName()
                                             + " message=" + error.replace ("\n", " / "));
        showAlert (jp (u8"候補を読めません"), error);
        return;
    }

    // 配置位置（パーツ初回のみ有効。差し替えは同じ場所）:
    // - Drums: 再生ヘッドの小節頭（録音開始位置と同じ丸め）
    // - Bass: 共通の試聴開始 PPQ = 仮配置中ドラム → その開始 / 選択中 Drum Kit リージョン →
    //   その先頭 / 無し → 再生ヘッドの小節頭（ドラムと重なって初めて評価できるため）
    juce::int64 startPpq = playheadBarStartPpq();
    if (part == GachaSession::Part::bass)
    {
        if (gachaSession.hasPreview (GachaSession::Part::drums))
            startPpq = gachaSession.previewStartPpq (GachaSession::Part::drums);
        else if (const auto source = resolveDrumsSource(); source.isValid())
            startPpq = project->tracks[(size_t) source.trackIndex]
                           .midiRegions[(size_t) source.regionIndex].startPpq;
    }

    // 仮配置（2回目以降は差し替え）。対象トラックの決定は GachaSession（Drum Kit / GM ベース系の
    // 流用 or 自動作成）。undo には積まない（「残す」で全パーツまとめて1件積む）
    if (! gachaSession.previewCandidate (part, *project, parsed, startPpq))
    {
        // 失敗でセッションが畳まれた場合、BPM・キー・アンカーが baseline へ戻っている
        syncTransportAfterGachaRestore();
        return;
    }

    // 延長再生成用に「どのカードのどの seed から来た候補か」を記録する
    GachaSession::PreviewSource source;
    source.cardFolder = cardFolder;
    const auto lengthPpq = part == GachaSession::Part::drums ? parsed.drumRegionLengthPpq
                                                             : parsed.otherRegionLengthPpq;
    source.bars = (int) ((lengthPpq + Ppq::ticksPerBar - 1) / Ppq::ticksPerBar);
    if (part == GachaSession::Part::drums)
        source.laneSeeds = { candidate.kickSeed, candidate.snareSeed, candidate.hatSeed };
    else
        source.laneSeeds = { candidate.progSeed, candidate.rhythmSeed };
    gachaSession.setPreviewSource (part, std::move (source));

    headers.rebuild(); // 自動作成トラックが増えたかもしれない
    for (int i = 0; i < (int) project->tracks.size(); ++i)
        if (project->tracks[(size_t) i].id == gachaSession.previewTrackId (part))
            selectTrackFromUser (i);
    pushSnapshot(); // 普段の再生で曲と一緒に鳴らす（単体プレビュー経路は作らない）
    timeline.refresh();
    panel.setPreviewActive (part, true);
    Log::info ("gacha.pick", juce::String ("part=") + (part == GachaSession::Part::drums ? "drums" : "bass")
                                 + " candidate=" + candidate.base);
}

void MainComponent::keepGachaCandidate()
{
    if (! gachaSession.hasPreview())
        return;
    // keep が失敗＝セッション全体キャンセルになるケース（ループのWAV書き出し失敗等）に備えて、
    // 撤去されるトラックの有無を**先に**記録する（cancelGachaPreview と同じ手順）
    bool trackRemoved = false;
    for (int i = 0; i < GachaSession::numParts; ++i)
        trackRemoved = trackRemoved
                       || gachaSession.trackIsPreviewOwned (
                              gachaSession.previewTrackId ((GachaSession::Part) i));
    const bool hadLoops = gachaSession.hasPreview (GachaSession::Part::loops);

    // SR 未確定プロジェクトのループ確定（確定をここまで遅らせるのは、キャンセルだけで
    // SR と dirty が残るのを避けるため）。仮配置から「残す」までの間にデバイス SR が
    // 変わっていることがある（44.1k→48k 等）— 変換時のレートのまま確定すると、以後この
    // プロジェクトは現デバイスと恒久的にずれる。**keep 直前に現在レートと突き合わせ、
    // ずれていたら現在レートで変換し直して**（同一アンカーの差し替え＝進行・値は不変）から確定する
    const double sampleRateBeforeKeep = project->sampleRate;
    bool loopReconverted = false;
    if (hadLoops && project->sampleRate <= 0.0)
    {
        const double deviceRate = transport.sampleRate.load();
        if (deviceRate > 0.0 && gachaSession.loopPreviewSampleRate() > 0.0
            && ! juce::approximatelyEqual (deviceRate, gachaSession.loopPreviewSampleRate())
            && project->loopAnchor.has_value())
        {
            const auto wavFile = ReferenceTools::libraryRoot()
                                     .getChildFile (project->loopAnchor->libraryPath);
            auto audio = Project::loadWavResampled (wavFile, deviceRate);
            bool replaced = false;
            if (audio != nullptr)
            {
                GachaSession::LoopPreviewInput input;
                input.anchor = *project->loopAnchor;
                input.audio = std::move (audio);
                input.audioSampleRate = deviceRate;
                input.displayName = wavFile.getFileNameWithoutExtension();
                input.startSample = 0;
                input.loopCount = 1;
                input.applyKeyBpm = false; // 値は適用済み。クリップだけ現在レートに入れ替える
                replaced = gachaSession.previewLoopCandidate (*project, input);
            }
            if (! replaced)
            {
                // 再変換できないまま確定すると変換時レートで固定され、現デバイスと恒久的にずれる。
                // また差し替え失敗（対象トラック消失）を無視すると、ループだけ欠けた部分確定になる。
                // どちらも Keep を中止し、通常キャンセルと同じ後処理まで通す（差し替え失敗は
                // ループ取り消し相当の巻き戻し＝追従ベースの撤去・値の復元を伴うため、
                // transport 同期だけでなくヘッダ・スナップショットの整理が要る）
                afterGachaCancel (trackRemoved);
                showAlert (jp (u8"残せません"),
                           jp (u8"デバイスのサンプルレートが変わったため、ループを変換し直せません"
                               u8"でした。もう一度お試しください。"));
                Log::error ("gacha.keep_failed", audio == nullptr ? "reason=loop_reconvert_load"
                                                                  : "reason=loop_reconvert_replace");
                return;
            }
            loopReconverted = true;
            Log::info ("gacha.loop_reconvert", "rate=" + juce::String (deviceRate));
        }
        if (gachaSession.loopPreviewSampleRate() > 0.0)
            project->sampleRate = gachaSession.loopPreviewSampleRate();
    }

    if (! gachaSession.keep (*project, undoStack)) // 全パーツ一括で pushCommitted 1件（redo履歴破棄）
    {
        project->sampleRate = sampleRateBeforeKeep; // keep 不成立なら SR 確定も巻き戻す
        // keep 不成立＝キャンセル相当（仮オブジェクトの撤去・値の復元が走っている）。
        // transport 同期だけでなく、通常キャンセルと同じ後処理（選択解除・ヘッダの
        // unbind/rebuild・pushSnapshot）まで通す — 省くと削除済みプレビューが鳴り続ける
        afterGachaCancel (trackRemoved);
        if (hadLoops)
            rightPanel.gachaPanel().showStatus (
                jp (u8"ループの WAV 書き出しに失敗したため、仮配置をキャンセルしました"));
        Log::error ("gacha.keep_failed", hadLoops ? "reason=loop_materialize_or_missing"
                                                  : "reason=missing_objects");
        return;
    }
    setDirty (true); // 「残す」で初めてプロジェクトの変更として扱う（保存は⌘S）
    if (loopReconverted)
        pushSnapshot(); // エンジンは旧バッファの shared_ptr を掴んだまま — 差し替えを再生へ反映する
    rightPanel.gachaPanel().clearAllPreviewBadges();
    timeline.refresh();
    Log::info ("gacha.keep");
}

// 撤去後のUI・スナップショット同期（全撤去・パーツ撤去の共通の尻尾）
// GachaSession の復元（キャンセル・差し替え失敗・keep不成立）は Project の BPM・キー・アンカーを
// baseline へ戻すことがある。transport と LCD をここで追従させる — 同期しないと、直後の保存が
// project->bpm = transport.bpm.load() で候補BPMを再代入し「アンカー無し・BPMだけ候補値」が
// 保存される（undo復元後の同期と同じ規則）
void MainComponent::syncTransportAfterGachaRestore()
{
    if (! juce::approximatelyEqual (transport.bpm.load(), project->bpm))
    {
        transport.bpm.store (project->bpm);
        lcd.tempoLabel().setText (juce::String (project->bpm), juce::dontSendNotification);
        timeline.refresh(); // BPM が変わったら小節幅を描き直す
    }
    refreshKeyDisplay();

    // バッジもセッションの真実へ同期する — 失敗でセッションが畳まれた場合に「仮配置中」の
    // 表示だけ残ると、Keep もキャンセルも効かない見た目になる（GachaSession 側は破棄済みのため）
    for (int i = 0; i < GachaSession::numParts; ++i)
        rightPanel.gachaPanel().setPreviewActive ((GachaSession::Part) i,
                                                  gachaSession.hasPreview ((GachaSession::Part) i));
}

void MainComponent::afterGachaCancel (bool trackRemoved)
{
    syncTransportAfterGachaRestore();

    timeline.clearSelection();
    if (trackRemoved)
    {
        // ヘッダは即座にバインドを外し（30Hz timer のダングリング防止）、rebuild は非同期にする。
        // begin フック経由でヘッダ自身のコールバック内から呼ばれることがあり、
        // 同期 rebuild は実行中のコンポーネントを破壊するため
        headers.unbindAll();
        selectTrack (selectedTrack); // 範囲内へクランプ（有効な範囲だけ再バインドされる）
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->headers.rebuild();
        });
    }
    pushSnapshot(); // 仮リージョンを消した状態を再生へ反映
    timeline.refresh();
}

// ---- Loops タブ（ループ検索ガチャ）。候補作りは recommend.py・進行検出は looproots.py・
// 仮配置と実体化は GachaSession（判断ロジックは shared 側でテスト済み）----

void MainComponent::performLoopRecommend (int page)
{
    auto& panel = rightPanel.gachaPanel();
    const auto refdir = panel.selectedCardFolder (GachaSession::Part::loops);
    if (refdir == juce::File())
        return;
    filePreview.stop(); // ページが変わると行の意味が変わる
    panel.setAuditioningRow (-1);

    // 10本/頁: パネルの縦空間はこの画面比率で15行以上入るが、スクロールなしで全行が
    // 見える本数に留める（試聴の行き来はページングが担う）
    constexpr int pageSize = 10;
    juce::StringArray argv { ReferenceTools::venvPython().getFullPathName(),
                             ReferenceTools::recommendScript().getFullPathName(),
                             refdir.getFullPathName(),
                             "--page", juce::String (juce::jmax (1, page)),
                             "--page-size", juce::String (pageSize),
                             "--json" };
    juce::StringArray stdoutLines;
    juce::String detail;
    // 30秒: upper-features キャッシュは analyze.py が分析中に温めるので通常0.5秒だが、
    // 本修正前に分析したリファレンスはキャッシュが無く初回に十数秒かかる（実測13秒）。
    // 10秒で見切ると kill→キャッシュ未生成→リトライも毎回失敗、の詰みになる
    if (! runGachaTool (argv, stdoutLines, detail, 30000))
    {
        Log::error ("gacha.loop_recommend_failed", "detail=" + detail);
        showAlert (jp (u8"おすすめの取得に失敗しました"),
                   detail.isNotEmpty() ? detail : jp (u8"recommend.py がエラーで終了しました。"));
        return;
    }
    GachaSession::LoopRecommendation recommendation;
    if (! GachaSession::parseRecommendJson (stdoutLines.joinIntoString ("\n"), recommendation))
    {
        Log::error ("gacha.loop_recommend_failed", "message=json_parse");
        showAlert (jp (u8"おすすめの取得に失敗しました"),
                   jp (u8"recommend.py の出力を解釈できませんでした。"));
        return;
    }
    const bool empty = recommendation.candidates.empty();
    Log::info ("gacha.loop_recommend", "ref=" + refdir.getFileName()
                                           + " page=" + juce::String (recommendation.page)
                                           + " total=" + juce::String (recommendation.total)
                                           + " count=" + juce::String ((int) recommendation.candidates.size()));
    panel.setLoopPage (std::move (recommendation));
    if (empty)
        panel.showStatus (jp (u8"キーと BPM の条件に合うループがありません — ライブラリを増やすか、"
                              u8"別のリファレンスで試してください"));
}

void MainComponent::toggleLoopAudition (int index)
{
    auto& panel = rightPanel.gachaPanel();
    const auto& page = panel.loopPage();
    if (index < 0 || index >= (int) page.candidates.size())
        return;
    if (panel.auditionRow() == index) // 同じ行の再クリック＝停止
    {
        filePreview.stop();
        panel.setAuditioningRow (-1);
        return;
    }
    const auto file = ReferenceTools::libraryRoot().getChildFile (page.candidates[(size_t) index].path);
    if (! file.existsAsFile())
    {
        panel.showStatus (jp (u8"ファイルが見つかりません: ") + page.candidates[(size_t) index].path);
        return;
    }
    if (filePreview.start (file))
    {
        panel.setAuditioningRow (index);
        Log::info ("gacha.loop_audition", "file=" + file.getFileName());
    }
}

void MainComponent::adoptLoopCandidate (int index)
{
    if (engine.isRecording())
        return;
    if (loopAdoptDetection != nullptr)
        return; // 採用フローが進行中（ダイアログ表示中 or 検出待ち）— 多重起動しない
    auto& panel = rightPanel.gachaPanel();
    const auto& page = panel.loopPage();
    if (index < 0 || index >= (int) page.candidates.size())
        return;
    const auto candidate = page.candidates[(size_t) index]; // コピー（非同期ダイアログの間のページ差し替え対策）

    filePreview.stop();
    panel.setAuditioningRow (-1);

    const auto wavFile = ReferenceTools::libraryRoot().getChildFile (candidate.path);
    if (! wavFile.existsAsFile())
    {
        showAlert (jp (u8"採用できません"), jp (u8"ライブラリにファイルがありません: ") + candidate.path);
        return;
    }

    // SR の確認だけ先に行う（wav の変換は**ダイアログ確定後**の placeLoopPreview — ダイアログ中に
    // デバイス SR が変わっても、決定時点のレートで変換するので速度・音程が狂わない。
    // キャンセル時に変換を捨てる無駄も無い）
    if (project->sampleRate <= 0.0 && transport.sampleRate.load() <= 0.0)
    {
        showAlert (jp (u8"採用できません"), jp (u8"オーディオデバイスが準備できていません。"));
        return;
    }

    // 進行検出（採用時に1回だけ。結果はアンカーとして project.json に永続化され、
    // ベースガチャは保存済みの値を読む — 生成のたびに再検出しない）
    const auto rootsFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("lala-looproots-" + juce::Uuid().toString() + ".json");
    const ProjectKey loopKey { candidate.keyRoot, candidate.keyMode };
    juce::StringArray argv { ReferenceTools::venvPython().getFullPathName(),
                             ReferenceTools::looprootsScript().getFullPathName(),
                             wavFile.getFullPathName(),
                             "--bpm", juce::String (candidate.bpm),
                             "--key", ProjectKeys::cliText (loopKey),
                             "--out", rootsFile.getFullPathName() };
    if (candidate.loopBars <= 0)
    {
        // 推定不明はそのまま採用不可（旧実装の「wav 実尺を無条件 floor」フォールバックは
        // 最大ほぼ1小節の実音を切るため削除 — 30ms 規則の判定は index 側が唯一の実装者。
        // docs/plans/2026-08-09-0024）
        Log::info ("gacha.loop_adopt_blocked", "reason=no_loop_bars file=" + candidate.path);
        showAlert (jp (u8"採用できません"),
                   jp (u8"ループ長を判定できない素材です（実尺が整数小節に合いません）。"
                       u8"ライブラリを再インデックスすると直ることがあります。"));
        return;
    }
    argv.add ("--bars");
    argv.add (juce::String (candidate.loopBars));
    // 検出はワーカーで並走させ、完了は callAsync でメッセージスレッドへ戻す。
    // 結果の consume（契約検証〜敷く）はダイアログ確定後の finishLoopAdoption
    auto detection = std::make_shared<LoopAdoptDetection>();
    detection->libraryPath = candidate.path; // 契約の source は表示用ファイル名 — 相対パスで上書きする
    detection->bpm = candidate.bpm;
    loopAdoptDetection = detection;
    juce::Component::SafePointer<MainComponent> safe (this);
    const bool launched = juce::Thread::launch ([argv, rootsFile, detection, safe]
    {
        juce::StringArray stdoutLines;
        juce::String detail;
        const bool ok = runGachaTool (argv, stdoutLines, detail);
        const auto contract = ok ? rootsFile.loadFileAsString() : juce::String();
        rootsFile.deleteFile();
        juce::MessageManager::callAsync ([detection, ok, contract, detail]
        {
            detection->finished = true;
            detection->ok = ok;
            detection->contractText = contract;
            detection->detail = detail;
            if (detection->dialogCancelled)
                return; // キャンセル済み — 結果は黙って捨てる
            if (detection->pendingAction)
            {
                auto action = std::move (detection->pendingAction);
                detection->pendingAction = nullptr;
                action(); // 早押しの続き（MainComponent の生存は action 側の SafePointer が見る）
            }
        });
    });
    if (! launched)
    {
        // 起動失敗を無視すると finished が永遠に立たず、検出待ちで固まった上に
        // loopAdoptDetection が残って以後の採用が全部弾かれる（レビュー指摘）
        loopAdoptDetection = nullptr;
        rootsFile.deleteFile();
        Log::error ("gacha.loop_adopt_failed", "stage=thread_launch");
        showAlert (jp (u8"採用できません"), jp (u8"進行検出のワーカースレッドを起動できませんでした。"));
        return;
    }

    // 逆コピーの確認（モック確定仕様: 設定して敷く / 敷くだけ / キャンセル）。
    // **検出を待たずに即出す** — 読んでいる時間で検出が終わるのが普通で、待ちが見えない
    const auto keyText = ProjectKeys::displayName (loopKey);
    const auto bpmText = juce::String (candidate.bpm,
                                       candidate.bpm == std::floor (candidate.bpm) ? 0 : 1);
    juce::NativeMessageBox::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle (jp (u8"ループを採用"))
            .withMessage (jp (u8"このループのキーと BPM をプロジェクトに設定しますか？\n\n")
                          + jp (u8"ループ: ") + keyText + " / " + bpmText + "bpm\n"
                          + jp (u8"プロジェクト: ")
                          + (project->key.has_value() ? ProjectKeys::displayName (*project->key)
                                                      : jp (u8"キー未設定"))
                          + " / " + juce::String (project->bpm) + "bpm\n\n"
                          + jp (u8"採用した時点でトラックが作られ確定します（⌘Z で採用ごと戻せます）。"
                                u8"設定するとベースガチャはこのループの進行に自動で追従します。"
                                u8"未確定のドラム・ベースの仮配置は撤去されます。"))
            .withButton (jp (u8"設定して採用"))
            .withButton (jp (u8"採用のみ"))
            .withButton (jp (u8"キャンセル")),
        [safe, detection, wavFile] (int result)
        {
            if (safe == nullptr)
                return;
            if (result >= 2) // キャンセル — 検出は走り切らせて結果だけ捨てる（プロセス kill の複雑さを買わない）
            {
                detection->dialogCancelled = true;
                safe->loopAdoptDetection = nullptr;
                return;
            }
            safe->finishLoopAdoption (detection, wavFile, /*applyKeyBpm=*/ result == 0);
        });
}

void MainComponent::finishLoopAdoption (std::shared_ptr<LoopAdoptDetection> detection,
                                        const juce::File& wavFile, bool applyKeyBpm)
{
    if (! detection->finished)
    {
        // 早押し（読む時間より検出が長かった）: ダイアログはもう閉じている — パネルに
        // 理由を出して、検出完了時にこの関数へ再入する
        rightPanel.gachaPanel().showStatus (jp (u8"進行を検出中…"));
        juce::Component::SafePointer<MainComponent> safe (this);
        detection->pendingAction = [safe, detection, wavFile, applyKeyBpm]
        {
            if (safe != nullptr)
                safe->finishLoopAdoption (detection, wavFile, applyKeyBpm);
        };
        return;
    }
    loopAdoptDetection = nullptr;
    // 以降の失敗経路は「進行を検出中…」（早押し時）を必ず消す — アラートは出るが、
    // ステータスが残ると処理が続いているように見える（レビュー指摘）
    if (! detection->ok)
    {
        rightPanel.gachaPanel().showStatus ({});
        Log::error ("gacha.loop_adopt_failed", "stage=looproots detail=" + detection->detail);
        showAlert (jp (u8"採用できません"), jp (u8"ループの進行検出に失敗しました。") + detection->detail);
        return;
    }
    // 契約は**生JSONの型まで** strict に検証する（project.json のアンカー読込と同じ方針・
    // 同じヘルパー。変換後の isValid だけだと roots: [9.5] が 9 に化けて通る）
    LoopAnchor anchor;
    if (! LoopAnchors::anchorFromContractJson (detection->contractText, anchor))
    {
        rightPanel.gachaPanel().showStatus ({});
        Log::error ("gacha.loop_adopt_failed", "stage=contract_types file=" + detection->libraryPath);
        showAlert (jp (u8"採用できません"), jp (u8"進行検出の出力が契約に合いません（looproots.py の形式退行の疑い）。"));
        return;
    }
    anchor.libraryPath = detection->libraryPath;
    anchor.bpm = detection->bpm;
    if (! anchor.isValid())
    {
        rightPanel.gachaPanel().showStatus ({});
        Log::error ("gacha.loop_adopt_failed", "stage=contract file=" + detection->libraryPath);
        showAlert (jp (u8"採用できません"), jp (u8"進行検出の結果が契約に合いません（looproots.py の出力）。"));
        return;
    }

    // 「敷いています…」を先に描画してから、重い処理（wav 変換・実体化・undo）を次の
    // フレームへ逃す — 同期のまま続けるとラベルが描画されず「押したのに無反応」に見える
    rightPanel.gachaPanel().showStatus (jp (u8"配置しています…"));
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Timer::callAfterDelay (50, [safe, anchor, wavFile, applyKeyBpm]
    {
        if (safe != nullptr)
            safe->placeLoopPreview (anchor, wavFile, applyKeyBpm);
    });
}

void MainComponent::placeLoopPreview (const LoopAnchor& anchor, const juce::File& wavFile,
                                      bool applyKeyBpm)
{
    if (engine.isRecording())
    {
        // 検出待ちの間に録音が始まったケース。黙って戻ると「配置しています…」が残る（レビュー指摘）
        rightPanel.gachaPanel().showStatus (jp (u8"録音中のため採用を中止しました"));
        return;
    }
    // wav の変換は**この時点のレート**で行う（ダイアログ中にデバイス SR が変わっても、
    // 決定時の実レートに合わせるので再生速度・音程が狂わない）。
    // SR はここでも**確定しない**（確定は dirty 化を伴い、キャンセルで元に戻せないため）。
    // 変換に使ったレートをセッションに預け、keep 時にその値で確定する
    const double targetRate = project->sampleRate > 0.0 ? project->sampleRate
                                                        : transport.sampleRate.load();
    if (targetRate <= 0.0)
    {
        rightPanel.gachaPanel().showStatus ({}); // 「配置しています…」を残さない（アラートが理由を言う）
        showAlert (jp (u8"採用できません"), jp (u8"オーディオデバイスが準備できていません。"));
        return;
    }
    auto audio = Project::loadWavResampled (wavFile, targetRate);
    if (audio == nullptr)
    {
        rightPanel.gachaPanel().showStatus ({});
        showAlert (jp (u8"採用できません"), jp (u8"wav を読み込めませんでした: ") + wavFile.getFileName());
        return;
    }
    // 刻みの**プリフライト** — この後の cancelGachaPreview より前に検証する。後段の
    // previewLoopCandidate 内でも刻むが、そこで初めて失敗すると「採用は失敗したのに
    // 既存のドラム・ベース仮配置だけ撤去済み」になる（レビュー指摘）。ここで刻んでおけば
    // 内部の再刻みは「目標長と一致 → 無変更」で素通りするだけ
    if (! GachaSession::trimLoopBufferToBars (*audio, targetRate, anchor.bpm, anchor.loopBars))
    {
        rightPanel.gachaPanel().showStatus ({});
        Log::error ("gacha.loop_adopt_failed", "stage=trim file=" + anchor.libraryPath
                                                   + " bars=" + juce::String (anchor.loopBars));
        showAlert (jp (u8"採用できません"),
                   jp (u8"ループの実尺が小節長と合いません（インデックスの推定と実ファイルの"
                       u8"不一致。ライブラリの再インデックスで直ることがあります）。"));
        return;
    }

    GachaSession::LoopPreviewInput input;
    input.anchor = anchor;
    input.audio = std::move (audio);
    input.audioSampleRate = targetRate;
    input.displayName = wavFile.getFileNameWithoutExtension();
    input.startSample = 0; // ループはアンカー（曲のベッド）なので1小節目の頭に敷く
    input.loopCount = 1;   // 2周ぶん（ドラム・ベースを重ねたとき展開が分かる最小）
    input.applyKeyBpm = applyKeyBpm;

    // 採用＝即確定の通常編集（2026-08-08 変更 — 「残す」を別途押させない）。
    // 未確定のドラム・ベースの仮配置は先に畳む — willBegin（通常編集は仮配置を畳む）と同じ
    // 不変条件で、直後の keep に未選択の候補を巻き込まないため
    cancelGachaPreview();

    if (! gachaSession.previewLoopCandidate (*project, input))
    {
        rightPanel.gachaPanel().showStatus ({});
        syncTransportAfterGachaRestore(); // 失敗でセッションが畳まれた可能性
        return;
    }
    const auto loopTrackId = gachaSession.previewTrackId (GachaSession::Part::loops);
    Log::info ("gacha.loop_pick", "loop=" + anchor.libraryPath
                                      + " applyKeyBpm=" + (applyKeyBpm ? juce::String ("1") : juce::String ("0"))
                                      + " degraded=" + (anchor.degraded ? juce::String ("1") : juce::String ("0")));

    // その場で keep（WAV 実体化・SR 確定・undo 1件・dirty 化は keepGachaCandidate が全部やる。
    // 失敗時も同関数がキャンセル相当の巻き戻しまで通す）
    keepGachaCandidate();

    headers.rebuild(); // トラックが増えた（keep 失敗時は撤去済みなので実質 no-op）
    for (int i = 0; i < (int) project->tracks.size(); ++i)
        if (project->tracks[(size_t) i].id == loopTrackId)
            selectTrackFromUser (i);
    syncTransportAfterGachaRestore(); // 逆コピー後の transport / LCD / バッジを project の真実へ
    pushSnapshot();
    timeline.refresh();
    rightPanel.gachaPanel().refreshAnchorRow();
}

void MainComponent::releaseLoopAnchor()
{
    if (gachaSession.hasPreview (GachaSession::Part::loops))
    {
        // 仮配置中の解除＝ループパーツのキャンセル（クリップ・逆コピー・追従ベースまで巻き戻す）
        cancelGachaPart (GachaSession::Part::loops);
        rightPanel.gachaPanel().refreshAnchorRow();
        return;
    }
    if (! project->loopAnchor.has_value())
        return;
    // 確定済みアンカーの解除は通常の undo 1件。クリップ・BPM・キーは維持してアンカーだけ消す
    // （以後のベースガチャはカード駆動に戻り、⌘Z で追従へ復帰する）
    undoStack.begin (*project);
    project->loopAnchor.reset();
    setDirty (true);
    rightPanel.gachaPanel().refreshAnchorRow();
    Log::info ("gacha.anchor_release");
}

void MainComponent::cancelGachaPreview()
{
    if (! gachaSession.hasPreview())
        return;
    bool trackRemoved = false;
    for (int i = 0; i < GachaSession::numParts; ++i)
        trackRemoved = trackRemoved
                       || gachaSession.trackIsPreviewOwned (
                              gachaSession.previewTrackId ((GachaSession::Part) i));
    gachaSession.cancelPreview (*project);

    Log::info ("gacha.cancel_preview");
    rightPanel.gachaPanel().clearAllPreviewBadges();
    afterGachaCancel (trackRemoved);
}

void MainComponent::cancelGachaPart (GachaSession::Part part)
{
    if (! gachaSession.hasPreview (part))
        return;
    bool trackRemoved = gachaSession.trackIsPreviewOwned (gachaSession.previewTrackId (part));
    // ループの取り消しは追従ベースも連動撤去される（GachaSession::cancelPart）。
    // ベースの自動作成トラックも撤去対象に含めないと、ヘッダの unbind/rebuild が省略され
    // 30Hz 更新が削除済みモデルを参照する
    if (part == GachaSession::Part::loops)
        trackRemoved = trackRemoved
                       || gachaSession.trackIsPreviewOwned (
                              gachaSession.previewTrackId (GachaSession::Part::bass));
    gachaSession.cancelPart (part, *project);

    const char* partName = part == GachaSession::Part::drums ? "drums"
                         : part == GachaSession::Part::bass  ? "bass"
                                                             : "loops";
    Log::info ("gacha.cancel_part", juce::String ("part=") + partName);
    rightPanel.gachaPanel().setPreviewActive (part, false);
    afterGachaCancel (trackRemoved);
}

void MainComponent::cancelReferenceAnalysisForClose()
{
    if (! analysisActive)
        return;

    Log::info ("reference.analyze.cancel_requested", "source=close");
    referenceAnalyzer.cancelAndWait(); // SpawnedProcess がプロセスグループごと終了させてから戻る
    analysisActive = false;
    (void) referenceAnalyzer.takeResult();
    referenceOverlay.dismiss();
    if (analysisFolder != juce::File())
    {
        analysisFolder.deleteRecursively();
        Log::info ("reference.analyze.cancel", "name=" + analysisFolder.getFileName() + " reason=close");
        analysisFolder = juce::File();
    }
    refreshMacMenu();
}

void MainComponent::cancelImportForClose()
{
    if (! importActive)
        return;

    Log::info ("import.cancel_requested", "source=close");
    audioImporter.cancelAndWait(); // ワーカーが一時ファイルを削除してから戻る
    importActive = false;
    (void) audioImporter.takeResult();
    importOverlay.dismiss();
    cleanupUrlTempDir(); // URL取り込みの途中だった場合の後片付け
    refreshMacMenu();
    Log::info ("import.cancelled", "reason=close");
}

// ---- URLからの取り込み（yt-dlp） ----

void MainComponent::cleanupUrlTempDir()
{
    if (urlTempDir == juce::File())
        return;

    urlTempDir.deleteRecursively();
    urlTempDir = juce::File();
}

// 前回クラッシュ等で残った一時ディレクトリを起動時に掃除する。
// dev版とRelease版は並走できるので、名前に埋めたPIDが生きているものは他インスタンスの
// 作業中とみなして残す（PID再利用時は消し損ねるだけで、他インスタンスを壊さない）
void MainComponent::sweepStaleUrlTempDirs()
{
    const auto root = TempDirSweep::rootDirectory();

    juce::StringArray names;
    for (const auto& dir : root.findChildFiles (juce::File::findDirectories, false,
                                               juce::String (TempDirSweep::namePrefix) + "*"))
        names.add (dir.getFileName());

    const auto stale = TempDirSweep::selectStaleTempDirs (
        names, [] (int pid) { return ::kill ((pid_t) pid, 0) == 0 || errno == EPERM; });

    for (const auto& name : stale)
        root.getChildFile (name).deleteRecursively();

    if (! stale.isEmpty())
        Log::info ("url.tempdir.swept", "count=" + juce::String (stale.size()));
}

void MainComponent::startUrlImportFlow()
{
    // 録音中に入力だけ通してしまうと、ダウンロードが終わった時点で引き渡しに失敗する。
    // 入口で弾いておく（他の取り込み・書き出しと同じ扱い）
    if (importActive || bounceActive || engine.isRecording() || isUrlImporting())
        return;

    // yt-dlp が無いなら入力させる前に案内する
    if (UrlDownloader::findYtDlp() == juce::File())
    {
        showAlert (jp (u8"yt-dlp が見つかりません"),
                   jp (u8"URLからの取り込みには yt-dlp が必要です。\n"
                       "Brewfile に brew 'yt-dlp' を追加して brew bundle を実行してください。"));
        return;
    }

    urlStage = UrlStage::enteringUrl;

    urlOverlay.onCancel = [this]
    {
        urlStage = UrlStage::idle;
        refreshMacMenu();
    };

    urlOverlay.onSubmit = [this] (juce::String url)
    {
        // SRの確定はURLが確定してから（オーバーレイを開いて閉じただけでdirtyにしない）
        double targetRate = 0.0;
        if (! ensureProjectSampleRate (targetRate))
        {
            urlStage = UrlStage::idle;
            refreshMacMenu();
            return;
        }

        UrlDownloader::Request request;
        request.url = url;

        urlStage = UrlStage::downloading;
        importProgressBase = downloadProgressShare;
        importProgressSpan = 1.0f - downloadProgressShare;

        importOverlay.setLabels (jp (u8"ダウンロード中…"), jp (u8"取り込みが完了しました"));
        importOverlay.setBounds (getLocalBounds());
        importOverlay.show();
        importDoneTicks = 0;

        if (! urlDownloader.start (std::move (request)))
        {
            // 開始できなかったら表示も状態も元に戻す
            urlStage = UrlStage::idle;
            importOverlay.dismiss();
            importProgressBase = 0.0f;
            importProgressSpan = 1.0f;
            cleanupUrlTempDir();
            refreshMacMenu();
            showAlert (jp (u8"取り込めません"), jp (u8"前回のダウンロードが終了していません。"));
            return;
        }

        refreshMacMenu();
    };

    urlOverlay.setBounds (getLocalBounds());
    urlOverlay.show();
    refreshMacMenu(); // URL入力中もFileメニューはdisabledにする
}

void MainComponent::pollUrlImport()
{
    if (urlStage != UrlStage::downloading)
        return;

    // ダウンロードは進捗バーの前半（0〜downloadProgressShare）を使う
    importOverlay.setProgress (downloadProgressShare * urlDownloader.progress());
    if (urlDownloader.status() == UrlDownloader::Status::running)
        return;

    auto result = urlDownloader.takeResult();

    // startImport() のガードに isUrlImporting() が入っているので、渡す前に idle に戻す
    urlStage = UrlStage::idle;

    if (result.status == UrlDownloader::Status::success)
    {
        // 一時ディレクトリの所有権をここで受け取る（以降の削除は cleanupUrlTempDir が担う）
        urlTempDir = result.tempDirectory;

        importOverlay.setLabels (jp (u8"取り込み中…"), jp (u8"取り込みが完了しました"));
        if (startImport (result.audioFile, -1, 0, false, result.title))
            return; // 続きは pollImport() が拾う

        // ガードに掛かった（録音が始まった等）・SR未確定・ワーカー開始失敗
        importOverlay.dismiss();
        importProgressBase = 0.0f;
        importProgressSpan = 1.0f;
        cleanupUrlTempDir();
        refreshMacMenu();
        return;
    }

    importOverlay.dismiss();
    importProgressBase = 0.0f;
    importProgressSpan = 1.0f;
    cleanupUrlTempDir(); // 失敗・キャンセル時は worker が消しているので通常は空（保険）

    if (result.status == UrlDownloader::Status::cancelled)
        Log::info ("url.cancelled");
    else if (result.ytDlpMissing)
        showAlert (jp (u8"yt-dlp が見つかりません"),
                   jp (u8"URLからの取り込みには yt-dlp が必要です。\n"
                       "Brewfile に brew 'yt-dlp' を追加して brew bundle を実行してください。"));
    else
        showAlert (jp (u8"取り込めません"),
                   result.errorMessage + "\n\n"
                       + jp (u8"yt-dlp が古い場合は brew upgrade yt-dlp を試してください。"));

    refreshMacMenu();
}

void MainComponent::cancelUrlImportForClose()
{
    if (urlStage == UrlStage::idle)
        return;

    if (urlStage == UrlStage::downloading)
    {
        Log::info ("url.cancel_requested", "source=close");
        urlDownloader.cancelAndWait(); // プロセスグループごと終了してから戻る

        // ダウンロードが成功した直後（pollUrlImport が拾う前）に閉じられた場合、
        // 一時ディレクトリはまだ worker 側の Result が持っている。ここで回収して消す
        auto result = urlDownloader.takeResult();
        if (result.tempDirectory != juce::File())
            result.tempDirectory.deleteRecursively();
    }

    urlStage = UrlStage::idle;
    cleanupUrlTempDir();
    urlOverlay.dismiss();
    importOverlay.dismiss();
    importProgressBase = 0.0f;
    importProgressSpan = 1.0f;
    refreshMacMenu();
    Log::info ("url.cancelled", "reason=close");
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

    cancelGachaPreview(); // 未確定の仮リージョンを保存に混入させない

    project->bpm = transport.bpm.load();
    juce::String error;
    // undo/redo履歴が参照するWAVはGCから保護する（redoでの復元に備える）。
    // クリップボードのクリップも同じ「モデル外からの参照」なので保護する
    // （コピー → 元クリップを削除 → 保存、で実ファイルが消えるとペーストが壊れる）
    auto keepWavs = undoStack.referencedWavs();
    if (itemClipboard.kind == ItemClipboard::Kind::audioClip
        && itemClipboard.clip.fileName.isNotEmpty())
        keepWavs.addIfNotAlreadyThere (itemClipboard.clip.fileName);
    if (! project->save (error, keepWavs))
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

    // 構造編集で「原曲を頭出し」の可否が変わる（クリップの分割・複製・削除）。
    // ガチャモード表示中だけ引き直す（判定は小さなJSONの読み直しを含むため常時はやらない）
    if (rightPanel.isOpen() && rightPanel.mode() == RightPanel::Mode::gacha)
        rightPanel.gachaPanel().refreshAlignAvailability();

    pushSnapshotWithChange (Project::SnapshotChange::midiStructure);

    // モデルが変わった可能性があるのでピアノロールも同期（対象リージョンが消えていれば閉じる）
    pianoRoll.refreshFromModel();
}

// オーディオ側の値（クリップゲイン）だけの差し替え。MIDI構成の世代を据え置くことで、
// エンジン側の消音＋跨ぎノート再発音（PlaybackEngine の snapshotChanged 判定）を起こさない。
// synthBank.sync() も呼ばない（音源の作り直しは不要で、ドラッグ中に毎イベント走らせたくない）
void MainComponent::pushAudioValueSnapshot()
{
    pushSnapshotWithChange (Project::SnapshotChange::audioValuesOnly);
}

// ---- 曲末フェードアウト ----

// 「startSixteenths から曲末まで」に設定する（⌃F・ルーラー右クリック共通）。
// 開始点は呼び出し側で必ずグリッドへ丸めておくこと: 生の位置で判定すると、丸めた結果が
// 終端と一致するケース（終端の直前にヘッドがある）を取りこぼす
void MainComponent::setSongFadeFrom (int startSixteenths)
{
    // 終端の解決（アイテム有無・既存フェードの維持・開始点の妥当性）はモデル側に閉じてある。
    // 0 = no-op
    const int end = project->resolveSongFadeEnd (startSixteenths, timeline.effectiveSampleRate());
    if (end <= 0)
    {
        Log::info ("song_fade.noop", "start=" + juce::String (startSixteenths));
        return;
    }
    if (project->fadeOutStartSixteenths == startSixteenths && project->fadeOutEndSixteenths == end)
        return;

    // undoは clipValue（MIDI世代を据え置くpushで鳴っている音を乱さない。リージョンゲインと同じ）
    undoStack.begin (*project, UndoStack::EditKind::clipValue);
    project->fadeOutStartSixteenths = startSixteenths;
    project->fadeOutEndSixteenths = end;
    setDirty (true);
    pushAudioValueSnapshot();
    timeline.refresh();
    Log::info ("song_fade.set", "start=" + juce::String (startSixteenths) + " end=" + juce::String (end));
}

void MainComponent::clearSongFade()
{
    if (! project->hasFadeOut())
        return;

    undoStack.begin (*project, UndoStack::EditKind::clipValue);
    project->fadeOutStartSixteenths = 0;
    project->fadeOutEndSixteenths = 0;
    setDirty (true);
    pushAudioValueSnapshot();
    timeline.refresh();
    Log::info ("song_fade.clear", "");
}

void MainComponent::pushSnapshotWithChange (Project::SnapshotChange change)
{
    auto snapshot = project->buildSnapshot (change);
    for (size_t i = 0; i < project->tracks.size() && i < snapshot->tracks.size(); ++i)
        if (project->tracks[i].type == TrackType::midi)
            snapshot->tracks[i].synth = synthBank.get (project->tracks[i].id);

    snapshots.push (std::move (snapshot));
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

    // 取り込み中もモーダル: ショートカット経由の編集（トラック削除等）を塞ぎ、
    // 開始時に保持した配置先が完了まで有効であることを保証する
    if (importActive)
    {
        if (escape)
        {
            Log::info ("import.cancel_requested", "source=escape");
            audioImporter.cancel(); // 非同期。完了はpollImport()が拾う
        }
        return true;
    }

    // リファレンス分析中もモーダル（取り込み中と同じ扱い）
    if (analysisActive)
    {
        if (escape)
        {
            Log::info ("reference.analyze.cancel_requested", "source=escape");
            referenceAnalyzer.cancel(); // 非同期。完了はpollReferenceAnalysis()が拾う
        }
        return true;
    }

    // URLのダウンロード中もモーダル（取り込み中と同じ扱い）
    if (urlStage == UrlStage::downloading)
    {
        if (escape)
        {
            Log::info ("url.cancel_requested", "source=escape");
            urlDownloader.cancel(); // 非同期。完了はpollUrlImport()が拾う
        }
        return true;
    }

    // URL入力中はTextEditorがフォーカスを持つのでキーはここに来ないのが通常だが、
    // 入力欄の外をクリックした後などに来ることがあるので同じくモーダルにしておく
    if (urlStage == UrlStage::enteringUrl)
    {
        if (escape)
            urlOverlay.dismissWithCancel();
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
        if (is (SC::shortcutList) || is (SC::toggleMixer) || is (SC::toggleFxEditor)
            || is (SC::toggleNotes) || is (SC::toggleFiles)
            || is (SC::toggleBottomPanel) || is (SC::bottomHistory))
            return true; // オーバーレイは高々1枚: AddTrack表示中の⌘?/X/I/⌘N/F/E/[]は無視
    }
    // Logic準拠: X = ミキサー。表示中もモーダルにしない（Space再生・シーク等はそのまま効く）
    if (is (SC::toggleMixer))
    {
        if (mixerWindow.isVisible())
        {
            Log::info ("mixer.close");
            mixerWindow.dismiss();
        }
        else
        {
            Log::info ("mixer.open");
            mixerWindow.openOver (this, selectedTrack);
        }
        return true;
    }
    if (mixerWindow.isVisible() && escape)
    {
        Log::info ("mixer.close", "source=escape");
        mixerWindow.dismiss();
        return true;
    }
    // Logic準拠: B = 下部FXエディタ（Smart Controls相当）
    if (is (SC::toggleFxEditor))
    {
        toggleFxEditor();
        return true;
    }
    // Logic準拠: E = Show/Hide Editor（下部エリア）。閉じても履歴に残るので同じEで戻せる
    if (is (SC::toggleBottomPanel))
    {
        toggleBottomPanel();
        return true;
    }
    // [ / ] = 下部エリアの履歴を戻る/進む（⌘付きはファイルパネルの履歴で別物）
    if (is (SC::bottomHistory))
    {
        navigateBottomHistory (key.getTextCharacter() == '[' ? -1 : 1);
        return true;
    }
    // ⌘N = 右ドックのメモ。開くとメモ欄にフォーカスが移るが、⌘付きなのでTextEditorに
    // 消費されず同じキーで閉じられる（Shortcuts.h の toggleNotes のコメント参照）
    if (is (SC::toggleNotes))
    {
        toggleRightPanel (RightPanel::Mode::notes);
        return true;
    }
    // Logic準拠: F = ブラウザ（本アプリでは右ドックの音声ファイル一覧）
    if (is (SC::toggleFiles))
    {
        toggleRightPanel (RightPanel::Mode::files);
        return true;
    }
    // ⌘[ / ⌘] = ファイルパネルの履歴を戻る/進む（パネルを開いているときだけ効く）
    if (is (SC::browserHistory))
    {
        if (! rightPanel.isOpen() || rightPanel.mode() != RightPanel::Mode::files)
            return false;
        return rightPanel.fileBrowser().navigateHistory (key.getKeyCode() == '[' ? -1 : 1);
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
    // ⌘C/⌘V はノート（ピアノロール）とリージョン/クリップ（タイムライン）で共用。
    // ピアノロールが開いていてノートが選択されていればノート、そうでなければタイムライン側へ
    // フォールバックする（Deleteと同じ裁き方。copySelection/pasteAtPlayhead は空ならfalseを返す）
    if (is (SC::copyItem))
    {
        if (pianoRoll.isOpen() && pianoRoll.copySelection())
            return true;
        return copySelectedItem(); // コピーはモデルを触らないので録音中も許可
    }
    if (is (SC::pasteItem))
    {
        if (pianoRoll.isOpen() && ! engine.isRecording() && pianoRoll.pasteAtPlayhead())
            return true;
        return pasteItemAtPlayhead();
    }
    // ピアノロールの選択ノートへのキー操作（Logic準拠: ↑↓=半音・⌥↑↓=オクターブ）
    if (pianoRoll.isOpen() && ! engine.isRecording())
    {
        const bool up = key.isKeyCode (juce::KeyPress::upKey);
        if (is (SC::noteOctave))
            return pianoRoll.transposeSelection (up ? 12 : -12);
        if (is (SC::noteSemitone))
            return pianoRoll.transposeSelection (up ? 1 : -1);
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
    // Logic準拠: ⇧⌘I = オーディオファイルを読み込む（⌘Bと同じくメニューのフォールバック）
    if (is (SC::importAudio))
    {
        startImportFlow();
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
        toggleDeviceSettings ("shortcut");
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
    // Logic準拠: ⌘R = 選択中のリージョン/クリップを終端直後へ複製（連打で伸ばせる）
    if (is (SC::repeatItem))
    {
        repeatSelectedItem();
        return true;
    }
    // ⌃F = 再生ヘッド位置から曲末までフェードアウト（プロジェクトに1本だけ）
    if (is (SC::songFadeOut))
    {
        // 基準は再生ヘッド（⌘Tの分割・⌘Vの貼り付けと同じ「編集の基準」。再生開始位置ではない）。
        // uiPositionSample() は0クランプしないので編集側でjmaxする
        setSongFadeFrom (timeline.sampleToSnappedSixteenths (
            juce::jmax ((juce::int64) 0, transport.uiPositionSample())));
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
    const auto pos = timeline.editPositionSample();
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
    locate (target);
}

void MainComponent::seekToSection (int direction, int keyCode)
{
    if (engine.isRecording() || project->markers.empty())
        return;

    // 前=厳密に前の境界、次=厳密に次の境界。境界ちょうどに居るときも1つ進む/戻る（1拍/1小節シークと同じ流儀）
    const auto pos = timeline.editPositionSample();
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
    locate (target);
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
    mixerWindow.content().sync (selectedTrack); // ミキサー表示中のmキーでもM点灯を同期する
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
    mixerWindow.content().sync (selectedTrack); // ミキサー表示中のsキーでもS点灯を同期する（全ストリップ再バインド）
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

bool MainComponent::copySelectedItem()
{
    if (project == nullptr)
        return false;

    // 選択は clips / midiRegions のどちらか排他（exportSelectedItem と同じ順で見る）
    if (const auto sel = timeline.getSelection(); sel.isValid())
    {
        if (sel.track < 0 || sel.track >= (int) project->tracks.size())
            return false;
        const auto& clips = project->tracks[(size_t) sel.track].clips;
        if (sel.clip < 0 || sel.clip >= (int) clips.size())
            return false;
        itemClipboard.kind = ItemClipboard::Kind::audioClip;
        itemClipboard.clip = clips[(size_t) sel.clip]; // fileName/audioは共有参照、peakCacheは値コピー
        itemClipboard.region = {};
        Log::info ("region.copy", "type=audio track=" + juce::String (sel.track)
                                      + " item=" + juce::String (sel.clip));
        return true;
    }
    if (const auto rsel = timeline.getRegionSelection(); rsel.isValid())
    {
        if (rsel.track < 0 || rsel.track >= (int) project->tracks.size())
            return false;
        const auto& regions = project->tracks[(size_t) rsel.track].midiRegions;
        if (rsel.region < 0 || rsel.region >= (int) regions.size())
            return false;
        // ガチャの仮リージョンは Copy も「撤去して中止」（⌘C→閉じる→⌘V で「残す」を
        // 迂回して確定できてしまうため。通常はmouseDownガードで選択自体ができないが最終防衛線）
        if (gachaSession.isPreviewObject (project->tracks[(size_t) rsel.track].id,
                                          regions[(size_t) rsel.region].id))
        {
            cancelGachaPreview();
            return false;
        }
        itemClipboard.kind = ItemClipboard::Kind::midiRegion;
        itemClipboard.region = regions[(size_t) rsel.region]; // ノート・ミュートごと丸ごと
        itemClipboard.clip = {};
        Log::info ("region.copy", "type=midi track=" + juce::String (rsel.track)
                                      + " item=" + juce::String (rsel.region));
        return true;
    }
    return false;
}

bool MainComponent::pasteItemAtPlayhead()
{
    if (engine.isRecording() || project == nullptr)
        return false;
    if (itemClipboard.kind == ItemClipboard::Kind::none)
        return false;
    // begin フックより先に撤去する（下で取るトラック参照が、フック内の撤去で失効しないように）
    cancelGachaPreview();
    if (selectedTrack < 0 || selectedTrack >= (int) project->tracks.size())
        return false;

    auto& track = project->tracks[(size_t) selectedTrack];
    const bool wantMidi = itemClipboard.kind == ItemClipboard::Kind::midiRegion;
    if (wantMidi != (track.type == TrackType::midi))
        return false; // 型不一致（MIDIリージョンをオーディオトラックへ等）は何もしない

    const auto playhead = timeline.editPositionSample();
    undoStack.begin (*project);

    int pastedIndex = 0;
    if (wantMidi)
    {
        MidiRegion pasted = itemClipboard.region;
        pasted.id = project->allocateId();
        for (auto& note : pasted.notes)
            note.id = project->allocateId();
        // 再生ヘッドを表示グリッドの最近傍へ（リージョン移動と同じ規則）
        const double bpm = juce::jlimit (20.0, 400.0, transport.bpm.load());
        const auto absPpq = (juce::int64) std::llround (
            (double) playhead * Ppq::ticksPerSample (bpm, timeline.effectiveSampleRate()));
        const auto grid = juce::jmax ((juce::int64) 1, timeline.gridPpq());
        pasted.startPpq = juce::jmax ((juce::int64) 0,
                                      (juce::int64) std::llround ((double) absPpq / (double) grid) * grid);
        track.midiRegions.push_back (std::move (pasted));
        pastedIndex = (int) track.midiRegions.size() - 1;
    }
    else
    {
        Clip pasted = itemClipboard.clip; // fileName/audioは共有参照、peakCacheは値コピー
        pasted.startSample = timeline.snapSampleToVisibleGrid (playhead);
        track.clips.push_back (std::move (pasted));
        pastedIndex = (int) track.clips.size() - 1;
    }

    timeline.selectItem (selectedTrack, pastedIndex, wantMidi);
    Log::info ("region.paste", juce::String (wantMidi ? "type=midi" : "type=audio")
                                   + " track=" + juce::String (selectedTrack)
                                   + " item=" + juce::String (pastedIndex)
                                   + " pos=" + juce::String (playhead));
    pushSnapshot();
    setDirty (true);
    timeline.refresh();
    return true;
}

void MainComponent::repeatSelectedItem()
{
    if (engine.isRecording())
        return;
    // duplicateAt が複製を選択状態にするので、連打すると後ろへ伸びていく（Logicの⌘Rと同じ）
    if (const auto sel = timeline.getSelection(); sel.isValid())
        timeline.duplicateAt (sel.track, sel.clip);
    else if (const auto rsel = timeline.getRegionSelection(); rsel.isValid())
        timeline.duplicateAt (rsel.track, rsel.region);
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
    const auto playhead = timeline.editPositionSample();
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

    // 右上: パネルトグル（メモ・ファイル）と設定を隔てる区切り線
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.fillRect (topBarSeparator);
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

    // 右上の補助ボタン。パネルトグル（メモ・ファイル）は隣接させて一組に見せ、
    // 性質の違う設定（歯車）は区切り線を挟んで離す
    constexpr int auxSize = 30;
    auto auxButton = [&topRow] { return topRow.removeFromRight (auxSize)
                                            .withSizeKeepingCentre (auxSize, auxSize); };
    settingsButton.setBounds (auxButton());
    topRow.removeFromRight (8);
    topBarSeparator = juce::Rectangle<float> (1.0f, 16.0f)
                          .withCentre (topRow.removeFromRight (1).toFloat().getCentre());
    topRow.removeFromRight (8);
    gachaButton.setBounds (auxButton());
    topRow.removeFromRight (2);
    filesButton.setBounds (auxButton());
    topRow.removeFromRight (2);
    notesButton.setBounds (auxButton());
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

    // 右ドックは上部バー直下の全高。先に右側を取ることで、下部エディタも
    // ドックの下へ潜り込まず中央領域だけを使う
    rightResizeBar.setVisible (rightPanel.isOpen());
    if (rightPanel.isOpen())
    {
        const int leftChrome = (fxEditor.isOpen() ? FxEditorView::preferredWidth : 0)
                             + TrackHeadersView::preferredWidth;
        const int available = juce::jmax (0, area.getWidth() - leftChrome - 160);
        const int effectiveWidth = juce::jmin (rightPanelWidth, available);
        auto panelArea = area.removeFromRight (effectiveWidth);
        rightPanel.setBounds (panelArea);
        rightResizeBar.setBounds (panelArea.getX() - 4, panelArea.getY(), 8, panelArea.getHeight());
    }

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

    // ＋ボタンの帯はヘッダー列の中だけに置く（全幅に取るとタイムライン下に死にスペースができる）
    auto headerColumn = area.removeFromLeft (TrackHeadersView::preferredWidth);
    headerColumn.removeFromTop (timeline.laneTop()); // ルーラー＋マーカーレーン（＋曲末フェード帯）分の高さを合わせる
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
    if (importOverlay.isVisible())
        importOverlay.setBounds (getLocalBounds());
    if (urlOverlay.isVisible())
        urlOverlay.setBounds (getLocalBounds());
    if (referenceOverlay.isVisible())
        referenceOverlay.setBounds (getLocalBounds());

    // 右下の一時通知（他のUIと重なってよい。表示中の位置追従のため常に置く）
    toast.setBounds (getLocalBounds().reduced (16).removeFromBottom (44).removeFromRight (360));
}
