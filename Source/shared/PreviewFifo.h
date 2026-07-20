#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// ピアノロールのプレビュー発音コマンドをUI→オーディオへ渡すロックフリーFIFO。
// GOTCHAS.md パターン2と同じ AbstractFifo＋事前確保バッファ（シングルライター・シングルリーダー）。
// ノートオフはオーディオ側が固定発音長をサンプルカウントして自動送出するため、コマンドはオンと全消音のみ。
class PreviewFifo
{
public:
    struct Command
    {
        enum class Type { noteOn, allNotesOff };
        Type type = Type::noteOn;
        juce::uint64 trackId = 0;
        int pitch = 60;
        int velocity = 100;
    };

    // UIスレッド専用。満杯なら黙って捨てる（プレビューなので取りこぼし許容）
    void push (const Command& command)
    {
        const auto scope = fifo.write (1);
        if (scope.blockSize1 > 0)
            buffer[(size_t) scope.startIndex1] = command;
    }

    // オーディオスレッド専用
    bool pop (Command& command)
    {
        const auto scope = fifo.read (1);
        if (scope.blockSize1 == 0)
            return false;
        command = buffer[(size_t) scope.startIndex1];
        return true;
    }

private:
    static constexpr int capacity = 256;
    juce::AbstractFifo fifo { capacity };
    Command buffer[capacity];
};
