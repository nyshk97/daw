#include "PlaybackEngine.h"

#include <cmath>

PlaybackEngine::PlaybackEngine (TransportState& transportState, SnapshotExchange& snapshotExchange)
    : transport (transportState), snapshots (snapshotExchange)
{
}

void PlaybackEngine::prepareToPlay (int, double sampleRate)
{
    currentSampleRate = sampleRate;
    transport.sampleRate.store (sampleRate);
}

void PlaybackEngine::releaseResources()
{
}

void PlaybackEngine::process (const juce::AudioSourceChannelInfo& bufferToFill)
{
    auto& buffer = *bufferToFill.buffer;
    const int numSamples = bufferToFill.numSamples;
    const int startSample = bufferToFill.startSample;

    if (buffer.getNumChannels() == 0 || numSamples == 0 || currentSampleRate <= 0.0)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    const double sr = currentSampleRate;
    const double bpm = juce::jlimit (20.0, 400.0, transport.bpm.load());
    const double beatLen = sr * 60.0 / bpm;

    // ---- シーク適用と再生開始検出（クリックの拍トラッキングもここでリセット）----
    const auto seek = transport.seekRequest.exchange (TransportState::kNoSeek);
    if (seek != TransportState::kNoSeek)
        transport.playheadSamplePos.store (seek);

    const bool playing = transport.isPlaying.load();
    if (seek != TransportState::kNoSeek || (playing && ! prevPlaying))
    {
        // 「1サンプル前の拍番号」にしておくと、ちょうど拍頭から始めたときだけ即クリックが鳴る
        const auto pos = transport.playheadSamplePos.load();
        lastBeatIndex = (juce::int64) std::floor ((double) (pos - 1) / beatLen);
        clickSamplesLeft = 0;
    }
    prevPlaying = playing;

    const juce::int64 pos = transport.playheadSamplePos.load();

    // ---- 入力（出力で上書きする前に消費する）----
    const float* input = buffer.getReadPointer (0, startSample);
    transport.inputPeakLevel.store (buffer.getMagnitude (0, startSample, numSamples));

    const bool armed = transport.recordArmed.load();
    const juce::int64 punchIn = transport.punchInSample.load();

    if (armed && playing && pos + numSamples > punchIn)
    {
        // パンチイン位置をまたぐブロックは途中から書く
        const int offset = (int) juce::jmax ((juce::int64) 0, punchIn - pos);
        const float* channels[] = { input + offset };
        recorder.write (channels, numSamples - offset);
        transport.recordedSamples.fetch_add (numSamples - offset);
    }

    // ---- 出力: クリップのミックス再生 ----
    bufferToFill.clearActiveBufferRegion();

    if (playing)
    {
        if (auto* snapshot = snapshots.acquire())
        {
            bool anySolo = false;
            for (auto& track : snapshot->tracks)
                if (track.params->solo.load())
                    { anySolo = true; break; }

            for (auto& track : snapshot->tracks)
            {
                const bool audible = ! track.params->mute.load()
                                     && (! anySolo || track.params->solo.load());
                if (! audible)
                    continue;

                const float gain = track.params->gain.load();
                if (gain <= 0.0f)
                    continue;

                for (auto& clip : track.clips)
                {
                    const auto clipLen = (juce::int64) clip.audio->getNumSamples();
                    const auto overlapStart = juce::jmax (pos, clip.startSample);
                    const auto overlapEnd = juce::jmin (pos + numSamples, clip.startSample + clipLen);
                    if (overlapEnd <= overlapStart)
                        continue;

                    const int destOffset = (int) (overlapStart - pos);
                    const int srcOffset = (int) (overlapStart - clip.startSample);
                    const int count = (int) (overlapEnd - overlapStart);
                    const float* src = clip.audio->getReadPointer (0, srcOffset);

                    // 重なったクリップは加算再生
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                        buffer.addFrom (ch, startSample + destOffset, src, count, gain);
                }
            }
        }
    }

    // ---- クリック（カウントイン中は常に・それ以外はトグルON時のみ）----
    if (playing)
    {
        const bool clickOn = transport.clickEnabled.load();
        const int clickLen = (int) (sr * 0.045);

        for (int i = 0; i < numSamples; ++i)
        {
            const juce::int64 p = pos + i;
            const auto beatIndex = (juce::int64) std::floor ((double) p / beatLen);

            if (beatIndex != lastBeatIndex)
            {
                lastBeatIndex = beatIndex;
                const bool countingIn = armed && p < punchIn;
                if (clickOn || countingIn)
                {
                    const bool downbeat = (beatIndex % 4 == 0); // 4/4固定。負の位置でも4の倍数判定は正しい
                    clickFreq = downbeat ? 1760.0 : 880.0;
                    clickAmp = downbeat ? 0.4f : 0.25f;
                    clickPhase = 0.0;
                    clickSamplesLeft = clickLen;
                    clickTotalSamples = juce::jmax (1, clickLen);
                }
            }

            if (clickSamplesLeft > 0)
            {
                const float env = (float) clickSamplesLeft / (float) clickTotalSamples;
                const float sample = (float) std::sin (clickPhase) * clickAmp * env * env;
                clickPhase += juce::MathConstants<double>::twoPi * clickFreq / sr;
                --clickSamplesLeft;

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.addSample (ch, startSample + i, sample);
            }
        }

        transport.playheadSamplePos.store (pos + numSamples);
    }
}

void PlaybackEngine::play()
{
    transport.isPlaying.store (true);
}

void PlaybackEngine::stop()
{
    transport.isPlaying.store (false);
}

bool PlaybackEngine::startRecording (const juce::File& file, juce::int64 punchInSample,
                                     juce::int64 countInSamples, double deviceSampleRate)
{
    recorder.startRecording (file, deviceSampleRate);
    if (! recorder.isRecording())
        return false;

    // 順序が重要: recordArmed を立てる前に punchIn/カウンタを確定させ、
    // isPlaying より先に seekRequest を積む（オーディオスレッドはシークを先に適用する）
    transport.recordedSamples.store (0);
    transport.punchInSample.store (punchInSample);
    transport.seekRequest.store (punchInSample - countInSamples);
    transport.recordArmed.store (true);
    transport.isPlaying.store (true);
    return true;
}

void PlaybackEngine::stopRecording()
{
    transport.recordArmed.store (false);
    recorder.stop(); // flush待ち。armed=false の後なので新規writeは来ない
}

bool PlaybackEngine::isRecording() const
{
    return recorder.isRecording();
}
