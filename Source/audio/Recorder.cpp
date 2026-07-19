#include "Recorder.h"

Recorder::Recorder()
{
    // 忘れるとコンパイルは通り write() も成功し続けるが、ディスクに何も書かれない
    backgroundThread.startThread();
}

Recorder::~Recorder()
{
    stop();
}

void Recorder::startRecording (const juce::File& file, double sampleRate)
{
    stop(); // 必ず先に前回分を止める

    if (sampleRate <= 0.0)
        return;

    file.deleteFile();

    if (std::unique_ptr<juce::OutputStream> fileStream { file.createOutputStream() })
    {
        juce::WavAudioFormat wavFormat;
        using Opts = juce::AudioFormatWriterOptions; // JUCE 8.0.9で導入されたAPI

        if (auto writer = wavFormat.createWriterFor (fileStream,
                Opts{}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (16)))
        {
            threadedWriter.reset (new juce::AudioFormatWriter::ThreadedWriter (
                writer.release(), backgroundThread, 32768)); // 32768 = 内部FIFOのサンプル数
            const juce::ScopedLock sl (writerLock);
            activeWriter = threadedWriter.get(); // ポインタの差し替えだけをロック内で
        }
    }
}

void Recorder::stop()
{
    // 1. まずactiveWriterをnullにして、コールバックにwriterを使わせなくする
    {
        const juce::ScopedLock sl (writerLock);
        activeWriter = nullptr;
    }
    // 2. その後、ロックの外でwriterを破棄する。
    //    reset()は残データのディスクflushを待つため時間がかかる。
    //    これをロック内でやるとその間オーディオコールバックがブロックされる
    threadedWriter.reset();
}

bool Recorder::isRecording() const
{
    return activeWriter.load() != nullptr;
}

void Recorder::write (const float* const* inputChannelData, int numSamples)
{
    // atomicのnullチェックだけでは reset() とのレースで use-after-free になるためロックが必須。
    // 相手側（メッセージスレッド）がロック内で行うのはポインタ代入のみなので待ち時間は有界
    const juce::ScopedLock sl (writerLock);
    if (activeWriter.load() != nullptr)
        activeWriter.load()->write (inputChannelData, numSamples);
}
