#include "Project.h"

#include <algorithm>

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }
}

void Clip::buildPeakCache()
{
    peakCache.clear();
    if (audio == nullptr)
        return;

    const int numSamples = audio->getNumSamples();
    const float* data = audio->getReadPointer (0);
    peakCache.reserve ((size_t) (numSamples / samplesPerPeak + 1));

    for (int i = 0; i < numSamples; i += samplesPerPeak)
    {
        const int count = juce::jmin (samplesPerPeak, numSamples - i);
        float peak = 0.0f;
        for (int j = 0; j < count; ++j)
            peak = juce::jmax (peak, std::abs (data[i + j]));
        peakCache.push_back (peak);
    }
}

juce::File Project::projectsRoot()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("daw");
}

std::shared_ptr<juce::AudioBuffer<float>> Project::loadWavMono (const juce::File& file)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return nullptr;

    auto buffer = std::make_shared<juce::AudioBuffer<float>> (1, (int) reader->lengthInSamples);
    reader->read (buffer.get(), 0, (int) reader->lengthInSamples, 0, true, false); // ch0のみ使用
    return buffer;
}

bool Project::save (juce::String& error, const juce::StringArray& keepReferencedWavs)
{
    directory.createDirectory();

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", currentVersion);
    root->setProperty ("nextId", (juce::int64) nextId);
    root->setProperty ("bpm", bpm);
    root->setProperty ("sampleRate", sampleRate);

    juce::Array<juce::var> tracksArray;
    for (auto& track : tracks)
    {
        auto* trackObj = new juce::DynamicObject();
        trackObj->setProperty ("id", (juce::int64) track.id);
        trackObj->setProperty ("type", track.type == TrackType::midi ? "midi" : "audio");
        trackObj->setProperty ("name", track.name);
        trackObj->setProperty ("mute", track.params->mute.load());
        trackObj->setProperty ("solo", track.params->solo.load());
        trackObj->setProperty ("volume", (double) track.params->gain.load());

        if (track.type == TrackType::audio)
        {
            juce::Array<juce::var> clipsArray;
            for (auto& clip : track.clips)
            {
                auto* clipObj = new juce::DynamicObject();
                clipObj->setProperty ("file", clip.fileName);
                clipObj->setProperty ("startSample", clip.startSample);
                clipsArray.add (juce::var (clipObj));
            }
            trackObj->setProperty ("clips", clipsArray);
        }
        else
        {
            trackObj->setProperty ("gmProgram", track.gmProgram);
            trackObj->setProperty ("drums", track.drums);
            trackObj->setProperty ("drumPitch", track.drumPitch);

            juce::Array<juce::var> regionsArray;
            for (auto& region : track.midiRegions)
            {
                auto* regionObj = new juce::DynamicObject();
                regionObj->setProperty ("id", (juce::int64) region.id);
                regionObj->setProperty ("startPpq", region.startPpq);
                regionObj->setProperty ("lengthPpq", region.lengthPpq);

                juce::Array<juce::var> notesArray;
                for (auto& note : region.notes)
                {
                    auto* noteObj = new juce::DynamicObject();
                    noteObj->setProperty ("id", (juce::int64) note.id);
                    noteObj->setProperty ("pitch", note.pitch);
                    noteObj->setProperty ("startPpq", note.startPpq);
                    noteObj->setProperty ("lengthPpq", note.lengthPpq);
                    noteObj->setProperty ("velocity", note.velocity);
                    notesArray.add (juce::var (noteObj));
                }
                regionObj->setProperty ("notes", notesArray);
                regionsArray.add (juce::var (regionObj));
            }
            trackObj->setProperty ("regions", regionsArray);
        }
        tracksArray.add (juce::var (trackObj));
    }
    root->setProperty ("tracks", tracksArray);

    const auto jsonFile = directory.getChildFile ("project.json");
    if (! jsonFile.replaceWithText (juce::JSON::toString (juce::var (root))))
    {
        error = jp (u8"project.json の書き込みに失敗しました");
        return false;
    }

    // どのクリップからも参照されていない録音WAVを掃除する。
    // クリップ削除は「モデルから外すだけ」で、実ファイルの削除は保存時にここでまとめて行う
    // （未保存のまま終了→再読込しても欠損しないようにするため）。
    // keepReferencedWavs（undo/redo履歴が参照するファイル）はredoでの復元に備えて消さない
    juce::StringArray referenced (keepReferencedWavs);
    for (auto& track : tracks)
        for (auto& clip : track.clips)
            referenced.add (clip.fileName);

    for (auto& file : directory.findChildFiles (juce::File::findFiles, false, "clip-*.wav"))
        if (! referenced.contains (file.getFileName()))
            file.deleteFile();

    return true;
}

