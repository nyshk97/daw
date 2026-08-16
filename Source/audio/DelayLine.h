#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

// 共有ディレイライン（バスDelay本体と BusReverb の pre-delay で共用）。
// plan: docs/plans/2026-08-16-2305-fx-batch4-delay-reverb.md
//
// 容量契約: prepare (sampleRate, maxDelaySeconds) が「maxDelaySeconds × SR ＋ マージン」を
// 2の冪へ切り上げて確保する。サンプル数固定にしないのは高SRデバイスで秒数が不足するため
// （必要最大長はバスDelayの 1/2音符 × BPM下限30 = 4秒）。
// 読み書きは整数サンプルタップのみ。テンポ同期Delay・pre-delayは整数遅延で足り、
// 補間が要る変調ディレイ（Lo-fiのWow）は固定スタックリング＋エルミート補間が要件なので
// 共通化しない（TrackLofi 側のまま。plan ログ参照）。
//
// スレッド契約: prepare() はメッセージ/ワーカースレッド専用（ヒープ確保）。
// write() / read() / clear() は確保なしでRT安全。
class DelayLine
{
public:
    void prepare (double sampleRate, double maxDelaySeconds)
    {
        const auto needed = (int) std::ceil (sampleRate * maxDelaySeconds) + 8;
        capacityValue = juce::nextPowerOfTwo (needed);
        mask = capacityValue - 1;
        buffer.assign ((size_t) capacityValue, 0.0f);
        writeIndex = 0;
    }

    void clear()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    // delaySamples 回前に write() した値を返す（0 は未定義: 呼び出し側が入力そのものを使う）。
    // 有効範囲は 1..capacity()。呼び出し側がクランプすること
    float read (int delaySamples) const noexcept
    {
        return buffer[(size_t) ((writeIndex - delaySamples) & mask)];
    }

    void write (float value) noexcept
    {
        buffer[(size_t) writeIndex] = value;
        writeIndex = (writeIndex + 1) & mask;
    }

    int capacity() const noexcept { return capacityValue; }

private:
    std::vector<float> buffer;
    int capacityValue = 0;
    int mask = 0;
    int writeIndex = 0;
};
