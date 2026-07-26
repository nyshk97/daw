#include "AudioImporter.h"

#include <cmath>

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

constexpr int chunkFrames = 32768; // 1回の変換・書き込み単位（キャンセル応答性と進捗の粒度）
}

void AudioImporter::run()
{
    const auto st = decodeAndWrite();
    result.status = st;

    if (st != Status::success)
        request.tempFile.deleteFile(); // 中断・失敗で書きかけを残さない

    request = {};
    currentStatus.store (st); // 最後に更新（呼び出し側はstatusを見てからresultを読む）
}

AudioImporter::Status AudioImporter::decodeAndWrite()
{
    request.tempFile.deleteFile();

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats(); // macではCoreAudio経由でMP3/M4Aも読める

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (request.sourceFile));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        result.errorMessage = jp (u8"対応していない形式か、壊れたファイルです: ")
                              + request.sourceFile.getFileName();
        return Status::failed;
    }
    if (reader->lengthInSamples > maxInputFrames)
    {
        result.errorMessage = jp (u8"ファイルが長すぎます（30分程度まで）: ")
                              + request.sourceFile.getFileName();
        return Status::failed;
    }

    const int numChannels = reader->numChannels >= 2 ? 2 : 1; // 3ch以上は先頭2chのみ
    const auto inputFrames = (juce::int64) reader->lengthInSamples;
    const double sourceRate = reader->sampleRate;
    // targetSampleRate <= 0 は「元のSRを保つ」。0のまま以降の計算へ流すと出力フレーム数も
    // writerのSRも0になるため、ここで実効値へ畳んで下流すべてでこの値を使う
    const double targetRate = request.targetSampleRate > 0.0 ? request.targetSampleRate : sourceRate;
    result.numChannels = numChannels;
    result.sourceSampleRate = sourceRate;

    // デコードは全量メモリへ（maxInputFramesで有界。クリップは再生時もメモリ常駐なので同規模）。
    // 末尾に tailPadding 分のゼロを確保する: リサンプル時、レイテンシ補償で入力実長を超えて
    // 消費が進む（最大で約100入力サンプル＋端数）ため、補間器へ渡すポインタと available を
    // 常にバッファ実体の範囲内に収める。JUCEの process(available, wrap) は available==0 でも
    // 先頭1サンプルを先読みするため（interpolateImplが境界判定より先に input を読む）、
    // available を 0 にしない設計が必要
    constexpr int tailPadding = 256;
    juce::AudioBuffer<float> source (numChannels, (int) inputFrames + tailPadding);
    source.clear();
    if (! reader->read (&source, 0, (int) inputFrames, 0, true, numChannels >= 2))
    {
        result.errorMessage = jp (u8"デコードに失敗しました: ") + request.sourceFile.getFileName();
        return Status::failed;
    }
    reader.reset();

    if (threadShouldExit())
        return Status::cancelled;

    const bool needsResample = std::abs (sourceRate - targetRate) > 0.5;
    const auto outputFrames = needsResample
                                  ? (juce::int64) std::llround ((double) inputFrames * targetRate / sourceRate)
                                  : inputFrames;
    result.outputFrames = outputFrames;

    juce::WavAudioFormat wav;
    using Opts = juce::AudioFormatWriterOptions;
    std::unique_ptr<juce::OutputStream> stream (request.tempFile.createOutputStream());
    auto writer = stream != nullptr
                      ? wav.createWriterFor (stream, Opts {}.withSampleRate (targetRate)
                                                            .withNumChannels (numChannels)
                                                            .withBitsPerSample (24))
                      : nullptr;
    if (writer == nullptr)
    {
        result.errorMessage = jp (u8"一時ファイルを作成できませんでした: ")
                              + request.tempFile.getFullPathName();
        return Status::failed;
    }

    // SR一致はリサンプルをバイパス（デコードのみ。24bit化はwriterが行う）
    if (! needsResample)
    {
        for (juce::int64 pos = 0; pos < outputFrames; pos += chunkFrames)
        {
            if (threadShouldExit())
                return Status::cancelled;
            const int n = (int) juce::jmin ((juce::int64) chunkFrames, outputFrames - pos);
            if (! writer->writeFromAudioSampleBuffer (source, (int) pos, n))
            {
                result.errorMessage = jp (u8"書き込みに失敗しました（ディスク容量を確認してください）。");
                return Status::failed;
            }
            progressValue.store ((float) ((double) (pos + n) / (double) outputFrames));
        }
        return Status::success;
    }

    // WindowedSinc（200タップ）で各chを独立にストリーミング変換。
    // speedRatio = 出力1サンプルあたりの入力サンプル数。入力実長を超えた分は
    // バッファ末尾のゼロパディング（tailPadding）を読ませる。
    //
    // レイテンシ補償: WindowedSincは入力100サンプル分のアルゴリズム遅延を持つ
    // （juce_Interpolators.h の WindowedSincTraits::algorithmicLatency = 100 と照合済み。
    // 8.0.9からバージョンを上げるときは再照合すること）。補償しないと全体が
    // 100入力サンプル（44.1k→48k変換で約2.3ms）後ろへずれ、末尾が同じ分だけ欠落する。
    // 先頭の遅延分を捨て、その分だけ余計に生成して出力長を outputFrames に合わせる。
    // 総消費は約 inputFrames + 100 + 端数 に収まり tailPadding(256) の範囲内
    const double speedRatio = sourceRate / targetRate;
    const auto latencyOut = (juce::int64) std::llround (100.0 * targetRate / sourceRate);
    const auto paddedFrames = inputFrames + tailPadding;
    juce::Interpolators::WindowedSinc interpolators[2];
    for (auto& interpolator : interpolators)
        interpolator.reset();

    juce::AudioBuffer<float> out (numChannels, chunkFrames);
    juce::int64 consumed[2] = { 0, 0 };
    const auto totalToProduce = outputFrames + latencyOut;

    for (juce::int64 produced = 0; produced < totalToProduce; produced += chunkFrames)
    {
        if (threadShouldExit())
            return Status::cancelled;

        const int n = (int) juce::jmin ((juce::int64) chunkFrames, totalToProduce - produced);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            // ポインタと available は常にバッファ実体の範囲内（available >= 1）に収める。
            // process は available == 0 でも先頭1サンプルを先読みするため 0 を渡してはいけない
            const auto safeConsumed = juce::jmin (consumed[ch], paddedFrames - 1);
            const auto available = (int) (paddedFrames - safeConsumed);
            const int used = interpolators[ch].process (speedRatio,
                                                        source.getReadPointer (ch) + safeConsumed,
                                                        out.getWritePointer (ch),
                                                        n, available, 0);
            consumed[ch] = safeConsumed + used;
        }

        // 先頭 latencyOut サンプル（遅延分の無音）は書かない
        const int skip = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, latencyOut - produced);
        if (skip < n && ! writer->writeFromAudioSampleBuffer (out, skip, n - skip))
        {
            result.errorMessage = jp (u8"書き込みに失敗しました（ディスク容量を確認してください）。");
            return Status::failed;
        }
        progressValue.store ((float) ((double) (produced + n) / (double) totalToProduce));
    }
    return Status::success;
}
