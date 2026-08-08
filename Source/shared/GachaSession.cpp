#include "GachaSession.h"

#include <algorithm>

namespace
{
// porcelain 行の共通部（base / status / lane_seeds オブジェクト）を読む
bool parsePorcelainCommon (const juce::String& line, juce::var& seedsOut,
                           GachaSession::Candidate& out)
{
    const auto parsed = juce::JSON::parse (line);
    if (! parsed.isObject())
        return false;
    const auto base = parsed.getProperty ("base", {});
    const auto seeds = parsed.getProperty ("lane_seeds", {});
    const auto status = parsed.getProperty ("status", {});
    if (! base.isString() || ! seeds.isObject() || ! status.isString())
        return false;
    out.base = base.toString();
    out.status = status.toString();
    seedsOut = seeds;
    return out.base.isNotEmpty();
}
}

bool GachaSession::parsePorcelainLine (const juce::String& line, Candidate& out)
{
    Candidate candidate;
    juce::var seeds;
    if (! parsePorcelainCommon (line, seeds, candidate))
        return false;
    candidate.kickSeed = seeds.getProperty ("kick", {}).toString();
    candidate.snareSeed = seeds.getProperty ("snare", {}).toString();
    candidate.hatSeed = seeds.getProperty ("hat", {}).toString();
    if (candidate.kickSeed.isEmpty() || candidate.snareSeed.isEmpty()
        || candidate.hatSeed.isEmpty())
        return false;
    out = std::move (candidate);
    return true;
}

bool GachaSession::parseBassPorcelainLine (const juce::String& line, Candidate& out)
{
    Candidate candidate;
    juce::var seeds;
    if (! parsePorcelainCommon (line, seeds, candidate))
        return false;
    candidate.progSeed = seeds.getProperty ("prog", {}).toString();
    candidate.rhythmSeed = seeds.getProperty ("rhythm", {}).toString();
    if (candidate.progSeed.isEmpty() || candidate.rhythmSeed.isEmpty())
        return false;
    out = std::move (candidate);
    return true;
}

GachaSession::Pattern GachaSession::patternFromDrumNotes (const std::vector<MidiNote>& notes)
{
    Pattern pattern {};
    const juce::int64 slotTicks = Ppq::ticksPerBar / patternSlots; // 16分 = 240 tick
    for (const auto& note : notes)
    {
        if (note.startPpq >= Ppq::ticksPerBar)
            continue; // 2小節目以降（1小節パターンの繰り返し）は見ない
        const int lane = note.pitch == 36 ? 0 : note.pitch == 38 ? 1 : note.pitch == 42 ? 2 : -1;
        if (lane < 0)
            continue;
        // 最近傍の16分スロットへ丸める（小節末のスウィング裏拍が16へ丸まったら15に収める）
        const int slot = juce::jmin (patternSlots - 1,
                                     (int) ((note.startPpq + slotTicks / 2) / slotTicks));
        // 骨格(強度>=0.35)は vel ≈ 20+0.35×96 ≈ 53 以上になる（生成器の式の逆算。表示専用の近似）
        const int strength = note.velocity >= 53 ? 2 : 1;
        auto& cell = pattern[(size_t) lane][(size_t) slot];
        cell = juce::jmax (cell, strength);
    }
    return pattern;
}

std::vector<GachaSession::BassDot> GachaSession::bassDotsFromNotes (const std::vector<MidiNote>& notes,
                                                                    juce::int64 patternTicks)
{
    std::vector<BassDot> dots;
    if (patternTicks <= 0)
        return dots;
    // ベース生成のハード範囲 MIDI 28..51（bass.py の HARD_RANGE と同じ値。表示専用の正規化）
    constexpr int low = 28, high = 51;
    for (const auto& note : notes)
    {
        if (note.startPpq >= patternTicks)
            continue; // パターン1周ぶんだけ描く（繰り返しは同じ絵になる）
        dots.push_back ({ (float) note.startPpq / (float) patternTicks,
                          (float) juce::jlimit (0, high - low, note.pitch - low) / (float) (high - low) });
    }
    return dots;
}

