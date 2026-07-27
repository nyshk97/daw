#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

// 曲末フェードアウト（マスターフェード）。プロジェクトに1本だけ持つ。
//
// オートメーションの一般形ではなく「曲末の1本」に絞った専用機能（docs/design/region-settings.md）。
// マスターフェーダー値に**掛け算**で乗るだけなのでフェード区間でもフェーダーは普通に効き、
// Read/Touch/Latch/Write/Off のようなモードが要らない。
//
// 位置は Project 側で16分音符単位（サイクル範囲と同じ流儀）で保持し、サンプルへの換算は
// 再生・書き出しのたびにここで行う。スナップショットにサンプル位置を焼き込まないのは、
// BPM変更（MainComponent::applyBpmText）もSR変更もスナップショットを再pushしないため、
// 焼き込むと「表示は新しい小節位置へ動くのに音は古い位置でフェードする」状態になるから。
namespace SongFade
{
// 16分音符 → サンプル位置。RT（PlaybackEngine）とオフライン（BounceRenderer）は必ずこれを通す。
// 片方だけ別式で書くと再生とバウンスの出力一致が崩れる（GOTCHAS.md「PPQ⇄サンプル換算の丸め」）。
// bpmのクランプ範囲は TimelineView::barLengthSamples() と揃える（表示と音の位置をずらさない）
inline juce::int64 sixteenthsToSamples (int sixteenths, double bpm, double sampleRate)
{
    if (sixteenths <= 0 || sampleRate <= 0.0)
        return 0;

    const double barLen = sampleRate * 60.0 / juce::jlimit (20.0, 400.0, bpm) * 4.0; // 4/4固定
    return (juce::int64) std::llround ((double) sixteenths * barLen / 16.0);
}

// フェードのゲイン（S字＝raised cosine。両端で傾きが0になるので、フェードの開始に
// 気づかれにくく無音への着地も目立たない）。リージョンフェードのリニア固定とは別カーブで、
// あちらは20〜50msで知覚差が出ないのに対しこちらは10〜30秒あり明確に差が出る。
//
// ⚠️ 判定順を変えないこと。`end <= start` を先頭に置かないと、未設定（0/0）のプロジェクトで
// `pos >= end` が先に成立してしまい全出力が無音になる。
//
// オーディオスレッドから呼ばれる（確保・ロックなし）
inline float gainAt (juce::int64 pos, juce::int64 startSample, juce::int64 endSample)
{
    if (endSample <= startSample)
        return 1.0f; // 未設定・不正区間はユニティ
    if (pos <= startSample)
        return 1.0f;
    if (pos >= endSample)
        return 0.0f;

    const double t = (double) (pos - startSample) / (double) (endSample - startSample);
    return (float) (0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * t)));
}
}
