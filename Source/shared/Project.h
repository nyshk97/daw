#pragma once

#include <memory>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>

#include "PlaybackSnapshot.h"

// メッセージスレッドが所有するデータモデル。オーディオスレッドへは
// buildSnapshot() で作った PlaybackSnapshot を SnapshotExchange 経由で渡す。

struct Clip
{
    static constexpr int samplesPerPeak = 512; // 描画用ピークキャッシュの集約単位

    juce::String fileName;      // プロジェクトフォルダ相対（例: clip-001.wav）
    juce::int64 startSample = 0;
    std::shared_ptr<juce::AudioBuffer<float>> audio; // モノラル・メモリ常駐
    std::vector<float> peakCache;                    // samplesPerPeak ごとの絶対値ピーク

    juce::int64 lengthSamples() const { return audio != nullptr ? audio->getNumSamples() : 0; }
    void buildPeakCache();
};

struct Track
{
    juce::String name;
    std::shared_ptr<TrackParams> params = std::make_shared<TrackParams>();
    std::vector<Clip> clips;
};

class Project
{
public:
    static constexpr int currentVersion = 1;

    juce::File directory;
    double bpm = 120.0;
    double sampleRate = 0.0; // 0 = 未確定（最初の録音時にデバイスレートで確定）
    std::vector<Track> tracks;

    juce::String name() const { return directory.getFileName(); }

    // project.json 書き出し＋未参照 clip-*.wav のGC。失敗時は error にメッセージ
    bool save (juce::String& error);

    static std::unique_ptr<Project> load (const juce::File& dir,
                                          juce::StringArray& warnings,
                                          juce::String& error);
    static std::unique_ptr<Project> createNew (const juce::File& dir, juce::String& error);

    juce::File nextClipFile() const; // clip-NNN.wav の空き連番
    std::unique_ptr<PlaybackSnapshot> buildSnapshot() const;

    static juce::File projectsRoot(); // ~/Music/daw
    static std::shared_ptr<juce::AudioBuffer<float>> loadWavMono (const juce::File& file);
};
