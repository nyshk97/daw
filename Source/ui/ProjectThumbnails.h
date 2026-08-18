#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../shared/Ppq.h"
#include "../shared/Project.h" // maxLoopCount（クランプ上限をモデル側と共有する）

// 1反復ぶんに集約したローカルビン（クリップ内を numBins 等分したピーク）を、ループ反復ぶん
// グローバルビン（曲全体を numBins 等分）へ転写する。
//
// ループ回数ぶん元サンプルを走査し直すと O(サンプル数 × 回数) になり、上限回数では
// 48kHz・1分のクリップで数秒かかってサムネイル用ワーカーを占有する（実測 8.7秒 → 9.5ms）。
// 「集約は1回・転写は回数ぶん」に分けて O(サンプル数 + 回数 × numBins) に保つのが要点
inline void spreadLoopedBins (const std::vector<float>& local, int numBins,
                              juce::int64 clipStart, juce::int64 clipLength, int reps,
                              juce::int64 totalSamples, std::vector<float>& out,
                              const std::atomic<bool>* abortFlag = nullptr)
{
    if (clipLength <= 0 || totalSamples <= 0 || numBins <= 0)
        return;
    // 集約側は floor(sample * numBins / clipLength) でローカルビンを決めているので、
    // 逆算（ビン → サンプル範囲）は切り上げになる。両端とも切り捨てると1サンプル手前へずれ、
    // クリップが短い（clipLength が numBins に近い/下回る）ほどグローバルビンを跨いで見える
    const auto ceilDiv = [] (juce::int64 a, juce::int64 b) { return (a + b - 1) / b; };
    for (int r = 0; r < reps; ++r)
    {
        if (abortFlag != nullptr && abortFlag->load())
            return;
        const auto repStart = clipStart + (juce::int64) r * clipLength;
        for (int k = 0; k < numBins && k < (int) local.size(); ++k)
        {
            const float peak = local[(size_t) k];
            if (peak <= 0.0f)
                continue;
            // ローカルビンkが覆うサンプル範囲を、それが跨るグローバルビンすべてへ広げる
            // （通常ローカルの方が細かいので1個。クリップが全長を占めるときだけ複数になる）
            const auto s0 = repStart + ceilDiv ((juce::int64) k * clipLength, numBins);
            const auto s1 = repStart + juce::jmax ((juce::int64) 0,
                                                   ceilDiv ((juce::int64) (k + 1) * clipLength, numBins) - 1);
            const auto b0 = (size_t) juce::jmin ((juce::int64) numBins - 1, s0 * numBins / totalSamples);
            const auto b1 = (size_t) juce::jmin ((juce::int64) numBins - 1,
                                                 juce::jmax (s0, s1) * numBins / totalSamples);
            for (size_t b = b0; b <= b1 && b < out.size(); ++b)
                out[b] = juce::jmax (out[b], peak);
        }
    }
}

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
    // v3: ループ（loopCount）を曲長・ミニ波形に反映。計算結果が変わるので古いキャッシュを捨てる
    // （mtimeでも大半は無効化されるが、ループ入りで保存済みのプロジェクトを旧版で1度でも
    //   サムネイル生成していると mtime が同じまま古い結果を返してしまうため）
    static constexpr juce::int32 cacheVersion = 3;

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
            // タイムライン上の見かけ長（v20 の stretchRatio を掛けた長さ。無加工なら length と同じ）。
            // 波形はソース範囲から正規化ビンで集約するので、読み出しは length・配置はこちらを使う
            juce::int64 displayLength = 0;
            int reps = 1; // ループを含む再生回数（1 = ループなし）。同じソース範囲を繰り返す
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
                    {
                        // ループ回数ぶん伸ばす（欠損＝ループなし。v8以前のJSONもここを通る）
                        const auto reps = (juce::int64) (1 + juce::jlimit (0, maxLoopCount,
                                                                           (int) r.getProperty ("loopCount", 0)));
                        maxPpq = juce::jmax (maxPpq,
                                             (juce::int64) r.getProperty ("startPpq", 0)
                                                 + (juce::int64) r.getProperty ("lengthPpq", 0) * reps);
                    }
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
                const double stretchRatio = ClipStretchLimits::clampRatio (
                    (double) c.getProperty ("stretchRatio", 1.0));
                ref.displayLength = juce::jmax ((juce::int64) 1,
                                                (juce::int64) std::llround ((double) ref.length
                                                                            * stretchRatio));
                ref.reps = 1 + juce::jlimit (0, maxLoopCount, (int) c.getProperty ("loopCount", 0));
                if (sampleRate <= 0)
                    sampleRate = reader->sampleRate;
                ref.reader = std::move (reader);
                clips.push_back (std::move (ref));
            }
        }

        juce::int64 totalSamples = 0;
        for (const auto& c : clips)
            totalSamples = juce::jmax (totalSamples, c.start + c.displayLength * c.reps);

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

        // ループは同じソース範囲の繰り返しなので、まず1反復ぶんを numBins 個のローカルビンへ
        // 集約し、そのあと各反復のグローバルビンへ転写する。サンプルごとに反復数だけ回すと
        // O(サンプル数 × 回数) になり、回数が大きいとワーカーを長時間占有してしまう
        // （ここは O(サンプル数 + 回数 × numBins)）
        std::vector<float> localPeaks ((size_t) numBins);
        for (auto& c : clips)
        {
            localPeaks.assign ((size_t) numBins, 0.0f);
            for (juce::int64 pos = 0; pos < c.length; pos += chunkSize)
            {
                if (abortFlag)
                    return overview;
                const int n = (int) juce::jmin ((juce::int64) chunkSize, c.length - pos);
                if (! c.reader->read (&bufferPtr, 1, c.offset + pos, n))
                    break;
                for (int i = 0; i < n; ++i)
                {
                    const auto local = (size_t) juce::jmin ((juce::int64) numBins - 1,
                                                            (pos + i) * numBins / c.length);
                    localPeaks[local] = juce::jmax (localPeaks[local], std::abs (bufferPtr[i]));
                }
            }

            spreadLoopedBins (localPeaks, numBins, c.start, c.displayLength, c.reps, totalSamples,
                              overview.peaks, &abortFlag);
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
