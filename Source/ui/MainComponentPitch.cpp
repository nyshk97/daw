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

#include "../audio/VocalResynth.h"
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
        if (pitchPreviewActive())
            return ! pitchPreviewCache.isIdle();
        const auto* clip = pitchTargetClip();
        return clip != nullptr && clip->renderPending (renderSampleRate());
    };
    view.getAnalysisProgress = [this] { return pitchWorker.progress(); };
    view.getBlockedMessage = [this] { return pitchBlockedMessage; };
    view.onKey = [this] (const juce::KeyPress& key) { return keyPressed (key); };

    view.onBeginEdit = [this] { pitchBeginEdit(); };
    view.onEndEdit = [this] { pitchEndEdit(); };
    view.onPreviewEdit = [this] { pitchPreviewWorking(); };
    view.onEnable = [this]
    {
        if (pitchSession.mode() != PitchEditorSession::Mode::initialPreview || ! pitchSession.editable())
            return;
        Log::info ("pitch.enable", "clip=" + juce::String (pitchSession.clipId()));
        pitchBeginEdit();       // 初回プレビューの確定（1件の undo）
        pitchEndEdit();
    };
    view.onApply = [this]
    {
        auto* clip = pitchTargetClip();
        if (clip == nullptr || pitchSession.mode() != PitchEditorSession::Mode::changePreview)
            return;
        Log::info ("pitch.apply", "clip=" + juce::String (clip->id));
        undoStack.begin (*project);
        pitchSession.applyChange();
        pitchWriteWorkingToClip (*clip);
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
    view.onScaleChanged = [this] (PitchScaleMode mode, ProjectKey key)
    {
        auto* clip = pitchTargetClip();
        if (clip == nullptr || pitchSession.curve() == nullptr)
            return;
        using Mode = PitchEditorSession::Mode;
        if (pitchSession.mode() == Mode::changePreview)
            pitchSession.cancelChange(); // 選び直し: 前の案を捨てて元から付け直す
        if (pitchSession.mode() != Mode::committed && pitchSession.mode() != Mode::initialPreview)
            return;
        auto proposal = pitchSession.working();
        proposal.scaleMode = mode;
        proposal.customKey = key;
        PitchCorrections::resnap (proposal, *pitchSession.curve(), clip->offsetSamples, clip->lengthSamples,
                                  PitchCorrections::effectiveScale (proposal, project->key));
        Log::info ("pitch.scale_changed", "clip=" + juce::String (clip->id) + " mode=" + juce::String ((int) mode)
                                              + " key=" + ProjectKeys::cliText (key));
        if (pitchSession.mode() == Mode::initialPreview)
        {
            // 未確定プレビューの中なら単に作り直す（まだ何も確定していない）
            pitchSession.mutableWorking() = proposal;
            pitchRequestPreview();
        }
        else
        {
            // 確定状態からは「付け直しのプレビュー」（元の目標はゴーストで見える）。Apply / Cancel で確定・取消
            pitchSession.beginChangePreview (proposal, pitchSession.curve());
            pitchRequestPreview();
        }
        pitchWindow.content().refresh();
    };
    view.onReset = [this]
    {
        auto* clip = pitchTargetClip();
        if (clip == nullptr || pitchSession.mode() != PitchEditorSession::Mode::committed || pitchSession.curve() == nullptr)
            return;
        // 「すべてリセット」= 現在のスケール設定で自動スナップし直し（手直し・つまみを捨てる）。変更プレビューとして見せる
        const auto& w = pitchSession.working();
        auto proposal = PitchCorrections::autoSnap (*pitchSession.curve(), pitchSession.detected(), clip->offsetSamples, clip->lengthSamples,
                                                    PitchCorrections::effectiveScale (w, project->key), w.scaleMode, w.customKey);
        if (proposal == w)
        {
            pitchWindow.content().showStatus (jp (u8"Reset: 既に自動スナップの状態です"));
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
        const auto off = clip->offsetSamples, len = clip->lengthSamples; // working は自範囲
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

    view.onDragAuditionStart = [this] (int noteIndex)
    {
        noteAudition.reset();
        auto* clip = pitchTargetClip();
        if (clip == nullptr || pitchSession.curve() == nullptr || transport.isPlaying.load() || engine.isRecording())
            return;
        const auto& w = pitchSession.working();
        if (noteIndex < 0 || noteIndex >= (int) w.notes.size())
            return;
        const auto off = clip->offsetSamples, len = clip->lengthSamples; // working は自範囲
        const auto s = w.resolve (w.notes[(size_t) noteIndex].start, off, len);
        const auto e = w.resolve (w.notes[(size_t) noteIndex].end, off, len);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const bool ok = noteAudition.prepare (*clip->audio, *pitchSession.curve(), renderSampleRate(), s, e);
        Log::info ("pitch.drag_audition_prepare", "note=" + juce::String (noteIndex) + " ok=" + juce::String ((int) ok)
                                                      + " ms=" + juce::String (juce::Time::getMillisecondCounterHiRes() - t0, 1));
    };
    view.onDragAuditionUpdate = [this]
    {
        auto* clip = pitchTargetClip();
        if (clip == nullptr || ! noteAudition.isPrepared() || pitchSession.curve() == nullptr)
            return;
        auto buf = noteAudition.render (pitchSession.working(), *pitchSession.curve(), clip->offsetSamples, clip->lengthSamples,
                                        clip->transposeSemitones);
        if (buf == nullptr)
            return;
        filePreview.stop();
        bufferAudition.start (buf, 0, buf->getNumSamples(), renderSampleRate());
    };
    view.onDragAuditionEnd = [this] { noteAudition.reset(); };

    pitchWindow.onDismissed = [this]
    {
        pitchEndEdit(); // 進行中のジェスチャーを閉じる
        bufferAudition.stop();
        noteAudition.reset();
        VocalResynth::clearAnalysisCache();
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
        if (pitchPreviewRequest.has_value() && pitchPreviewActive()
            && pitchPreviewCache.lookup (pitchPreviewRequest->fingerprint) == nullptr)
            requests.push_back (*pitchPreviewRequest);
        return requests;
    };
    pitchPreviewCache.onRenderReady = [this] (const std::shared_ptr<const RenderedDomain>& domain)
    {
        auto* clip = pitchTargetClip();
        if (domain == nullptr || clip == nullptr)
            return;
        // 指紋が現在の要求と一致すれば受け取る（ジェスチャー終了直後の in-flight も含む。その後 dropPreviewIfCurrent が寿命を決める）
        if (domain->fingerprint() != pitchPreviewFingerprint || domain->sourceAudio.get() != clip->audio.get())
            return; // 遅着・不一致は捨てる（指紋はモード変更のたびに変わるので generation の代わりになる）
        clip->previewDomain = domain;
        pitchSuppressPreviewFailDigest = {}; // 成功したら抑止は外す
        pitchDropStalePreview(); // 活動中でなく本レンダーも追いついていれば即外す
        pushAudioValueSnapshot();
        timeline.refresh();
        pitchWindow.content().refresh();
        Log::info ("pitch.preview_ready", "clip=" + juce::String (clip->id));
    };
    pitchPreviewCache.onRenderFailed = [this] (const RenderFingerprint& failed)
    {
        if (failed != pitchPreviewFingerprint || ! pitchPreviewActive())
            return; // 誰も待っていない遅着失敗にはトーストを出さない
        pitchPreviewFingerprint = {};
        Log::warn ("pitch.preview_failed", "clip=" + juce::String (pitchSession.clipId()));
        if (! failed.recipe.isNull() && failed.recipe == pitchSuppressPreviewFailDigest)
            pitchSuppressPreviewFailDigest = {}; // 巻き戻し直後の再プレビュー失敗: 赤の失敗トーストを残す（同じ原因）
        else
            toast.show (jp (u8"ピッチ補正のプレビューを作れませんでした（原音で鳴っています）"), true, nullptr);
        pitchWindow.content().showStatus (jp (u8"プレビューのレンダーに失敗（原音で鳴っています）"));
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
        bufferAudition.stop();
        noteAudition.reset();
        pitchWorker.cancelAndWait();
        pitchReanalyzing = false;
        pitchClearPreview();
        pitchSession.close();
        pitchPreviewRequest.reset();
    }
    Log::info ("pitch.open", "clip=" + juce::String (clip.id) + " corrected=" + juce::String ((int) clip.pitchCorrection.has_value()));
    pitchBlockedMessage.clear();
    VocalResynth::clearAnalysisCache(); // 前の対象の分解キャッシュ（数百 MB）を持ち越さない
    if (clip.pitchCorrection.has_value() && clip.pitchCurve != nullptr)
    {
        // 保存済みの手直しを隠さない。分割子（親の domain を共有）は**開いた時点で**自範囲へ写した作業コピーにする —
        // 編集中に detach するとノート index が途中でずれる（レビュー指摘）。クリップ側は最初の書き込みまで親の
        // domain を共有したまま（再レンダーしない）
        pitchSession.openCommitted (clip.id, *clip.pitchCorrectionInOwnDomain(), clip.pitchCurve);
    }
    else
    {
        pitchSession.openForAnalysis (clip.id);
        pitchStartAnalysis (clip, /*reanalyze=*/ false);
    }
    pitchWindow.content().showStatus ({}); // 前のクリップの状態表示を持ち越さない
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
    {
        Log::info (reanalyze ? "pitch.reanalyze" : "pitch.analyze", "clip=" + juce::String (clip.id)
                                                                         + " gen=" + juce::String (pitchAnalysisGeneration));
        if (reanalyze)
            pitchWindow.content().showStatus (jp (u8"Re-analyze: 解析中…"));
    }
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
    // ワーカーが書いてから回収するまでの間に ⌘S の GC が世代を消していることがある（保持リストに無いため）。
    // ここで存在を確かめ、無ければ書き直す（write は同一内容なら冪等）
    if (result.sidecarWritten && ! result.sidecarFile.existsAsFile())
    {
        juce::String err;
        result.sidecarWritten = PitchSidecar::write (*curve, project->directory.getChildFile (clip->fileName), &err);
        if (! result.sidecarWritten) result.errorMessage = err;
        Log::info ("pitch.sidecar_rewrite", "ok=" + juce::String ((int) result.sidecarWritten));
    }
    pitchBlockedMessage = result.sidecarWritten ? juce::String() : result.errorMessage;
    Log::info ("pitch.analyzed", "clip=" + juce::String (clip->id) + " frames=" + juce::String (curve->numFrames())
                                     + " digest=" + curve->digest().toShortHex()
                                     + " sidecar=" + juce::String ((int) result.sidecarWritten));
    if (pitchReanalyzing)
    {
        pitchReanalyzing = false;
        if (pitchSession.mode() != PitchEditorSession::Mode::committed)
            return;
        if (curve->digest() == pitchSession.working().curveDigest)
        {
            pitchWindow.content().showStatus (jp (u8"Re-analyze: 結果は同じでした（解析器が同じなので変わりません）"));
            return;
        }
        // 内容が変わった: 新しいカーブで自動スナップし直した案を変更プレビューとして見せる
        const auto& w = pitchSession.working();
        auto proposal = PitchCorrections::autoSnap (*curve, PitchNotes::detect (*curve), clip->offsetSamples, clip->lengthSamples,
                                                    PitchCorrections::effectiveScale (w, project->key), w.scaleMode, w.customKey);
        proposal.strength = w.strength;
        proposal.speedMs = w.speedMs;
        int pinnedCount = 0;
        for (const auto& n : pitchSession.working().notes) pinnedCount += n.pinned ? 1 : 0;
        pitchSession.beginChangePreview (proposal, curve);
        pitchSession.setSidecarWritten (result.sidecarWritten);
        pitchRequestPreview();
        // Re-analyze はゼロから（新しい検出結果でノート境界が変わるため手で置いた目標は引き継がない）。Cancel で戻せる
        pitchWindow.content().showStatus (pinnedCount > 0
            ? jp (u8"Re-analyze: 検出が変わりました。手で置いた ") + juce::String (pinnedCount) + jp (u8" 音も自動スナップに戻ります（Cancel で元へ）")
            : jp (u8"Re-analyze: 検出が変わりました（Apply で確定・Cancel で元へ）"));
    }
    else
    {
        if (pitchSession.mode() != PitchEditorSession::Mode::analyzing)
            return;
        pitchSession.analysisFinished (curve, clip->offsetSamples, clip->lengthSamples, project->key, result.sidecarWritten);
        pitchRequestPreview();
    }
    pitchWindow.content().refresh();
}

void MainComponent::pitchRequestPreview (bool suppressFailToast)
{
    pitchSuppressPreviewFailDigest = {}; // 全出口で「前の抑止」を持ち越さない（成功・早期 return・別要求）
    auto* clip = pitchTargetClip();
    if (clip == nullptr || ! pitchPreviewActive() || pitchSession.curve() == nullptr)
    {
        pitchPreviewRequest.reset();
        return;
    }
    const auto sr = renderSampleRate();
    if (pitchSession.working().isAudiblyNeutral())
    {
        // 中立（Strength 0%・タイミング未編集）: 確定時の requestedRecipeDigest と同じく recipe を出さない。
        // activeDomain に補正が入っていなければ外すだけでよい（無加工 or signalsmith の音がそのまま鳴る）。
        // 補正付きでレンダー済みなら、外すと旧補正が鳴ってしまうので「補正なしの鳴り」をプレビューに載せる
        const bool activeHasCorrection = clip->activeDomain != nullptr && clip->activeDomain->correction != nullptr;
        if (! activeHasCorrection)
        {
            const bool had = clip->previewDomain != nullptr;
            clip->previewDomain = nullptr;
            pitchPreviewRequest.reset();
            pitchPreviewFingerprint = {};
            if (had) { pushAudioValueSnapshot(); timeline.refresh(); }
            return;
        }
        if (clip->transposeSemitones == 0 && juce::exactlyEqual (clip->stretchRatio, 1.0))
        {
            clip->previewDomain = ClipDomains::makeNeutralDomain (clip->audio, clip->offsetSamples, clip->lengthSamples, sr); // 即座
            pitchPreviewRequest.reset();
            pitchPreviewFingerprint = {};
            pushAudioValueSnapshot();
            timeline.refresh();
            return;
        }
        // 移調・伸縮あり: recipe 無し（signalsmith）の要求をプレビュー経路に積む
        ClipDomains::Request request;
        request.fingerprint = { clip->audio.get(), clip->offsetSamples, clip->lengthSamples, clip->transposeSemitones,
                                clip->stretchRatio, sr, ContentDigest{} };
        request.sourceAudio = clip->audio;
        pitchPreviewFingerprint = request.fingerprint;
        if (auto cached = pitchPreviewCache.lookup (request.fingerprint))
        {
            clip->previewDomain = cached;
            pitchPreviewRequest.reset();
            pushAudioValueSnapshot();
            timeline.refresh();
            return;
        }
        clip->previewDomain = nullptr; // 旧プレビュー（WORLD）を外してから要求（失敗しても「原音で鳴っています」が嘘にならない）
        pushAudioValueSnapshot();
        pitchPreviewRequest = std::move (request);
        pitchPreviewCache.syncNow();
        return;
    }
    auto recipe = std::make_shared<RenderRecipe>();
    recipe->sourceAudio = clip->audio;
    recipe->sampleRate = sr;
    recipe->domainOffset = clip->offsetSamples;   // working は常にクリップ自身の範囲
    recipe->domainLength = clip->lengthSamples;
    recipe->transposeSemitones = clip->transposeSemitones;
    recipe->stretchRatio = clip->stretchRatio;
    recipe->curve = pitchSession.curve();
    recipe->correction = pitchSession.working();
    ClipDomains::Request request;
    request.fingerprint = { clip->audio.get(), recipe->domainOffset, recipe->domainLength, recipe->transposeSemitones,
                            recipe->stretchRatio, sr, recipe->digest() };
    request.sourceAudio = clip->audio;
    request.recipe = recipe;
    pitchPreviewFingerprint = request.fingerprint;
    if (auto cached = pitchPreviewCache.lookup (request.fingerprint))
    {
        clip->previewDomain = cached;
        pitchSuppressPreviewFailDigest = {}; // キャッシュ命中も成功扱い
        pitchPreviewRequest.reset();
        pushAudioValueSnapshot();
        timeline.refresh();
        return;
    }
    pitchPreviewRequest = std::move (request);
    pitchSuppressPreviewFailDigest = suppressFailToast ? pitchPreviewFingerprint.recipe : ContentDigest{};
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
    pitchPreviewFingerprint = {};
    if (changed)
    {
        pushAudioValueSnapshot();
        timeline.refresh();
    }
}

// working（常にクリップ自身の範囲で表現）を永続モデルへ書く。分割子は最初の書き込みでここで自範囲へ移り
// 親とのドメイン共有が終わる（親子の digest がここで分かれる＝再レンダーはこのとき初めて）
void MainComponent::pitchWriteWorkingToClip (Clip& clip)
{
    const bool detaching = clip.sharesInheritedDomain();
    clip.pitchCorrection = pitchSession.working(); // working は自範囲で表現済み
    clip.pitchCurve = pitchSession.curve();
    if (detaching)
    {
        clip.resetRenderDomainToSelf(); // working は既に自範囲なので中の detach は冪等（無害）。domain を自範囲へ
        Log::info ("pitch.detach", "clip=" + juce::String (clip.id));
    }
}

void MainComponent::pitchDropStalePreview()
{
    auto* clip = pitchTargetClip();
    const bool dropped = clip != nullptr && clip->dropPreviewIfCurrent (renderSampleRate(), pitchPreviewActive());
    // 対象外のクリップに previewDomain が残っていることは無い（cloneForNewId が落とす）が、念のため全クリップも見る
    bool others = false;
    for (auto& track : project->tracks)
        for (auto& c : track.clips)
            if (&c != clip && c.previewDomain != nullptr) { c.previewDomain = nullptr; others = true; }
    if (dropped || others)
    {
        if (dropped) pitchPreviewFingerprint = {};
        pushAudioValueSnapshot();
        timeline.refresh();
        pitchWindow.content().refresh();
    }
}

bool MainComponent::pitchPreviewActive() const
{
    return pitchSession.hasPreview()
        || (pitchEdit.has_value() && pitchSession.mode() == PitchEditorSession::Mode::committed);
}

void MainComponent::pitchBeginEdit()
{
    if (pitchEdit.has_value())
        pitchEndEdit(); // ネストさせない（前のジェスチャーを先に閉じる）
    auto* clip = pitchTargetClip();
    if (clip == nullptr || ! pitchSession.editable())
        return;
    PitchEdit edit;
    edit.token = undoStack.begin (*project); // 1 操作 = 1 件（最初の実変更・クリック列の先頭で区切る）
    if (pitchSession.mode() == PitchEditorSession::Mode::initialPreview && pitchSession.commitInitial().has_value())
    {
        // 最初の明示編集で初回プレビューを確定（以後の編集は永続モデルへ直接）。確定＝実変更なのでこの begin は取り消さない
        edit.committedInitial = true;
        clip->previewDomain = nullptr;
        pitchPreviewRequest.reset();
        pitchApplyWorking(); // 書き込み・dirty・レンダー同期
        Log::info ("pitch.commit_initial", "clip=" + juce::String (clip->id));
    }
    edit.before = pitchSession.working();
    edit.revisionAtBegin = pitchSession.revision();
    pitchEdit = std::move (edit);
}

void MainComponent::pitchEndEdit()
{
    if (! pitchEdit.has_value())
        return;
    const auto edit = std::move (*pitchEdit);
    pitchEdit.reset(); // トークンは使い捨て（古いトークンで別の begin を消さない）
    const bool replaced = pitchSession.revision() != edit.revisionAtBegin; // 外部置換（巻き戻し・再解析の着地）はユーザーの編集ではない
    const bool changed = pitchSession.isOpen() && ! replaced && ! (pitchSession.working() == edit.before);
    // previewDomain の寿命は Clip::dropPreviewIfCurrent の 1 文（活動中 or 要求未達なら残す）。ここでは確定/取消だけ行い、
    // 最後に寿命規則を 1 回適用する（取消でも確定でも、本レンダーが要るなら着くまで今の音のまま＝音飛びなし）
    if (changed)
        pitchApplyWorking();
    else if (! edit.committedInitial)
        undoStack.abandon (edit.token); // 変化なし or 外部置換: undo も dirty も無し（判定はここ 1 箇所）
    if (replaced && pitchSession.hasPreview())
        pitchRequestPreview(); // 置換後のプレビュー（変更プレビューの着地）を鳴らし直す
    pitchDropStalePreview();
    if (replaced)
        Log::info ("pitch.edit_replaced", "clip=" + juce::String (pitchSession.clipId()));
}

void MainComponent::pitchPreviewWorking()
{
    // ジェスチャー途中は永続モデル（clip）を触らない。初回プレビューと同じ previewDomain 経路で鳴りだけ追従させる
    //（CLAUDE.md: プレビュー段階の副作用はメモリ内だけ。確定は pitchEndEdit → pitchApplyWorking の一括）
    if (! pitchEdit.has_value() || pitchSession.mode() != PitchEditorSession::Mode::committed)
        return;
    pitchRequestPreview();
    pitchWindow.content().refresh();
}

void MainComponent::pitchApplyWorking()
{
    auto* clip = pitchTargetClip();
    if (clip == nullptr || pitchSession.mode() != PitchEditorSession::Mode::committed)
        return;
    pitchWriteWorkingToClip (*clip);
    setDirty (true);
    renderCache.requestSync(); // デバウンス → 完了で activeDomain が一斉に切り替わる（完了までは古い音）
    timeline.refresh();
    pitchWindow.content().refresh();
}

bool MainComponent::pitchSyncAfterModelChange (bool quiet)
{
    bool droppedChangePreview = false;
    // quiet: 失敗巻き戻し直後の呼び出し。失敗トースト（赤・dirty 化の事実）を後勝ちのトーストで上書きしない
    if (! pitchSession.isOpen())
        return false;
    auto* clip = pitchTargetClip();
    if (clip == nullptr)
    {
        Log::info ("pitch.target_lost", "clip=" + juce::String (pitchSession.clipId()));
        pitchWindow.dismiss(); // 対象が消えた（削除・undo）: プレビューを破棄して閉じる（別クリップへは戻さない）
        return false;
    }
    switch (pitchSession.mode())
    {
        case PitchEditorSession::Mode::committed:
            if (clip->pitchCorrection.has_value() && clip->pitchCurve != nullptr)
            {
                const bool changed = pitchSession.syncCommitted (*clip->pitchCorrectionInOwnDomain(), clip->pitchCurve); // working は常に自範囲
                if (quiet && changed)
                    pitchWindow.content().showStatus (jp (u8"レンダーに失敗したため編集を元に戻しました")); // 独立ウィンドウはメインのトーストが見えない
            }
            else
                pitchWindow.dismiss(); // undo で「補正なし」へ戻った
            break;
        case PitchEditorSession::Mode::changePreview:
            // undo で永続値が変わっていたら backup が陳腐化している → 変更プレビューは破棄（Cancel 相当）
            if (const auto& bk = pitchSession.backup(); bk.has_value())
            {
                const auto current = clip->pitchCorrectionInOwnDomain();
                if (! current.has_value() || ! (*current == *bk))
                {
                    pitchSession.cancelChange();
                    pitchClearPreview();
                    droppedChangePreview = true;
                    if (! quiet)
                        toast.show (jp (u8"モデルが変わったためピッチ補正の変更プレビューを取り消しました"), false, nullptr); // トーストは持ち越さないので dismiss 前でよい
                    if (! current.has_value())
                    {
                        pitchWindow.dismiss(); // 閉じるのでエディタ内の状態表示は出さない（次のクリップに残さない）
                        break;
                    }
                    pitchSession.syncCommitted (*current, clip->pitchCurve);
                    if (quiet)
                        pitchWindow.content().showStatus (jp (u8"変更プレビューを取り消しました（レンダー失敗）"));
                    break;
                }
            }
            [[fallthrough]];
        case PitchEditorSession::Mode::initialPreview:
        {
            // undo は previewDomain を剥がす（UndoStack::stripClipDomains）。移調・伸縮・範囲が変わった場合も作り直す
            const auto* pd = clip->previewDomain.get();
            const bool stale = pd == nullptr || pd->semitones != clip->transposeSemitones || ! juce::exactlyEqual (pd->ratio, clip->stretchRatio)
                            || pd->domainOffset != clip->offsetSamples || pd->domainLength != clip->lengthSamples;
            if (stale)
            {
                // 巻き戻し直後の再プレビューが同じ原因で失敗したときだけ青トーストを出さない。抑止対象は
                // これから出す要求の digest に限る。pitchRequestPreview が成功（キャッシュ命中）や早期 return で
                // 終わった場合はその中でクリアされるので、代入は呼び出しの**前**
                pitchRequestPreview (quiet);
            }
            break;
        }
        case PitchEditorSession::Mode::analyzing:
        case PitchEditorSession::Mode::closed:
            break;
    }
    pitchWindow.content().refresh();
    return droppedChangePreview;
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
    else if (action == "resnap" && view.onScaleChanged) view.onScaleChanged (PitchScaleMode::chromatic, {});
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
        pitchEndEdit();
    }
    else if (action.startsWith ("bypass"))
    {
        const int idx = action.substring (6).getIntValue();
        if (idx >= 0 && idx < (int) pitchSession.working().notes.size())
        {
            pitchBeginEdit();
            PitchCorrections::toggleNoteBypass (pitchSession.mutableWorking(), idx);
            pitchEndEdit();
        }
    }
    else if (action.startsWith ("move"))
    {
        const int idx = action.substring (4).getIntValue();
        if (auto* clip = pitchTargetClip(); clip != nullptr && idx >= 0 && idx < (int) pitchSession.working().notes.size())
        {
            pitchBeginEdit();
            const auto d = PitchCorrections::moveNote (pitchSession.mutableWorking(), idx, (juce::int64) (renderSampleRate() * 0.04),
                                                       clip->offsetSamples, clip->lengthSamples, clip->stretchRatio, renderSampleRate());
            Log::info ("debug.pitch_move", "note=" + juce::String (idx) + " applied=" + juce::String (d));
            pitchEndEdit(); // クランプで動かなければ undo も dirty も残らない
        }
    }
    else if (action.startsWith ("target"))
    {
        // targetN:+M
        const int idx = action.fromFirstOccurrenceOf ("target", false, false).upToFirstOccurrenceOf (":", false, false).getIntValue();
        const int delta = action.fromFirstOccurrenceOf (":", false, false).getIntValue();
        if (idx >= 0 && idx < (int) pitchSession.working().notes.size())
        {
            const auto before = pitchSession.working().notes[(size_t) idx];
            pitchBeginEdit();
            PitchCorrections::setNoteTarget (pitchSession.mutableWorking(), idx, before.targetMidi + delta, before.targetMidi, before.pinned);
            pitchEndEdit();
        }
    }
    view.refresh();
}

void MainComponent::debugPitchSnapshot (const juce::File& target)
{
    if (pitchSession.mode() == PitchEditorSession::Mode::analyzing || (pitchPreviewActive() && ! pitchPreviewCache.isIdle())
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
