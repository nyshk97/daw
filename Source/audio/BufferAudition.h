#pragma once

#include <atomic>
#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>

// ピッチエディタの「ブロブクリックで単独試聴」。メモリ内の不変バッファ（effectiveDomain の audio）の
// render 範囲を、ファイル試聴（AudioFilePreview）と同じ post-master の経路で鳴らす専用 audition。
// AudioFilePreview はファイル先頭からのデコード専用で流用できないため、交換・ミックスの構造だけ写した。
// start()/stop() はメッセージスレッド、mixInto() はオーディオスレッド専用。
// 排他規則（plan）: ファイルブラウザ試聴とは相互排他（後勝ち・呼び出し側で止める）、メイン再生中は
// 呼ばない（再生を止めない）、録音中は無効
class BufferAudition final
{
public:
    BufferAudition() = default;
    ~BufferAudition();

    // audio の [start, start + length) を鳴らす（sampleRate はバッファの SR）。前の試聴は置き換える
    void start (std::shared_ptr<const juce::AudioBuffer<float>> audio, juce::int64 start, juce::int64 length,
                double sampleRate);
    void stop();
    bool isPlaying() const { return playingGeneration.load() != 0; }
    void deleteRetired(); // メッセージスレッドから定期的に（Timer）

    void prepareToPlay (double deviceSampleRate) { outputSampleRate = deviceSampleRate; }
    void releaseResources() { outputSampleRate = 0.0; }
    void mixInto (juce::AudioBuffer<float>& output, int startSample, int numSamples);

private:
    struct Snapshot
    {
        std::shared_ptr<const juce::AudioBuffer<float>> audio; // 強参照（試聴中に消えない）
        juce::int64 start = 0, length = 0;
        double sampleRate = 0.0;
        juce::uint64 generation = 0;
    };
    std::atomic<Snapshot*> pending { nullptr };
    std::atomic<Snapshot*> retired { nullptr };
    std::atomic<Snapshot*> current { nullptr };
    std::atomic<juce::uint64> playingGeneration { 0 };
    juce::uint64 nextGeneration = 1;
    juce::uint64 audioGeneration = 0;
    double outputSampleRate = 0.0;
    double sourcePosition = 0.0;
    static constexpr float gain = 0.8f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BufferAudition)
};
