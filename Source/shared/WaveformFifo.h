#pragma once

#include <algorithm>
#include <juce_audio_basics/juce_audio_basics.h>

// GOTCHAS.md パターン2: juce::AbstractFifo で波形データを渡す。
// シングルライター（オーディオスレッド）・シングルリーダー（UIスレッドのTimer）専用。
class WaveformFifo
{
public:
    // オーディオスレッドから呼ぶ。ロックもアロケーションもしない
    void push (const float* data, int numSamples)
    {
        const auto scope = fifo.write (numSamples); // ScopedWrite: スコープ終了時にfinishedWrite相当が走る
        if (scope.blockSize1 > 0)
            buffer.copyFrom (0, scope.startIndex1, data, scope.blockSize1);
        if (scope.blockSize2 > 0)
            buffer.copyFrom (0, scope.startIndex2, data + scope.blockSize1, scope.blockSize2);
        // FIFOが満杯なら blockSize1+blockSize2 < numSamples になり、余りは黙って捨てる。
        // オーディオスレッドを待たせないためのトレードオフで、これで正しい
    }

    // UIスレッド（Timerコールバック）から呼ぶ
    int pull (float* dest, int maxSamples)
    {
        const auto scope = fifo.read (juce::jmin (maxSamples, fifo.getNumReady()));
        if (scope.blockSize1 > 0)
            std::copy_n (buffer.getReadPointer (0, scope.startIndex1), scope.blockSize1, dest);
        if (scope.blockSize2 > 0)
            std::copy_n (buffer.getReadPointer (0, scope.startIndex2), scope.blockSize2, dest + scope.blockSize1);
        return scope.blockSize1 + scope.blockSize2;
    }

private:
    static constexpr int capacity = 1 << 15; // 電源投入時に確保し、以後サイズ変更しない
    juce::AbstractFifo fifo { capacity };
    juce::AudioBuffer<float> buffer { 1, capacity };
};
