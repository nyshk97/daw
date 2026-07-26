#include "PlaybackEngine.h"

#include <cmath>

#include "../shared/ClipFade.h"
#include "../shared/Pan.h"
#include "AudioFilePreview.h"

namespace
{
// メーター用ピークのCAS max更新（UI側の exchange(0) と組。TrackParams::peakL/peakR のコメント参照）
void storePeakMax (std::atomic<float>& target, float value)
{
    float current = target.load();
    while (value > current && ! target.compare_exchange_weak (current, value))
    {
    }
}
}

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
    trackScratch.setSize (2, maxBlock); // モノ経路はch0のみ使用・ステレオ経路はch0/1（post-pan）
    mixScratch.setSize (2, maxBlock);
    for (auto& bus : busScratch)
        bus.setSize (2, maxBlock);

    // ノートオン上限（1024）＋オフ（activeNotes追跡により最大 SynthInstance::maxActiveNotes = 256 に有界）
    // ＋All Notes Off等。1イベントあたりの格納コストは数バイト＋ヘッダなので、1イベント16バイト換算で余裕を持って確保
    midiScratch.ensureSize ((size_t) (maxNoteOnsPerBlock * 2 + 512) * 16);
    if (filePreview != nullptr)
        filePreview->prepareToPlay (sampleRate);
}

void PlaybackEngine::releaseResources()
{
    if (filePreview != nullptr)
        filePreview->releaseResources();
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

    const bool armed = transport.recordArmed.load();
    const juce::int64 punchIn = transport.punchInSample.load();

    if (armed && playing && pos + numSamples > punchIn)
    {
        // パンチイン位置をまたぐブロックは途中から書く
        const int offset = (int) juce::jmax ((juce::int64) 0, punchIn - pos);
        const float* channels[] = { input + offset };
        if (! recorder.write (channels, numSamples - offset))
            transport.recordDroppedBlocks.fetch_add (1); // FIFO満杯。録音WAVに欠落が生じている
        transport.recordedSamples.fetch_add (numSamples - offset);
    }

    // ---- 出力: クリップ＋MIDIのミックス再生 ----
    bufferToFill.clearActiveBufferRegion();

    // 停止中もスナップショットを取得する（MIDIはリリース余韻と停止エッジの消音のため常時レンダリング）
    auto* snapshot = snapshots.acquire();

    // 再生中の編集（ノート削除等）でノートオフが失われて鳴りっぱなしになるのを防ぐため、
    // MIDI構成の差し替えを検出して消音→跨ぎノート再発音を行う。
    // 判定にポインタでなく midiGeneration を使うのは、オーディオ側だけの差し替え
    // （リージョンゲインのドラッグ中の更新）でMIDIを鳴り直させないため（整数比較のみ）
    const auto midiGeneration = snapshot != nullptr ? snapshot->midiGeneration : 0;
    const bool snapshotChanged = midiGeneration != lastSeenMidiGeneration;
    lastSeenMidiGeneration = midiGeneration;

    bool anySolo = false;
    if (snapshot != nullptr)
        for (auto& track : snapshot->tracks)
            if (track.params->solo.load())
                { anySolo = true; break; }

    // 想定外の巨大ブロックが来たときだけミックス経路を諦めてクリップの直接加算に
    // フォールバックする（音を落とさないことを最優先。send・メーターは失う）
    const bool canProcess = numSamples <= mixScratch.getNumSamples();

    const float masterGain = (snapshot != nullptr && snapshot->masterParams != nullptr)
                                 ? snapshot->masterParams->gain.load()
                                 : 1.0f;

    // ---- プレビューコマンドの取り込み（コールバックごとに1回。処理する分だけ固定配列へ）----
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

    // ---- サイクル（ループ範囲）。区間は [start, end) で終端は排他的。録音中は無効 ----
    const auto cyclePacked = transport.cycleRange.load();
    const auto cycleStart = TransportState::cycleStartOf (cyclePacked);
    const auto cycleEnd = TransportState::cycleEndOf (cyclePacked);
    const bool cycleActive = playing && ! armed && cycleStart < cycleEnd
                             && transport.cycleEnabled.load();
    if (! playing)
        cycleWrapPending = false; // 停止で持ち越さない（再開時はstartedOrSeekedが消音・再発音する）

    juce::int64 segPos = pos;
    if (cycleActive && segPos >= cycleEnd)
    {
        // 範囲外（終端ちょうど含む）から始まるブロックは先に範囲頭へ正規化する
        segPos = cycleStart;
        cycleWrapPending = true;
    }

    // サイクル境界をまたぐブロックはセグメントに分割し、残サンプルが0になるまで繰り返す
    // （範囲長 < ブロック長だと1コールバックで複数回ラップする）。サイクル無効時は1周で抜ける
    int done = 0;
    bool firstSegment = true;
    while (done < numSamples)
    {
        const bool wrapSeek = cycleWrapPending;
        cycleWrapPending = false;
        if (wrapSeek)
        {
            // ラップ＝内部シーク: クリックの拍トラッキングもシークと同じ流儀でリセット
            lastBeatIndex = (juce::int64) std::floor ((double) (segPos - 1) / beatLen);
            clickSamplesLeft = 0;
        }

        int segLen = numSamples - done;
        if (cycleActive)
            segLen = (int) juce::jmin ((juce::int64) segLen, cycleEnd - segPos);

        processSegment (buffer, startSample + done, segLen, segPos,
                        playing, armed, punchIn, snapshot, anySolo, canProcess, masterGain,
                        sr, bpm, beatLen,
                        (firstSegment && (startedOrSeeked || stoppedNow || snapshotChanged)) || wrapSeek,
                        (firstSegment && (startedOrSeeked || stoppedNow)) || wrapSeek,
                        (firstSegment && playing && (startedOrSeeked || snapshotChanged)) || wrapSeek);

        segPos += segLen;
        done += segLen;
        firstSegment = false;

        if (cycleActive && segPos >= cycleEnd)
        {
            segPos = cycleStart; // 終端は排他的: ちょうど終端で終わるブロックも次は範囲頭から
            cycleWrapPending = true; // 次セグメント（無ければ次コールバック先頭）で内部シーク処理
        }
    }

    if (playing)
        transport.playheadSamplePos.store (segPos);

    // ファイル試聴はプロジェクトのMaster・トランスポート・バウンスから独立した
    // post-master信号。デコード済み不変バッファを線形補間して加算するだけ。
    if (filePreview != nullptr)
        filePreview->mixInto (buffer, startSample, numSamples);
}

