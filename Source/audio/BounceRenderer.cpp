#include "BounceRenderer.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "../shared/Pan.h"
#include "../shared/Ppq.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

constexpr float silenceThreshold = 0.001f; // -60dB。テールの無音判定
constexpr int maxScratchChannels = 8;      // DLSは4ch（2バス）。これを超える音源は想定しない
}

void BounceRenderer::run()
{
    const auto st = renderAndWrite();
    result.status = st;
    result.peak = runningPeak;
    result.writtenSamples = samplesWritten;

    // Requestが握る専用synth・クリップバッファはここ（ワーカー）で手放してよい
    // （RTスレッドとは無共有。shared_ptrの参照カウントはスレッド安全）
    request = {};

    currentStatus.store (st); // 最後に更新（呼び出し側はstatusを見てからresultを読む）
}

BounceRenderer::Status BounceRenderer::renderAndWrite()
{
    runningPeak = 0.0f;
    samplesWritten = 0;
    renderFailed = false;

    const auto target = request.targetFile;
    tempFloatFile = target.getSiblingFile ("." + target.getFileName() + ".f32.tmp");
    tempFinalFile = target.getSiblingFile ("." + target.getFileName() + ".tmp");
    tempFloatFile.deleteFile();
    tempFinalFile.deleteFile();

    juce::WavAudioFormat wav;
    using Opts = juce::AudioFormatWriterOptions;

    // ---- パス1: float32一時WAVへストリームレンダリング（＋ピーク計測）----
    auto st = Status::failed;
    {
        std::unique_ptr<juce::OutputStream> stream (tempFloatFile.createOutputStream());
        auto writer = stream != nullptr
                          ? wav.createWriterFor (stream, Opts {}.withSampleRate (request.sampleRate)
                                                                .withNumChannels (2)
                                                                .withBitsPerSample (32)) // 32bit = IEEE float
                          : nullptr;
        if (writer == nullptr)
        {
            result.errorMessage = jp (u8"一時ファイルを作成できませんでした: ") + tempFloatFile.getFullPathName();
            tempFloatFile.deleteFile();
            return Status::failed;
        }
        const bool ok = renderPass (*writer);
        st = ok ? Status::success : (renderFailed ? Status::failed : Status::cancelled);
    } // writerを閉じてflush

    // ---- パス2: ピークに応じてスケールしつつ24bitへ変換 ----
    if (st == Status::success)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (
            wav.createReaderFor (new juce::FileInputStream (tempFloatFile), true));
        std::unique_ptr<juce::OutputStream> stream (tempFinalFile.createOutputStream());
        auto writer = (reader != nullptr && stream != nullptr)
                          ? wav.createWriterFor (stream, Opts {}.withSampleRate (request.sampleRate)
                                                                .withNumChannels (2)
                                                                .withBitsPerSample (24))
                          : nullptr;
        if (writer == nullptr)
        {
            result.errorMessage = jp (u8"書き出しファイルを作成できませんでした: ") + tempFinalFile.getFullPathName();
            st = Status::failed;
        }
        else
        {
            std::swap (convertReader, reader);
            const bool ok = convertPass (*writer);
            st = ok ? Status::success : (renderFailed ? Status::failed : Status::cancelled);
            convertReader.reset();
        }
    }

    tempFloatFile.deleteFile();

    if (st != Status::success)
    {
        tempFinalFile.deleteFile();
        return st;
    }

    // ---- 原子的置換（同一ディレクトリの一時ファイル→出力先。既存ファイルはrenameで上書きされる）----
    // moveFileTo()は「既存の出力先を先に削除してから移動」なので使わない（移動失敗時に既存を壊す）
    if (::rename (tempFinalFile.getFullPathName().toRawUTF8(),
                  target.getFullPathName().toRawUTF8()) != 0)
    {
        // レンダリング結果を救出できるよう一時ファイルは削除せず残す
        result.errorMessage = jp (u8"保存先への配置に失敗しました。レンダリング結果は残っています: ")
                              + tempFinalFile.getFullPathName();
        return Status::failed;
    }

    progressValue.store (1.0f);
    return Status::success;
}