GachaSession::BassRollPlan GachaSession::planBassRoll (const Project& project, int drumsTrackIndex,
                                                       int drumsRegionIndex, int loopBars)
{
    BassRollPlan plan;
    loopBars = juce::jlimit (1, 16, loopBars);

    const Track* track = nullptr;
    const MidiRegion* region = nullptr;
    if (drumsTrackIndex >= 0 && drumsTrackIndex < (int) project.tracks.size())
    {
        track = &project.tracks[(size_t) drumsTrackIndex];
        if (drumsRegionIndex >= 0 && drumsRegionIndex < (int) track->midiRegions.size())
            region = &track->midiRegions[(size_t) drumsRegionIndex];
    }

    if (region != nullptr)
        plan.drumsBars = (int) ((region->totalLengthPpq() + Ppq::ticksPerBar - 1) / Ppq::ticksPerBar);

    plan.previewBars = juce::jmax (loopBars, plan.drumsBars);
    plan.previewBars = ((plan.previewBars + loopBars - 1) / loopBars) * loopBars;

    if (region != nullptr)
    {
        const auto limitPpq = (juce::int64) plan.previewBars * Ppq::ticksPerBar;
        const int reps = 1 + juce::jmax (0, region->loopCount);
        for (int r = 0; r < reps; ++r)
        {
            for (const auto& note : region->notes)
            {
                // 固定ピッチ打楽器トラック（Kick 等）はトラックの drumPitch が実効ピッチ
                const int effectivePitch = track->drumPitch >= 0 ? track->drumPitch : note.pitch;
                if (effectivePitch != 36)
                    continue;
                const auto tick = (juce::int64) r * region->lengthPpq + note.startPpq;
                if (tick < limitPpq)
                    plan.kickTicks.add (juce::String (tick / 2)); // LaLa PPQ 960 → bass.py PPQ 480
            }
        }
    }
    return plan;
}

int GachaSession::findTrack (const Project& project, juce::uint64 trackId) const
{
    for (int i = 0; i < (int) project.tracks.size(); ++i)
        if (project.tracks[(size_t) i].id == trackId)
            return i;
    return -1;
}

bool GachaSession::hasPreview() const
{
    return std::any_of (previews.begin(), previews.end(),
                        [] (const PartPreview& p) { return p.active; });
}

juce::uint64 GachaSession::previewRegionId (Part part) const
{
    const auto& p = previews[(size_t) part];
    return p.active ? p.regionId : 0;
}

juce::uint64 GachaSession::previewTrackId (Part part) const
{
    const auto& p = previews[(size_t) part];
    return p.active ? p.trackId : 0;
}

juce::int64 GachaSession::previewStartPpq (Part part) const
{
    const auto& p = previews[(size_t) part];
    return p.active ? p.startPpq : 0;
}

bool GachaSession::isPreviewObject (juce::uint64 objectTrackId, juce::uint64 objectRegionId) const
{
    for (const auto& p : previews)
        // regionId の一致は**非0のときだけ**見る — loops パーツの regionId は常に0で、
        // 空MIDIトラックのダブルクリック等が (trackId, 0) を渡すと誤ってヒットしてしまう
        if (p.active && ((p.regionId != 0 && objectRegionId == p.regionId)
                         || (p.autoCreatedTrack && objectTrackId == p.trackId)))
            return true;
    return false;
}

bool GachaSession::isPreviewClip (juce::uint64 objectTrackId, const juce::String& fileName) const
{
    const auto& p = previews[(size_t) Part::loops];
    return p.active && objectTrackId == p.trackId
        && fileName.isNotEmpty() && fileName == p.clipFileName;
}

bool GachaSession::trackIsPreviewOwned (juce::uint64 objectTrackId) const
{
    for (const auto& p : previews)
        if (p.active && p.autoCreatedTrack && objectTrackId == p.trackId)
            return true;
    return false;
}

void GachaSession::setPreviewSource (Part part, PreviewSource source)
{
    if (previews[(size_t) part].active)
        sources[(size_t) part] = std::move (source);
}

