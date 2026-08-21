#include "RenderCache.h"

#include <algorithm>

#include "ClipStretcher.h"
#include "VocalResynth.h"
#include "../shared/Log.h"
#include "../shared/Project.h" // Clip::samplesPerPeak（peakCache の集約単位）

RenderCache::~RenderCache()
{
    // 停止要求 → join。「キャッシュとキューを捨てる」だけでは実行中のスレッドが
    // 破棄後の this に触れる（planの寿命管理）。join 後は完了通知も配達されない
    {
        std::lock_guard<std::mutex> lock (mutex);
        stopRequested = true;
        pendingJobs.clear();
    }
    wakeWorker.notify_all();
    if (worker.joinable())
        worker.join();
    cancelPendingUpdate();
    stopTimer();
}

void RenderCache::requestSync()
{
    startTimer (debounceMs); // 走行中なら打ち直し＝値が落ち着いてから積む
}

void RenderCache::syncNow()
{
    stopTimer();
    if (collectRequests == nullptr)
        return;
    auto requests = collectRequests();

    {
        std::lock_guard<std::mutex> lock (mutex);
        // 待機キューは毎回作り直す（集合に含まれる指紋はすべて残し、要求クリップが
        // 0件になった指紋だけが自然に落ちる）。実行中の1件は完走してよい —
        // 結果が不要になっていれば配達時に誰も引き取らないだけ。
        // ただし**実行中・完成済み（未配達）の指紋は積み直さない**: クリップ側は配達まで
        // pending のままなので、素通しすると同じ DSP 処理が二重実行される
        const auto inFlight = [this] (const RenderFingerprint& fingerprint)
        {
            if (jobRunning && runningFingerprint == fingerprint)
                return true;
            for (const auto& item : completed)
                if (item.fingerprint == fingerprint)
                    return true;
            return false;
        };
        requests.erase (std::remove_if (requests.begin(), requests.end(),
                                        [&inFlight] (const Request& r)
                                        { return inFlight (r.fingerprint); }),
                        requests.end());
        pendingJobs = std::move (requests);
        if (! pendingJobs.empty() && ! worker.joinable())
            worker = std::thread ([this] { workerLoop(); });
    }
    wakeWorker.notify_all();
}

std::shared_ptr<const RenderedDomain> RenderCache::lookup (const RenderFingerprint& fingerprint)
{
    const auto found = cache.find (fingerprint);
    if (found == cache.end())
        return nullptr;
    found->second.lastUse = ++useCounter;
    return found->second.domain;
}

void RenderCache::workerLoop()
{
    for (;;)
    {
        Request job;
        {
            std::unique_lock<std::mutex> lock (mutex);
            jobRunning = false;
            wakeWorker.wait (lock, [this] { return stopRequested || ! pendingJobs.empty(); });
            if (stopRequested)
                return;
            job = std::move (pendingJobs.front());
            pendingJobs.erase (pendingJobs.begin());
            jobRunning = true;
            runningFingerprint = job.fingerprint;
        }

        // job.sourceAudio の強参照がレンダリング中の原音の生存を保証する
        std::shared_ptr<const RenderedDomain> domain;
        if (job.sourceAudio != nullptr)
        {
            // 補正付きは WORLD（VocalResynth）、それ以外は signalsmith（ClipStretcher）。
            // どちらも失敗は nullptr（原音でごまかさない）
            std::unique_ptr<juce::AudioBuffer<float>> rendered;
            if (job.recipe != nullptr)
                rendered = VocalResynth::render (*job.recipe);
            else
                rendered = ClipStretcher::render (*job.sourceAudio,
                                                  job.fingerprint.domainOffset,
                                                  job.fingerprint.domainLength,
                                                  job.fingerprint.semitones,
                                                  job.fingerprint.ratio,
                                                  job.fingerprint.sampleRate);
            if (rendered != nullptr)
            {
                auto result = std::make_shared<RenderedDomain>();
                result->peakCache = buildDomainPeakCache (*rendered, 0, rendered->getNumSamples(),
                                                          Clip::samplesPerPeak);
                result->audio = std::shared_ptr<const juce::AudioBuffer<float>> (std::move (rendered));
                result->sourceAudio = job.sourceAudio;
                result->audioBaseOffset = 0; // 加工済みバッファはドメイン先頭から始まる
                result->domainOffset = job.fingerprint.domainOffset;
                result->domainLength = job.fingerprint.domainLength;
                result->semitones = job.fingerprint.semitones;
                result->ratio = job.fingerprint.ratio;
                result->sampleRate = job.fingerprint.sampleRate;
                if (job.recipe != nullptr)
                {
                    result->recipeDigest = job.fingerprint.recipe;
                    result->correction = std::make_shared<const PitchCorrection> (job.recipe->correction);
                    result->timeMap = job.recipe->timeMap();
                }
                else
                    result->timeMap = TimeMap::uniform (job.fingerprint.domainOffset, job.fingerprint.domainLength,
                                                        job.fingerprint.ratio);
                domain = std::move (result);
            }
        }

        {
            std::lock_guard<std::mutex> lock (mutex);
            jobRunning = false;
            // 完了コールバックの配達まで結果の強参照を保持する（weak だと完了〜装着の間に消える）
            completed.push_back ({ job.fingerprint, std::move (domain) });
        }
        triggerAsyncUpdate();
    }
}

