// MainComponent のうち、ボーカルのピッチ補正エディタ（独立ウィンドウ）の配線。
// 設計の真実の源: docs/plans/2026-08-20-2244-vocal-pitch-correction.md（Phase 3）
//
// 規則の要点:
// - 対象は Clip::id で毎回引き直す（index は vector 更新・undo で無効化される）。見つからなければ閉じる
// - 未確定プレビューは Clip::previewDomain（一時）に載せる。activeDomain・永続値は触らない。
//   再生 snapshot・描画・単独試聴は Clip::effectiveDomain() を通る（previewDomain 優先）
// - プレビューのレンダーは専用の RenderCache（pitchPreviewCache）。結果は digest とモードで照合し、
//   遅着・不一致は捨てる。attachRenderResult / rollbackFailedRequest は通さない
// - 確定（Enable／最初の手編集／Apply）は 1 件の undo。開いただけ・閉じただけでは project/dirty/undo が変わらない
// - ⌘B/⌘E・閉じる・対象切替・undo はプレビューを破棄（初回: 捨てて閉じる／変更: 旧状態へ）
#include "MainComponent.h"

#include "../shared/Log.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

Clip* findClipById (Project& project, juce::uint64 id)
{
    if (id == 0)
        return nullptr;
    for (auto& track : project.tracks)
        for (auto& clip : track.clips)
            if (clip.id == id)
                return &clip;
    return nullptr;
}
} // namespace

Clip* MainComponent::pitchTargetClip()
{
    return pitchSession.isOpen() ? findClipById (*project, pitchSession.clipId()) : nullptr;
}

juce::StringArray MainComponent::pitchPreviewSidecars() const
{
    juce::StringArray out;
    if (! pitchSession.isOpen() || pitchSession.curve() == nullptr)
        return out;
    for (const auto& track : project->tracks)
        for (const auto& clip : track.clips)
            if (clip.id == pitchSession.clipId())
                out.add (PitchSidecar::fileNameFor (clip.fileName, pitchSession.curve()->digest()));
    return out;
}

