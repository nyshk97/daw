#pragma once

#include <atomic>
#include <memory>
#include <juce_audio_formats/juce_audio_formats.h>

// GOTCHAS.md パターン4: 録音のディスク書き込みは ThreadedWriter で第3のスレッドへ。
// 所有（unique_ptr、メッセージスレッド専用）と使用（atomicな生ポインタ、オーディオスレッド）を分離する。
class Recorder
{
public:
    Recorder();
    ~Recorder();

    // ---- メッセージスレッド専用 ----
    void startRecording (const juce::File& file, double sampleRate);
    void stop();
    bool isRecording() const;

    // ---- オーディオスレッド専用 ----
    void write (const float* const* inputChannelData, int numSamples);

private:
    juce::TimeSliceThread backgroundThread { "Audio Recorder Thread" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter; // 所有権（メッセージスレッド専用）
    juce::CriticalSection writerLock;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr }; // コールバックが見るのはこれだけ
};