bool GachaSession::previewCandidate (Part part, Project& project, const MidiImport::Result& parsed,
                                     int preferredTrackIndex, juce::int64 startPpq)
{
    const bool isDrums = part == Part::drums;
    const auto& notes = isDrums ? parsed.drumNotes : parsed.otherNotes;
    const auto lengthPpq = isDrums ? parsed.drumRegionLengthPpq : parsed.otherRegionLengthPpq;
    if (notes.empty())
        return false;

    auto& preview = previews[(size_t) part];
    if (! preview.active)
    {
        // セッション初回だけ baseline を保存（後から始めたパーツの before に未確定の
        // 他パーツが混ざらないよう、パーツごとには持たない）。
        // TrackParams・オーディオバッファは shared_ptr 共有なのでコピーは安価
        if (! baselineValid)
        {
            beforeTracks = project.tracks;
            beforeBpm = project.bpm;
            beforeKey = project.key;
            beforeAnchor = project.loopAnchor;
            baselineValid = true;
        }
        preview.startPpq = juce::jmax ((juce::int64) 0, startPpq);

        const auto preferredUsable = [&]() -> bool
        {
            if (preferredTrackIndex < 0 || preferredTrackIndex >= (int) project.tracks.size())
                return false;
            const auto& track = project.tracks[(size_t) preferredTrackIndex];
            if (track.type != TrackType::midi)
                return false;
            if (isDrums)
                return track.drums;
            // bass: GM ベース系（program 32..39 = GM のベースファミリー）だけを流用する。
            // それ以外の GM トラック（Keys 等）へ黙って上書き配置しない
            return ! track.drums && track.instrument == InstrumentKind::gm
                   && track.gmProgram >= 32 && track.gmProgram <= 39;
        }();

        if (preferredUsable)
        {
            preview.trackId = project.tracks[(size_t) preferredTrackIndex].id;
            preview.autoCreatedTrack = false;
        }
        else
        {
            Track track;
            track.id = project.allocateId();
            track.type = TrackType::midi;
            track.instrument = InstrumentKind::gm;
            track.drums = isDrums;
            track.drumPitch = -1;
            track.gmProgram = isDrums ? 0 : 33; // bass は Finger Bass (33) 既定
            track.name = isDrums ? "Drums" : "Bass";
            preview.trackId = track.id;
            project.tracks.push_back (std::move (track));
            preview.autoCreatedTrack = true;
        }
        preview.active = true;
    }

    const int trackIndex = findTrack (project, preview.trackId);
    if (trackIndex < 0)
    {
        // 対象トラックが消えている＝入口の撤去漏れ。このパーツの状態だけ畳んで失敗を返す（触らない）。
        // セッションが終わるなら BPM・キー・アンカーは baseline へ戻す（失敗経路でも
        // 候補値を取り残さない — 通常のキャンセルと同じ規則）
        resetPart (part);
        if (! hasPreview())
        {
            restoreProjectValues (project);
            resetSession();
        }
        return false;
    }
    auto& regions = project.tracks[(size_t) trackIndex].midiRegions;

    // 差し替え: 前候補の仮リージョンを取り除いてから同じ場所に置く
    if (preview.regionId != 0)
        regions.erase (std::remove_if (regions.begin(), regions.end(),
                                       [&preview] (const MidiRegion& r)
                                       { return r.id == preview.regionId; }),
                       regions.end());

    MidiRegion region;
    region.id = project.allocateId();
    region.startPpq = preview.startPpq;
    region.lengthPpq = juce::jmax ((juce::int64) 1, lengthPpq);
    region.notes = notes;
    for (auto& note : region.notes)
    {
        note.id = project.allocateId();
        region.clampNote (note);
    }
    preview.regionId = region.id;
    regions.push_back (std::move (region));
    return true;
}

