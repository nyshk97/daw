#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "PlaybackSnapshot.h"

// オーディオリージョンのフェード（リニア）を「重なり範囲 → 最大3区間」へ割るヘルパー。
// 音を出す箇所は PlaybackEngine のモノ経路・ステレオ経路・BounceRenderer の3つあり、
// いずれも `addFrom (..., 定数gain)` では傾斜を表現できないため、区間分割の規則をここへ集める。
//
// エンベロープの定義（**閉区間・両端が厳密**）:
//   fadeIn  (n = fadeInEnd - fadeInStart):   g(s) = (s - fadeInStart) / (n - 1)
//   fadeOut (n = fadeOutEnd - fadeOutStart): g(s) = (fadeOutEnd - 1 - s) / (n - 1)
//   n <= 1 は g = 0（1サンプルのフェード＝そのサンプルが無音）
// 半開区間（n で割る）にすると最終サンプルにゲイン 1/n が残り（n=240 で -47.6dB）、
// 段差が消え切らない。段差を消すことがこの機能の第一目的なので閉区間を採る。
namespace ClipFade
{
// 加算1回分の指示。destOffset は segPos（呼び出し側のブロック先頭の絶対位置）からのオフセット。
// ramp は区間種別で決まる（傾斜部だけ true）。ゲインの値を比べて判定しないのは、
// 平坦部を必ず `addFrom`（＝既存経路とビット一致する形）へ落としたいため
struct Segment
{
    int destOffset = 0;
    int count = 0;
    float startGain = 1.0f;
    float endGain = 1.0f;
    bool ramp = false;
};

inline constexpr int maxSegments = 3; // フェードイン部・平坦部・フェードアウト部

// 絶対位置 s のフェードインゲイン。区間の**排他端**（s == fadeInEnd）を渡すと n/(n-1) > 1 を返すが、
// これは `addFromWithRamp` の endGain に渡す値として正しい（実際には適用されない外挿値）。
// ここでクランプすると傾きが狂うのでクランプしないこと
inline float fadeInGainAt (const ClipPlayback& clip, juce::int64 s)
{
    const auto n = clip.fadeInEnd - clip.fadeInStart;
    if (n <= 1)
        return 0.0f;
    return (float) ((double) (s - clip.fadeInStart) / (double) (n - 1));
}

// 同じくフェードアウト。排他端では負になる（これも外挿値なのでクランプしない）
inline float fadeOutGainAt (const ClipPlayback& clip, juce::int64 s)
{
    const auto n = clip.fadeOutEnd - clip.fadeOutStart;
    if (n <= 1)
        return 0.0f;
    return (float) ((double) (clip.fadeOutEnd - 1 - s) / (double) (n - 1));
}

inline bool hasFadeIn (const ClipPlayback& clip)  { return clip.fadeInEnd > clip.fadeInStart; }
inline bool hasFadeOut (const ClipPlayback& clip) { return clip.fadeOutEnd > clip.fadeOutStart; }

// [overlapStart, overlapEnd) を最大3区間へ割る。戻り値 = 区間数（1..maxSegments）。
// フェードが無ければ **1区間・startGain == endGain == 1.0f** に落ちるので、呼び出し側は
// 既存の `addFrom (..., clipGain * 1.0f)` 1回へ戻れる（`x * 1.0f` は IEEE754 上 x と厳密同値
// ＝既存プロジェクトの出力とビット一致する）。
//
// 前提: overlapStart <= overlapEnd かつ [overlapStart, overlapEnd) ⊆ 連なり全体
// （＝fadeInStart <= overlapStart, overlapEnd <= fadeOutEnd）。
// appendClipPlaybacks がこれを保証する
inline int segments (const ClipPlayback& clip, juce::int64 overlapStart, juce::int64 overlapEnd,
                     juce::int64 segPos, Segment* out)
{
    const auto emit = [&] (juce::int64 from, juce::int64 to, float startGain, float endGain,
                           bool ramp, int index)
    {
        out[index] = { (int) (from - segPos), (int) (to - from), startGain, endGain, ramp };
    };

    const bool fadeIn = hasFadeIn (clip);
    const bool fadeOut = hasFadeOut (clip);
    if (! fadeIn && ! fadeOut)
    {
        emit (overlapStart, overlapEnd, 1.0f, 1.0f, false, 0);
        return 1;
    }

    // 境目は「フェードインの終わり」と「フェードアウトの始まり」の2つだけ。
    // jlimit の連鎖で overlapStart <= inEnd <= outStart <= overlapEnd を保つ
    const auto inEnd = fadeIn ? juce::jlimit (overlapStart, overlapEnd, clip.fadeInEnd) : overlapStart;
    const auto outStart = fadeOut ? juce::jlimit (inEnd, overlapEnd, clip.fadeOutStart) : overlapEnd;

    int count = 0;
    if (inEnd > overlapStart) // 傾斜（フェードイン部）
        emit (overlapStart, inEnd, fadeInGainAt (clip, overlapStart), fadeInGainAt (clip, inEnd),
              true, count++);
    if (outStart > inEnd)    // 平坦部
        emit (inEnd, outStart, 1.0f, 1.0f, false, count++);
    if (overlapEnd > outStart) // 傾斜（フェードアウト部）
        emit (outStart, overlapEnd, fadeOutGainAt (clip, outStart), fadeOutGainAt (clip, overlapEnd),
              true, count++);
    return count;
}

// 1区間をバッファの1chへ加算する。gain はフェード以外の全ゲイン（トラック・リージョン・pan・send等）。
// 平坦区間で startGain == 1.0f のときは `gain * 1.0f == gain` の addFrom 1回になる
inline void addSegment (juce::AudioBuffer<float>& dest, int destChannel, int destStartSample,
                        const float* source, const Segment& segment, float gain)
{
    if (segment.count <= 0)
        return;
    if (! segment.ramp)
        dest.addFrom (destChannel, destStartSample, source, segment.count, gain * segment.startGain);
    else
        dest.addFromWithRamp (destChannel, destStartSample, source, segment.count,
                              gain * segment.startGain, gain * segment.endGain);
}
}