void RenderCache::drainCompletedNow()
{
    std::vector<Completed> delivered;
    {
        std::lock_guard<std::mutex> lock (mutex);
        delivered.swap (completed);
    }

    for (auto& item : delivered)
    {
        if (item.domain != nullptr)
        {
            // バイト数はレンダー結果＋強参照で抱える原音（共有元が同じでもエントリごとに
            // 数える = 安全側の過大見積もり）
            juce::int64 bytes = 0;
            if (item.domain->audio != nullptr)
                bytes += (juce::int64) item.domain->audio->getNumSamples()
                       * item.domain->audio->getNumChannels() * (juce::int64) sizeof (float);
            if (item.domain->sourceAudio != nullptr)
                bytes += (juce::int64) item.domain->sourceAudio->getNumSamples()
                       * item.domain->sourceAudio->getNumChannels() * (juce::int64) sizeof (float);

            const auto existing = cache.find (item.fingerprint);
            if (existing == cache.end())
            {
                cache[item.fingerprint] = { item.domain, bytes, ++useCounter };
                cacheBytes += bytes;
                evictOverBudget();
            }
            {
                // 同じ指紋がまだ待機キューに残っていたら落とす（実行中に sync が同じ要求を
                // 積み直したケースの二重レンダリング防止）
                std::lock_guard<std::mutex> lock (mutex);
                pendingJobs.erase (std::remove_if (pendingJobs.begin(), pendingJobs.end(),
                                                   [&item] (const Request& r)
                                                   { return r.fingerprint == item.fingerprint; }),
                                   pendingJobs.end());
            }
            if (onRenderReady != nullptr)
                onRenderReady (item.domain);
        }
        else
        {
            Log::error ("clip_stretch.render_failed",
                        "offset=" + juce::String (item.fingerprint.domainOffset)
                            + " length=" + juce::String (item.fingerprint.domainLength)
                            + " semitones=" + juce::String (item.fingerprint.semitones)
                            + " ratio=" + juce::String (item.fingerprint.ratio, 4));
            if (onRenderFailed != nullptr)
                onRenderFailed (item.fingerprint);
        }
    }
}

void RenderCache::evictOverBudget()
{
    // バイト上限付きの強参照 LRU。追い出しても使用中のクリップは自分の shared_ptr を
    // 持っているので音は消えない。単独で上限を超えるエントリも外す（size() > 1 を条件に
    // すると、最初の1件が512MB超のとき上限が一度も効かない）
    while (cacheBytes > maxCacheBytes && ! cache.empty())
    {
        auto oldest = cache.begin();
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->second.lastUse < oldest->second.lastUse)
                oldest = it;
        cacheBytes -= oldest->second.bytes;
        cache.erase (oldest);
    }
}

bool RenderCache::isIdle() const
{
    std::lock_guard<std::mutex> lock (mutex);
    return pendingJobs.empty() && ! jobRunning && completed.empty();
}

bool RenderCache::waitForRenders (int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (pendingJobs.empty() && ! jobRunning)
                return true;
        }
        if (juce::Time::getMillisecondCounterHiRes() > deadline)
            return false;
        juce::Thread::sleep (5);
    }
}
