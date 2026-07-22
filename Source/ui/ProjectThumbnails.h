#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../shared/Ppq.h"

// プロジェクト選択画面の行に出すオーバービュー（ミニ波形＋メタ情報）
struct ProjectOverview
{
    std::vector<float> peaks;   // タイムライン全長のピーク（空 = 音声クリップなし）
    double bpm = 0;             // 0 = 未着（project.jsonが読めなかった場合も0のまま）
    int numTracks = 0;
    double lengthSeconds = 0;   // 音声クリップとMIDIリージョンの終端の遅い方
};

// オーバービューを非同期に用意する。project.json を軽量パース（Project::loadと違い
// WAVをメモリに常駐させない）して音声クリップの配置を取り、WAVを逐次読みしながら
// タイムライン全長を numBins 個のピークへ集約する。結果は ~/Library/Caches/daw/overviews/
// にキャッシュし、project.json のmtimeが変わるまで再利用する。
//
// スレッド構成: request()/onLoaded はメッセージスレッド、計算はThreadPool。
// 結果は MessageManager::callAsync で戻す。onLoaded にはSafePointer入りのラムダを
// 入れること（ジョブがコピーを持つため、本体・呼び出し側の破棄後も安全に空振りする）
class ProjectThumbnailLoader
{
public:
    static constexpr int numBins = 240;

    std::function<void (const juce::File& projectDir, ProjectOverview overview)> onLoaded;

    ~ProjectThumbnailLoader()
    {
        *aborted = true;
        pool.removeAllJobs (true, 5000);
    }

    // 同じ (dir, mtime) の依頼は一度だけ実行する（フォーカス復帰の再読込で重複依頼されるため）
    void request (const juce::File& projectDir, juce::Time projectJsonMtime)
    {
        const auto mtimeMs = projectJsonMtime.toMilliseconds();
        const auto key = projectDir.getFullPathName() + "|" + juce::String (mtimeMs);
        if (! requested.insert (key).second)
            return;

        pool.addJob ([callback = onLoaded, abortFlag = aborted, dir = projectDir, mtimeMs]
        {
            auto overview = loadOrCompute (dir, mtimeMs, *abortFlag);
            if (*abortFlag || callback == nullptr)
                return;
            juce::MessageManager::callAsync (
                [callback, dir, overview = std::move (overview)] { callback (dir, overview); });
        });
    }

private:
    juce::ThreadPool pool { 2 };
    std::set<juce::String> requested; // メッセージスレッドからのみ触る
    std::shared_ptr<std::atomic<bool>> aborted = std::make_shared<std::atomic<bool>> (false);

    static juce::File cacheFileFor (const juce::File& projectDir)
    {
        const auto hash = juce::String::toHexString (projectDir.getFullPathName().hashCode64());
        return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
            .getChildFile ("Library/Caches/daw/overviews")
            .getChildFile (hash + ".bin");
    }

    // キャッシュ形式: int32 version / int64 sourceMtimeMs / double bpm / int32 numTracks /
    //                double lengthSeconds / int32 count / float×count
    static constexpr juce::int32 cacheVersion = 2;

    static ProjectOverview loadOrCompute (const juce::File& dir, juce::int64 mtimeMs,
                                          std::atomic<bool>& abortFlag)
    {
        const auto cacheFile = cacheFileFor (dir);
        bool cacheValid = false;
        auto cached = readCache (cacheFile, mtimeMs, cacheValid);
        if (cacheValid)
            return cached;

        auto overview = compute (dir, abortFlag);
        if (! abortFlag)
            writeCache (cacheFile, mtimeMs, overview);
        return overview;
    }

    static ProjectOverview readCache (const juce::File& file, juce::int64 expectedMtimeMs,
                                      bool& valid)
    {
        valid = false;
        ProjectOverview overview;
        juce::FileInputStream in (file);
        if (! in.openedOk())
            return overview;
        if (in.readInt() != cacheVersion || in.readInt64() != expectedMtimeMs)
            return overview;
        overview.bpm = in.readDouble();
        overview.numTracks = in.readInt();
        overview.lengthSeconds = in.readDouble();
        const int count = in.readInt();
        if (count < 0 || count > numBins)
            return overview;
        overview.peaks.resize ((size_t) count);
        if (count > 0 && in.read (overview.peaks.data(), count * (int) sizeof (float))
                             != count * (int) sizeof (float))
        {
            overview.peaks.clear();
            return overview;
        }
        valid = true;
        return overview;
    }

    static void writeCache (const juce::File& file, juce::int64 mtimeMs,
                            const ProjectOverview& overview)
    {
        file.getParentDirectory().createDirectory();
        juce::FileOutputStream out (file);
        if (! out.openedOk())
            return;
        out.setPosition (0);
        out.truncate();
        out.writeInt (cacheVersion);
        out.writeInt64 (mtimeMs);
        out.writeDouble (overview.bpm);
        out.writeInt (overview.numTracks);
        out.writeDouble (overview.lengthSeconds);
        out.writeInt ((int) overview.peaks.size());
        out.write (overview.peaks.data(), overview.peaks.size() * sizeof (float));
    }