std::unique_ptr<Project> Project::load (const juce::File& dir,
                                        juce::StringArray& warnings,
                                        juce::String& error)
{
    const auto jsonFile = dir.getChildFile ("project.json");
    const auto parsed = juce::JSON::parse (jsonFile.loadFileAsString());

    if (! parsed.isObject())
    {
        error = jp (u8"project.json を読み込めませんでした: ") + jsonFile.getFullPathName();
        return nullptr;
    }

    auto project = std::make_unique<Project>();
    project->directory = dir;

    const int version = (int) parsed.getProperty ("version", 1);
    if (version > currentVersion)
        warnings.add (jp (u8"より新しいバージョンで保存されたプロジェクトです（一部読み飛ばす可能性があります）"));

    project->bpm = juce::jlimit (30.0, 300.0, (double) parsed.getProperty ("bpm", 120.0));
    project->sampleRate = juce::jmax (0.0, (double) parsed.getProperty ("sampleRate", 0.0));
    project->nextId = (juce::uint64) juce::jmax ((juce::int64) 1, (juce::int64) parsed.getProperty ("nextId", 1));

    if (auto* tracksArray = parsed.getProperty ("tracks", {}).getArray())
    {
        for (auto& trackVar : *tracksArray)
        {
            if (! trackVar.isObject())
                continue;

            Track track;
            track.id = (juce::uint64) juce::jmax ((juce::int64) 0, (juce::int64) trackVar.getProperty ("id", 0));
            track.type = trackVar.getProperty ("type", "audio").toString() == "midi" ? TrackType::midi
                                                                                     : TrackType::audio;
            track.name = trackVar.getProperty ("name", jp (u8"トラック")).toString();
            track.params->mute.store ((bool) trackVar.getProperty ("mute", false));
            track.params->solo.store ((bool) trackVar.getProperty ("solo", false));
            track.params->gain.store (juce::jlimit (0.0f, 1.0f,
                                                    (float) (double) trackVar.getProperty ("volume", 0.8)));

            if (track.type == TrackType::audio)
            {
                if (auto* clipsArray = trackVar.getProperty ("clips", {}).getArray())
                {
                    for (auto& clipVar : *clipsArray)
                    {
                        if (! clipVar.isObject())
                            continue;

                        Clip clip;
                        clip.fileName = clipVar.getProperty ("file", "").toString();
                        clip.startSample = (juce::int64) clipVar.getProperty ("startSample", 0);
                        clip.audio = loadWavMono (dir.getChildFile (clip.fileName));

                        if (clip.audio == nullptr)
                        {
                            warnings.add (clip.fileName + jp (u8" を読み込めないためスキップしました"));
                            continue;
                        }
                        clip.buildPeakCache();
                        track.clips.push_back (std::move (clip));
                    }
                }
            }
            else
            {
                track.gmProgram = juce::jlimit (0, 127, (int) trackVar.getProperty ("gmProgram", 0));
                track.drums = (bool) trackVar.getProperty ("drums", false);
                track.drumPitch = juce::jlimit (-1, 127, (int) trackVar.getProperty ("drumPitch", -1));

                if (auto* regionsArray = trackVar.getProperty ("regions", {}).getArray())
                {
                    for (auto& regionVar : *regionsArray)
                    {
                        if (! regionVar.isObject())
                            continue;

                        MidiRegion region;
                        region.id = (juce::uint64) juce::jmax ((juce::int64) 0,
                                                               (juce::int64) regionVar.getProperty ("id", 0));
                        region.startPpq = juce::jmax ((juce::int64) 0,
                                                      (juce::int64) regionVar.getProperty ("startPpq", 0));
                        region.lengthPpq = juce::jmax ((juce::int64) 1,
                                                       (juce::int64) regionVar.getProperty ("lengthPpq", Ppq::ticksPerBar));

                        if (auto* notesArray = regionVar.getProperty ("notes", {}).getArray())
                        {
                            for (auto& noteVar : *notesArray)
                            {
                                if (! noteVar.isObject())
                                    continue;

                                MidiNote note;
                                note.id = (juce::uint64) juce::jmax ((juce::int64) 0,
                                                                     (juce::int64) noteVar.getProperty ("id", 0));
                                note.pitch = (int) noteVar.getProperty ("pitch", 60);
                                note.startPpq = (juce::int64) noteVar.getProperty ("startPpq", 0);
                                note.lengthPpq = (juce::int64) noteVar.getProperty ("lengthPpq", Ppq::ticksPerQuarter);
                                note.velocity = (int) noteVar.getProperty ("velocity", 100);
                                region.clampNote (note); // 範囲外の値はここで不変条件内に丸める
                                region.notes.push_back (note);
                            }
                        }
                        track.midiRegions.push_back (std::move (region));
                    }
                }
            }
            project->tracks.push_back (std::move (track));
        }
    }

    project->ensureUniqueIds();
    return project;
}

