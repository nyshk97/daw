#pragma once

#include <memory>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../shared/PitchCorrection.h"
#include "../shared/PitchCurve.h"

// ピッチエディタの「ドラッグ中にその音が鳴る」ための軽量レンダラー（メロダインの挙動）。
// WORLD は「分解（CheapTrick/D4C・重い）」と「合成（Synthesis・軽い）」に分かれ、ドラッグで変わるのは f0 だけなので、
// ドラッグ開始時に**そのノートの範囲だけ**分解してキャッシュし、目標が変わるたびに合成だけやり直す
// （1 秒の範囲で分解 数十 ms・合成 数 ms。メッセージスレッドで同期実行できる）。
// 本レンダー（VocalResynth・クリップ全体・デバウンス）とは別経路で、結果はモデルに入れず BufferAudition で鳴らすだけ。
// 時間写像は持たない（横移動の試聴は対象外。ピッチの上下だけ）。モノ（Mid）で鳴らす
class VocalNoteAudition
{
public:
    VocalNoteAudition();
    ~VocalNoteAudition();

    // ノート範囲 [startSample, endSample) を前後 marginMs の余白込みで分解。失敗は false
    bool prepare (const juce::AudioBuffer<float>& source, const PitchCurve& curve, double sampleRate,
                  juce::int64 startSample, juce::int64 endSample, double marginMs = 120.0);
    bool isPrepared() const { return prepared; }
    juce::int64 rangeStart() const { return noteStart; }
    juce::int64 rangeEnd() const { return noteEnd; }

    // 現在の補正状態から目標カーブを作り、ノート範囲だけを合成して返す（失敗は nullptr）
    std::shared_ptr<const juce::AudioBuffer<float>> render (const PitchCorrection& correction, const PitchCurve& curve,
                                                            juce::int64 domainOffset, juce::int64 domainLength,
                                                            int transposeSemitones);
    void reset();

private:
    struct Analysis;
    std::unique_ptr<Analysis> analysis;
    bool prepared = false;
    juce::int64 noteStart = 0, noteEnd = 0;
    double sr = 0.0;

    JUCE_DECLARE_NON_COPYABLE (VocalNoteAudition)
};
