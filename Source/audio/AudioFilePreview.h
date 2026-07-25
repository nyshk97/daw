#pragma once

#include <atomic>
#include <memory>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

// 右ファイルブラウザの試聴。デコードはワーカー、mixInto() はオーディオスレッド専用。
// Snapshotの破棄はdeleteRetired()を呼ぶメッセージスレッドに限定する。
class AudioFilePreview final : private juce::Thread
{
public:
    enum class Status { idle, loading, playing, failed };

    AudioFilePreview();
    ~AudioFilePreview() override;

    bool start (const juce::File& file);
    void stop();
    void cancelAndWait();

    Status status() const { return currentStatus.load(); }
    juce::String takeError();
    void deleteRetired();

    void prepareToPlay (double deviceSampleRate);
    void releaseResources();
    void mixInto (juce::AudioBuffer<float>& output, int startSample, int numSamples);

private:
    struct Snapshot
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 0.0;
        juce::uint64 generation = 0;
    };

    class Exchange
    {
    public:
        ~Exchange();
        void push (std::unique_ptr<Snapshot> snapshot);
        void deleteRetired();
        Snapshot* acquire();

    private:
        std::atomic<Snapshot*> pending { nullptr };
        std::atomic<Snapshot*> retired { nullptr };
        std::atomic<Snapshot*> current { nullptr };
    };

    void run() override;
    void setError (const juce::String& message);

    Exchange exchange;
    juce::File requestedFile;
    juce::CriticalSection requestLock;
    juce::CriticalSection errorLock;
    juce::String errorMessage;

    std::atomic<Status> currentStatus { Status::idle };
    std::atomic<juce::uint64> requestedGeneration { 0 };
    std::atomic<juce::uint64> playingGeneration { 0 };
    double outputSampleRate = 0.0;
    juce::uint64 audioGeneration = 0;
    double sourcePosition = 0.0;

    static constexpr float previewGain = 0.5f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioFilePreview)
};