bool BounceRenderer::renderPass (juce::AudioFormatWriter& writer)
{
    const double sr = request.sampleRate;
    const double tps = Ppq::ticksPerSample (request.bpm, sr);
    const auto rangeStart = request.startSample;
    const auto rangeEnd = request.endSample;

    int synthChannels = 2;
    for (auto& track : request.tracks)
        if (track.synth != nullptr)
            synthChannels = juce::jmax (synthChannels, track.synth->totalOutputChannels);
    jassert (synthChannels <= maxScratchChannels);
    synthScratch.setSize (juce::jmin (synthChannels, maxScratchChannels), renderBlockSize);
    midiScratch.ensureSize (4096);

    cursors.assign (request.tracks.size(), {});
    // 範囲開始より前に鳴り終わるノートの読み飛ばし（サイクル範囲書き出しで rangeStart > 0 になる。
    // startPpq昇順の並びなので先頭からの連続分だけ飛ばす）。範囲頭を跨いで鳴っているノートは
    // 読み飛ばされず、scheduleBlockMidi のオンオフセットのクランプ（jlimit 0..）で範囲頭から発音される
    const double rangeStartPpq = (double) rangeStart * tps;
    for (size_t i = 0; i < request.tracks.size(); ++i)
        while (cursors[i].nextNote < request.tracks[i].notes.size()
               && (double) request.tracks[i].notes[cursors[i].nextNote].endPpq <= rangeStartPpq)
            ++cursors[i].nextNote;

    juce::AudioBuffer<float> mix (2, renderBlockSize);
    std::vector<juce::AudioBuffer<float>> busMix;
    for (int b = 0; b < numSendBuses; ++b)
        busMix.emplace_back (2, renderBlockSize);

    // ---- 本編: クリップ＋MIDIのミックス ----
    for (juce::int64 pos = rangeStart; pos < rangeEnd; pos += renderBlockSize)
    {
        if (threadShouldExit())
            return false;

        const int n = (int) juce::jmin ((juce::int64) renderBlockSize, rangeEnd - pos);
        mix.clear();
        for (auto& bus : busMix)
            bus.clear();

        for (size_t ti = 0; ti < request.tracks.size(); ++ti)
        {
            auto& track = request.tracks[ti];

            // クリップ: 重なりは加算再生・モノラルソースを等パワー補正型panで両chへ
            // （RTのprocessと同じ規則・同じ法則）。sendはpost-fader（gain・pan適用後）
            float panL = 1.0f, panR = 1.0f;
            Pan::monoGains (track.pan, panL, panR);
            for (auto& clip : track.clips)
            {
                const auto overlapStart = juce::jmax (pos, clip.startSample);
                const auto overlapEnd = juce::jmin (pos + n, clip.startSample + clip.lengthSamples);
                if (overlapEnd <= overlapStart)
                    continue;

                const int destOffset = (int) (overlapStart - pos);
                const int srcOffset = (int) (clip.offsetSamples + (overlapStart - clip.startSample));
                const int count = (int) (overlapEnd - overlapStart);
                const float* src = clip.audio->getReadPointer (0, srcOffset);
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float gain = track.gain * (ch == 0 ? panL : panR);
                    mix.addFrom (ch, destOffset, src, count, gain);
                    for (int b = 0; b < numSendBuses; ++b)
                        if (track.sends[b] > 0.0f)
                            busMix[(size_t) b].addFrom (ch, destOffset, src, count, gain * track.sends[b]);
                }
            }

            if (track.synth != nullptr && track.synth->plugin != nullptr)
            {
                scheduleBlockMidi (track, cursors[ti], pos, n, tps);
                renderSynthInto (mix, busMix, track, n);
            }
        }

        mixBusesAndMaster (mix, busMix, n);

        for (int ch = 0; ch < 2; ++ch)
            runningPeak = juce::jmax (runningPeak, mix.getMagnitude (ch, 0, n));

        if (! writer.writeFromAudioSampleBuffer (mix, 0, n))
        {
            result.errorMessage = jp (u8"一時ファイルへの書き込みに失敗しました（ディスク容量を確認してください）。");
            renderFailed = true;
            return false;
        }
        samplesWritten += n;

        progressValue.store (0.85f * (float) ((double) (pos + n - rangeStart) / (double) (rangeEnd - rangeStart)));
    }

    // ---- テール: 範囲終端で鳴り残ったノートを止め、余韻が減衰しきるまで延長 ----
    if (request.wantTail)
    {
        const auto maxTailSamples = (juce::int64) (sr * 5.0);
        juce::int64 tailRendered = 0;
        bool firstTailBlock = true;

        while (tailRendered < maxTailSamples)
        {
            if (threadShouldExit())
                return false;

            mix.clear();
            for (auto& bus : busMix)
                bus.clear();
            for (size_t ti = 0; ti < request.tracks.size(); ++ti)
            {
                auto& track = request.tracks[ti];
                if (track.synth == nullptr || track.synth->plugin == nullptr)
                    continue;

                midiScratch.clear();
                if (firstTailBlock)
                {
                    const int ch = track.synth->midiChannel;
                    for (const auto& [endPpq, pitch] : cursors[ti].active)
                        midiScratch.addEvent (juce::MidiMessage::noteOff (ch, pitch), 0);
                    cursors[ti].active.clear();
                }
                renderSynthInto (mix, busMix, track, renderBlockSize);
            }
            firstTailBlock = false;

            mixBusesAndMaster (mix, busMix, renderBlockSize);

            // 無音判定はMaster適用後の最終出力で行う（聞こえる信号が-60dBを下回ったら終了）
            float magnitude = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                magnitude = juce::jmax (magnitude, mix.getMagnitude (ch, 0, renderBlockSize));
            runningPeak = juce::jmax (runningPeak, magnitude);

            if (! writer.writeFromAudioSampleBuffer (mix, 0, renderBlockSize))
            {
                result.errorMessage = jp (u8"一時ファイルへの書き込みに失敗しました（ディスク容量を確認してください）。");
                renderFailed = true;
                return false;
            }
            samplesWritten += renderBlockSize;
            tailRendered += renderBlockSize;
            progressValue.store (0.85f + 0.05f * (float) ((double) tailRendered / (double) maxTailSamples));

            if (magnitude < silenceThreshold)
                break; // -60dBを下回ったら余韻は減衰しきったとみなす
        }
    }

    progressValue.store (0.9f);
    return true;
}

