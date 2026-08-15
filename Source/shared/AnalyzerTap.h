#pragma once

#include <atomic>
#include <cstring>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

// EQエディタのスペクトラムアナライザ用タップ（オーディオ→UI）。
// GOTCHAS.md パターン2と同じ AbstractFifo＋事前確保バッファ（シングルライター・シングルリーダー）。
// plan: docs/plans/2026-08-15-1506-track-eq.md
//
// L/Rを**単一のFIFOで2chブロック管理**する（別FIFO2本にすると読み書きの割り込みで左右の
// 位置がずれる）。各ブロックには**世代IDと実際にタップしたtrackId**を添付する — atomicの
// 世代だけでは、キューに残った旧トラックのサンプルをUI側が識別できないため。
//
// atomicの公開順序（GOTCHAS「対になるatomicは中間状態が安全な順序で」と同じ問題）:
//   - UI: 対象trackId・enabledを書いてから世代を**release**で進める（setTarget）
//   - 音声側: ブロックを積む直前に世代を**acquire**でもう一度読み、ブロック蓄積開始時点から
//     変わっていたらそのブロックを捨てる（「新世代＋旧trackId」の混成ブロックを作らない）
//   - UI: (trackId, 世代) の**両方**が現在値と一致するブロックだけ採用する（SpectrumAnalyzer側）
class AnalyzerTap
{
public:
    static constexpr int blockSamples = 512; // 1ブロックのサンプル数（ch共通）
    static constexpr int capacityBlocks = 64; // 約0.7秒@48k（UIは30Hzで読むので十分な余裕）

    struct Header
    {
        juce::uint64 trackId = 0;
        juce::uint32 generation = 0;
    };

    AnalyzerTap()
    {
        bufferL.resize ((size_t) blockSamples * capacityBlocks);
        bufferR.resize ((size_t) blockSamples * capacityBlocks);
        headers.resize (capacityBlocks);
        stagingL.resize (blockSamples);
        stagingR.resize (blockSamples);
    }

    // ---- UI（メッセージスレッド）専用 ----

    // 対象の切替・有効/無効。targetを書いてから世代をrelease-storeする（順序が本体）
    void setTarget (juce::uint64 trackId, bool enable)
    {
        targetTrackId.store (trackId, std::memory_order_relaxed);
        enabledFlag.store (enable, std::memory_order_relaxed);
        currentGeneration.fetch_add (1, std::memory_order_release);
    }

    juce::uint32 generation() const { return currentGeneration.load (std::memory_order_acquire); }
    juce::uint64 targetId() const { return targetTrackId.load (std::memory_order_relaxed); }

    // 1ブロック取り出し（left/right は blockSamples 分の書き込み先）。無ければ false
    bool popBlock (Header& headerOut, float* left, float* right)
    {
        const auto scope = fifo.read (1);
        if (scope.blockSize1 + scope.blockSize2 < 1)
            return false;
        const int index = scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2;
        headerOut = headers[(size_t) index];
        std::memcpy (left, bufferL.data() + (size_t) index * blockSamples,
                     sizeof (float) * blockSamples);
        std::memcpy (right, bufferR.data() + (size_t) index * blockSamples,
                     sizeof (float) * blockSamples);
        return true;
    }

    // ---- オーディオスレッド専用 ----

    bool wantsTrack (juce::uint64 trackId) const
    {
        return enabledFlag.load (std::memory_order_relaxed)
               && targetTrackId.load (std::memory_order_relaxed) == trackId;
    }

    // EQ直後（フェーダー前）のL/Rを積む。right == nullptr はモノ（Lを両chに複製）。
    // ブロック境界はセグメント長と無関係（stagingで貯めて blockSamples 単位でコミット）
    void pushSamples (juce::uint64 trackId, const float* left, const float* right, int numSamples)
    {
        if (! wantsTrack (trackId))
        {
            stagingCount = 0; // 対象でなくなったら作りかけを捨てる（混成ブロックを残さない）
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            if (stagingCount == 0)
                stagingGeneration = currentGeneration.load (std::memory_order_acquire);
            stagingL[(size_t) stagingCount] = left[i];
            stagingR[(size_t) stagingCount] = right != nullptr ? right[i] : left[i];

            if (++stagingCount == blockSamples)
            {
                stagingCount = 0;
                // コミット直前に世代を再読（前後2回のacquire-load）。蓄積中に切り替わっていたら捨てる
                const auto generationNow = currentGeneration.load (std::memory_order_acquire);
                if (generationNow != stagingGeneration)
                    continue;

                const auto scope = fifo.write (1);
                if (scope.blockSize1 + scope.blockSize2 < 1)
                    continue; // 満杯 = UIの読み遅れ。落としてよい（表示用データ）
                const int index = scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2;
                std::memcpy (bufferL.data() + (size_t) index * blockSamples, stagingL.data(),
                             sizeof (float) * blockSamples);
                std::memcpy (bufferR.data() + (size_t) index * blockSamples, stagingR.data(),
                             sizeof (float) * blockSamples);
                headers[(size_t) index] = { trackId, generationNow };
            }
        }
    }

private:
    juce::AbstractFifo fifo { capacityBlocks };
    std::vector<float> bufferL, bufferR;
    std::vector<Header> headers;

    // オーディオスレッド専用のブロック組み立て場所
    std::vector<float> stagingL, stagingR;
    int stagingCount = 0;
    juce::uint32 stagingGeneration = 0;

    std::atomic<juce::uint64> targetTrackId { 0 };
    std::atomic<bool> enabledFlag { false };
    std::atomic<juce::uint32> currentGeneration { 0 };

    static_assert (std::atomic<juce::uint64>::is_always_lock_free);

    JUCE_DECLARE_NON_COPYABLE (AnalyzerTap)
};
