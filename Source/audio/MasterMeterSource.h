#pragma once

#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../shared/LoudnessMeter.h"
#include "../shared/MasterMeterStats.h"
#include "../shared/TruePeakDetector.h"
#include "BiquadFilter.h"

// Masterメーターの計測DSP（オーディオスレッド側）。
// plan: docs/plans/2026-08-16-1523-fx-batch2-meters-limiter.md
//
// 計測点はMasterの最終出力（Limiter・曲末フェード後・メトロノーム加算前）＝書き出しファイルと
// 同一の信号。**再生中は常時稼働**（詳細ビューの表示有無と無関係。「再生開始からの
// Integrated/TP」を成立させる）。毎サンプル K-weighting・4x TP・相関の統計を進め、
// 100ms刻みの十分統計量だけをリング（MasterMeterRing）へ書く。
//
// 契約:
//   - リセット（世代の切替）は**再生開始エッジのみ**。シーク・サイクルラップでは
//     リセットしない（integratedは「再生開始から鳴った音すべて」の平均）
//   - 停止エッジでは100ms未満の部分統計を確定してリングへ出し、TPのFIRを無音でflushする
//     （停止直前のピークを取りこぼさない）。部分ブロックは numSamples < nominalSamples で
//     識別され、集約側がintegratedから除外する（規格の不完全窓の扱い）
//
// RT安全: 固定サイズ・確保なし
class MasterMeterSource
{
public:
    MasterMeterSource() = default;

    // リングは非所有（MainComponentが所有）。オーディオ開始前にセットすること
    void setRing (MasterMeterRing* ringToUse) { ring = ringToUse; }

    // オーディオ開始前（PlaybackEngine::prepareToPlay＝非RTスレッド）に呼ぶ:
    // K-weighting係数の tan/pow など重い計算をコールバック外で済ませておく。
    // process内のSR不一致検知は防御用で、通常はここで設定済みのまま走る
    void prepare (double sampleRate)
    {
        configureRate (sampleRate);
        shelf.resetState();
        highpass.resetState();
        truePeak.reset();
        clearAccumulators();
    }

    // 1セグメント分の計測。playStartEdge = 再生開始エッジ（このセグメントから新世代）・
    // stopEdge = 停止エッジ（部分統計の確定。このセグメント自体は計測しない）
    void process (const float* left, const float* right, int numSamples, double sampleRate,
                  bool playing, bool playStartEdge, bool stopEdge)
    {
        if (ring == nullptr)
            return;

        if (stopEdge)
            finalizePartial();

        if (! playing || left == nullptr || right == nullptr || numSamples <= 0)
            return;

        if (playStartEdge || sampleRate != preparedRate)
            beginSession (sampleRate, playStartEdge);

        for (int i = 0; i < numSamples; ++i)
        {
            const float l = left[i];
            const float r = right[i];

            // LUFS用: K-weighting後の二乗和（ch別）
            const float wl = highpass.processSample (0, shelf.processSample (0, l));
            const float wr = highpass.processSample (1, shelf.processSample (1, r));
            kwSumSqL += (double) wl * wl;
            kwSumSqR += (double) wr * wr;

            // 相関・TP用: 加工前のpost-master信号（BS.1770のTPは重み付けなし）
            rawLL += (double) l * l;
            rawRR += (double) r * r;
            rawLR += (double) l * r;
            blockMaxTp = juce::jmax (blockMaxTp,
                                     juce::jmax (truePeak.processSample (0, l),
                                                 truePeak.processSample (1, r)));

            if (++blockCount >= nominalCount)
                pushBlock (blockCount);
        }
    }

private:
    void configureRate (double sampleRate)
    {
        preparedRate = sampleRate;
        nominalCount = juce::jmax (1, (int) std::lround (Loudness::subBlockSeconds * sampleRate));
        Loudness::kWeightingCoefficients (sampleRate, shelf, highpass);
    }

    // 再生開始エッジの仕切り直し（状態リセット＋世代のみ＝軽量。係数はprepare済み）
    void beginSession (double sampleRate, bool newGeneration)
    {
        if (sampleRate != preparedRate)
            configureRate (sampleRate); // 防御（通常はprepareToPlay経由で設定済み）
        shelf.resetState();
        highpass.resetState();
        truePeak.reset();
        clearAccumulators();
        if (newGeneration)
            ++generation;
    }

    void clearAccumulators()
    {
        kwSumSqL = kwSumSqR = rawLL = rawRR = rawLR = 0.0;
        blockMaxTp = 0.0f;
        blockCount = 0;
    }

    void pushBlock (int numSamples)
    {
        MasterMeterBlock block;
        block.kwSumSqL = (float) kwSumSqL;
        block.kwSumSqR = (float) kwSumSqR;
        block.rawSumLL = (float) rawLL;
        block.rawSumRR = (float) rawRR;
        block.rawSumLR = (float) rawLR;
        block.maxTruePeak = blockMaxTp;
        block.numSamples = numSamples;
        block.nominalSamples = nominalCount;
        block.generation = generation;
        ring->push (block);
        clearAccumulators();
    }

    // 停止エッジ: TP FIRの残りを無音でflushしてから、100ms未満の部分統計を確定する
    void finalizePartial()
    {
        if (preparedRate <= 0.0)
            return;
        const float tailTp = juce::jmax (truePeak.flush (0), truePeak.flush (1));
        blockMaxTp = juce::jmax (blockMaxTp, tailTp);
        if (blockCount > 0 || blockMaxTp > 0.0f)
            pushBlock (blockCount);
    }

    MasterMeterRing* ring = nullptr; // 非所有

    Biquad shelf, highpass; // K-weighting（適用者はここだけ。LoudnessMeterは係数提供のみ）
    TruePeakDetector truePeak;

    double preparedRate = 0.0;
    int nominalCount = 0;
    juce::uint32 generation = 0;

    double kwSumSqL = 0.0, kwSumSqR = 0.0;
    double rawLL = 0.0, rawRR = 0.0, rawLR = 0.0;
    float blockMaxTp = 0.0f;
    int blockCount = 0;

    JUCE_DECLARE_NON_COPYABLE (MasterMeterSource)
};