bool BounceRenderer::convertPass (juce::AudioFormatWriter& writer)
{
    auto& reader = *convertReader;
    const auto total = reader.lengthInSamples;

    const float scale = runningPeak > 1.0f ? 0.999f / runningPeak : 1.0f;
    result.scaled = scale < 1.0f;

    juce::AudioBuffer<float> buffer (2, renderBlockSize);
    for (juce::int64 pos = 0; pos < total; pos += renderBlockSize)
    {
        if (threadShouldExit())
            return false;

        const int n = (int) juce::jmin ((juce::int64) renderBlockSize, total - pos);
        if (! reader.read (&buffer, 0, n, pos, true, true))
        {
            result.errorMessage = jp (u8"一時ファイルの読み戻しに失敗しました。");
            renderFailed = true;
            return false;
        }
        if (scale < 1.0f)
            buffer.applyGain (0, n, scale);

        if (! writer.writeFromAudioSampleBuffer (buffer, 0, n))
        {
            result.errorMessage = jp (u8"書き出しファイルへの書き込みに失敗しました（ディスク容量を確認してください）。");
            renderFailed = true;
            return false;
        }
        progressValue.store (0.9f + 0.1f * (float) ((double) (pos + n) / (double) total));
    }
    return true;
}

void BounceRenderer::scheduleBlockMidi (const TrackRender& track, SynthCursor& cursor,
                                        juce::int64 pos, int numSamples, double tps)
{
    midiScratch.clear();
    const double blockEndPpq = (double) (pos + numSamples) * tps;
    const int ch = track.synth->midiChannel;

    // 1) このブロック内で終わる発音中ノートのオフ（同位置のオンより先に積む。RTと同じ流儀）
    for (size_t i = 0; i < cursor.active.size();)
    {
        const auto [endPpq, pitch] = cursor.active[i];
        if ((double) endPpq < blockEndPpq)
        {
            const int off = juce::jlimit (0, numSamples - 1,
                                          (int) ((juce::int64) std::llround ((double) endPpq / tps) - pos));
            midiScratch.addEvent (juce::MidiMessage::noteOff (ch, pitch), off);
            cursor.active[i] = cursor.active.back(); // swap-with-last（順序は不要）
            cursor.active.pop_back();
        }
        else
        {
            ++i;
        }
    }

    // 2) このブロックで始まるノートのオン（ブロック内で終わる短いノートはオフも同時に積む）
    const auto& notes = track.notes;
    while (cursor.nextNote < notes.size())
    {
        const auto& note = notes[cursor.nextNote];
        if ((double) note.startPpq >= blockEndPpq)
            break; // startPpq昇順なので以降は全てブロック外

        const int on = juce::jlimit (0, numSamples - 1,
                                     (int) ((juce::int64) std::llround ((double) note.startPpq / tps) - pos));
        midiScratch.addEvent (juce::MidiMessage::noteOn (ch, note.pitch, (juce::uint8) note.velocity), on);

        if ((double) note.endPpq < blockEndPpq)
        {
            const int off = juce::jlimit (on, numSamples - 1,
                                          (int) ((juce::int64) std::llround ((double) note.endPpq / tps) - pos));
            midiScratch.addEvent (juce::MidiMessage::noteOff (ch, note.pitch), off);
        }
        else
        {
            cursor.active.push_back ({ note.endPpq, note.pitch });
        }
        ++cursor.nextNote;
    }
}

