#pragma once

#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../shared/LimiterParams.h"

// Master Limiter のDSP本体（lookahead brickwall・ステレオ）。
// plan: docs/plans/2026-08-16-1523-fx-batch2-meters-limiter.md
//
// 構成: 入力Gain（平滑）→ 検波（サンプルピーク・ステレオリンク）→ 2msディレイライン →
// ゲインエンベロープ（lookahead窓内の最大GRへ先回りアタック＋release ワンポール）→
// 最終安全クランプ。天井の絶対保証はクランプが担い、エンベロープは歪ませないための平滑を担う。
//
// 天井保証の3点セット（planの「パラメータ操作中もbrickwall」契約）:
//   - Gainの平滑適用は検波・ディレイ格納より**前**（検波が見る信号と出力される信号が同一）
//   - Ceilingの実効値はディレイと並走するリングでLサンプル整列（GR計算時のceilingと
//     出力時の天井判定が同じサンプルを指す）
//   - 最終クランプは**ステレオリンク**: max(|L|,|R|) から安全ゲインを1個求めて両chへ
//     同量適用（L/R個別クランプは大きい側だけ削れて定位が動く）
//
// リセット契約（TrackCompのtimelineJumpedをそのまま流用しない）:
//   リセットは「再生開始・明示シーク・SR変更」のみ。**サイクルラップでは呼ばない**
//   （呼ぶと毎ループ先頭にLサンプルの無音が入る。ラップは連続ストリームとして扱う）。
//   リセット直後のLサンプルは無音（ディレイライン充填中）。
//
// 遅延契約: 出力は入力より lookaheadSamples() 遅れる。バウンス側は先頭Lサンプルを捨て、
// 末尾にLサンプルの無音を流してflushする（BounceRenderer）。
//
// RT安全性: 全状態は固定サイズ（ヒープ確保なし）。パラメータ平滑はサンプル単位。
class MasterLimiter
{
public:
    static constexpr double lookaheadSeconds = 0.002; // 2ms固定（Logic既定と同程度）
    static constexpr int maxLookaheadSamples = 512;   // 256kHzまで対応（2ms分）

    MasterLimiter() = default;

    // このSRでのlookaheadサンプル数（configureForRateと同一式）。エンジンがセグメント境界の
    // 計算に「Limiterを構成する前」から必要とするため、静的に引けるようにしておく
    static int lookaheadForRate (double sampleRate)
    {
        return juce::jlimit (1, maxLookaheadSamples,
                             (int) std::lround (lookaheadSeconds * sampleRate));
    }

    // 平滑を挟まず即座に目標状態にし、ディレイ・エンベロープを全消去する。
    // バウンス開始前と、RTの「再生開始・明示シーク」で呼ぶ
    void snapTo (double sampleRate, const Limiter::Values& targets);

    // インプレース処理（ステレオ必須ではない: right == nullptr はモノ＝Lのみ処理）。
    // SR変更を検知したら内部で snapTo 相当のリセットを行う
    void process (float* left, float* right, int numSamples, double sampleRate,
                  const Limiter::Values& targets);

    // 出力が入力から遅れるサンプル数（直近の snapTo / process のSR基準）
    int lookaheadSamples() const { return lookahead; }

    // ---- UI向け（直近の process 1回分。バウンス側インスタンスでは読まない）----
    float blockMaxGainReductionDb() const { return blockMaxGr; } // 正の減衰量dB（クランプ分込み）

    // 現在の平滑化GR（正のdB）。テスト用
    float currentGainReductionDb() const { return grEnvDb; }

private:
    void configureForRate (double sampleRate);
    void updateReleaseAlpha (float releaseMs);
    void resetState();

    // 平滑化パラメータ（TrackCompと同じ 10ms ランプ）
    juce::SmoothedValue<float> gainDb, ceilingDb;

    double preparedRate = 0.0;
    int lookahead = 0;          // Lサンプル（SR依存）
    float alphaAttack = 0.0f;   // τ = L/4 サンプル（lookahead内に目標GRへ98%到達）
    float alphaRelease = 0.0f;
    float lastReleaseMs = -1.0f;

    // ディレイライン＋整列ceiling（リング。idx が書き込み位置＝Lサンプル前の読み出し位置）
    float delayL[maxLookaheadSamples] = {};
    float delayR[maxLookaheadSamples] = {};
    float ceilRingDb[maxLookaheadSamples] = {};
    int ringIdx = 0;

    // lookahead窓（W = L+1）の sliding max（単調deque。容量は2の冪でマスク演算）
    static constexpr int dequeCapacity = 1024; // > maxLookaheadSamples + 1
    static constexpr juce::uint64 dequeMask = dequeCapacity - 1;
    float dqVal[dequeCapacity] = {};
    juce::uint64 dqPos[dequeCapacity] = {};
    juce::uint64 dqHead = 0, dqTail = 0;
    juce::uint64 samplePos = 0;

    float grEnvDb = 0.0f;    // 平滑化GR（正のdB）
    float blockMaxGr = 0.0f;

    JUCE_DECLARE_NON_COPYABLE (MasterLimiter)
};
