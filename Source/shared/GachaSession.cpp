#include "GachaSession.h"

#include <algorithm>

bool GachaSession::parsePorcelainLine (const juce::String& line, Candidate& out)
{
    const auto parsed = juce::JSON::parse (line);
    if (! parsed.isObject())
        return false;
    const auto base = parsed.getProperty ("base", {});
    const auto seeds = parsed.getProperty ("lane_seeds", {});
    const auto status = parsed.getProperty ("status", {});
    if (! base.isString() || ! seeds.isObject() || ! status.isString())
        return false;

    Candidate candidate;
    candidate.base = base.toString();
    candidate.kickSeed = seeds.getProperty ("kick", {}).toString();
    candidate.snareSeed = seeds.getProperty ("snare", {}).toString();
    candidate.hatSeed = seeds.getProperty ("hat", {}).toString();
    candidate.status = status.toString();
    if (candidate.base.isEmpty() || candidate.kickSeed.isEmpty()
        || candidate.snareSeed.isEmpty() || candidate.hatSeed.isEmpty())
        return false;
    out = std::move (candidate);
    return true;
}

int GachaSession::findPreviewTrack (const Project& project) const
{
    for (int i = 0; i < (int) project.tracks.size(); ++i)
        if (project.tracks[(size_t) i].id == trackId)
            return i;
    return -1;
}

bool GachaSession::previewCandidate (Project& project, const MidiImport::Result& parsed,
                                     int preferredTrackIndex, juce::int64 startPpq)
{
    if (parsed.drumNotes.empty())
        return false;

    if (! previewActive)
    {
        // 仮配置開始。「残す」の before はこの時点の tracks
        // （TrackParams・オーディオバッファは shared_ptr 共有なのでコピーは安価）
        beforeTracks = project.tracks;
        previewStartPpq = juce::jmax ((juce::int64) 0, startPpq);

        const bool preferredIsDrumKit =
            preferredTrackIndex >= 0 && preferredTrackIndex < (int) project.tracks.size()
            && project.tracks[(size_t) preferredTrackIndex].type == TrackType::midi
            && project.tracks[(size_t) preferredTrackIndex].drums;
        if (preferredIsDrumKit)
        {
            trackId = project.tracks[(size_t) preferredTrackIndex].id;
            autoCreatedTrack = false;
        }
        else
        {
            Track track;
            track.id = project.allocateId();
            track.type = TrackType::midi;
            track.instrument = InstrumentKind::gm;
            track.drums = true; // Drum Kit
            track.drumPitch = -1;
            track.name = "Drums";
            trackId = track.id;
            project.tracks.push_back (std::move (track));
            autoCreatedTrack = true;
        }
        previewActive = true;
    }

    const int trackIndex = findPreviewTrack (project);
    if (trackIndex < 0)
    {
        // 対象トラックが消えている＝入口の撤去漏れ。状態だけ畳んで失敗を返す（触らない）
        previewActive = false;
        autoCreatedTrack = false;
        beforeTracks.clear();
        return false;
    }
    auto& regions = project.tracks[(size_t) trackIndex].midiRegions;

    // 差し替え: 前候補の仮リージョンを取り除いてから同じ場所に置く
    if (regionId != 0)
        regions.erase (std::remove_if (regions.begin(), regions.end(),
                                       [this] (const MidiRegion& r) { return r.id == regionId; }),
                       regions.end());

    MidiRegion region;
    region.id = project.allocateId();
    region.startPpq = previewStartPpq;
    region.lengthPpq = juce::jmax ((juce::int64) 1, parsed.drumRegionLengthPpq);
    region.notes = parsed.drumNotes;
    for (auto& note : region.notes)
    {
        note.id = project.allocateId();
        region.clampNote (note);
    }
    regionId = region.id;
    regions.push_back (std::move (region));
    return true;
}

bool GachaSession::cancelPreview (Project& project)
{
    if (! previewActive)
        return false;

    bool removed = false;
    const int trackIndex = findPreviewTrack (project);
    if (trackIndex >= 0)
    {
        if (autoCreatedTrack)
        {
            // このセッションで作った「Drums」はトラックごと撤去
            project.tracks.erase (project.tracks.begin() + trackIndex);
            removed = true;
        }
        else
        {
            auto& regions = project.tracks[(size_t) trackIndex].midiRegions;
            const auto before = regions.size();
            regions.erase (std::remove_if (regions.begin(), regions.end(),
                                           [this] (const MidiRegion& r) { return r.id == regionId; }),
                           regions.end());
            removed = regions.size() != before;
        }
    }

    previewActive = false;
    autoCreatedTrack = false;
    trackId = regionId = 0;
    beforeTracks.clear();
    return removed;
}

bool GachaSession::keep (Project& project, UndoStack& undoStack)
{
    if (! previewActive)
        return false;

    // 仮リージョンが（入口の撤去漏れ等で）すでに消えていたら確定するものが無い
    bool regionExists = false;
    const int trackIndex = findPreviewTrack (project);
    if (trackIndex >= 0)
        for (const auto& region : project.tracks[(size_t) trackIndex].midiRegions)
            regionExists = regionExists || region.id == regionId;
    if (! regionExists)
    {
        previewActive = false;
        autoCreatedTrack = false;
        trackId = regionId = 0;
        beforeTracks.clear();
        return false;
    }

    undoStack.pushCommitted (std::move (beforeTracks), project);
    previewActive = false;
    autoCreatedTrack = false;
    trackId = regionId = 0;
    beforeTracks.clear();
    return true;
}
