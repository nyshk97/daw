#include "RegionExport.h"

namespace
{
constexpr int chunkSamples = 8192;

// 区間をミックスしながら1チャンク読む。戻り値は実際に埋めたサンプル数
void mixChunk (std::vector<std::unique_ptr<juce::AudioFormatReader>>& readers,
               const std::vector<RegionExport::Source>& sources,
               juce::int64 position, int numSamples,
               juce::AudioBuffer<float>& mix, juce::AudioBuffer<float>& temp)
{
    mix.clear (0, 0, numSamples);
    mix.clear (1, 0, numSamples);
    for (size_t i = 0; i < readers.size(); ++i)
    {
        temp.clear (0, 0, numSamples);
        temp.clear (1, 0, numSamples);
        readers[i]->read (&temp, 0, numSamples, position, true, true);
        mix.addFrom (0, 0, temp, 0, 0, numSamples, sources[i].gain);
        mix.addFrom (1, 0, temp, 1, 0, numSamples, sources[i].gain);
    }
}
} // namespace

namespace RegionExport
{
Result renderMix (const std::vector<Source>& sources, juce::int64 startSample, juce::int64 endSample,
                  double sampleRate, const juce::File& outFile,
                  const std::function<bool()>& shouldCancel)
{
    Result result;
    const auto cancelled = [&shouldCancel] { return shouldCancel != nullptr && shouldCancel(); };
    if (sources.empty() || endSample <= startSample || sampleRate <= 0.0)
    {
        result.error = juce::String::fromUTF8 (u8"書き出す内容がありません");
        return result;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    for (const auto& s : sources)
    {
        std::unique_ptr<juce::AudioFormatReader> r (formatManager.createReaderFor (s.file));
        if (r == nullptr)
        {
            result.error = juce::String::fromUTF8 (u8"ソースを開けません: ") + s.file.getFileName();
            return result;
        }
        readers.push_back (std::move (r));
    }

    juce::AudioBuffer<float> mix (2, chunkSamples), temp (2, chunkSamples);

    // 1パス目: ピーク測定
    float peak = 0.0f;
    for (auto pos = startSample; pos < endSample; pos += chunkSamples)
    {
        if (cancelled())
        {
            result.error = juce::String::fromUTF8 (u8"キャンセルされました");
            return result;
        }
        const int n = (int) juce::jmin ((juce::int64) chunkSamples, endSample - pos);
        mixChunk (readers, sources, pos, n, mix, temp);
        peak = juce::jmax (peak, mix.getMagnitude (0, n));
    }

    // M/S加算後のピークが0dBFSを超える場合のみ、収まるまで全体ゲインを下げる
    result.appliedGain = peak > 1.0f ? 1.0f / peak : 1.0f;

    // 2パス目: 24bit書き込み（一時ファイル→成功時atomic rename）
    const auto tmpFile = outFile.getSiblingFile (
        "." + outFile.getFileNameWithoutExtension() + ".tmp-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)) + ".wav");
    {
        std::unique_ptr<juce::OutputStream> stream = tmpFile.createOutputStream();
        if (stream == nullptr)
        {
            result.error = juce::String::fromUTF8 (u8"書き出し先に書き込めません");
            return result;
        }
        juce::WavAudioFormat wav;
        using Opts = juce::AudioFormatWriterOptions;
        auto writer = wav.createWriterFor (
            stream, Opts {}.withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (24));
        if (writer == nullptr)
        {
            tmpFile.deleteFile();
            result.error = juce::String::fromUTF8 (u8"WAVライターを作成できません");
            return result;
        }
        for (auto pos = startSample; pos < endSample; pos += chunkSamples)
        {
            if (cancelled())
            {
                writer.reset();
                tmpFile.deleteFile(); // キャンセルでも一時ファイルを残さない
                result.error = juce::String::fromUTF8 (u8"キャンセルされました");
                return result;
            }
            const int n = (int) juce::jmin ((juce::int64) chunkSamples, endSample - pos);
            mixChunk (readers, sources, pos, n, mix, temp);
            if (! juce::exactlyEqual (result.appliedGain, 1.0f))
                mix.applyGain (0, n, result.appliedGain);
            if (! writer->writeFromAudioSampleBuffer (mix, 0, n))
            {
                writer.reset();
                tmpFile.deleteFile(); // 途中失敗で壊れた成果物を残さない
                result.error = juce::String::fromUTF8 (u8"書き込みに失敗しました（ディスク容量を確認）");
                return result;
            }
        }
    } // writerがヘッダを確定

    if (! tmpFile.moveFileTo (outFile))
    {
        tmpFile.deleteFile();
        result.error = juce::String::fromUTF8 (u8"書き出しファイルの確定に失敗しました");
        return result;
    }

    result.ok = true;
    return result;
}
} // namespace RegionExport

bool ExportWorker::startExport (std::vector<RegionExport::Source> newSources, juce::int64 newStart,
                                juce::int64 newEnd, double newSampleRate, const juce::File& newOutFile)
{
    if (busy.load() || isThreadRunning())
        return false;
    sources = std::move (newSources);
    startSample = newStart;
    endSample = newEnd;
    sampleRate = newSampleRate;
    outFile = newOutFile;
    done.store (false);
    busy.store (true);
    startThread();
    return true;
}

bool ExportWorker::consumeResult (RegionExport::Result& outResult, juce::File& outFileResult)
{
    if (! done.load() || isThreadRunning())
        return false;
    done.store (false);
    outResult = result;
    outFileResult = outFile;
    return true;
}

void ExportWorker::run()
{
    result = RegionExport::renderMix (sources, startSample, endSample, sampleRate, outFile,
                                      [this] { return threadShouldExit(); });
    busy.store (false);
    done.store (true);
}
