#pragma once

#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

// バスReverb（send用固定バス1/2 = "Reverb A" / "Reverb B"）のパラメータ定義。
// plan: docs/plans/2026-08-16-2305-fx-batch4-delay-reverb.md
//
// A/Bは同一DSP（juce::Reverb ベース）の初期値違い（A=短めRoom系・B=長めHall系）。
// 種類セレクタは作らない（単一アルゴリズムのプリセットに過ぎず、ノブ5個の中身が
// 見えなくなる）。send バスの返しなので Mix ノブは無く full wet。
// ノブ5点:
// - Size / Damp / Width: juce::Reverb の roomSize / damping / width
//   （Damp = 残響の高域が減る速さ。上げるほど「柔らかい部屋」になる）
// - Pre-delay: 原音が鳴ってから残響が始まるまでの隙間（Logic ChromaVerb の Pre-Dly）。
//   30〜80ms空けると子音が濡れる前に届き、深くかけても声が手前に残る
// - Low Cut: 残響へ送る前のハイパス。残響は音を引き伸ばすので低域が入ると床にモヤが
//   溜まる（「リバーブの前にハイパス」はボーカルのハイパスと同じ因果）
namespace Reverb
{
inline constexpr float minPreDelayMs = 0.0f, maxPreDelayMs = 100.0f;
inline constexpr float minLowCutHz = 20.0f, maxLowCutHz = 500.0f; // 20Hz = 実質OFF

// プレーン値（保存・バウンス・UI表示用）
struct Values
{
    float size = 0.5f;       // 0..1（juce::Reverb roomSize）
    float damp = 0.5f;       // 0..1（同 damping）
    float width = 1.0f;      // 0..1（同 width。0=モノ残響）
    float preDelayMs = 20.0f;
    float lowCutHz = 100.0f;
};

// バスindex別の既定値テーブル = 「新規作成」「旧版読込（キー欠損）」両経路の単一の真実の源。
// A: 短めRoom系（近い部屋の「居場所」を付ける）・B: 長めHall系（サビで奥へ飛ばす）。
// Low Cut 100Hz はどちらも既定（キック・ベース帯域を残響に入れない定石）
inline Values defaultsForBus (int busIndex)
{
    if (busIndex == 1)
        return { 0.85f, 0.35f, 1.0f, 50.0f, 100.0f }; // B: 長め・明るめ・手前確保に深めのPre-delay
    return { 0.35f, 0.55f, 1.0f, 20.0f, 100.0f };      // A: 短め・柔らかめ
}

// 範囲クランプ＋非有限値（NaN/inf）の既定値化。fallback は対象バスの defaultsForBus を渡す。
// 読込と全代入経路で通すこと
inline Values normalized (Values value, const Values& fallback)
{
    auto fix = [] (float& v, float fallbackValue, float lo, float hi)
    {
        if (! std::isfinite (v))
            v = fallbackValue;
        v = juce::jlimit (lo, hi, v);
    };
    fix (value.size, fallback.size, 0.0f, 1.0f);
    fix (value.damp, fallback.damp, 0.0f, 1.0f);
    fix (value.width, fallback.width, 0.0f, 1.0f);
    fix (value.preDelayMs, fallback.preDelayMs, minPreDelayMs, maxPreDelayMs);
    fix (value.lowCutHz, fallback.lowCutHz, minLowCutHz, maxLowCutHz);
    return value;
}

// RT共有用（busParams[0/1] に置く）。UI書込とオーディオ読取が並行するため個別atomic。
// 既定はA相当（Project側が生成時に defaultsForBus を store して上書きする）。
// 代入前に normalized() を通すのは書き込み側の責任
struct Params
{
    std::atomic<float> size { 0.35f };
    std::atomic<float> damp { 0.55f };
    std::atomic<float> width { 1.0f };
    std::atomic<float> preDelayMs { 20.0f };
    std::atomic<float> lowCutHz { 100.0f };

    static_assert (std::atomic<float>::is_always_lock_free);
};

inline void store (Params& params, const Values& value)
{
    params.size.store (value.size);
    params.damp.store (value.damp);
    params.width.store (value.width);
    params.preDelayMs.store (value.preDelayMs);
    params.lowCutHz.store (value.lowCutHz);
}

inline Values load (const Params& params)
{
    return { params.size.load(), params.damp.load(), params.width.load(),
             params.preDelayMs.load(), params.lowCutHz.load() };
}
} // namespace Reverb