// オーディオスレッド専用。スクラッチバッファは常にオフセット0から segLen 分を使い、
// デバイスバッファへの最終出力だけ outOffset へずらす（サイクル分割の後半セグメント対応）
void PlaybackEngine::processSegment (juce::AudioBuffer<float>& buffer, int outOffset, int segLen,
                                     juce::int64 segPos, bool playing, bool armed, juce::int64 punchIn,
                                     PlaybackSnapshot* snapshot, bool anySolo, bool canProcess,
                                     float masterGain, double sr, double bpm, double beatLen,
                                     bool silenceTransport, bool silenceAll, bool resound)
{
    // ステレオミックス（mixScratch）とsendバスの準備。全信号は
    // 「トラック（gain・pan）→ mixScratch/busScratch → バス素通し加算 → Masterゲイン → 出力ch0/1」
    // の順で流れる
    if (canProcess)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            mixScratch.clear (ch, 0, segLen);
            for (auto& bus : busScratch)
                bus.clear (ch, 0, segLen);
        }
    }

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

            float panL = 1.0f, panR = 1.0f;
            Pan::monoGains (track.params->pan.load(), panL, panR);

            if (! track.hasStereoClip)
            {
                // ---- モノのみトラック（現行経路）----
                // このブロックは演算順序を含め一切変えないこと。クリップ単位のpan分配へ寄せると
                // 重なりクリップの加算順序・sendの乗算順序が変わり、既存プロジェクトの出力との
                // ビット一致が崩れる（回帰確認は testMonoRenderRegressionHash のハッシュ比較）。
                // リージョンゲインは「トラックゲインの手前に1回掛ける」形で足しており、
                // ユニティ(1.0f)なら gain と厳密に同値なので既存の出力は変わらない。
                // フェードも同じ約束で足している: フェードなしなら ClipFade::segments が
                // 「1区間・ゲイン1.0f」に落ち、addFrom 1回（clipGain * 1.0f == clipGain）へ戻る
                if (canProcess)
                    trackScratch.clear (0, 0, segLen); // 全トラックで再利用するため毎回必ずclear

                bool anyOverlap = false;
                for (auto& clip : track.clips)
                {
                    const auto clipLen = clip.lengthSamples;
                    const auto overlapStart = juce::jmax (segPos, clip.startSample);
                    const auto overlapEnd = juce::jmin (segPos + segLen, clip.startSample + clipLen);
                    if (overlapEnd <= overlapStart)
                        continue;
                    anyOverlap = true;

                    const int destOffset = (int) (overlapStart - segPos);
                    const int srcOffset = (int) (clip.offsetSamples + (overlapStart - clip.startSample));
                    const float* src = clip.audio->getReadPointer (0, srcOffset);
                    const float clipGain = gain * clip.gain; // 素材のトリム → 曲中のバランスの順

                    // フェードの傾斜を表現するため重なり範囲を最大3区間へ割る（フェードなしなら1区間）
                    ClipFade::Segment segs[ClipFade::maxSegments];
                    const int numSegs = ClipFade::segments (clip, overlapStart, overlapEnd, segPos, segs);

                    // 重なったクリップは加算再生（メーターは加算後ピークを測る必要があるため
                    // 一旦モノスクラッチへ合算し、pan分配は後でまとめて行う）
                    for (int s = 0; s < numSegs; ++s)
                    {
                        const auto& seg = segs[s];
                        const float* segSrc = src + (seg.destOffset - destOffset);
                        if (canProcess)
                        {
                            ClipFade::addSegment (trackScratch, 0, seg.destOffset, segSrc, seg, clipGain);
                        }
                        else if (buffer.getNumChannels() >= 2)
                        {
                            // フォールバックの縮退はsend/メーターのみ。出力ルール（ch0/1・1chダウンミックス）と
                            // pan・Masterは本編と揃える
                            ClipFade::addSegment (buffer, 0, outOffset + seg.destOffset, segSrc, seg,
                                                  clipGain * panL * masterGain);
                            ClipFade::addSegment (buffer, 1, outOffset + seg.destOffset, segSrc, seg,
                                                  clipGain * panR * masterGain);
                        }
                        else
                        {
                            ClipFade::addSegment (buffer, 0, outOffset + seg.destOffset, segSrc, seg,
                                                  clipGain * 0.5f * (panL + panR) * masterGain);
                        }
                    }
                }

                if (canProcess && anyOverlap)
                {
                    // メーター: モノソース×pan分配なので L/R = 合算ピーク×panL/panR
                    const float mag = trackScratch.getMagnitude (0, 0, segLen);
                    storePeakMax (track.params->peakL, mag * panL);
                    storePeakMax (track.params->peakR, mag * panR);
                    mixScratch.addFrom (0, 0, trackScratch, 0, 0, segLen, panL);
                    mixScratch.addFrom (1, 0, trackScratch, 0, 0, segLen, panR);

                    // post-fader send（gain・pan適用後のコピーをバスへ）
                    for (int b = 0; b < numSendBuses; ++b)
                    {
                        const float send = track.params->sends[b].load();
                        if (send <= 0.0f)
                            continue;
                        busScratch[b].addFrom (0, 0, trackScratch, 0, 0, segLen, panL * send);
                        busScratch[b].addFrom (1, 0, trackScratch, 0, 0, segLen, panR * send);
                    }
                }
            }
            else
            {
                // ---- ステレオクリップを含むトラック（新経路）----
                // クリップごとにpan込みでL/Rへ分配する（BounceRendererのクリップミックスと同じ規則）。
                // モノクリップは等パワー補正型（モノのみトラックと同じ法則）、ステレオクリップは
                // バランス型（MIDIシンセのステレオ出力と同じ法則）。trackScratchはpost-panの2chになるので
                // メーターは真のL/R別ピーク、send・ミックスへの合流はch対応のコピーになる
                float balL = 1.0f, balR = 1.0f;
                Pan::stereoGains (track.params->pan.load(), balL, balR);

                if (canProcess)
                {
                    trackScratch.clear (0, 0, segLen);
                    trackScratch.clear (1, 0, segLen);
                }

                bool anyOverlap = false;
                for (auto& clip : track.clips)
                {
                    const auto clipLen = clip.lengthSamples;
                    const auto overlapStart = juce::jmax (segPos, clip.startSample);
                    const auto overlapEnd = juce::jmin (segPos + segLen, clip.startSample + clipLen);
                    if (overlapEnd <= overlapStart)
                        continue;
                    anyOverlap = true;

                    const int destOffset = (int) (overlapStart - segPos);
                    const int srcOffset = (int) (clip.offsetSamples + (overlapStart - clip.startSample));
                    const bool stereo = clip.audio->getNumChannels() >= 2;
                    const float* srcL = clip.audio->getReadPointer (0, srcOffset);
                    const float* srcR = clip.audio->getReadPointer (stereo ? 1 : 0, srcOffset);
                    const float clipGain = gain * clip.gain; // 素材のトリム → 曲中のバランスの順
                    const float gainL = clipGain * (stereo ? balL : panL);
                    const float gainR = clipGain * (stereo ? balR : panR);

                    // フェードの区間分割はモノ経路と共通（フェードなしなら1区間・ゲイン1.0）
                    ClipFade::Segment segs[ClipFade::maxSegments];
                    const int numSegs = ClipFade::segments (clip, overlapStart, overlapEnd, segPos, segs);

                    for (int s = 0; s < numSegs; ++s)
                    {
                        const auto& seg = segs[s];
                        const int rel = seg.destOffset - destOffset;
                        if (canProcess)
                        {
                            ClipFade::addSegment (trackScratch, 0, seg.destOffset, srcL + rel, seg, gainL);
                            ClipFade::addSegment (trackScratch, 1, seg.destOffset, srcR + rel, seg, gainR);
                        }
                        else if (buffer.getNumChannels() >= 2)
                        {
                            ClipFade::addSegment (buffer, 0, outOffset + seg.destOffset, srcL + rel, seg,
                                                  gainL * masterGain);
                            ClipFade::addSegment (buffer, 1, outOffset + seg.destOffset, srcR + rel, seg,
                                                  gainR * masterGain);
                        }
                        else
                        {
                            ClipFade::addSegment (buffer, 0, outOffset + seg.destOffset, srcL + rel, seg,
                                                  gainL * 0.5f * masterGain);
                            ClipFade::addSegment (buffer, 0, outOffset + seg.destOffset, srcR + rel, seg,
                                                  gainR * 0.5f * masterGain);
                        }
                    }
                }

                if (canProcess && anyOverlap)
                {
                    storePeakMax (track.params->peakL, trackScratch.getMagnitude (0, 0, segLen));
                    storePeakMax (track.params->peakR, trackScratch.getMagnitude (1, 0, segLen));
                    mixScratch.addFrom (0, 0, trackScratch, 0, 0, segLen);
                    mixScratch.addFrom (1, 0, trackScratch, 1, 0, segLen);

                    // post-fader send（trackScratchは既にgain・pan適用済み）
                    for (int b = 0; b < numSendBuses; ++b)
                    {
                        const float send = track.params->sends[b].load();
                        if (send <= 0.0f)
                            continue;
                        busScratch[b].addFrom (0, 0, trackScratch, 0, 0, segLen, send);
                        busScratch[b].addFrom (1, 0, trackScratch, 1, 0, segLen, send);
                    }
            }
            }
        }
    }

    // ---- MIDIトラック（ミュート/ソロでもイベント送信・レンダリングは止めず、ミックスゲインだけ0にする）----
    // 出力は mixScratch / busScratch へ（プレビュー発音もこの経路 = pan/send/Masterを通る）。
    // canProcessでない巨大ブロックでは各シンセのサイズガードが働いて書き込みは起きない
    if (snapshot != nullptr)
        renderMidiTracks (*snapshot, segLen, segPos,
                          playing, silenceTransport, silenceAll, resound,
                          sr, bpm, anySolo);

    // ---- sendバス（当面FXは無く素通し）→ ミックス、Masterゲイン → 出力バッファ ----
    // 出力ルール: ch0/1にのみ書く。1chデバイスはL+R等分ダウンミックス、2ch超の余剰chはclearのまま無音
    if (canProcess && snapshot != nullptr)
    {
        for (int b = 0; b < numSendBuses; ++b)
        {
            auto* busParams = snapshot->busParams[b].get();
            if (busParams == nullptr || busParams->mute.load())
                continue;
            const float busGain = busParams->gain.load();
            if (busGain <= 0.0f)
                continue;

            storePeakMax (busParams->peakL, busScratch[b].getMagnitude (0, 0, segLen) * busGain);
            storePeakMax (busParams->peakR, busScratch[b].getMagnitude (1, 0, segLen) * busGain);
            mixScratch.addFrom (0, 0, busScratch[b], 0, 0, segLen, busGain);
            mixScratch.addFrom (1, 0, busScratch[b], 1, 0, segLen, busGain);
        }

        if (snapshot->masterParams != nullptr)
        {
            storePeakMax (snapshot->masterParams->peakL,
                          mixScratch.getMagnitude (0, 0, segLen) * masterGain);
            storePeakMax (snapshot->masterParams->peakR,
                          mixScratch.getMagnitude (1, 0, segLen) * masterGain);
        }

        if (buffer.getNumChannels() >= 2)
        {
            buffer.addFrom (0, outOffset, mixScratch, 0, 0, segLen, masterGain);
            buffer.addFrom (1, outOffset, mixScratch, 1, 0, segLen, masterGain);
        }
        else
        {
            buffer.addFrom (0, outOffset, mixScratch, 0, 0, segLen, 0.5f * masterGain);
            buffer.addFrom (0, outOffset, mixScratch, 1, 0, segLen, 0.5f * masterGain);
        }
    }

    // ---- クリック（カウントイン中は常に・それ以外はトグルON時のみ）----
    // Masterフェーダーの後（post-master）に加算する: Masterを絞ってもカウントイン/クリックは聞こえる
    if (playing)
    {
        const bool clickOn = transport.clickEnabled.load();
        const int clickLen = (int) (sr * 0.045);

        for (int i = 0; i < segLen; ++i)
        {
            const juce::int64 p = segPos + i;
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

                // 出力ルールは本編と同じ（ch0/1のみ。余剰chへ漏らさない）
                for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
                    buffer.addSample (ch, outOffset + i, sample);
            }
        }
    }
}