bool GachaSession::previewLoopCandidate (Project& project, const LoopPreviewInput& input,
                                         int preferredTrackIndex)
{
    if (input.audio == nullptr || input.audio->getNumSamples() <= 0 || ! input.anchor.isValid())
        return false;

    auto& preview = previews[(size_t) Part::loops];
    if (! preview.active)
    {
        if (! baselineValid)
        {
            beforeTracks = project.tracks;
            beforeBpm = project.bpm;
            beforeKey = project.key;
            beforeAnchor = project.loopAnchor;
            baselineValid = true;
        }
        preview.clipFileName = loopPreviewMarker;

        // 対象トラック: preferred がオーディオトラックならそれ、無ければ「Loops」を自動作成
        // （drums/bass の Drum Kit / GM ベース系の流用と同じ規則の音声版）
        const bool preferredUsable = preferredTrackIndex >= 0
                                  && preferredTrackIndex < (int) project.tracks.size()
                                  && project.tracks[(size_t) preferredTrackIndex].type == TrackType::audio;
        if (preferredUsable)
        {
            preview.trackId = project.tracks[(size_t) preferredTrackIndex].id;
        }
        else
        {
            Track track;
            track.id = project.allocateId();
            track.type = TrackType::audio;
            track.name = "Loops";
            preview.trackId = track.id;
            project.tracks.push_back (std::move (track));
            preview.autoCreatedTrack = true;
        }
        preview.active = true;
    }

    // ベース撤去の判定と実行は Loops トラックの index 取得より**前**に行う
    // （自動作成 Bass トラックの削除で index がずれ、Bass→Loops の順で仮配置していると
    // 範囲外アクセスになるため）。撤去条件は2系統:
    // - 逆コピーで BPM/キーが実際に変わる
    // - **進行（roots）そのものが変わる** — ベースが従う本体はルート列なので、
    //   キー/BPM が同じでも進行の違うループに差し替えたら古いベースは嘘になる
    //   （applyKeyBpm=false の「敷くだけ」でも同様）。ドラムは音程が無いので維持
    if (previews[(size_t) Part::bass].active)
    {
        const bool valuesChange = input.applyKeyBpm
            && (! juce::approximatelyEqual (project.bpm, input.anchor.bpm)
                || project.key != std::optional<ProjectKey> (input.anchor.key));
        const bool progressionChange = ! project.loopAnchor.has_value()
            || project.loopAnchor->roots != input.anchor.roots
            || project.loopAnchor->slotsPerBar != input.anchor.slotsPerBar
            || project.loopAnchor->loopBars != input.anchor.loopBars;
        if (valuesChange || progressionChange)
            cancelPart (Part::bass, project);
    }

    const int trackIndex = findTrack (project, preview.trackId);
    if (trackIndex < 0)
    {
        // 対象トラックが消えている＝入口の撤去漏れ（MIDI パーツの失敗経路と同じ規則）
        resetPart (Part::loops);
        if (! hasPreview())
        {
            restoreProjectValues (project);
            resetSession();
        }
        return false;
    }

    // 配置位置は**毎回** input の値を使う（MIDI パーツの「初回位置を維持」と違い、
    // BPM が変わると同じ絶対サンプルは小節頭でなくなる — 呼び出し側が新 BPM で
    // 小節頭を換算し直して渡す契約）
    preview.startSample = juce::jmax ((juce::int64) 0, input.startSample);

    // 差し替え: 前候補の仮クリップを取り除いてから置き直す
    auto& clips = project.tracks[(size_t) trackIndex].clips;
    clips.erase (std::remove_if (clips.begin(), clips.end(),
                                 [&preview] (const Clip& c)
                                 { return c.fileName == preview.clipFileName; }),
                 clips.end());

    Clip clip;
    clip.fileName = preview.clipFileName; // マーカー（保存は仮配置を先に撤去するので書かれない）
    clip.name = input.displayName;
    clip.loopSource = input.anchor.libraryPath;
    clip.startSample = preview.startSample;
    clip.audio = input.audio;
    clip.lengthSamples = input.audio->getNumSamples();
    clip.loopCount = juce::jlimit (0, maxLoopCount, input.loopCount);
    clip.buildPeakCache();
    clips.push_back (std::move (clip));

    // アンカーは採用のたびに更新。逆コピー（BPM・キー）はダイアログの選択次第
    project.loopAnchor = input.anchor;
    if (input.applyKeyBpm)
    {
        project.bpm = input.anchor.bpm;
        project.key = input.anchor.key;
    }
    return true;
}