void MainComponent::wirePitchEditor()
{
    auto& view = pitchWindow.content();
    view.session = &pitchSession;
    view.getClip = [this]() -> const Clip* { return pitchTargetClip(); };
    view.getSampleRate = [this] { return renderSampleRate(); };
    view.getBpm = [this] { return project->bpm; };
    view.getProjectKey = [this] { return project->key; };
    view.getPlayheadSample = [this] { return transport.playheadSamplePos.load(); };
    view.isRendering = [this]
    {
        if (pitchSession.hasPreview())
            return ! pitchPreviewCache.isIdle();
        const auto* clip = pitchTargetClip();
        return clip != nullptr && clip->renderPending (renderSampleRate());
    };
    view.getAnalysisProgress = [this] { return pitchWorker.progress(); };
    view.getBlockedMessage = [this] { return pitchBlockedMessage; };
    view.onKey = [this] (const juce::KeyPress& key) { return keyPressed (key); };

    view.onBeginEdit = [this] { pitchBeginEdit(); };
    view.onEdited = [this] { pitchApplyWorking(); };
    view.onEnable = [this]
    {
        if (pitchSession.mode() != PitchEditorSession::Mode::initialPreview || ! pitchSession.editable())
            return;
        Log::info ("pitch.enable", "clip=" + juce::String (pitchSession.clipId()));
        pitchBeginEdit();       // 初回プレビューの確定（1件の undo）
        pitchApplyWorking();
    };
    view.onApply = [this]
    {
        auto* clip = pitchTargetClip();
        if (clip == nullptr || pitchSession.mode() != PitchEditorSession::Mode::changePreview)
            return;
        Log::info ("pitch.apply", "clip=" + juce::String (clip->id));
        undoStack.begin (*project);
        const auto applied = pitchSession.applyChange();
        clip->pitchCorrection = applied;
        clip->pitchCurve = pitchSession.curve();
        clip->previewDomain = nullptr;
        pitchPreviewRequest.reset();
        setDirty (true);
        renderCache.requestSync();
        pushAudioValueSnapshot();
        timeline.refresh();
        pitchWindow.content().refresh();
    };
    view.onCancel = [this]
    {
        if (pitchSession.mode() != PitchEditorSession::Mode::changePreview)
            return;
        Log::info ("pitch.cancel_change", "clip=" + juce::String (pitchSession.clipId()));
        pitchSession.cancelChange();
        pitchClearPreview();
        pitchWindow.content().refresh();
    };
    view.onResnap = [this]
    {
        auto* clip = pitchTargetClip();
        if (clip == nullptr || pitchSession.mode() != PitchEditorSession::Mode::committed || pitchSession.curve() == nullptr)
            return;
        auto proposal = pitchSession.working();
        PitchCorrections::resnap (proposal, *pitchSession.curve(), clip->requestedDomainOffset(), clip->requestedDomainLength(),
                                  PitchCorrections::effectiveScale (proposal, project->key));
        if (proposal == pitchSession.working())
        {
            toast.show (jp (u8"キーに合わせ直しても変わる音はありません"), false, nullptr);
            return;
        }
        Log::info ("pitch.resnap_preview", "clip=" + juce::String (clip->id));
        pitchSession.beginChangePreview (proposal, pitchSession.curve());
        pitchRequestPreview();
        pitchWindow.content().refresh();
    };
    view.onReset = [this]
    {
        auto* clip = pitchTargetClip();
        if (clip == nullptr || pitchSession.mode() != PitchEditorSession::Mode::committed || pitchSession.curve() == nullptr)
            return;
        // 「すべてリセット」= 現在のスケール設定で自動スナップし直し（手直し・つまみを捨てる）。変更プレビューとして見せる
        const auto& w = pitchSession.working();
        auto proposal = PitchCorrections::autoSnap (*pitchSession.curve(), pitchSession.detected(), clip->requestedDomainOffset(),
                                                    clip->requestedDomainLength(), PitchCorrections::effectiveScale (w, project->key),
                                                    w.scaleMode, w.customKey);
        if (proposal == w)
        {
            toast.show (jp (u8"既に自動スナップの状態です"), false, nullptr);
            return;
        }
        Log::info ("pitch.reset_preview", "clip=" + juce::String (clip->id));
        pitchSession.beginChangePreview (proposal, pitchSession.curve());
        pitchRequestPreview();
        pitchWindow.content().refresh();
    };
    view.onReanalyze = [this]
    {
        auto* clip = pitchTargetClip();
        if (clip == nullptr || pitchSession.mode() != PitchEditorSession::Mode::committed)
            return;
        pitchStartAnalysis (*clip, /*reanalyze=*/ true);
    };
    view.onRetrySidecar = [this]
    {
        auto* clip = pitchTargetClip();
        if (clip == nullptr || pitchSession.curve() == nullptr)
            return;
        juce::String err;
        const bool ok = PitchSidecar::write (*pitchSession.curve(), project->directory.getChildFile (clip->fileName), &err);
        Log::info ("pitch.sidecar_retry", "ok=" + juce::String ((int) ok) + (ok ? juce::String() : " error=" + err));
        pitchBlockedMessage = ok ? juce::String() : err;
        pitchSession.setSidecarWritten (ok);
        pitchWindow.content().refresh();
    };
    view.onSetProjectKey = [this]
    {
        const auto est = PitchNotes::estimateKey (pitchSession.detected());
        if (est.valid)
            setProjectKey (est.key);
        pitchWindow.content().refresh();
    };
    view.onAudition = [this] (int noteIndex)
    {
        auto* clip = pitchTargetClip();
        if (clip == nullptr || ! pitchSession.isOpen() || transport.isPlaying.load() || engine.isRecording())
            return; // メイン再生中は再生を止めない・録音中は無効
        const auto& w = pitchSession.working();
        if (noteIndex < 0 || noteIndex >= (int) w.notes.size())
            return;
        const auto* domain = clip->effectiveDomain();
        if (domain == nullptr || domain->audio == nullptr)
            return;
        const auto off = clip->requestedDomainOffset(), len = clip->requestedDomainLength();
        const auto s = w.resolve (w.notes[(size_t) noteIndex].start, off, len);
        const auto e = w.resolve (w.notes[(size_t) noteIndex].end, off, len);
        const auto r0 = domain->audioBaseOffset + domain->mapBoundary (s);
        const auto r1 = domain->audioBaseOffset + domain->mapBoundary (e);
        if (r1 <= r0)
            return;
        filePreview.stop(); // ファイルブラウザ試聴とは相互排他（後勝ち）
        bufferAudition.start (domain->audio, r0, r1 - r0, domain->sampleRate);
        Log::info ("pitch.audition", "note=" + juce::String (noteIndex) + " start=" + juce::String (r0) + " len=" + juce::String (r1 - r0));
    };

    pitchWindow.onDismissed = [this]
    {
        bufferAudition.stop();
        if (pitchSession.mode() == PitchEditorSession::Mode::changePreview)
        {
            pitchSession.cancelChange();
            toast.show (jp (u8"ピッチ補正の変更プレビューを取り消しました"), false, nullptr);
        }
        pitchWorker.cancelAndWait();
        pitchReanalyzing = false;
        pitchClearPreview();
        pitchSession.close();
        pitchPreviewRequest.reset();
        Log::info ("pitch.closed");
    };

    // ---- プレビュー専用レンダー経路 ----
    pitchPreviewCache.collectRequests = [this]
    {
        std::vector<ClipDomains::Request> requests;
        if (pitchPreviewRequest.has_value() && pitchSession.hasPreview()
            && pitchPreviewCache.lookup (pitchPreviewRequest->fingerprint) == nullptr)
            requests.push_back (*pitchPreviewRequest);
        return requests;
    };
    pitchPreviewCache.onRenderReady = [this] (const std::shared_ptr<const RenderedDomain>& domain)
    {
        auto* clip = pitchTargetClip();
        if (domain == nullptr || clip == nullptr || ! pitchSession.hasPreview() || domain->recipeDigest != pitchPreviewDigest
            || domain->sourceAudio.get() != clip->audio.get())
            return; // 遅着・不一致は捨てる（generation はモード変更のたびに digest が変わることで代替）
        clip->previewDomain = domain;
        pushAudioValueSnapshot();
        timeline.refresh();
        pitchWindow.content().refresh();
        Log::info ("pitch.preview_ready", "clip=" + juce::String (clip->id));
    };
    pitchPreviewCache.onRenderFailed = [this] (const RenderFingerprint& failed)
    {
        if (failed.recipe != pitchPreviewDigest)
            return;
        Log::warn ("pitch.preview_failed", "clip=" + juce::String (pitchSession.clipId()));
        toast.show (jp (u8"ピッチ補正のプレビューを作れませんでした（原音で鳴っています）"), true, nullptr);
        // 永続値の巻き戻しも dirty 化もしない（エディタ内表示のみ）
    };

    timeline.onPitchEditRequested = [this] (int trackIndex, int itemIndex) { openPitchEditor (trackIndex, itemIndex); };
    engine.setBufferAudition (&bufferAudition);
}

