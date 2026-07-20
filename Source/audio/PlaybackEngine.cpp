#include "PlaybackEngine.h"

#include <cmath>

PlaybackEngine::PlaybackEngine (TransportState& transportState, SnapshotExchange& snapshotExchange,
                                PreviewFifo& previewFifoToUse)
    : transport (transportState), snapshots (snapshotExchange), previewFifo (previewFifoToUse)
{
}

void PlaybackEngine::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate = sampleRate;
    transport.sampleRate.store (sampleRate);
    transport.blockSizeExpected.store (samplesPerBlockExpected);

    // コールバック内での確保を避けるため、ここで全バッファを確保する。
    // SynthBank側のAUも同じ基準（max(4096, expected)）で prepareToPlay される
    const int maxBlock = juce::jmax (4096, samplesPerBlockExpected);
    synthScratch.setSize (maxSynthChannels, maxBlock);

    // ノートオン上限＋オフ（発音中は最大128ピッチ×count上限255だが実用上128個程度）＋All Notes Off。
    // 1イベントあたりの格納コストは数バイト＋ヘッダなので、余裕を持って1イベント16バイト換算で確保
    midiScratch.ensureSize ((size_t) (maxNoteOnsPerBlock * 2 + 512) * 16);
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
    const bool startedOrSeeked = seek != TransportState::kNoSeek || (playing && ! prevPlaying);
    const bool stoppedNow = ! playing && prevPlaying;
    if (startedOrSeeked)
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

    // ---- 出力: クリップ＋MIDIのミックス再生 ----
    bufferToFill.clearActiveBufferRegion();

    // 停止中もスナップショットを取得する（MIDIはリリース余韻と停止エッジの消音のため常時レンダリング）
    auto* snapshot = snapshots.acquire();

    bool anySolo = false;
    if (snapshot != nullptr)
        for (auto& track : snapshot->tracks)
            if (track.params->solo.load())
                { anySolo = true; break; }

    if (playing && snapshot != nullptr)
    {
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

    // ---- プレビューコマンドの取り込み（このブロックで処理する分だけ固定配列へ）----
    numPreviewCommands = 0;
    {
        PreviewFifo::Command command;
        while (numPreviewCommands < maxPreviewCommandsPerBlock && previewFifo.pop (command))
        {
            // 再生中のプレビュー発音は破棄（UI側でも抑止しているが防御的に）
            if (command.type == PreviewFifo::Command::Type::noteOn && playing)
                continue;
            previewCommands[numPreviewCommands++] = command;
        }
    }

    // 再生開始・シーク・停止時はプレビュー中ノートを打ち切る（オフはsilenceFirstが送る）
    if (startedOrSeeked || stoppedNow)
        numPreviewNotes = 0;

    // ---- MIDIトラック（ミュート/ソロでもイベント送信・レンダリングは止めず、ミックスゲインだけ0にする）----
    if (snapshot != nullptr)
        renderMidiTracks (*snapshot, buffer, startSample, numSamples, pos,
                          playing,
                          startedOrSeeked || stoppedNow,      // silenceFirst
                          playing && startedOrSeeked,         // resound
                          sr, bpm, anySolo);

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

// オーディオスレッド専用。snapshot 内の SynthInstance は shared_ptr 共有所有により
// このブロックの処理中は生存が保証されている（生ポインタ参照のみ・コピーはしない）。
void PlaybackEngine::renderMidiTracks (PlaybackSnapshot& snapshot, juce::AudioBuffer<float>& buffer,
                                       int startSample, int numSamples, juce::int64 pos,
                                       bool playing, bool silenceFirst, bool resound,
                                       double sr, double bpm, bool anySolo)
{
    const double tps = Ppq::ticksPerSample (bpm, sr);
    const double blockStartPpq = (double) pos * tps;
    const double blockEndPpq = (double) (pos + numSamples) * tps;

    for (auto& track : snapshot.tracks)
    {
        auto* synth = track.synth.get();
        if (synth == nullptr || synth->plugin == nullptr)
            continue;

        // レート/ブロックサイズ不一致は安全側でスキップ（メッセージスレッドのSynthBankが作り直して再pushする）
        if (! juce::approximatelyEqual (synth->preparedSampleRate, sr)
            || numSamples > synth->preparedBlockSize
            || numSamples > synthScratch.getNumSamples()
            || synth->totalOutputChannels > synthScratch.getNumChannels()
            || synth->totalOutputChannels > maxSynthChannels)
            continue;

        midiScratch.clear(); // 容量は維持される（prepareToPlayでensureSize済み）
        const int ch = synth->midiChannel;
        int noteOnsAdded = 0;

        if (silenceFirst)
        {
            // 発音中ノートを明示的に止める（All Notes Offを無視する音源への保険も兼ねて両方送る）
            for (int pitch = 0; pitch < 128; ++pitch)
            {
                while (synth->soundingCount[pitch] > 0)
                {
                    midiScratch.addEvent (juce::MidiMessage::noteOff (ch, pitch), 0);
                    --synth->soundingCount[pitch];
                }
            }
            midiScratch.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
        }

        // ---- プレビュー発音（停止中のみ。オンはコマンド、オフは固定発音長の自動送出）----
        // 再生中は一切処理しない: プレビューは停止中しか発生せず、ここでallNotesOffを
        // 処理すると再生中の伴奏ノートまで消してしまうため
        if (! playing)
        {
            const auto previewLength = (juce::int64) (sr * 0.5);
            for (int c = 0; c < numPreviewCommands; ++c)
            {
                const auto& command = previewCommands[c];
                if (command.trackId != track.trackId)
                    continue;

                if (command.type == PreviewFifo::Command::Type::noteOn)
                {
                    if (numPreviewNotes < maxPreviewNotes)
                    {
                        midiScratch.addEvent (juce::MidiMessage::noteOn (ch, command.pitch,
                                                                         (juce::uint8) juce::jlimit (1, 127, command.velocity)), 0);
                        ++synth->soundingCount[juce::jlimit (0, 127, command.pitch)];
                        previewNotes[numPreviewNotes++] = { command.trackId,
                                                            juce::jlimit (0, 127, command.pitch),
                                                            previewLength };
                    }
                }
                else // allNotesOff（ピアノロールを閉じた等）
                {
                    for (int pitch = 0; pitch < 128; ++pitch)
                    {
                        while (synth->soundingCount[pitch] > 0)
                        {
                            midiScratch.addEvent (juce::MidiMessage::noteOff (ch, pitch), 0);
                            --synth->soundingCount[pitch];
                        }
                    }
                    midiScratch.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
                    for (int n = numPreviewNotes - 1; n >= 0; --n)
                        if (previewNotes[n].trackId == track.trackId)
                            previewNotes[n] = previewNotes[--numPreviewNotes];
                }
            }

            // 発音長を消化したプレビューノートのオフ
            for (int n = numPreviewNotes - 1; n >= 0; --n)
            {
                auto& note = previewNotes[n];
                if (note.trackId != track.trackId)
                    continue;
                note.samplesLeft -= numSamples;
                if (note.samplesLeft <= 0)
                {
                    if (synth->soundingCount[note.pitch] > 0)
                    {
                        midiScratch.addEvent (juce::MidiMessage::noteOff (ch, note.pitch), 0);
                        --synth->soundingCount[note.pitch];
                    }
                    previewNotes[n] = previewNotes[--numPreviewNotes];
                }
            }
        }

        if (resound)
        {
            // 再生位置を跨いでいるノートを offset 0 で再発音（シーク途中の持続音）
            for (auto& note : track.notes)
            {
                const double s = (double) note.startPpq;
                if (s >= blockStartPpq)
                    break; // startPpq昇順なので以降は跨ぎ得ない
                if ((double) note.endPpq > blockStartPpq && noteOnsAdded < maxNoteOnsPerBlock)
                {
                    midiScratch.addEvent (juce::MidiMessage::noteOn (ch, note.pitch, (juce::uint8) note.velocity), 0);
                    ++synth->soundingCount[note.pitch];
                    ++noteOnsAdded;
                }
            }
        }

        if (playing)
        {
            for (auto& note : track.notes)
            {
                const double s = (double) note.startPpq;
                const double e = (double) note.endPpq;
                if (s >= blockEndPpq)
                    break; // startPpq昇順なので以降は全てブロック外

                // 過去に鳴らしたノートのオフ（同一オフセットではオンより先に並ぶよう、先に追加する）
                if (s < blockStartPpq && e >= blockStartPpq && e < blockEndPpq
                    && synth->soundingCount[note.pitch] > 0)
                {
                    const int offOffset = juce::jlimit (0, numSamples - 1,
                                                        (int) ((juce::int64) std::llround (e / tps) - pos));
                    midiScratch.addEvent (juce::MidiMessage::noteOff (ch, note.pitch), offOffset);
                    --synth->soundingCount[note.pitch];
                }

                // このブロックで始まるノートのオン
                if (s >= blockStartPpq)
                {
                    if (noteOnsAdded >= maxNoteOnsPerBlock)
                    {
                        // 上限超過: 新規ノートオンを対応オフごと捨てる（soundingCountを増やさないので
                        // オフも送られない＝鳴りっぱなしは構造的に起きない）
                        transport.midiOverflow.store (true);
                        continue;
                    }

                    const int onOffset = juce::jlimit (0, numSamples - 1,
                                                       (int) ((juce::int64) std::llround (s / tps) - pos));
                    midiScratch.addEvent (juce::MidiMessage::noteOn (ch, note.pitch, (juce::uint8) note.velocity), onOffset);
                    ++synth->soundingCount[note.pitch];
                    ++noteOnsAdded;

                    // ブロック内で終わる短いノートはオフも同時に予約
                    if (e < blockEndPpq)
                    {
                        const int offOffset = juce::jlimit (onOffset, numSamples - 1,
                                                            (int) ((juce::int64) std::llround (e / tps) - pos));
                        midiScratch.addEvent (juce::MidiMessage::noteOff (ch, note.pitch), offOffset);
                        --synth->soundingCount[note.pitch];
                    }
                }
            }
        }

        // ---- AUレンダリング（synthScratch の先頭 numSamples 分だけを非所有ビューで渡す）----
        // DLSは全出力バス分（4ch）のバッファを要求する。ミックスに使うのはメインバスのch0/1のみ
        float* chans[maxSynthChannels] = {};
        for (int sc = 0; sc < synth->totalOutputChannels; ++sc)
            chans[sc] = synthScratch.getWritePointer (sc);
        juce::AudioBuffer<float> block (chans, synth->totalOutputChannels, numSamples); // 外部データ参照・ヒープ確保なし
        block.clear();
        synth->plugin->processBlock (block, midiScratch);

        // ---- ミックス（ミュート/非ソロはゲイン0＝加算しないだけで、上のレンダリングは常に行う）----
        const bool audible = ! track.params->mute.load()
                             && (! anySolo || track.params->solo.load());
        const float gain = audible ? track.params->gain.load() : 0.0f;
        if (gain > 0.0f)
            for (int outCh = 0; outCh < buffer.getNumChannels(); ++outCh)
                buffer.addFrom (outCh, startSample, block, juce::jmin (outCh, 1), 0, numSamples, gain);
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
