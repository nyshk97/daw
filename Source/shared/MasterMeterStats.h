#pragma once

#include <atomic>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

// Masterメーター（LUFS・相関・トゥルーピーク）の受け渡し構造。
// plan: docs/plans/2026-08-16-1523-fx-batch2-meters-limiter.md
//
// オーディオスレッド（MasterMeterSource）が100ms刻みの**十分統計量**を書き、
// メッセージスレッド（MasterMeterAggregator = LoudnessMeter.h）が読む。
// LUFSや相関の完成値は渡さない — dB領域の平均は誤りで、short-term/integratedの窓は
// 線形エネルギーを合算してからLUFSへ変換する必要があるため。
//
// リング契約:
//   - 書き手はオーディオスレッドのみ・読み手はMainComponentの常時30Hz Timerの一箇所のみ
//   - 各エントリにplay-session世代を付与。リセット（integrated/TPの仕切り直し）は
//     音声スレッドの再生開始エッジのみ（シーク・サイクルラップではリセットしない）
//   - 容量は数時間分。読み手が追い越された（あふれた）ら計測無効フラグを立てる
//     （黙って欠落させない。次の再生セッションで回復する）

// 100msサブブロック1個分の十分統計量
struct MasterMeterBlock
{
    float kwSumSqL = 0.0f, kwSumSqR = 0.0f;             // K-weighting後のch別二乗和（LUFS用）
    float rawSumLL = 0.0f, rawSumRR = 0.0f, rawSumLR = 0.0f; // 加工前信号のΣL²/ΣR²/ΣLR（相関用）
    float maxTruePeak = 0.0f;                            // 区間最大トゥルーピーク（リニア振幅）
    juce::int32 numSamples = 0;                          // 実サンプル数（停止時の部分ブロック < 公称）
    juce::int32 nominalSamples = 0;                      // 公称100msのサンプル数（SR依存）
    juce::uint32 generation = 0;                         // play-session世代
};

// SPSC（シングルライター・シングルリーダー）の有界リング。
// 書き手はスロットへ書いてから writeCount を release で進め、読み手は readCount を進める。
// **満杯時は新規エントリを破棄して dropped フラグを立てる**（読み手が読んでいる最中の
// スロットを上書きしない＝同一スロットへの並行アクセスが構造上起きない。上書き方式だと
// 1周遅れの読み手とのデータ競合＝未定義動作になる）
class MasterMeterRing
{
public:
    // 既定容量 2^17 = 131072 ブロック ≈ 3.6時間（100ms/個）。テストは小さい容量を渡せる
    explicit MasterMeterRing (int capacityPowerOfTwo = 1 << 17)
        : slots ((size_t) capacityPowerOfTwo), mask ((juce::uint64) capacityPowerOfTwo - 1)
    {
        jassert ((capacityPowerOfTwo & (capacityPowerOfTwo - 1)) == 0);
    }

    int capacity() const { return (int) slots.size(); }

    // ---- オーディオスレッド専用 ----
    void push (const MasterMeterBlock& block)
    {
        const auto w = writeCount.load (std::memory_order_relaxed);
        if (w - readCount.load (std::memory_order_acquire) >= (juce::uint64) slots.size())
        {
            // 満杯 = 読み手のあふれ。新規を破棄し、汚染された世代を添えて通知する。
            // 複数回破棄されたら**最後（=最大）の世代**が残る。読み手は「その世代以前は全部
            // 無効」と解釈する（間の世代の破棄が上書きで見えなくなっても安全側に倒れる）
            droppedGeneration.store (block.generation, std::memory_order_relaxed);
            dropped.store (true, std::memory_order_release);
            return;
        }
        slots[(size_t) (w & mask)] = block;
        writeCount.store (w + 1, std::memory_order_release);
    }

    // ---- メッセージスレッド専用 ----
    bool pop (MasterMeterBlock& out)
    {
        const auto r = readCount.load (std::memory_order_relaxed);
        if (r == writeCount.load (std::memory_order_acquire))
            return false;
        out = slots[(size_t) (r & mask)];
        readCount.store (r + 1, std::memory_order_release);
        return true;
    }

    // あふれ（エントリ破棄）が起きていたか。true のとき generationOut = 汚染された世代
    bool takeDropped (juce::uint32& generationOut)
    {
        if (! dropped.exchange (false, std::memory_order_acquire))
            return false;
        generationOut = droppedGeneration.load (std::memory_order_relaxed);
        return true;
    }

private:
    std::vector<MasterMeterBlock> slots;
    const juce::uint64 mask;
    std::atomic<juce::uint64> writeCount { 0 };
    std::atomic<juce::uint64> readCount { 0 };
    std::atomic<bool> dropped { false };
    std::atomic<juce::uint32> droppedGeneration { 0 };

    JUCE_DECLARE_NON_COPYABLE (MasterMeterRing)
};

// 集約済みの表示値（MainComponentが30Hzで組み立て、ビューはこれを描くだけ）
struct MasterMeterFeed
{
    float shortTermLufs = 0.0f;  // 直近3秒（hasShortTerm が true のときのみ有効）
    float integratedLufs = 0.0f; // 再生開始から（ゲーティング済み）
    float correlation = 0.0f;    // -1..+1（直近約300ms）
    float maxTruePeakDb = 0.0f;  // 再生開始からの最大dBTP
    bool hasShortTerm = false;
    bool hasIntegrated = false;
    bool hasCorrelation = false; // 無音中は false（「無音=逆相」の誤読を避ける）
    bool hasTruePeak = false;
    bool measurementValid = true; // リングあふれ検知で false（次の再生開始で回復）
};
