#include "Recorder.h"

#include "../shared/Log.h"

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

    // ここはメッセージスレッドなので直接ログしてよい（write()からは書かない）
    if (sampleRate <= 0.0)
    {
        Log::warn ("record.start_failed", "reason=invalid_samplerate sr=" + juce::String (sampleRate));
        return;
    }

    file.deleteFile();

    std::unique_ptr<juce::OutputStream> fileStream { file.createOutputStream() };
    if (fileStream == nullptr)
    {
        Log::warn ("record.start_failed", "reason=open_stream file=" + file.getFullPathName());
        return;
    }

    juce::WavAudioFormat wavFormat;
    using Opts = juce::AudioFormatWriterOptions; // JUCE 8.0.9で導入されたAPI

    auto writer = wavFormat.createWriterFor (fileStream,
        Opts{}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24));
    if (writer == nullptr)
    {
        Log::warn ("record.start_failed", "reason=create_writer file=" + file.getFullPathName()
                                              + " sr=" + juce::String (sampleRate));
        return;
    }

    threadedWriter.reset (new juce::AudioFormatWriter::ThreadedWriter (
        writer.release(), backgroundThread, 32768)); // 32768 = 内部FIFOのサンプル数
    const juce::ScopedLock sl (writerLock);
    activeWriter = threadedWriter.get(); // ポインタの差し替えだけをロック内で
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

bool Recorder::write (const float* const* inputChannelData, int numSamples)
{
    // atomicのnullチェックだけでは reset() とのレースで use-after-free になるためロックが必須。
    // 相手側（メッセージスレッド）がロック内で行うのはポインタ代入のみなので待ち時間は有界
    const juce::ScopedLock sl (writerLock);
    if (activeWriter.load() == nullptr)
        return true;
    return activeWriter.load()->write (inputChannelData, numSamples);
}
