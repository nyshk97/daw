#pragma once

#include <atomic>
#include <limits>
#include <juce_core/juce_core.h>

// GOTCHAS.md パターン1: 単一値は std::atomic で渡す。
// UI→オーディオはフラグ・リクエスト値、オーディオ→UIは再生位置・レベルを渡す。
struct TransportState
{
    static constexpr juce::int64 kNoSeek = std::numeric_limits<juce::int64>::min();

    // ---- 再生 ----
    std::atomic<bool> isPlaying { false };
    std::atomic<juce::int64> playheadSamplePos { 0 }; // カウントイン中は負になりうる
    std::atomic<juce::int64> seekRequest { kNoSeek }; // UI→オーディオ。適用はオーディオスレッド側で行う
    std::atomic<double> bpm { 120.0 };                // 曲中固定・拍子4/4固定
    std::atomic<bool> clickEnabled { false };

    // ---- 録音（punchInSample 以降のサンプルだけがディスクに書かれる）----
    std::atomic<bool> recordArmed { false };
    std::atomic<juce::int64> punchInSample { 0 };
    std::atomic<juce::int64> recordedSamples { 0 };   // 実際に書いたサンプル数（録音中クリップの描画用）

    // ---- オーディオ→UI ----
    std::atomic<double> sampleRate { 0.0 };           // prepareToPlay で確定した実サンプルレート
    std::atomic<int> blockSizeExpected { 0 };         // prepareToPlay で通知されたブロックサイズ（SynthBankの確保基準）
    std::atomic<int> midiDroppedNoteOns { 0 };        // MIDIイベント上限超過で捨てたノートオン数。UIのTimerが集約ログして0に戻す
    std::atomic<int> recordDroppedBlocks { 0 };       // 録音FIFO満杯で捨てたブロック数。UIのTimerが集約ログして0に戻す

    // lock-free でない型が紛れ込むとミューテックスにフォールバックして禁止事項違反になる
    static_assert (std::atomic<int>::is_always_lock_free);
    static_assert (std::atomic<bool>::is_always_lock_free);
    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<double>::is_always_lock_free);
    static_assert (std::atomic<juce::int64>::is_always_lock_free);
};
