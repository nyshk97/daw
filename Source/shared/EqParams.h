#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

// トラックEQ（4バンド固定）のパラメータ定義。plan: docs/plans/2026-08-15-1506-track-eq.md
//
// バンドは [0]=ハイパス / [1]=ベル1 / [2]=ベル2 / [3]=ハイシェルフ の固定並びで、
// UI・保存形式（fx.eq.bands 配列）・エンジンすべてこの順序を前提にする。
// typeは実行時に持たず index から決める（4本固定なので状態にすると不整合の余地が増えるだけ）。
//
// 「操作は削る、概念は隠さない」（ui-principles.md）に対応する不変条件:
//   - enabled を操作できるのはハイパスだけ（ベル/シェルフは gain=0dB が実質OFF）
//   - ハイパスは gain の概念なし・Q=0.71固定（Butterworth＝山も谷もない素直な12dB/octの遮断）
//   - ハイシェルフは Q=0.71固定
// UIから操作できない値（ベルのenabled=false等）がJSON手編集で入ると戻せない隠れ状態になるため、
// 読込・代入経路は必ず normalized() を通す。
namespace Eq
{
inline constexpr int numBands = 4;
enum Band : int { highpass = 0, bell1 = 1, bell2 = 2, highShelf = 3 };

// 1/√2。2次フィルタで通過帯域が最も平坦になるQ（これより大きいとカットオフ付近が持ち上がる）
inline constexpr float fixedQ = 0.70710678f;

inline constexpr float maxGainDb = 24.0f; // ±24dB（実用は±6dB程度だがスイープ探索用に広く。Logicと同じ）
inline constexpr float minQ = 0.1f;
inline constexpr float maxQ = 10.0f;

// バンド1本のプレーン値。保存・バウンス・UI表示はこちらを使い、
// RT共有（TrackParams::eqBands）は下の BandParams（atomic）を使う
struct BandValue
{
    bool enabled = true;
    float freqHz = 1000.0f;
    float gainDb = 0.0f;
    float q = 1.0f;
};

// 全バンドのプレーン値スナップショット（保存・バウンス用）
using Values = std::array<BandValue, numBands>;

inline Values defaultValues(); // 前方宣言（defaults 定義の直後で実装）

inline constexpr BandValue defaults[numBands] = {
    { false, 80.0f, 0.0f, fixedQ },   // ハイパス: 既定OFF・80Hz
    { true, 250.0f, 0.0f, 1.0f },     // ベル1: こもり帯スタート
    { true, 3500.0f, 0.0f, 1.0f },    // ベル2: 抜け帯スタート
    { true, 10000.0f, 0.0f, fixedQ }, // ハイシェルフ: エア
};

inline Values defaultValues()
{
    Values values;
    for (int i = 0; i < numBands; ++i)
        values[(size_t) i] = defaults[i];
    return values;
}

// 周波数レンジはバンドの役割に合わせて制限する（ハイパスを20kHzまで上げても全帯域カットの
// 事故装置にしかならず、シェルフを低域まで下げるとローシェルフの代用という隠れ機能になる）
inline float minFreq (int band) { return band == highShelf ? 1000.0f : 20.0f; }
inline float maxFreq (int band) { return band == highpass ? 1000.0f : 20000.0f; }

// バンド種別ごとの不変条件＋範囲クランプを強制する。読込と全代入経路で通すこと。
// 非有限値（NaN/inf。手編集JSONで混入しうる）は jlimit がすり抜けさせるため先に既定値へ落とす
inline BandValue normalized (int band, BandValue value)
{
    if (! std::isfinite (value.freqHz))
        value.freqHz = defaults[band].freqHz;
    if (! std::isfinite (value.gainDb))
        value.gainDb = defaults[band].gainDb;
    if (! std::isfinite (value.q))
        value.q = defaults[band].q;

    value.freqHz = juce::jlimit (minFreq (band), maxFreq (band), value.freqHz);
    value.gainDb = juce::jlimit (-maxGainDb, maxGainDb, value.gainDb);
    value.q = juce::jlimit (minQ, maxQ, value.q);

    if (band == highpass)
    {
        value.gainDb = 0.0f; // ハイパスにgainの概念はない（保存値も無視）
        value.q = fixedQ;
    }
    else
    {
        value.enabled = true; // OFF相当はgain=0dBで表現する（enabledを操作できるのはHPだけ）
        if (band == highShelf)
            value.q = fixedQ;
    }
    return value;
}

// RT共有用（TrackParams に4本並ぶ）。UI書込とオーディオ読取が並行するため個別atomic。
// 値の意味・不変条件は BandValue と同じ（代入前に normalized() を通すのは書き込み側の責任）
struct BandParams
{
    std::atomic<bool> enabled { true };
    std::atomic<float> freqHz { 1000.0f };
    std::atomic<float> gainDb { 0.0f };
    std::atomic<float> q { 1.0f };

    static_assert (std::atomic<float>::is_always_lock_free);
};

inline void store (BandParams& params, const BandValue& value)
{
    params.enabled.store (value.enabled);
    params.freqHz.store (value.freqHz);
    params.gainDb.store (value.gainDb);
    params.q.store (value.q);
}

inline BandValue load (const BandParams& params)
{
    return { params.enabled.load(), params.freqHz.load(), params.gainDb.load(), params.q.load() };
}

inline void applyDefaults (BandParams (&bands)[numBands])
{
    for (int i = 0; i < numBands; ++i)
        store (bands[i], defaults[i]);
}

inline Values loadAll (const BandParams (&bands)[numBands])
{
    Values values;
    for (int i = 0; i < numBands; ++i)
        values[(size_t) i] = load (bands[i]);
    return values;
}

// 全バンド中立＝EQが音を一切変えない状態か（HP無効 かつ ベル・シェルフのgainが0dB）。
// エンジンの高速パス判定は `!eqEnabled || isNeutral(target値)` のORで行う
inline bool isNeutral (const Values& values)
{
    return ! values[highpass].enabled
        && values[bell1].gainDb == 0.0f
        && values[bell2].gainDb == 0.0f
        && values[highShelf].gainDb == 0.0f;
}
} // namespace Eq