void MainComponent::openPitchEditor (int trackIndex, int itemIndex)
{
    if (trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    if (track.type != TrackType::audio || itemIndex < 0 || itemIndex >= (int) track.clips.size())
        return;
    auto& clip = track.clips[(size_t) itemIndex];
    if (clip.audio == nullptr)
        return;
    if (clip.id == 0)
        project->ensureUniqueIds();

    if (pitchSession.isOpen() && pitchSession.clipId() == clip.id)
    {
        pitchWindow.openOver (this);
        return;
    }
    // 対象切替: 前のプレビューは破棄（変更プレビューはキャンセル＋トースト）
    if (pitchSession.isOpen())
    {
        if (pitchSession.mode() == PitchEditorSession::Mode::changePreview)
        {
            pitchSession.cancelChange();
            toast.show (jp (u8"ピッチ補正の変更プレビューを取り消しました（対象を切り替え）"), false, nullptr);
        }
        pitchWorker.cancelAndWait();
        pitchReanalyzing = false;
        pitchClearPreview();
        pitchSession.close();
        pitchPreviewRequest.reset();
    }
    Log::info ("pitch.open", "clip=" + juce::String (clip.id) + " corrected=" + juce::String ((int) clip.pitchCorrection.has_value()));
    pitchBlockedMessage.clear();
    if (clip.pitchCorrection.has_value() && clip.pitchCurve != nullptr)
        pitchSession.openCommitted (clip.id, *clip.pitchCorrection, clip.pitchCurve); // 保存済みの手直しを隠さない
    else
    {
        pitchSession.openForAnalysis (clip.id);
        pitchStartAnalysis (clip, /*reanalyze=*/ false);
    }
    pitchWindow.openOver (this);
    pitchWindow.content().refresh();
}

void MainComponent::pitchStartAnalysis (const Clip& clip, bool reanalyze)
{
    pitchWorker.cancelAndWait();
    if (pitchWorker.status() != PitchAnalysisWorker::Status::idle)
        pitchWorker.takeResult();
    pitchReanalyzing = reanalyze;
    PitchAnalysisWorker::Request request;
    request.source = clip.audio;
    request.sampleRate = renderSampleRate();
    request.wavFile = project->directory.getChildFile (clip.fileName);
    request.generation = ++pitchAnalysisGeneration;
    if (! pitchWorker.start (std::move (request)))
    {
        Log::error ("pitch.analyze_start_failed", "clip=" + juce::String (clip.id));
        toast.show (jp (u8"ピッチ解析を開始できませんでした"), true, nullptr);
        pitchReanalyzing = false;
    }
    else
        Log::info (reanalyze ? "pitch.reanalyze" : "pitch.analyze", "clip=" + juce::String (clip.id)
                                                                         + " gen=" + juce::String (pitchAnalysisGeneration));
}

void MainComponent::pitchEditorTick()
{
    bufferAudition.deleteRetired();
    const auto status = pitchWorker.status();
    if (status == PitchAnalysisWorker::Status::idle || status == PitchAnalysisWorker::Status::running)
        return;
    auto result = pitchWorker.takeResult();
    if (result.generation != pitchAnalysisGeneration || ! pitchSession.isOpen())
        return; // 古い generation の結果は着地させない
    auto* clip = pitchTargetClip();
    if (clip == nullptr)
        return;
    if (result.status != PitchAnalysisWorker::Status::success)
    {
        if (result.status == PitchAnalysisWorker::Status::failed)
        {
            Log::error ("pitch.analyze_failed", "error=" + result.errorMessage);
            toast.show (jp (u8"ピッチ解析に失敗しました: ") + result.errorMessage, true, nullptr);
            if (pitchSession.mode() == PitchEditorSession::Mode::analyzing)
                pitchWindow.dismiss();
        }
        pitchReanalyzing = false;
        return;
    }
    auto curve = std::make_shared<const PitchCurve> (std::move (result.curve));
    pitchBlockedMessage = result.sidecarWritten ? juce::String() : result.errorMessage;
    Log::info ("pitch.analyzed", "clip=" + juce::String (clip->id) + " frames=" + juce::String (curve->numFrames())
                                     + " digest=" + curve->digest().toHex().substring (0, 8)
                                     + " sidecar=" + juce::String ((int) result.sidecarWritten));
    if (pitchReanalyzing)
    {
        pitchReanalyzing = false;
        if (pitchSession.mode() != PitchEditorSession::Mode::committed)
            return;
        if (curve->digest() == pitchSession.working().curveDigest)
        {
            toast.show (jp (u8"再解析しましたが結果は同じでした"), false, nullptr);
            return;
        }
        // 内容が変わった: 新しいカーブで自動スナップし直した案を変更プレビューとして見せる
        const auto& w = pitchSession.working();
        auto proposal = PitchCorrections::autoSnap (*curve, PitchNotes::detect (*curve), clip->requestedDomainOffset(),
                                                    clip->requestedDomainLength(), PitchCorrections::effectiveScale (w, project->key),
                                                    w.scaleMode, w.customKey);
        proposal.strength = w.strength;
        proposal.speedMs = w.speedMs;
        pitchSession.beginChangePreview (proposal, curve);
        pitchSession.setSidecarWritten (result.sidecarWritten);
        pitchRequestPreview();
    }
    else
    {
        if (pitchSession.mode() != PitchEditorSession::Mode::analyzing)
            return;
        pitchSession.analysisFinished (curve, clip->requestedDomainOffset(), clip->requestedDomainLength(), project->key,
                                       result.sidecarWritten);
        pitchRequestPreview();
    }
    pitchWindow.content().refresh();
}

void MainComponent::pitchRequestPreview()
{
    auto* clip = pitchTargetClip();
    if (clip == nullptr || ! pitchSession.hasPreview() || pitchSession.curve() == nullptr)
    {
        pitchPreviewRequest.reset();
        return;
    }
    const auto sr = renderSampleRate();
    auto recipe = std::make_shared<RenderRecipe>();
    recipe->sourceAudio = clip->audio;
    recipe->sampleRate = sr;
    recipe->domainOffset = clip->requestedDomainOffset();
    recipe->domainLength = clip->requestedDomainLength();
    recipe->transposeSemitones = clip->transposeSemitones;
    recipe->stretchRatio = clip->stretchRatio;
    recipe->curve = pitchSession.curve();
    recipe->correction = pitchSession.working();
    ClipDomains::Request request;
    request.fingerprint = { clip->audio.get(), recipe->domainOffset, recipe->domainLength, recipe->transposeSemitones,
                            recipe->stretchRatio, sr, recipe->digest() };
    request.sourceAudio = clip->audio;
    request.recipe = recipe;
    pitchPreviewDigest = request.fingerprint.recipe;
    if (auto cached = pitchPreviewCache.lookup (request.fingerprint))
    {
        clip->previewDomain = cached;
        pitchPreviewRequest.reset();
        pushAudioValueSnapshot();
        timeline.refresh();
        return;
    }
    pitchPreviewRequest = std::move (request);
    pitchPreviewCache.syncNow();
}

void MainComponent::pitchClearPreview()
{
    bool changed = false;
    for (auto& track : project->tracks)
        for (auto& clip : track.clips)
            if (clip.previewDomain != nullptr)
            {
                clip.previewDomain = nullptr;
                changed = true;
            }
    pitchPreviewRequest.reset();
    pitchPreviewDigest = {};
    if (changed)
    {
        pushAudioValueSnapshot();
        timeline.refresh();
    }
}

void MainComponent::pitchBeginEdit()
{
    auto* clip = pitchTargetClip();
    if (clip == nullptr || ! pitchSession.editable())
        return;
    undoStack.begin (*project); // 1 操作 = 1 件（ドラッグ開始・クリック列の先頭で区切る）
    if (pitchSession.mode() == PitchEditorSession::Mode::initialPreview)
    {
        // 最初の明示編集で初回プレビューを確定（以後の編集は永続モデルへ直接）
        const auto committed = pitchSession.commitInitial();
        if (committed.has_value())
        {
            clip->pitchCorrection = committed;
            clip->pitchCurve = pitchSession.curve();
            clip->previewDomain = nullptr;
            pitchPreviewRequest.reset();
            Log::info ("pitch.commit_initial", "clip=" + juce::String (clip->id));
        }
    }
    // 分割の子（親の domain を共有）を初めて編集するときは自範囲へ detach（親子の digest がここで分かれる）
    if (clip->requestedDomainOffset() != clip->offsetSamples || clip->requestedDomainLength() != clip->lengthSamples)
    {
        PitchCorrections::detachToDomain (pitchSession.mutableWorking(), clip->requestedDomainOffset(), clip->requestedDomainLength(),
                                          clip->offsetSamples, clip->lengthSamples);
        clip->resetRenderDomainToSelf();
        Log::info ("pitch.detach", "clip=" + juce::String (clip->id));
    }
}

void MainComponent::pitchApplyWorking()
{
    auto* clip = pitchTargetClip();
    if (clip == nullptr || pitchSession.mode() != PitchEditorSession::Mode::committed)
        return;
    clip->pitchCorrection = pitchSession.working();
    clip->pitchCurve = pitchSession.curve();
    setDirty (true);
    renderCache.requestSync(); // デバウンス → 完了で activeDomain が一斉に切り替わる（完了までは古い音）
    timeline.refresh();
    pitchWindow.content().refresh();
}

void MainComponent::pitchSyncAfterModelChange()
{
    if (! pitchSession.isOpen())
        return;
    auto* clip = pitchTargetClip();
    if (clip == nullptr)
    {
        Log::info ("pitch.target_lost", "clip=" + juce::String (pitchSession.clipId()));
        pitchWindow.dismiss(); // 対象が消えた（削除・undo）: プレビューを破棄して閉じる（別クリップへは戻さない）
        return;
    }
    switch (pitchSession.mode())
    {
        case PitchEditorSession::Mode::committed:
            if (clip->pitchCorrection.has_value() && clip->pitchCurve != nullptr)
                pitchSession.syncCommitted (*clip->pitchCorrection, clip->pitchCurve);
            else
                pitchWindow.dismiss(); // undo で「補正なし」へ戻った
            break;
        case PitchEditorSession::Mode::initialPreview:
        case PitchEditorSession::Mode::changePreview:
            // undo は previewDomain を剥がす（UndoStack::stripClipDomains）。現在の状態から作り直す
            if (clip->previewDomain == nullptr)
                pitchRequestPreview();
            break;
        case PitchEditorSession::Mode::analyzing:
        case PitchEditorSession::Mode::closed:
            break;
    }
    pitchWindow.content().refresh();
}

void MainComponent::pitchDiscardPreviewForExport()
{
    if (! pitchSession.hasPreview())
        return;
    Log::info ("pitch.preview_discarded", "reason=export mode=" + juce::String ((int) pitchSession.mode()));
    if (pitchSession.mode() == PitchEditorSession::Mode::changePreview)
    {
        pitchSession.cancelChange();
        pitchClearPreview();
        toast.show (jp (u8"書き出しのためピッチ補正の変更プレビューを取り消しました"), false, nullptr);
        pitchWindow.content().refresh();
    }
    else
    {
        toast.show (jp (u8"書き出しのため未確定のピッチ補正プレビューを破棄しました"), false, nullptr);
        pitchWindow.dismiss(); // 初回プレビューは手作業を含まないので捨てて閉じる
    }
}

#if JUCE_DEBUG
void MainComponent::debugOpenPitchEditor (int trackIndex, int clipIndex)
{
    openPitchEditor (trackIndex, clipIndex);
}

void MainComponent::debugPitchAction (const juce::String& action)
{
    // 解析中なら終わるまで待ってから（pull 型のポーリングと同じ Timer で再試行）
    if (pitchSession.mode() == PitchEditorSession::Mode::analyzing || ! pitchSession.isOpen())
    {
        if (pitchSession.isOpen())
        {
            juce::Timer::callAfterDelay (200, [safe = juce::Component::SafePointer<MainComponent> (this), action]
            {
                if (safe != nullptr)
                    safe->debugPitchAction (action);
            });
        }
        return;
    }
    auto& view = pitchWindow.content();
    Log::info ("debug.pitch_action", "action=" + action + " mode=" + juce::String ((int) pitchSession.mode()));
    if (action == "enable" && view.onEnable) view.onEnable();
    else if (action == "apply" && view.onApply) view.onApply();
    else if (action == "cancel" && view.onCancel) view.onCancel();
    else if (action == "resnap" && view.onResnap) view.onResnap();
    else if (action == "reset" && view.onReset) view.onReset();
    else if (action == "reanalyze" && view.onReanalyze) view.onReanalyze();
    else if (action == "close") pitchWindow.dismiss();
    else if (action == "save") trySave();
    else if (action == "undo") performUndo();
    else if (action == "bounce") startBounceFlow();
    else if (action == "kero")
    {
        pitchBeginEdit();
        pitchSession.mutableWorking().speedMs = PitchCorrection::keroSpeedMs;
        pitchApplyWorking();
    }
    else if (action.startsWith ("bypass"))
    {
        const int idx = action.substring (6).getIntValue();
        if (idx >= 0 && idx < (int) pitchSession.working().notes.size())
        {
            pitchBeginEdit();
            auto& n = pitchSession.mutableWorking().notes[(size_t) idx];
            n.bypass = ! n.bypass;
            pitchApplyWorking();
        }
    }
    else if (action.startsWith ("move"))
    {
        const int idx = action.substring (4).getIntValue();
        if (auto* clip = pitchTargetClip(); clip != nullptr && idx >= 0 && idx < (int) pitchSession.working().notes.size())
        {
            pitchBeginEdit();
            const auto d = PitchCorrections::moveNote (pitchSession.mutableWorking(), idx, (juce::int64) (renderSampleRate() * 0.04),
                                                       clip->requestedDomainOffset(), clip->requestedDomainLength(),
                                                       clip->stretchRatio, renderSampleRate());
            Log::info ("debug.pitch_move", "note=" + juce::String (idx) + " applied=" + juce::String (d));
            pitchApplyWorking();
        }
    }
    else if (action.startsWith ("target"))
    {
        // targetN:+M
        const int idx = action.fromFirstOccurrenceOf ("target", false, false).upToFirstOccurrenceOf (":", false, false).getIntValue();
        const int delta = action.fromFirstOccurrenceOf (":", false, false).getIntValue();
        if (idx >= 0 && idx < (int) pitchSession.working().notes.size())
        {
            pitchBeginEdit();
            pitchSession.mutableWorking().notes[(size_t) idx].targetMidi += delta;
            pitchApplyWorking();
        }
    }
    view.refresh();
}

void MainComponent::debugPitchSnapshot (const juce::File& target)
{
    if (pitchSession.mode() == PitchEditorSession::Mode::analyzing || (pitchSession.hasPreview() && ! pitchPreviewCache.isIdle())
        || (pitchSession.isOpen() && pitchTargetClip() != nullptr && pitchTargetClip()->renderPending (renderSampleRate())))
    {
        juce::Timer::callAfterDelay (200, [safe = juce::Component::SafePointer<MainComponent> (this), target]
        {
            if (safe != nullptr)
                safe->debugPitchSnapshot (target);
        });
        return;
    }
    auto& view = pitchWindow.content();
    auto image = view.createComponentSnapshot (view.getLocalBounds(), true, 2.0f);
    target.deleteFile();
    juce::PNGImageFormat png;
    std::unique_ptr<juce::OutputStream> out (target.createOutputStream());
    const bool ok = out != nullptr && png.writeImageToStream (image, *out);
    const auto* clip = pitchTargetClip();
    Log::info ("debug.pitch_snapshot", "path=" + target.getFullPathName() + " ok=" + juce::String (ok ? 1 : 0)
                                           + " mode=" + juce::String ((int) pitchSession.mode())
                                           + " notes=" + juce::String ((int) pitchSession.working().notes.size())
                                           + " preview=" + juce::String (clip != nullptr && clip->previewDomain != nullptr ? 1 : 0)
                                           + " corrected=" + juce::String (clip != nullptr && clip->pitchCorrection.has_value() ? 1 : 0)
                                           + " dirty=" + juce::String (dirty ? 1 : 0)
                                           + " visible=" + juce::String (pitchWindow.isVisible() ? 1 : 0));
}
#endif