// ループ仮クリップの実体化: プロジェクトSR変換済みのバッファを 24bit WAV（clip-NNN.wav）として
// 書き出し、fileName をマーカーから実名へ差し替える（出力形式は取り込み・録音と同じ）
bool GachaSession::materializeLoopClip (Project& project)
{
    auto& preview = previews[(size_t) Part::loops];
    const int trackIndex = findTrack (project, preview.trackId);
    if (trackIndex < 0)
        return false;
    auto& clips = project.tracks[(size_t) trackIndex].clips;
    const auto it = std::find_if (clips.begin(), clips.end(),
                                  [&preview] (const Clip& c)
                                  { return c.fileName == preview.clipFileName; });
    if (it == clips.end() || it->audio == nullptr)
        return false;
    if (project.sampleRate <= 0.0)
        return false; // SR 未確定のプロジェクトに音声は書けない（呼び出し側が先に確定させる契約）

    project.directory.createDirectory();
    const auto dest = project.nextClipFile();
    std::unique_ptr<juce::OutputStream> stream { dest.createOutputStream() };
    if (stream == nullptr)
        return false;
    juce::WavAudioFormat wav;
    using Opts = juce::AudioFormatWriterOptions;
    auto writer = wav.createWriterFor (stream,
                                       Opts {}.withSampleRate (project.sampleRate)
                                              .withNumChannels (it->audio->getNumChannels())
                                              .withBitsPerSample (24));
    if (writer == nullptr)
    {
        // writer 生成失敗時は stream の所有権が移っていない。閉じてから空ファイルを消す
        // （残すと次回の clip-NNN 採番が使用済み扱いで飛ぶ）
        stream.reset();
        dest.deleteFile();
        return false;
    }
    if (! writer->writeFromAudioSampleBuffer (*it->audio, 0, it->audio->getNumSamples()))
    {
        writer.reset();
        dest.deleteFile();
        return false;
    }
    writer.reset(); // flush してから fileName を差し替える
    it->fileName = dest.getFileName();
    preview.clipFileName = it->fileName;
    return true;
}

bool GachaSession::removePartObjects (Part part, Project& project)
{
    auto& preview = previews[(size_t) part];
    if (! preview.active)
        return false;

    bool removed = false;
    const int trackIndex = findTrack (project, preview.trackId);
    if (trackIndex >= 0)
    {
        if (preview.autoCreatedTrack)
        {
            // このセッションで作ったトラックはトラックごと撤去
            project.tracks.erase (project.tracks.begin() + trackIndex);
            removed = true;
        }
        else if (part == Part::loops)
        {
            auto& clips = project.tracks[(size_t) trackIndex].clips;
            const auto before = clips.size();
            clips.erase (std::remove_if (clips.begin(), clips.end(),
                                         [&preview] (const Clip& c)
                                         { return c.fileName == preview.clipFileName; }),
                         clips.end());
            removed = clips.size() != before;
        }
        else
        {
            auto& regions = project.tracks[(size_t) trackIndex].midiRegions;
            const auto before = regions.size();
            regions.erase (std::remove_if (regions.begin(), regions.end(),
                                           [&preview] (const MidiRegion& r)
                                           { return r.id == preview.regionId; }),
                           regions.end());
            removed = regions.size() != before;
        }
    }
    return removed;
}

void GachaSession::resetPart (Part part)
{
    previews[(size_t) part] = {};
    sources[(size_t) part] = {};
}

void GachaSession::resetSession()
{
    baselineValid = false;
    beforeTracks.clear();
    beforeBpm = 120.0;
    beforeKey.reset();
    beforeAnchor.reset();
}

