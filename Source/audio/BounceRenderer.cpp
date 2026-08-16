#include "BounceRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../shared/ClipFade.h"
#include "../shared/Pan.h"
#include "../shared/Ppq.h"
#include "../shared/Project.h"
#include "../shared/SongFade.h"
#include "TrackFxChain.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

constexpr float silenceThreshold = 0.001f; // -60dB。テールの無音判定
constexpr int maxScratchChannels = 8;      // DLSは4ch（2バス）。これを超える音源は想定しない
}

bool BounceRenderer::buildItemRender (const Track& track, int itemIndex, double bpm, double sampleRate,
                                      TrackRender& out, juce::int64& rangeStart, juce::int64& rangeEnd)
{
    out = {};
    out.gain = track.params->gain.load();
    out.pan = track.params->pan.load();
    for (int b = 0; b < numSendBuses; ++b)
        out.sends[b] = track.params->sends[b].load();
    out.eqEnabled = track.params->eqEnabled.load();
    out.eqBands = Eq::loadAll (track.params->eqBands);
    out.compEnabled = track.params->compEnabled.load();
    out.comp = Comp::load (track.params->comp);

    if (track.type == TrackType::audio)
    {
        if (itemIndex < 0 || itemIndex >= (int) track.clips.size())
            return false;
        const auto& clip = track.clips[(size_t) itemIndex];
        if (clip.audio == nullptr)
            return false;

        // ループ展開と範囲クランプは buildSnapshot と共通のヘルパー（appendClipPlaybacks）。
        // muted は見ない（明示選択が優先という⌘Eの既存仕様。ヘルパー側も判断しない）
        appendClipPlaybacks (clip, out.clips);
        if (out.clips.empty())
            return false;

        rangeStart = clip.startSample;
        rangeEnd = clip.startSample + clip.totalLengthSamples(); // ループ終端まで書き出す
        return true;
    }

    if (itemIndex < 0 || itemIndex >= (int) track.midiRegions.size())
        return false;
    const auto& region = track.midiRegions[(size_t) itemIndex];

    // ノートのフラット化・境界マスク・ループ展開・固定ピッチ置換は buildSnapshot と共通の
    // ヘルパー（appendRegionNotes）。固定ピッチ置換はGM音源専用（サンプラーはピッチを見ずに
    // 等速で鳴らすので置換しない）。muted は見ない（明示選択が優先という⌘Eの既存仕様）
    const bool fixedPitch = track.instrument == InstrumentKind::gm
                            && track.drums && track.drumPitch >= 0;
    appendRegionNotes (region, fixedPitch ? track.drumPitch : -1, out.notes);
    std::stable_sort (out.notes.begin(), out.notes.end(),
                      [] (const MidiNotePlayback& a, const MidiNotePlayback& b)
                      { return a.startPpq < b.startPpq; });

    const double tps = Ppq::ticksPerSample (bpm, sampleRate);
    rangeStart = (juce::int64) std::llround ((double) region.startPpq / tps);
    // ループ終端まで書き出す（リージョン境界の余白も含めるためノート終端でなくモデル側から算出）
    rangeEnd = (juce::int64) std::llround ((double) (region.startPpq + region.totalLengthPpq()) / tps);
    return true;
}