    static ProjectOverview compute (const juce::File& dir, std::atomic<bool>& abortFlag)
    {
        // project.json から音声クリップの配置とメタ情報だけ拾う（旧バージョンのJSONは
        // type省略=audio・offset/length省略=WAV全長として扱う）
        struct ClipRef
        {
            std::unique_ptr<juce::AudioFormatReader> reader;
            juce::int64 start = 0, offset = 0, length = 0;
        };

        ProjectOverview overview;
        const auto root = juce::JSON::parse (dir.getChildFile ("project.json"));
        const auto* tracks = root.getProperty ("tracks", {}).getArray();
        if (tracks == nullptr)
            return overview;

        overview.bpm = (double) root.getProperty ("bpm", 0.0);
        overview.numTracks = tracks->size();
        double sampleRate = (double) root.getProperty ("sampleRate", 0.0);

        juce::WavAudioFormat wavFormat;
        std::vector<ClipRef> clips;
        juce::int64 maxPpq = 0;
        for (const auto& track : *tracks)
        {
            if (track.getProperty ("type", "audio").toString() != "audio")
            {
                if (const auto* regions = track.getProperty ("regions", {}).getArray())
                    for (const auto& r : *regions)
                        maxPpq = juce::jmax (maxPpq,
                                             (juce::int64) r.getProperty ("startPpq", 0)
                                                 + (juce::int64) r.getProperty ("lengthPpq", 0));
                continue;
            }

            const auto* clipsArray = track.getProperty ("clips", {}).getArray();
            if (clipsArray == nullptr)
                continue;

            for (const auto& c : *clipsArray)
            {
                if ((bool) c.getProperty ("muted", false))
                    continue;
                const auto wav = dir.getChildFile (c.getProperty ("file", "").toString());
                auto stream = std::make_unique<juce::FileInputStream> (wav);
                if (! stream->openedOk())
                    continue;
                std::unique_ptr<juce::AudioFormatReader> reader (
                    wavFormat.createReaderFor (stream.release(), true));
                if (reader == nullptr)
                    continue;

                ClipRef ref;
                ref.start = (juce::int64) c.getProperty ("startSample", 0);
                ref.offset = (juce::int64) c.getProperty ("offsetSamples", 0);
                const auto available = reader->lengthInSamples - ref.offset;
                const auto declared = (juce::int64) c.getProperty ("lengthSamples", 0);
                ref.length = declared > 0 ? juce::jmin (declared, available) : available;
                if (ref.length <= 0)
                    continue;
                if (sampleRate <= 0)
                    sampleRate = reader->sampleRate;
                ref.reader = std::move (reader);
                clips.push_back (std::move (ref));
            }
        }

        juce::int64 totalSamples = 0;
        for (const auto& c : clips)
            totalSamples = juce::jmax (totalSamples, c.start + c.length);

        // 曲長 = 音声クリップとMIDIリージョンの終端の遅い方
        const double audioSeconds = sampleRate > 0 ? (double) totalSamples / sampleRate : 0.0;
        const double midiSeconds = overview.bpm > 0
            ? ((double) maxPpq / (double) Ppq::ticksPerQuarter) * 60.0 / overview.bpm
            : 0.0;
        overview.lengthSeconds = juce::jmax (audioSeconds, midiSeconds);

        if (clips.empty())
            return overview;

        overview.peaks.assign ((size_t) numBins, 0.0f);
        constexpr int chunkSize = 1 << 16;
        std::vector<float> buffer ((size_t) chunkSize);
        float* bufferPtr = buffer.data();

        for (auto& c : clips)
        {
            for (juce::int64 pos = 0; pos < c.length; pos += chunkSize)
            {
                if (abortFlag)
                    return overview;
                const int n = (int) juce::jmin ((juce::int64) chunkSize, c.length - pos);
                if (! c.reader->read (&bufferPtr, 1, c.offset + pos, n))
                    break;
                for (int i = 0; i < n; ++i)
                {
                    const auto bin = juce::jmin (
                        (size_t) ((c.start + pos + i) * numBins / totalSamples),
                        (size_t) numBins - 1);
                    overview.peaks[bin] = juce::jmax (overview.peaks[bin],
                                                      std::abs (bufferPtr[i]));
                }
            }
        }

        // 見た目の正規化: 最大値を1に合わせ、0.7乗で持ち上げて小音量部も形が見えるようにする
        float maxPeak = 0.0f;
        for (auto p : overview.peaks)
            maxPeak = juce::jmax (maxPeak, p);
        if (maxPeak > 0.0f)
            for (auto& p : overview.peaks)
                p = std::pow (p / maxPeak, 0.7f);
        return overview;
    }
};