bool GachaSession::restoreProjectValues (Project& project)
{
    if (! baselineValid)
        return false;
    const bool changed = ! juce::approximatelyEqual (project.bpm, beforeBpm)
                      || project.key != beforeKey
                      || project.loopAnchor.has_value() != beforeAnchor.has_value()
                      || (project.loopAnchor.has_value() && beforeAnchor.has_value()
                          && project.loopAnchor->libraryPath != beforeAnchor->libraryPath);
    project.bpm = beforeBpm;
    project.key = beforeKey;
    project.loopAnchor = beforeAnchor;
    return changed;
}

bool GachaSession::cancelPart (Part part, Project& project)
{
    if (! previews[(size_t) part].active)
        return false;
    bool removed = removePartObjects (part, project);
    resetPart (part);
    if (part == Part::loops)
    {
        // ループ採用の取り消しは、他パーツが残っていても**値（BPM・キー・アンカー）を戻す**
        // — 残したままだと Drums の Keep が「アンカー無しなのに候補ループの BPM」を確定させる。
        // 進行を追従していたベースも根拠を失うので連動撤去する（ドラムは音程が無いので維持）
        if (previews[(size_t) Part::bass].active)
        {
            removed = removePartObjects (Part::bass, project) || removed;
            resetPart (Part::bass);
        }
        removed = restoreProjectValues (project) || removed;
    }
    if (! hasPreview())
    {
        restoreProjectValues (project); // BPM・キー・アンカーを仮配置前へ（ループ採用の巻き戻し）
        resetSession(); // 最後の仮配置が消えた → baseline を破棄
    }
    return removed;
}

bool GachaSession::cancelPreview (Project& project)
{
    bool removed = false;
    for (int i = 0; i < numParts; ++i)
    {
        const auto part = (Part) i;
        if (previews[(size_t) i].active)
        {
            removed = removePartObjects (part, project) || removed;
            resetPart (part);
        }
    }
    removed = restoreProjectValues (project) || removed;
    resetSession();
    return removed;
}

bool GachaSession::keep (Project& project, UndoStack& undoStack)
{
    if (! hasPreview())
        return false;

    // ループ仮クリップを先に実体化する（マーカー → clip-NNN.wav の 24bit WAV 書き出し）。
    // 失敗したら**セッション全体をキャンセル**する — keep は全パーツ一括のトランザクション
    // なので、ループだけ欠けた部分確定（ループ由来のベースが根拠を失ったまま確定される等）を
    // 作らない。呼び出し側は false を受けて transport / バッジを同期する
    if (previews[(size_t) Part::loops].active && ! materializeLoopClip (project))
    {
        cancelPreview (project);
        return false;
    }

    // 仮オブジェクトが1つも実在しない（入口の撤去漏れ等）なら確定するものが無い。
    // 一部だけ実在するケースは実在分を確定する（消えた側は単に無い）
    bool anyExists = false;
    for (int i = 0; i < numParts; ++i)
    {
        const auto& preview = previews[(size_t) i];
        if (! preview.active)
            continue;
        const int trackIndex = findTrack (project, preview.trackId);
        if (trackIndex < 0)
            continue;
        const auto& track = project.tracks[(size_t) trackIndex];
        if ((Part) i == Part::loops)
            for (const auto& clip : track.clips)
                anyExists = anyExists || clip.fileName == preview.clipFileName;
        else
            for (const auto& region : track.midiRegions)
                anyExists = anyExists || region.id == preview.regionId;
    }
    if (! anyExists)
    {
        for (int i = 0; i < numParts; ++i)
            resetPart ((Part) i);
        restoreProjectValues (project); // 確定するものが無い＝キャンセル相当。候補値を取り残さない
        resetSession();
        return false;
    }

    // before は baseline の値（現在値ではない）。ループ採用で BPM/キー/アンカーが
    // 仮配置中に変わっていても、⌘Z 1回で仮配置前へ丸ごと戻る
    undoStack.pushCommitted (std::move (beforeTracks), project, beforeBpm, beforeKey, beforeAnchor);
    for (int i = 0; i < numParts; ++i)
        resetPart ((Part) i);
    resetSession();
    return true;
}
