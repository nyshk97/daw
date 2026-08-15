#pragma once

#include <atomic>

#include <juce_audio_formats/juce_audio_formats.h>

// レコード録音のディスク書き込み（GOTCHAS.md パターン4の4点セットをそのまま踏襲）。
//
// - ステレオ24bit WAV・サンプルレートはデバイス追従
// - 開始時にファイルを開き、以後ストリーミング書き込み（20分級はメモリ保持不可）
// - `ThreadedWriter` の内部FIFO=1秒相当＋ `setFlushInterval`=1秒の定期ヘッダ更新で、
//   クラッシュ時も直前flushまでのデータが有効なWAVとして残る（損失上限 ≒ flush間隔＋FIFO容量 = 2秒）
// - FIFO満杯（write()==false）はatomicカウンタに記録し、停止時に期待サンプル数と
//   実WAV長を照合する（shared/RecordingCheck.h）
//
// スレッド: start/stop はメッセージスレッド、pushBlock はオーディオスレッド
// （テストはpushBlockを直接呼んで同期駆動する）
class SalvaRecorder
{
public:
    SalvaRecorder();
    ~SalvaRecorder();

    // 録音開始（24bitステレオ・sampleRate）。既存ファイルは上書き。成功でtrue
    bool start (const juce::File& file, double sampleRate);

    // 停止してヘッダ確定。確定後のWAVの実サンプル長を返す（開けなければ-1、未録音なら-1）
    juce::int64 stop();

    bool isRecording() const { return activeWriter.load() != nullptr; }
    const juce::File& recordedFile() const { return currentFile; }
    double recordingSampleRate() const { return currentSampleRate; }

    // コールバックが書こうとした総サンプル数（ドロップ込み＝録音時間の真実）
    juce::int64 attemptedSamples() const { return attempted.load(); }
    juce::uint64 droppedWrites() const { return dropped.load(); }

    // 入力レベル（リニア振幅の直近ブロックピーク。録音していなくても更新される＝入力の当たり確認用）
    float peakL() const { return peaks[0].load(); }
    float peakR() const { return peaks[1].load(); }

    // オーディオスレッドから呼ぶ: 入力ブロックを書き込む＋ピーク測定。
    // numChannels==1 のときは両chへ複製する
    void pushBlock (const float* const* input, int numChannels, int numSamples);

private:
    juce::TimeSliceThread backgroundThread { "salva recorder disk" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter; // 所有（メッセージスレッド専用）
    juce::CriticalSection writerLock;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr }; // コールバックが見るのはこれだけ

    std::atomic<juce::int64> attempted { 0 };
    std::atomic<juce::uint64> dropped { 0 };
    std::atomic<float> peaks[2] { 0.0f, 0.0f };

    juce::File currentFile;
    double currentSampleRate = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SalvaRecorder)
};