// 未採番(0)・重複のIDを振り直す。v1プロジェクト（ID無し）の移行と、手編集されたJSONへの防御を兼ねる
void Project::ensureUniqueIds()
{
    // まず既存IDの最大値に合わせて採番カウンタを進める（振り直しが既存IDと衝突しないように）
    juce::uint64 maxId = 0;
    for (auto& track : tracks)
    {
        maxId = juce::jmax (maxId, track.id);
        for (auto& region : track.midiRegions)
        {
            maxId = juce::jmax (maxId, region.id);
            for (auto& note : region.notes)
                maxId = juce::jmax (maxId, note.id);
        }
    }
    nextId = juce::jmax (nextId, maxId + 1);

    std::vector<juce::uint64> seen;
    auto assignIfNeeded = [this, &seen] (juce::uint64& id)
    {
        if (id == 0 || std::find (seen.begin(), seen.end(), id) != seen.end())
            id = allocateId();
        seen.push_back (id);
    };

    for (auto& track : tracks)
    {
        assignIfNeeded (track.id);
        for (auto& region : track.midiRegions)
        {
            assignIfNeeded (region.id);
            for (auto& note : region.notes)
                assignIfNeeded (note.id);
        }
    }
}

std::unique_ptr<Project> Project::createNew (const juce::File& dir, juce::String& error)
{
    if (dir.exists())
    {
        error = jp (u8"同名のプロジェクトが既にあります");
        return nullptr;
    }
    if (! dir.createDirectory())
    {
        error = jp (u8"フォルダを作成できませんでした: ") + dir.getFullPathName();
        return nullptr;
    }

    auto project = std::make_unique<Project>();
    project->directory = dir;

    Track track;
    track.id = project->allocateId();
    track.name = jp (u8"トラック 1");
    project->tracks.push_back (std::move (track));

    if (! project->save (error))
        return nullptr;

    return project;
}

juce::File Project::nextClipFile() const
{
    for (int i = 1; i < 10000; ++i)
    {
        auto file = directory.getChildFile (juce::String::formatted ("clip-%03d.wav", i));
        if (! file.existsAsFile())
            return file;
    }
    jassertfalse;
    return directory.getChildFile ("clip-overflow.wav");
}

std::unique_ptr<PlaybackSnapshot> Project::buildSnapshot() const
{
    auto snapshot = std::make_unique<PlaybackSnapshot>();
    snapshot->tracks.reserve (tracks.size());

    for (auto& track : tracks)
    {
        TrackPlayback trackPlayback;
        trackPlayback.trackId = track.id;
        trackPlayback.params = track.params;

        if (track.type == TrackType::audio)
        {
            for (auto& clip : track.clips)
                if (clip.audio != nullptr)
                    trackPlayback.clips.push_back ({ clip.audio, clip.startSample });
        }
        else
        {
            // ノートを絶対PPQへフラット化し、リージョン境界でマスクする。
            // 固定ピッチ打楽器（Kick等）はここでピッチを置き換える。
            // synth の参照は呼び出し側（MainComponent::pushSnapshot）が SynthBank から埋める
            const bool fixedPitch = track.drums && track.drumPitch >= 0;
            for (auto& region : track.midiRegions)
            {
                const auto regionEnd = region.startPpq + region.lengthPpq;
                for (auto& note : region.notes)
                {
                    const auto absStart = region.startPpq + note.startPpq;
                    const auto absEnd = juce::jmin (absStart + note.lengthPpq, regionEnd);
                    if (absEnd <= absStart)
                        continue;
                    trackPlayback.notes.push_back ({ absStart, absEnd,
                                                     fixedPitch ? track.drumPitch : note.pitch,
                                                     note.velocity });
                }
            }
            // stable_sort: 同時刻ノートの順序をリージョン/ノートの並びで決定的にする（イベント上限時の挙動を再現可能に）
            std::stable_sort (trackPlayback.notes.begin(), trackPlayback.notes.end(),
                              [] (const MidiNotePlayback& a, const MidiNotePlayback& b)
                              { return a.startPpq < b.startPpq; });
        }
        snapshot->tracks.push_back (std::move (trackPlayback));
    }

    return snapshot;
}
