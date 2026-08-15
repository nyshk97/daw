#pragma once

#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

// トラックComp（透明コンプ）のパラメータ定義と静的ゲイン計算。
// plan: docs/plans/2026-08-15-1650-track-comp.md
//
// 「操作は削る、概念は隠さない」（ui-principles.md）:
//   - ノブは Threshold / Ratio / Attack / Release / Make Up の5個
//   - Knee は 6dB soft 固定（UIは「Knee: 6dB (soft)」の静的ラベルで概念だけ見せる）
//   - 検波サイドチェインHPFは 80Hz 固定のON/OFFトグルのみ（既定OFF）
//
// 静的ゲイン計算（computeOutputDb）はDSP（TrackComp）とUIの伝達カーブ描画の両方が
// 同じ関数を呼ぶ＝描画と音が同じ数式（EQの getMagnitudeForFrequencyArray と同じ思想）。
namespace Comp
{
inline constexpr float kneeDb = 6.0f;         // soft kneeの全幅（Threshold±3dBで徐々に掛かり始める）
inline constexpr float detectorHpfHz = 80.0f; // 検波HPF（キックの基音50〜80Hzを検波から外す定番値）

// dB変換の有限フロア。素の log10(0) は -inf になり「出力dB−入力dB」型のGR計算で
// -inf − -inf = NaN が出る。-120dBはUI下限-60dBより十分下で聴こえ方に影響しない
inline constexpr float silenceFloorDb = -120.0f;

inline constexpr float minThresholdDb = -60.0f, maxThresholdDb = 0.0f;
inline constexpr float minRatio = 1.0f, maxRatio = 20.0f;      // ∞:1（リミッター）は作らない
inline constexpr float minAttackMs = 0.1f, maxAttackMs = 100.0f;
inline constexpr float minReleaseMs = 10.0f, maxReleaseMs = 1000.0f;
inline constexpr float minMakeupDb = 0.0f, maxMakeupDb = 24.0f;

// プレーン値（保存・バウンス・UI表示用）。既定は中立スタート:
// Threshold 0dB＝挿した直後はほぼ無効（floatは0dBFSを超えうるので保証ではない。
// バイパスの保証は compEnabled が担う）。他は実用帯
struct Values
{
    float thresholdDb = 0.0f;
    float ratio = 4.0f;
    float attackMs = 10.0f;
    float releaseMs = 100.0f;
    float makeupDb = 0.0f;
    bool detectorHpf = false;
};

inline constexpr Values defaults {};

// 範囲クランプ＋非有限値（NaN/inf。手編集JSONで混入しうる）の既定値化。
// 読込と全代入経路で通すこと
inline Values normalized (Values value)
{
    if (! std::isfinite (value.thresholdDb))
        value.thresholdDb = defaults.thresholdDb;
    if (! std::isfinite (value.ratio))
        value.ratio = defaults.ratio;
    if (! std::isfinite (value.attackMs))
        value.attackMs = defaults.attackMs;
    if (! std::isfinite (value.releaseMs))
        value.releaseMs = defaults.releaseMs;
    if (! std::isfinite (value.makeupDb))
        value.makeupDb = defaults.makeupDb;

    value.thresholdDb = juce::jlimit (minThresholdDb, maxThresholdDb, value.thresholdDb);
    value.ratio = juce::jlimit (minRatio, maxRatio, value.ratio);
    value.attackMs = juce::jlimit (minAttackMs, maxAttackMs, value.attackMs);
    value.releaseMs = juce::jlimit (minReleaseMs, maxReleaseMs, value.releaseMs);
    value.makeupDb = juce::jlimit (minMakeupDb, maxMakeupDb, value.makeupDb);
    return value;
}

// RT共有用（TrackParams に置く）。UI書込とオーディオ読取が並行するため個別atomic。
// 代入前に normalized() を通すのは書き込み側の責任
struct Params
{
    std::atomic<float> thresholdDb { defaults.thresholdDb };
    std::atomic<float> ratio { defaults.ratio };
    std::atomic<float> attackMs { defaults.attackMs };
    std::atomic<float> releaseMs { defaults.releaseMs };
    std::atomic<float> makeupDb { defaults.makeupDb };
    std::atomic<bool> detectorHpf { defaults.detectorHpf };

    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<bool>::is_always_lock_free);
};

inline void store (Params& params, const Values& value)
{
    params.thresholdDb.store (value.thresholdDb);
    params.ratio.store (value.ratio);
    params.attackMs.store (value.attackMs);
    params.releaseMs.store (value.releaseMs);
    params.makeupDb.store (value.makeupDb);
    params.detectorHpf.store (value.detectorHpf);
}

inline Values load (const Params& params)
{
    return { params.thresholdDb.load(), params.ratio.load(), params.attackMs.load(),
             params.releaseMs.load(), params.makeupDb.load(), params.detectorHpf.load() };
}

// 静的ゲイン計算: 入力dB → 出力dB（**Make Up適用前**の圧縮特性。soft knee込み）。
// Threshold未満は素通し・十分上は傾き1/Ratio・knee帯（Threshold±3dB）は
// Giannoulis 2012 (JAES) の2次補間で滑らかに接続する
inline float computeOutputDb (float inputDb, float thresholdDb, float ratio)
{
    const float over = inputDb - thresholdDb;
    const float halfKnee = kneeDb * 0.5f;
    if (over <= -halfKnee)
        return inputDb;
    if (over >= halfKnee)
        return thresholdDb + over / ratio;
    const float t = over + halfKnee; // knee帯内の位置 0..kneeDb
    return inputDb + (1.0f / ratio - 1.0f) * t * t / (2.0f * kneeDb);
}

// GR（ゲインリダクション）＝入力dB−出力dB。常に0以上（正の減衰量で統一する）
inline float gainReductionDb (float inputDb, float thresholdDb, float ratio)
{
    return inputDb - computeOutputDb (inputDb, thresholdDb, ratio);
}
} // namespace Comp