// オーディオスレッド専用。snapshot 内の SynthInstance は shared_ptr 共有所有により
// このブロックの処理中は生存が保証されている（生ポインタ参照のみ・コピーはしない）。
void PlaybackEngine::renderMidiTracks (PlaybackSnapshot& snapshot, int numSamples, juce::int64 pos,
                                       bool playing, bool silenceTransport, bool silenceAll, bool resound,
                                       double sr, double bpm, bool anySolo)
{
    const double tps = Ppq::ticksPerSample (bpm, sr);

    for (auto& track : snapshot.tracks)
    {
        auto* synth = track.synth.get();
        if (synth == nullptr || ! synth->hasRenderer())
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

        if (silenceTransport)
        {
            // 送信済みノートを明示的に止める（オフ数は maxActiveNotes に有界）。
            // silenceAll でないとき（スナップショット差し替え）はプレビュー発音（endPpq==-1）を温存し、
            // プレビューまで殺す CC123 も送らない
            for (int i = synth->numActiveNotes - 1; i >= 0; --i)
            {
                if (! silenceAll && synth->activeNotes[i].endPpq < 0)
                    continue;
                midiScratch.addEvent (juce::MidiMessage::noteOff (ch, synth->activeNotes[i].pitch), 0);
                synth->removeActive (i);
            }
            if (silenceAll)
                midiScratch.addEvent (juce::MidiMessage::allNotesOff (ch), 0); // 保険（追跡漏れがあっても止まる）
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
                    const int pitch = juce::jlimit (0, 127, command.pitch);
                    if (numPreviewNotes < maxPreviewNotes && synth->addActive (-1, pitch))
                    {
                        midiScratch.addEvent (juce::MidiMessage::noteOn (ch, pitch,
                                                                         (juce::uint8) juce::jlimit (1, 127, command.velocity)), 0);
                        previewNotes[numPreviewNotes++] = { command.trackId, pitch, previewLength };
                    }
                }
                else // allNotesOff（ピアノロールを閉じた等）
                {
                    for (int i = synth->numActiveNotes - 1; i >= 0; --i)
                    {
                        midiScratch.addEvent (juce::MidiMessage::noteOff (ch, synth->activeNotes[i].pitch), 0);
                        synth->removeActive (i);
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
                    const int index = synth->findActive (-1, note.pitch);
                    if (index >= 0)
                    {
                        midiScratch.addEvent (juce::MidiMessage::noteOff (ch, note.pitch), 0);
                        synth->removeActive (index);
                    }
                    previewNotes[n] = previewNotes[--numPreviewNotes];
                }
            }
        }

        // One Shot（サンプル音源の固定モード）は resound しない。noteOff で消えないので
        // 復元が不要であり、再発音すると鳴っているワンショットに重なって二重発音になる
        // （副作用として、シーク先で「跨いでいる打楽器」が途中から鳴り出すこともなくなる）
        // ノートの位置判定は**サンプル位置**で行う（PPQ同士で比べない）。
        // pos は「グリッド位置をサンプル整数へ丸めた値」であることが多く、pos * tps でPPQへ戻すと
        // 元のPPQよりわずかに大きく（または小さく）なる。例: 127BPM/44.1kHzの16分音符1つ目は
        // PPQ 240 に対しサンプル5209で、戻すと240.0156 → 境界ちょうどのノートが「過去のノート」に
        // 落ちて発音されない（One Shotはresoundもしないので完全に消える）。
        // 判定・オフセット計算の両方を llround(ppq / tps) の整数サンプルに統一して取りこぼしを防ぐ
        const auto blockEnd = pos + numSamples;
        const auto noteStartSample = [tps] (const MidiNotePlayback& note)
        { return (juce::int64) std::llround ((double) note.startPpq / tps); };
        const auto noteEndSample = [tps] (const MidiNotePlayback& note)
        { return (juce::int64) std::llround ((double) note.endPpq / tps); };

        if (resound && ! synth->oneShot.load())
        {
            // 再生位置を跨いでいるノートを offset 0 で再発音（シーク途中・編集後の持続音）
            for (auto& note : track.notes)
            {
                if (noteStartSample (note) >= pos)
                    break; // startPpq昇順なので以降は跨ぎ得ない
                if (noteEndSample (note) > pos && noteOnsAdded < maxNoteOnsPerBlock
                    && synth->addActive (note.endPpq, note.pitch))
                {
                    midiScratch.addEvent (juce::MidiMessage::noteOn (ch, note.pitch, (juce::uint8) note.velocity), 0);
                    ++noteOnsAdded;
                }
            }
        }

        if (playing)
        {
            for (auto& note : track.notes)
            {
                const auto onSample = noteStartSample (note);
                const auto offSample = noteEndSample (note);
                if (onSample >= blockEnd)
                    break; // startPpq昇順なので以降は全てブロック外

                // 過去に鳴らしたノートのオフ。実際にオンを送ったもの（activeNotesに載っているもの）だけ送る。
                // 上限超過で捨てたノートの終端で、同ピッチの別ノートを誤って止めないため。
                // （同一オフセットではオンより先に並ぶよう、オンより先に追加する）
                if (onSample < pos && offSample >= pos && offSample < blockEnd)
                {
                    const int index = synth->findActive (note.endPpq, note.pitch);
                    if (index >= 0)
                    {
                        const int offOffset = juce::jlimit (0, numSamples - 1, (int) (offSample - pos));
                        midiScratch.addEvent (juce::MidiMessage::noteOff (ch, note.pitch), offOffset);
                        synth->removeActive (index);
                    }
                }

                // このブロックで始まるノートのオン
                if (onSample >= pos)
                {
                    if (noteOnsAdded >= maxNoteOnsPerBlock)
                    {
                        // 上限超過: 新規ノートオンを対応オフごと捨てる（activeNotesに載らないので
                        // オフも送られない＝鳴りっぱなしも誤オフも構造的に起きない）
                        transport.midiDroppedNoteOns.fetch_add (1);
                        continue;
                    }

                    const int onOffset = juce::jlimit (0, numSamples - 1, (int) (onSample - pos));

                    if (offSample < blockEnd)
                    {
                        // ブロック内で終わる短いノートはオンとオフを同時に予約（activeNotes追跡は不要）
                        midiScratch.addEvent (juce::MidiMessage::noteOn (ch, note.pitch, (juce::uint8) note.velocity), onOffset);
                        const int offOffset = juce::jlimit (onOffset, numSamples - 1, (int) (offSample - pos));
                        midiScratch.addEvent (juce::MidiMessage::noteOff (ch, note.pitch), offOffset);
                        ++noteOnsAdded;
                    }
                    else if (synth->addActive (note.endPpq, note.pitch))
                    {
                        midiScratch.addEvent (juce::MidiMessage::noteOn (ch, note.pitch, (juce::uint8) note.velocity), onOffset);
                        ++noteOnsAdded;
                    }
                    else
                    {
                        transport.midiDroppedNoteOns.fetch_add (1); // active枠が満杯: オンごと諦める
                    }
                }
            }
        }

        // ---- 音源レンダリング（synthScratch の先頭 numSamples 分だけを非所有ビューで渡す）----
        // DLSは全出力バス分（4ch）のバッファを要求する。ミックスに使うのはメインバスのch0/1のみ
        // （サンプラーは常に2ch）
        float* chans[maxSynthChannels] = {};
        for (int sc = 0; sc < synth->totalOutputChannels; ++sc)
            chans[sc] = synthScratch.getWritePointer (sc);
        juce::AudioBuffer<float> block (chans, synth->totalOutputChannels, numSamples); // 外部データ参照・ヒープ確保なし
        block.clear();
        if (synth->plugin != nullptr)
            synth->plugin->processBlock (block, midiScratch);
        else
            synth->sampler->processBlock (block, midiScratch);

        // ---- ミックス（ミュート/非ソロはゲイン0＝加算しないだけで、上のレンダリングは常に行う）----
        // シンセ出力はステレオなのでpanはバランス型（センター0dB・振った反対側だけ減衰）
        const bool audible = ! track.params->mute.load()
                             && (! anySolo || track.params->solo.load());
        const float gain = audible ? track.params->gain.load() : 0.0f;
        if (gain > 0.0f)
        {
            float balL = 1.0f, balR = 1.0f;
            Pan::stereoGains (track.params->pan.load(), balL, balR);
            const float gainL = gain * balL;
            const float gainR = gain * balR;
            const int srcR = juce::jmin (1, block.getNumChannels() - 1);

            mixScratch.addFrom (0, 0, block, 0, 0, numSamples, gainL);
            mixScratch.addFrom (1, 0, block, srcR, 0, numSamples, gainR);

            // post-fader send（gain・pan適用後のコピーをバスへ）
            for (int b = 0; b < numSendBuses; ++b)
            {
                const float send = track.params->sends[b].load();
                if (send <= 0.0f)
                    continue;
                busScratch[b].addFrom (0, 0, block, 0, 0, numSamples, gainL * send);
                busScratch[b].addFrom (1, 0, block, srcR, 0, numSamples, gainR * send);
            }

            storePeakMax (track.params->peakL, block.getMagnitude (0, 0, numSamples) * gainL);
            storePeakMax (track.params->peakR, block.getMagnitude (srcR, 0, numSamples) * gainR);
        }
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