juce::int64 BounceRenderer::oneShotEndSample (const Track& track,
                                              const std::vector<MidiNotePlayback>& notes,
                                              double bpm, double sampleRate)
{
    // 追従モードはノート長で止まる（既存のテールで足りる）ので延長しない
    if (! track.usesSampler() || track.samplePitchFollow || ! track.hasSample())
        return 0;
    if (sampleRate <= 0.0 || track.sampleSourceRate <= 0.0)
        return 0;

    const auto remaining = juce::jmax ((juce::int64) 0,
                                       (juce::int64) track.sampleAudio->getNumSamples()
                                           - track.sampleStartOffset);
    // SamplerEngine は「読み出し位置がバッファ末尾に達するまで」出力するので、必要な出力サンプル数は
    // ceil(remaining / rate)（rate = sourceRate/deviceRate）。四捨五入だと非整数SR比で末尾が1サンプル切れる
    // （例: 残り2サンプル・44.1k→48k は実際に3サンプル出る）
    const auto tail = (juce::int64) std::ceil ((double) remaining * sampleRate / track.sampleSourceRate);

    const double tps = Ppq::ticksPerSample (bpm, sampleRate);
    juce::int64 end = 0;
    for (const auto& note : notes)
    {
        const auto noteStart = (juce::int64) std::llround ((double) note.startPpq / tps);
        end = juce::jmax (end, noteStart + tail);
    }
    return end;
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
    eqTrackScratch.setSize (2, renderBlockSize);
    midiScratch.ensureSize (4096);

    // EQ・Compは平滑を挟まず開始時点の値で即確定する（バウンス中にパラメータは変わらない。
    // RT側のようにdryからフェードインすると冒頭だけ再生と違う音になる）
    eqBlockSerial = 0;
    for (auto& track : request.tracks)
    {
        track.eq.snapTo (sr, track.eqEnabled, track.eqBands);
        track.compDsp.snapTo (sr, track.compEnabled, track.comp);
    }

    cursors.assign (request.tracks.size(), {});
    // 範囲開始より前のノートの読み飛ばし（サイクル範囲書き出しで rangeStart > 0 になる。
    // startPpq昇順の並びなので先頭からの連続分だけ飛ばす）。
    // - 通常（GM・追従モード）: 鳴り終わったノートだけ飛ばす。範囲頭を跨いで鳴っているノートは残り、
    //   scheduleBlockMidi のオンオフセットのクランプ（jlimit 0..）で範囲頭から発音される
    //   ＝リアルタイム側の resound と同じ挙動
    // - One Shot（サンプルの固定モード）: **範囲頭より前に始まるノートも飛ばす**。リアルタイム側は
    //   oneShot のとき resound しない（シーク先で途中から鳴り出さない）ので、跨ぎノートを範囲頭で
    //   鳴らすと再生結果と食い違い、叩いていない打点が増える
    // 比較はPPQ同士で行わずサンプル位置で行う（PlaybackEngine::renderMidiTracks と同じ理由:
    // rangeStart は丸め済みのサンプル位置で、tps でPPQへ戻すと境界ちょうどのノートを取りこぼす）
    for (size_t i = 0; i < request.tracks.size(); ++i)
    {
        const auto& track = request.tracks[i];
        const bool oneShot = track.synth != nullptr && track.synth->oneShot.load();
        while (cursors[i].nextNote < track.notes.size())
        {
            const auto& note = track.notes[cursors[i].nextNote];
            const auto onSample = (juce::int64) std::llround ((double) note.startPpq / tps);
            const auto offSample = (juce::int64) std::llround ((double) note.endPpq / tps);
            if (! (oneShot ? onSample < rangeStart : offSample <= rangeStart))
                break;
            ++cursors[i].nextNote;
        }
    }

    juce::AudioBuffer<float> mix (2, renderBlockSize);
    std::vector<juce::AudioBuffer<float>> busMix;
    for (int b = 0; b < numSendBuses; ++b)
        busMix.emplace_back (2, renderBlockSize);

    // ---- 本編: クリップ＋MIDIのミックス ----
    // posは固定幅で進めず、実際に処理した長さ n だけ進める（曲末フェードの境界でブロックを
    // 短縮するため。固定幅のままだと境界後のサンプルを飛ばす）
    for (juce::int64 pos = rangeStart; pos < rangeEnd; )
    {
        if (threadShouldExit())
            return false;

        int n = (int) juce::jmin ((juce::int64) renderBlockSize, rangeEnd - pos);

        // 曲末フェードの境界で切り、ブロックが「フェード前／区間内／終端以後」の
        // いずれかに完全に収まるようにする（RTと同じ条件式。「現在位置より厳密に後ろ」の
        // 境界だけ採用する＝境界ちょうどで n=0 になって進まなくなるのを防ぐ）
        if (request.hasFadeOut())
        {
            for (const auto boundary : { request.fadeStartSample(), request.fadeEndSample() })
                if (pos < boundary && boundary < pos + n)
                    n = (int) (boundary - pos);
        }

        mix.clear();
        for (auto& bus : busMix)
            bus.clear();
        ++eqBlockSerial;

        for (size_t ti = 0; ti < request.tracks.size(); ++ti)
        {
            auto& track = request.tracks[ti];

            // トラックEQ・Comp（オーディオトラック。判定・適用はTrackFxChainに集約＝RTと同じ規則）。
            // RTのactive経路と同じ処理順:
            // 「クリップgainまで合算 → EQ → Comp → トラックgain → send/mix」。FXが働くときだけ
            // 専用スクラッチを経由し、バイパス時は既存の直接ミックスを**完全に**通す（ビット一致契約）
            const TrackFx::Settings fxSettings { track.eqEnabled, track.eqBands,
                                                 track.compEnabled, track.comp };
            auto fxActivity = TrackFx::evaluateActivity (TrackFx::Policy::offline,
                                                         track.eq, track.compDsp, fxSettings, false);
            if (track.synth != nullptr) // MIDIトラックのFXはシンセ出力側（renderSynthInto）で掛ける
                fxActivity = {};
            const bool activeFx = fxActivity.any();
            // モノクリップのみのトラックは、RTのモノ経路と同じく**pan前のモノ信号**にFXを掛けて
            // からpan分配する。EQは線形なのでpanと可換（stereoに焼き込んでも一致した）だが、
            // Compは非線形＝検波レベルがpanで変わるため、ここを揃えないと再生と書き出しの音が食い違う
            const bool monoFx = activeFx && TrackFx::isMonoClipTrack (track.clips);
            // ステレオのみのトラックはバランスpanをFX後段へ（RTの prePanFx と同じ理由・同じ点）
            const bool prePanFx = activeFx && ! monoFx && TrackFx::isStereoOnlyClipTrack (track.clips);
            juce::AudioBuffer<float>& clipDest = activeFx ? eqTrackScratch : mix;
            const float clipTrackGain = activeFx ? 1.0f : track.gain;
            if (activeFx)
                for (int ch = 0; ch < 2; ++ch)
                    eqTrackScratch.clear (ch, 0, n);

            // クリップ: 重なりは加算再生・クリップ単位でpan分配してL/Rへ（RTのprocessと同じ法則。
            // モノは等パワー補正型・ステレオはバランス型）。sendはpost-fader（gain・pan適用後）。
            // モノクリップの演算式・順序は変えないこと（既存プロジェクトの書き出しとのビット一致を保つ）
            float panL = 1.0f, panR = 1.0f;
            Pan::monoGains (track.pan, panL, panR);
            float balL = 1.0f, balR = 1.0f;
            Pan::stereoGains (track.pan, balL, balR);
            for (auto& clip : track.clips)
            {
                const auto overlapStart = juce::jmax (pos, clip.startSample);
                const auto overlapEnd = juce::jmin (pos + n, clip.startSample + clip.lengthSamples);
                if (overlapEnd <= overlapStart)
                    continue;

                const int destOffset = (int) (overlapStart - pos);
                const int srcOffset = (int) (clip.offsetSamples + (overlapStart - clip.startSample));
                const bool stereo = clip.audio->getNumChannels() >= 2;

                // フェードの区間分割（RTのprocessと共通のヘルパー。フェードなしなら
                // 1区間・ゲイン1.0＝既存の addFrom 1回に戻り、書き出しはビット一致する）
                ClipFade::Segment segs[ClipFade::maxSegments];
                const int numSegs = ClipFade::segments (clip, overlapStart, overlapEnd, pos, segs);

                // monoFx: RTのモノ経路と同じく、pan前のモノ合算（ch0のみ・クリップgainまで）を作る
                const int numDestChannels = monoFx ? 1 : 2;
                for (int ch = 0; ch < numDestChannels; ++ch)
                {
                    const float* src = clip.audio->getReadPointer (stereo ? ch : 0, srcOffset);
                    // リージョンゲインはトラックゲインの手前（RTのprocessと同じ順序）。
                    // ユニティ(1.0f)なら track.gain と厳密に同値＝既存の書き出しは変わらない
                    const float gain = (monoFx || prePanFx)
                                           ? clip.gain
                                           : clipTrackGain * clip.gain
                                                 * (stereo ? (ch == 0 ? balL : balR)
                                                           : (ch == 0 ? panL : panR));
                    for (int s = 0; s < numSegs; ++s)
                    {
                        const auto& seg = segs[s];
                        const float* segSrc = src + (seg.destOffset - destOffset);
                        ClipFade::addSegment (clipDest, ch, seg.destOffset, segSrc, seg, gain);
                        // sendにも同じフェードが掛かる（post-fader＝聴こえている信号のコピー）。
                        // EQ/Comp有効時はFX通過後にまとめてコピーする（下）
                        if (! activeFx)
                            for (int b = 0; b < numSendBuses; ++b)
                                if (track.sends[b] > 0.0f)
                                    ClipFade::addSegment (busMix[(size_t) b], ch, seg.destOffset, segSrc, seg,
                                                          gain * track.sends[b]);
                    }
                }
            }

            if (activeFx)
            {
                // クリップの重なりがないブロックも処理する（RTと同じ: リングアウトを切らない・連番の連続性）
                float* fxL = eqTrackScratch.getWritePointer (0);
                float* fxR = monoFx ? nullptr : eqTrackScratch.getWritePointer (1);
                TrackFx::process (track.eq, track.compDsp, fxActivity, fxSettings,
                                  fxL, fxR, n, sr, eqBlockSerial, false, {});
                for (int ch = 0; ch < 2; ++ch)
                {
                    // monoFx: モノ合算（ch0）をFX後にpan分配 / prePanFx: FX後にバランスpan
                    //（どちらもRTと同じ処理順）
                    const int srcCh = monoFx ? 0 : ch;
                    const float outGain = track.gain
                                          * (monoFx ? (ch == 0 ? panL : panR)
                                                    : prePanFx ? (ch == 0 ? balL : balR) : 1.0f);
                    mix.addFrom (ch, 0, eqTrackScratch, srcCh, 0, n, outGain);
                    for (int b = 0; b < numSendBuses; ++b)
                        if (track.sends[b] > 0.0f)
                            busMix[(size_t) b].addFrom (ch, 0, eqTrackScratch, srcCh, 0, n,
                                                        outGain * track.sends[b]);
                }
            }

            if (track.synth != nullptr && track.synth->hasRenderer())
            {
                scheduleBlockMidi (track, cursors[ti], pos, n, tps);
                renderSynthInto (mix, busMix, track, n);
            }
        }

        mixBusesAndMaster (mix, busMix, n, pos);

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
        pos += n;
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
            ++eqBlockSerial; // テール中もEQ連番を進める（連続処理＝リセットさせない）
            for (size_t ti = 0; ti < request.tracks.size(); ++ti)
            {
                auto& track = request.tracks[ti];

                // オーディオトラックのリングアウト（入力は無音・フィルタ履歴が減衰しきるまで）。
                // RT再生は範囲終了後もactive経路が回り続けるので、切ると書き出しだけ余韻が欠ける。
                // テールに入る条件は TrackFx::producesTail（現在はEQのみ＝無音入力から出力を生む
                // FXがあるか）。Comp有効ならリングアウトにも掛ける（RTと同じ処理順）
                const TrackFx::Settings fxSettings { track.eqEnabled, track.eqBands,
                                                     track.compEnabled, track.comp };
                if (track.synth == nullptr && TrackFx::producesTail (fxSettings))
                {
                    // 本編と同じ経路構成（monoFx＝モノ処理→pan分配 / prePanFx＝FX後バランスpan）を
                    // 維持する。途中でチャンネル構成やpanの掛け場所を変えるとフィルタ履歴・音量が崩れる
                    const bool monoFx = TrackFx::isMonoClipTrack (track.clips);
                    const bool prePanFx = ! monoFx && TrackFx::isStereoOnlyClipTrack (track.clips);
                    float panL = 1.0f, panR = 1.0f;
                    Pan::monoGains (track.pan, panL, panR);
                    float balL = 1.0f, balR = 1.0f;
                    Pan::stereoGains (track.pan, balL, balR);
                    for (int ch = 0; ch < 2; ++ch)
                        eqTrackScratch.clear (ch, 0, renderBlockSize);
                    float* fxL = eqTrackScratch.getWritePointer (0);
                    float* fxR = monoFx ? nullptr : eqTrackScratch.getWritePointer (1);
                    const auto fxActivity = TrackFx::evaluateActivity (TrackFx::Policy::offline,
                                                                       track.eq, track.compDsp,
                                                                       fxSettings, false);
                    TrackFx::process (track.eq, track.compDsp, fxActivity, fxSettings,
                                      fxL, fxR, renderBlockSize,
                                      request.sampleRate, eqBlockSerial, false, {});
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const int srcCh = monoFx ? 0 : ch;
                        const float outGain = track.gain
                                              * (monoFx ? (ch == 0 ? panL : panR)
                                                        : prePanFx ? (ch == 0 ? balL : balR) : 1.0f);
                        mix.addFrom (ch, 0, eqTrackScratch, srcCh, 0, renderBlockSize, outGain);
                        for (int b = 0; b < numSendBuses; ++b)
                            if (track.sends[b] > 0.0f)
                                busMix[(size_t) b].addFrom (ch, 0, eqTrackScratch, srcCh, 0,
                                                            renderBlockSize,
                                                            outGain * track.sends[b]);
                    }
                    continue;
                }

                // サンプラーもテールを回す（範囲を跨ぐ追従ノートの余韻に必要。
                // pluginだけを見て continue するとサンプラーがテールで一切鳴らない）
                if (track.synth == nullptr || ! track.synth->hasRenderer())
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

            mixBusesAndMaster (mix, busMix, renderBlockSize, rangeEnd + tailRendered);

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
    // 位置判定はサンプル位置で行う（PPQ同士で比べない。理由は PlaybackEngine::renderMidiTracks の
    // コメント参照。RT側と同じ規則にしておかないと再生とバウンスの出力が一致しない）
    const auto blockEnd = pos + numSamples;
    const auto toSample = [tps] (juce::int64 ppq)
    { return (juce::int64) std::llround ((double) ppq / tps); };
    const int ch = track.synth->midiChannel;

    // 1) このブロック内で終わる発音中ノートのオフ（同位置のオンより先に積む。RTと同じ流儀）
    for (size_t i = 0; i < cursor.active.size();)
    {
        const auto [endPpq, pitch] = cursor.active[i];
        const auto offSample = toSample (endPpq);
        if (offSample < blockEnd)
        {
            const int off = juce::jlimit (0, numSamples - 1, (int) (offSample - pos));
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
        const auto onSample = toSample (note.startPpq);
        if (onSample >= blockEnd)
            break; // startPpq昇順なので以降は全てブロック外

        const int on = juce::jlimit (0, numSamples - 1, (int) (onSample - pos));
        midiScratch.addEvent (juce::MidiMessage::noteOn (ch, note.pitch, (juce::uint8) note.velocity), on);

        const auto offSample = toSample (note.endPpq);
        if (offSample < blockEnd)
        {
            const int off = juce::jlimit (on, numSamples - 1, (int) (offSample - pos));
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
                                      TrackRender& track, int numSamples)
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
    if (synth->plugin != nullptr)
        synth->plugin->processBlock (block, midiScratch);
    else
        synth->sampler->processBlock (block, midiScratch);

    // トラックEQ・Comp（シンセ出力直後・gain/panの前。RTのrenderMidiTracksと同じ処理順。
    // 判定・適用はTrackFxChainに集約）
    {
        const TrackFx::Settings fxSettings { track.eqEnabled, track.eqBands,
                                             track.compEnabled, track.comp };
        const auto fxActivity = TrackFx::evaluateActivity (TrackFx::Policy::offline,
                                                           track.eq, track.compDsp, fxSettings, false);
        const int fxSrcR = juce::jmin (1, total - 1);
        TrackFx::process (track.eq, track.compDsp, fxActivity, fxSettings,
                          block.getWritePointer (0),
                          fxSrcR != 0 ? block.getWritePointer (fxSrcR) : nullptr,
                          numSamples, request.sampleRate, eqBlockSerial, false, {});
    }

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

juce::int64 BounceRenderer::Request::fadeStartSample() const
{
    return SongFade::sixteenthsToSamples (fadeOutStartSixteenths, bpm, sampleRate);
}

juce::int64 BounceRenderer::Request::fadeEndSample() const
{
    return SongFade::sixteenthsToSamples (fadeOutEndSixteenths, bpm, sampleRate);
}

bool BounceRenderer::trackWantsTail (const TrackRender& track)
{
    if (track.synth != nullptr)
        return true;
    return TrackFx::producesTail ({ track.eqEnabled, track.eqBands,
                                    track.compEnabled, track.comp });
}

void BounceRenderer::Request::resolveWantTail()
{
    wantTail = false;
    for (const auto& track : tracks)
        if (trackWantsTail (track))
        {
            wantTail = true;
            return;
        }
}

void BounceRenderer::Request::applySongFadeToRange()
{
    if (! hasFadeOut())
        return;

    endSample = fadeEndSample(); // jminにしないこと（ヘッダのコメント参照）
    wantTail = false;
}

void BounceRenderer::mixBusesAndMaster (juce::AudioBuffer<float>& mix,
                                        std::vector<juce::AudioBuffer<float>>& busMix, int numSamples,
                                        juce::int64 absPos)
{
    for (int b = 0; b < numSendBuses; ++b)
    {
        if (request.busMute[b] || request.busGain[b] <= 0.0f)
            continue;
        for (int ch = 0; ch < 2; ++ch)
            mix.addFrom (ch, 0, busMix[(size_t) b], ch, 0, numSamples, request.busGain[b]);
    }

    // 曲末フェード。呼び出し側が境界でブロックを切っているので、このブロックは
    // 「フェード前」「区間内」「終端以後」のいずれかに完全に収まる（RTと同じ規則）
    if (request.hasFadeOut())
    {
        const auto fadeStart = request.fadeStartSample();
        const auto fadeEnd = request.fadeEndSample();
        if (absPos >= fadeEnd)
        {
            mix.clear(); // 終端以後は厳密に無音
            return;
        }
        if (absPos >= fadeStart)
        {
            // endGain は区間の排他端で評価する（GOTCHAS.md）。毎ブロック絶対位置から
            // 計算し直すので誤差はブロック長で頭打ちになる
            const float g0 = request.masterGain * SongFade::gainAt (absPos, fadeStart, fadeEnd);
            const float g1 = request.masterGain * SongFade::gainAt (absPos + numSamples, fadeStart, fadeEnd);
            mix.applyGainRamp (0, numSamples, g0, g1);
            return;
        }
    }

    // フェード前・未設定は既存の演算順序をそのまま通す（順序が変わると出力が微小に変わり、
    // フェードを使っていない既存プロジェクトの書き出しまで変わってしまう）
    if (request.masterGain != 1.0f)
        mix.applyGain (0, numSamples, request.masterGain);
}