void BounceRenderer::renderSynthInto (juce::AudioBuffer<float>& mix,
                                      std::vector<juce::AudioBuffer<float>>& busMix,
                                      const TrackRender& track, int numSamples)
{
    auto* synth = track.synth.get();
    const int total = synth->totalOutputChannels;
    if (total > synthScratch.getNumChannels() || numSamples > synthScratch.getNumSamples())
        return; // 想定外の構成（準備条件と不一致）。無音スキップ

    // DLSは全出力バス分（4ch）のバッファを要求する。ミックスに使うのはメインバスのch0/1のみ
    float* chans[maxScratchChannels] = {};
    for (int c = 0; c < total; ++c)
        chans[c] = synthScratch.getWritePointer (c);
    juce::AudioBuffer<float> block (chans, total, numSamples);
    block.clear();
    synth->plugin->processBlock (block, midiScratch);

    // ステレオソースなのでpanはバランス型（RTのrenderMidiTracksと同じ法則）。sendはpost-fader
    float balL = 1.0f, balR = 1.0f;
    Pan::stereoGains (track.pan, balL, balR);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float gain = track.gain * (ch == 0 ? balL : balR);
        mix.addFrom (ch, 0, block, juce::jmin (ch, 1), 0, numSamples, gain);
        for (int b = 0; b < numSendBuses; ++b)
            if (track.sends[b] > 0.0f)
                busMix[(size_t) b].addFrom (ch, 0, block, juce::jmin (ch, 1), 0, numSamples,
                                            gain * track.sends[b]);
    }
}

void BounceRenderer::mixBusesAndMaster (juce::AudioBuffer<float>& mix,
                                        std::vector<juce::AudioBuffer<float>>& busMix, int numSamples)
{
    for (int b = 0; b < numSendBuses; ++b)
    {
        if (request.busMute[b] || request.busGain[b] <= 0.0f)
            continue;
        for (int ch = 0; ch < 2; ++ch)
            mix.addFrom (ch, 0, busMix[(size_t) b], ch, 0, numSamples, request.busGain[b]);
    }
    if (request.masterGain != 1.0f)
        mix.applyGain (0, numSamples, request.masterGain);
}
