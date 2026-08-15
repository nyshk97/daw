#include "SalvaRecorder.h"

SalvaRecorder::SalvaRecorder()
{
    backgroundThread.startThread(); // 忘れるとFIFOが満杯になるまで静かにデータが落ちる（GOTCHAS）
}

SalvaRecorder::~SalvaRecorder()
{
    stop();
    backgroundThread.stopThread (2000);
}

bool SalvaRecorder::start (const juce::File& file, double sampleRate)
{
    stop(); // 必ず先に前回分を止める

    // 既存ファイルはunlinkせずtruncateで上書きする。deleteFile→再作成だと、
    // TakeName::claimTargetFileがO_EXCLで確保した名前が一瞬空き、同じ保存先を使う
    // 別プロセス（Salva/Salva-dev並走）に同名を取られて先発テイクを失う窓ができる
    auto fileStream = file.createOutputStream(); // 無ければ作成、あればそのまま開く
    if (fileStream == nullptr)
        return false;
    fileStream->setPosition (0);
    if (fileStream->truncate().failed())
        return false;
    std::unique_ptr<juce::OutputStream> stream = std::move (fileStream);

    juce::WavAudioFormat wavFormat;
    using Opts = juce::AudioFormatWriterOptions;
    auto writer = wavFormat.createWriterFor (
        stream, Opts {}.withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (24));
    if (writer == nullptr)
        return false;

    // FIFO容量=1秒相当（ステレオfloatで約384KB@48kHz）。numSamplesToBufferは
    // チャンネル共通の時間軸サンプル数（内部で AudioBuffer(channels, numSamples) 確保）
    const int fifoSamples = juce::roundToInt (sampleRate);
    threadedWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
        writer.release(), backgroundThread, fifoSamples);
    // 1秒ごとにヘッダ更新（クラッシュしても直前flushまでが有効なWAVとして残る）
    threadedWriter->setFlushInterval (juce::roundToInt (sampleRate));

    currentFile = file;
    currentSampleRate = sampleRate;
    attempted.store (0);
    dropped.store (0);

    const juce::ScopedLock sl (writerLock);
    activeWriter = threadedWriter.get(); // ポインタの差し替えだけをロック内で
    return true;
}

juce::int64 SalvaRecorder::stop()
{
    // 1. まずactiveWriterをnullにしてコールバックにwriterを使わせなくする
    {
        const juce::ScopedLock sl (writerLock);
        activeWriter = nullptr;
    }
    // 2. ロックの外で破棄する（reset()は残データのflushを待つ。ロック内でやると
    //    その間オーディオコールバックがブロックされる）
    if (threadedWriter == nullptr)
        return -1;
    threadedWriter.reset();

    // ヘッダ確定後の実サンプル長を読み戻す（期待値との照合は呼び出し側）
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wavFormat.createReaderFor (currentFile.createInputStream().release(), true));
    return reader != nullptr ? reader->lengthInSamples : -1;
}

void SalvaRecorder::pushBlock (const float* const* input, int numChannels, int numSamples)
{
    if (numChannels <= 0 || numSamples <= 0)
        return;

    const float* left = input[0];
    const float* right = numChannels > 1 ? input[1] : input[0]; // モノラル入力は両chへ複製

    // 入力レベル（録音していなくても更新＝レベル合わせに使う）。ピークは絶対値で取る
    const auto lr = juce::FloatVectorOperations::findMinAndMax (left, numSamples);
    const auto rr = juce::FloatVectorOperations::findMinAndMax (right, numSamples);
    peaks[0].store (juce::jmax (std::abs (lr.getStart()), std::abs (lr.getEnd())));
    peaks[1].store (juce::jmax (std::abs (rr.getStart()), std::abs (rr.getEnd())));

    const juce::ScopedLock sl (writerLock); // 破棄側を待たせるための短いロック（GOTCHAS: 折り合い）
    if (auto* writer = activeWriter.load())
    {
        attempted.fetch_add (numSamples); // ドロップしても録音時間は進んでいる
        const float* channels[2] = { left, right };
        if (! writer->write (channels, numSamples))
            dropped.fetch_add (1); // FIFO満杯。停止時にRecordingCheckで表面化する
    }
}
