#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "Recorder.h"
#include "../shared/TransportState.h"
#include "../shared/PlaybackSnapshot.h"

// サンプル位置ベースの自前ミックスエンジン。process() はオーディオスレッドで走る。
// クリップ/トラック構成は SnapshotExchange 経由で受け取り、単一値は TransportState の atomic を読む。
// UIへの直接参照は持たない。
class PlaybackEngine
{
public:
    PlaybackEngine (TransportState& transportState, SnapshotExchange& snapshotExchange);

    // ---- AudioAppComponent から転送される（processのみオーディオスレッド）----
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate);
    void process (const juce::AudioSourceChannelInfo& bufferToFill);
    void releaseResources();

    // ---- メッセージスレッド専用 ----
    void play();
    void stop();

    // カウントイン付き録音。punchInSample 以降の入力だけがファイルに書かれる。
    // 再生ヘッドは punchInSample - countInSamples に移動して再生が始まる（既存クリップも鳴る）
    bool startRecording (const juce::File& file, juce::int64 punchInSample,
                         juce::int64 countInSamples, double deviceSampleRate);
    void stopRecording(); // 残データのflushまで待つ（メッセージスレッドなのでブロックしてよい）
    bool isRecording() const;

private:
    TransportState& transport;
    SnapshotExchange& snapshots;
    Recorder recorder;

    double currentSampleRate = 0.0;

    // ---- 以下はオーディオスレッド専用の状態 ----
    bool prevPlaying = false;
    juce::int64 lastBeatIndex = 0;
    double clickPhase = 0.0;
    double clickFreq = 880.0;
    float clickAmp = 0.0f;
    int clickSamplesLeft = 0;
    int clickTotalSamples = 1;
};
