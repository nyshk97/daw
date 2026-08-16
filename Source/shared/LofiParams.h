#pragma once

#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

// トラックLo-fiのパラメータ定義とDSP/UI共有のマッピング計算。
// plan: docs/plans/2026-08-16-2058-fx-batch3-saturation-lofi.md
//
// Lo-fi＝独立した劣化処理の束。役割は「オリジナルドラムをレコード上モノの質感に寄せる糊」。
// 成分別1ノブ×4（Wow/Tone/Noise/Crush）・Mixなし＝full wet
//（ピッチ揺れ成分とdryを混ぜるとコーラス＝別エフェクトになるため）。
// 全ノブ0が中立（素通し）で、各成分もノブ0で個別スキップする。
namespace Lofi
{
// Wow: レート固定 0.55Hz（レコード33⅓回転の偏心と同じ周期）＋わずかな不規則ドリフト。
// 深さはノブ2乗カーブで最大±1.5%（≈±26セント）。ディレイ振幅 A = p/(2π·f) —
// 最大で A≈4.34ms・実時間遅延は 0〜2A（平均A）。PDCは作らず質感の一部として受容（plan）
inline constexpr float wowRateHz = 0.55f;
inline constexpr float maxWowDepth = 0.015f; // ピッチ偏差（比率）の最大

// Tone: ローパス。ノブ0=開放（20kHz・処理スキップ）→ 1=500Hz の対数カーブ・Q=0.707固定
inline constexpr float toneOpenHz = 20000.0f;
inline constexpr float toneClosedHz = 500.0f;

// Crush: ビット深度と内部サンプルレート低減の1ノブ連動（SP-1200/初期MPC的な質感）。
// 指数カーブの肩は「ノブ中央 ≈ 12bit/26kHz（48kHz時）・最大 ≈ 8bit/6kHz」に合わせて決定:
//   bits(k) = 24·(8/24)^(k^0.664)   → bits(0)=24, bits(0.5)≈12, bits(1)=8
//   ratio(k) = (1/8)^(k^1.762)      → ratio(0)=1, ratio(0.5)≈0.542(=26k/48k), ratio(1)=0.125
// レートは比率ベース（ネイティブSR×ratio）: 絶対Hzだと高SRデバイスでノブ0が中立にならない
inline constexpr float crushBitsExponent = 0.664f;
inline constexpr float crushRatioExponent = 1.762f;

// Noise: ヒス（フィルタ白色雑音）＋クラックル（ランダムインパルス）の内部合成・モノ1系統を
// 両chへ加算（レコードの溝ノイズと同じくL/R相関＝モノ互換）。入力エンベロープ追従
//（attack 10ms / release 200ms・無音では鳴らない）。最大レベル ≈ -20dBFS（ノブ2乗カーブ）
inline constexpr float noiseMaxGain = 0.1f;
inline constexpr float noiseEnvAttackMs = 10.0f;
inline constexpr float noiseEnvReleaseMs = 200.0f;

// プレーン値（保存・バウンス・UI表示用）。既定は中立スタート（ONにしても音が変わらない）
struct Values
{
    float wow = 0.0f;   // 0..1（揺れ深さ）
    float tone = 0.0f;  // 0..1（0=開放）
    float noise = 0.0f; // 0..1（量）
    float crush = 0.0f; // 0..1（ビット＋レート低減）
};

inline constexpr Values defaults {};

// 音響的に素通しか（高速パス・バウンスactive判定・producesTailの材料）
inline bool isNeutral (const Values& value)
{
    return value.wow <= 0.0f && value.tone <= 0.0f && value.noise <= 0.0f
           && value.crush <= 0.0f;
}

// 範囲クランプ＋非有限値（NaN/inf。手編集JSONで混入しうる）の既定値化。
// 読込と全代入経路で通すこと
inline Values normalized (Values value)
{
    auto fix = [] (float& v, float fallback)
    {
        if (! std::isfinite (v))
            v = fallback;
        v = juce::jlimit (0.0f, 1.0f, v);
    };
    fix (value.wow, defaults.wow);
    fix (value.tone, defaults.tone);
    fix (value.noise, defaults.noise);
    fix (value.crush, defaults.crush);
    return value;
}

// RT共有用（TrackParams に置く）。UI書込とオーディオ読取が並行するため個別atomic。
// 代入前に normalized() を通すのは書き込み側の責任
struct Params
{
    std::atomic<float> wow { defaults.wow };
    std::atomic<float> tone { defaults.tone };
    std::atomic<float> noise { defaults.noise };
    std::atomic<float> crush { defaults.crush };

    static_assert (std::atomic<float>::is_always_lock_free);
};

inline void store (Params& params, const Values& value)
{
    params.wow.store (value.wow);
    params.tone.store (value.tone);
    params.noise.store (value.noise);
    params.crush.store (value.crush);
}

inline Values load (const Params& params)
{
    return { params.wow.load(), params.tone.load(), params.noise.load(), params.crush.load() };
}

// ---- ノブ→物理量のマッピング（DSPとUI表示が共有＝表示と音が同じ数式）----

// Wowノブ → ピッチ偏差（比率）。2乗カーブで浅域の分解能を確保
inline float wowDepthRatio (float knob)
{
    return knob * knob * maxWowDepth;
}

// ピッチ偏差 p・LFO周波数 f → ディレイ振幅 A（秒）。A = p/(2π·f)
inline float wowDelayAmpSeconds (float depthRatio)
{
    return depthRatio / (juce::MathConstants<float>::twoPi * wowRateHz);
}

// Toneノブ → カットオフHz（対数カーブ。0=開放20kHz）
inline float toneCutoffHz (float knob)
{
    return toneOpenHz * std::pow (toneClosedHz / toneOpenHz, knob);
}

// Crushノブ → ビット深度（連続値）
inline float crushBits (float knob)
{
    return 24.0f * std::pow (8.0f / 24.0f, std::pow (knob, crushBitsExponent));
}

// Crushノブ → 内部レート比（ネイティブSRに掛ける）
inline float crushRateRatio (float knob)
{
    return std::pow (0.125f, std::pow (knob, crushRatioExponent));
}

// ビット深度 → 量子化（±1.0を全レンジとする振幅量子化。bits は連続値でよい）
inline float quantize (float x, float bits)
{
    const float step = std::pow (2.0f, 1.0f - bits);
    return std::round (x / step) * step;
}
} // namespace Lofi
