#pragma once

#include <atomic>
#include <juce_core/juce_core.h>

// GOTCHAS.md パターン1: 単一値は std::atomic で渡す。
// オーディオスレッドが書き、UIスレッドが Timer（30〜60Hz）で読む。
struct SharedAudioState
{
    std::atomic<float> peakLevel { 0.0f };              // 入力のピークレベル（メーター用）
    std::atomic<juce::int64> playheadSamplePos { 0 };   // 再生位置（サンプル数）
    std::atomic<double> sampleRate { 0.0 };             // prepareToPlay で確定した実サンプルレート

    // lock-free でない型が紛れ込むとミューテックスにフォールバックして禁止事項違反になる
    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<juce::int64>::is_always_lock_free);
    static_assert (std::atomic<double>::is_always_lock_free);
};
