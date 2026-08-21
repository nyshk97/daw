#include "PitchAnalysisWorker.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "PitchAnalyzer.h"
#include "../shared/Log.h"

bool PitchAnalysisWorker::start (Request&& requestToRun)
{
    if (currentStatus.load() != Status::idle || isThreadRunning())
        return false;
    if (requestToRun.source == nullptr || requestToRun.sampleRate <= 0.0)
        return false;
    request = std::move (requestToRun);
    result = {};
    result.generation = request.generation;
    progressValue.store (0.0f);
    currentGeneration.store (request.generation);
    currentStatus.store (Status::running);
    if (! startThread())
    {
        currentStatus.store (Status::idle); // 作成失敗を running のまま残さない（GOTCHAS）
        return false;
    }
    return true;
}

PitchAnalysisWorker::Result PitchAnalysisWorker::takeResult()
{
    // run() 最終行で status を公開してもスレッドハンドルはまだ閉じていないので、必ず終了を待つ
    waitForThreadToExit (-1);
    Result out = std::move (result);
    result = {};
    currentStatus.store (Status::idle);
    return out;
}

void PitchAnalysisWorker::run()
{
    Status terminal = Status::failed;
    auto curve = PitchAnalyzer::analyze (*request.source, request.sampleRate,
                                         [this] { return threadShouldExit(); },
                                         [this] (float p) { progressValue.store (p); });
    if (threadShouldExit() || curve.numFrames() == 0)
    {
        terminal = threadShouldExit() ? Status::cancelled : Status::failed;
        if (terminal == Status::failed)
            result.errorMessage = juce::String::fromUTF8 (u8"解析結果が空（原音が空か SR が不正）");
    }
    else
    {
        result.curve = std::move (curve);
        terminal = Status::success;

        if (request.wavFile != juce::File())
        {
            // WAV の存在と識別子を再確認してから書く（削除・差し替え済み WAV の孤児サイドカーを作らない）
            bool ok = request.wavFile.existsAsFile();
            if (ok)
            {
                juce::WavAudioFormat wav;
                auto stream = request.wavFile.createInputStream();
                std::unique_ptr<juce::AudioFormatReader> reader (stream != nullptr ? wav.createReaderFor (stream.release(), true) : nullptr);
                ok = reader != nullptr
                  && reader->lengthInSamples == result.curve.source.frames
                  && (int) reader->numChannels == result.curve.source.channels;
            }
            if (! ok)
                result.errorMessage = juce::String::fromUTF8 (u8"元 WAV が無いか内容が変わったためサイドカーを書かなかった");
            else
            {
                juce::String err;
                result.sidecarFile = PitchSidecar::fileFor (request.wavFile, result.curve.digest());
                result.sidecarWritten = PitchSidecar::write (result.curve, request.wavFile, &err);
                if (! result.sidecarWritten)
                    result.errorMessage = err;
            }
        }
    }
    result.status = terminal;
    progressValue.store (1.0f);
    // 終端状態の公開は run() の最後の1文（GOTCHAS）
    currentStatus.store (terminal);
}
