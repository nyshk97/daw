#include "Project.h"

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

bool Project::save (juce::String& error)
{
    directory.createDirectory();

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", currentVersion);
    root->setProperty ("bpm", bpm);
    root->setProperty ("sampleRate", sampleRate);

    juce::Array<juce::var> tracksArray;
    for (auto& track : tracks)
    {
        auto* trackObj = new juce::DynamicObject();
        trackObj->setProperty ("type", "audio"); // 将来のMIDIトラック追加のためのスキーマ拡張余地
        trackObj->setProperty ("name", track.name);
        trackObj->setProperty ("mute", track.params->mute.load());
        trackObj->setProperty ("solo", track.params->solo.load());
        trackObj->setProperty ("volume", (double) track.params->gain.load());

        juce::Array<juce::var> clipsArray;
        for (auto& clip : track.clips)
        {
            auto* clipObj = new juce::DynamicObject();
            clipObj->setProperty ("file", clip.fileName);
            clipObj->setProperty ("startSample", clip.startSample);
            clipsArray.add (juce::var (clipObj));
        }
        trackObj->setProperty ("clips", clipsArray);
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
    // （未保存のまま終了→再読込しても欠損しないようにするため）
    juce::StringArray referenced;
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

    project->bpm = (double) parsed.getProperty ("bpm", 120.0);
    project->sampleRate = (double) parsed.getProperty ("sampleRate", 0.0);

    if (auto* tracksArray = parsed.getProperty ("tracks", {}).getArray())
    {
        for (auto& trackVar : *tracksArray)
        {
            Track track;
            track.name = trackVar.getProperty ("name", jp (u8"トラック")).toString();
            track.params->mute.store ((bool) trackVar.getProperty ("mute", false));
            track.params->solo.store ((bool) trackVar.getProperty ("solo", false));
            track.params->gain.store ((float) (double) trackVar.getProperty ("volume", 0.8));

            if (auto* clipsArray = trackVar.getProperty ("clips", {}).getArray())
            {
                for (auto& clipVar : *clipsArray)
                {
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
            project->tracks.push_back (std::move (track));
        }
    }

    return project;
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
        trackPlayback.params = track.params;
        for (auto& clip : track.clips)
            if (clip.audio != nullptr)
                trackPlayback.clips.push_back ({ clip.audio, clip.startSample });
        snapshot->tracks.push_back (std::move (trackPlayback));
    }

    return snapshot;
}
