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
        if (p.active && (objectRegionId == p.regionId
                         || (p.autoCreatedTrack && objectTrackId == p.trackId)))
            return true;
    return false;
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
    const bool removed = removePartObjects (part, project);
    resetPart (part);
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

    // 仮リージョンが1つも実在しない（入口の撤去漏れ等）なら確定するものが無い。
    // 一部だけ実在するケースは実在分を確定する（消えた側は単に無い）
    bool anyExists = false;
    for (const auto& preview : previews)
    {
        if (! preview.active)
            continue;
        const int trackIndex = findTrack (project, preview.trackId);
        if (trackIndex < 0)
            continue;
        for (const auto& region : project.tracks[(size_t) trackIndex].midiRegions)
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
