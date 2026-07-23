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

    // ---- サイクル（ループ範囲）----
    // 範囲は [start, end) のサンプル区間。開始・終了は1つのatomicにパックする
    // （上位32bit=開始・下位32bit=終了。uint32は48kHzで約24時間まで表現できる）。
    // 個別atomic2本だと編集途中の「新開始＋旧終了」をオーディオスレッドが読み得るため。
    // UI側の書き込み順は「範囲を書いてから cycleEnabled を立てる／先に落としてから範囲を消す」に固定する
    std::atomic<juce::uint64> cycleRange { 0 };
    std::atomic<bool> cycleEnabled { false };

    static juce::uint64 packCycle (juce::int64 startSample, juce::int64 endSample)
    {
        const auto clamp32 = [] (juce::int64 v)
        { return (juce::uint64) juce::jlimit ((juce::int64) 0, (juce::int64) 0xffffffff, v); };
        return (clamp32 (startSample) << 32) | clamp32 (endSample);
    }
    static juce::int64 cycleStartOf (juce::uint64 packed) { return (juce::int64) (packed >> 32); }
    static juce::int64 cycleEndOf (juce::uint64 packed)   { return (juce::int64) (packed & 0xffffffffu); }

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
    static_assert (std::atomic<juce::uint64>::is_always_lock_free);
};
