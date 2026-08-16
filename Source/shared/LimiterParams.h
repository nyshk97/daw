#pragma once

#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

// Master Limiter のパラメータ定義（CompParams.h と同じ構成）。
// plan: docs/plans/2026-08-16-1523-fx-batch2-meters-limiter.md
//
// ノブは Gain / Ceiling / Release の3個（Logic の Gain / Output Level / Release と同じ読み替え）。
// Lookahead は 2ms 固定で UI に出さない（MasterLimiter.h 参照）。
// Master 常在（ON/OFFなし）なので enabled フラグは持たない。既定は Gain 0dB ＝
// 「天井（-1dB）を超えない限り素通し」の中立スタート。
namespace Limiter
{
inline constexpr float minGainDb = 0.0f, maxGainDb = 12.0f;
inline constexpr float minCeilingDb = -6.0f, maxCeilingDb = 0.0f;
inline constexpr float minReleaseMs = 5.0f, maxReleaseMs = 500.0f;

// dB変換の有限フロア（Comp::silenceFloorDb と同じ理由。log10(0) の -inf を避ける）
inline constexpr float silenceFloorDb = -120.0f;

// プレーン値（保存・バウンス・UI表示用）。Ceiling 既定 -1.0dB は配信安全の既定
// （サンプル間ピーク≈+0.3dB のマージン込みで -1dBTP 基準を実質カバーする）
struct Values
{
    float gainDb = 0.0f;
    float ceilingDb = -1.0f;
    float releaseMs = 60.0f;
};

inline constexpr Values defaults {};

// 範囲クランプ＋非有限値（NaN/inf。手編集JSONで混入しうる）の既定値化。
// 読込と全代入経路で通すこと
inline Values normalized (Values value)
{
    if (! std::isfinite (value.gainDb))
        value.gainDb = defaults.gainDb;
    if (! std::isfinite (value.ceilingDb))
        value.ceilingDb = defaults.ceilingDb;
    if (! std::isfinite (value.releaseMs))
        value.releaseMs = defaults.releaseMs;

    value.gainDb = juce::jlimit (minGainDb, maxGainDb, value.gainDb);
    value.ceilingDb = juce::jlimit (minCeilingDb, maxCeilingDb, value.ceilingDb);
    value.releaseMs = juce::jlimit (minReleaseMs, maxReleaseMs, value.releaseMs);
    return value;
}

// RT共有用（masterParams に置く）。UI書込とオーディオ読取が並行するため個別atomic。
// 代入前に normalized() を通すのは書き込み側の責任
struct Params
{
    std::atomic<float> gainDb { defaults.gainDb };
    std::atomic<float> ceilingDb { defaults.ceilingDb };
    std::atomic<float> releaseMs { defaults.releaseMs };

    static_assert (std::atomic<float>::is_always_lock_free);
};

inline void store (Params& params, const Values& value)
{
    params.gainDb.store (value.gainDb);
    params.ceilingDb.store (value.ceilingDb);
    params.releaseMs.store (value.releaseMs);
}

inline Values load (const Params& params)
{
    return { params.gainDb.load(), params.ceilingDb.load(), params.releaseMs.load() };
}
} // namespace Limiter
