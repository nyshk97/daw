#include "SamplerEngine.h"

#include <cmath>
#include <cstring>

SamplerEngine::SamplerEngine (std::shared_ptr<const juce::AudioBuffer<float>> audio,
                              double sourceSampleRate, double deviceSampleRate)
    : sample (std::move (audio))
{
    if (sample != nullptr && sample->getNumSamples() > 0 && sample->getNumChannels() > 0
        && sourceSampleRate > 0.0 && deviceSampleRate > 0.0)
    {
        sourceLength = (juce::int64) sample->getNumSamples();
        sourceL = sample->getReadPointer (0);
        sourceR = sample->getReadPointer (sample->getNumChannels() >= 2 ? 1 : 0); // 1ch→L/R複製
        baseRate = sourceSampleRate / deviceSampleRate;
    }
    releaseSamples = juce::jmax (1, (int) (deviceSampleRate * releaseSeconds));
}

int SamplerEngine::numActiveVoices() const
{
    int count = 0;
    for (const auto& voice : voices)
        if (voice.active)
            ++count;
    return count;
}

void SamplerEngine::processBlock (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi)
{
    if (sourceLength <= 0 || buffer.getNumChannels() <= 0)
        return;

    const int numSamples = buffer.getNumSamples();

    // 停止要求はMidiBufferのイベントより先に処理する。固定→追従の切り替えでは
    // 「旧ボイスのリリース → その後で resound の新ノートオン」の順序になり、二重発音が起きない
    if (stopAllRequested.exchange (false))
    {
        releaseAll (false);
        std::memset (pendingNoteOffs, 0, sizeof (pendingNoteOffs)); // 全部止めたので飲む予定も捨てる
    }

    // MIDIイベントの位置でブロックを分割してレンダリングする。
    // MidiMessage を構築せず生バイトを見る（3バイトのイベントなのでヒープ確保はそもそも起きないが、
    // オーディオスレッドから確保しうるAPIを呼ばない形に寄せる）
    int rendered = 0;
    for (const auto metadata : midi)
    {
        const int eventPos = juce::jlimit (0, numSamples, metadata.samplePosition);
        if (eventPos > rendered)
        {
            renderVoices (buffer, rendered, eventPos - rendered);
            rendered = eventPos;
        }

        if (metadata.numBytes < 3)
            continue;
        const auto* data = metadata.data;
        const int status = data[0] & 0xf0;
        const int d1 = data[1] & 0x7f;
        const int d2 = data[2] & 0x7f;

        // チャンネルは見ない（1トラック=1サンプラーで、届くイベントは全て自分のもの）
        if (status == 0x90 && d2 > 0)
            handleNoteOn (d1, d2);
        else if (status == 0x80 || (status == 0x90 && d2 == 0))
            handleNoteOff (d1);
        else if (status == 0xb0 && d1 == 123) // All Notes Off
        {
            releaseAll (false);
            std::memset (pendingNoteOffs, 0, sizeof (pendingNoteOffs));
        }
    }

    if (rendered < numSamples)
        renderVoices (buffer, rendered, numSamples - rendered);
}

void SamplerEngine::handleNoteOn (int pitch, int velocity)
{
    // モノ: 新しい打点で前の音を切る（Logicの Polyphony: 1 相当）。
    // 先にリリース化しておくと、下のボイス割り当ては空きスロットを選ぶので
    // フェード中の旧ボイスは奪われず、5msかけて滑らかに消える（切り替えのクリック防止）。
    // 切った追従ボイスのオフは飲む（後から届いて新しいボイスを止めるのを防ぐ）
    if (mono.load())
        releaseAll (true);

    // 空きボイス優先。満杯なら最古（orderが最小）を奪う＝連打で前の音を切らない
    int slot = -1;
    for (int i = 0; i < maxVoices; ++i)
    {
        if (! voices[i].active)
        {
            slot = i;
            break;
        }
        if (slot < 0 || voices[i].order < voices[slot].order)
            slot = i;
    }

    const bool follow = pitchFollow.load();
    const auto offset = juce::jlimit ((juce::int64) 0, sourceLength - 1, startOffset.load());
    double rate = baseRate;
    if (follow)
        rate *= std::pow (2.0, (double) (pitch - rootNote.load()) / 12.0);

    auto& voice = voices[slot];
    voice.active = true;
    voice.pitch = pitch;
    voice.position = (double) offset;
    // クランプは異常値（NaN・0除算由来の値）への防御のみで、MIDIの全域を通す幅を取る:
    // ルート音とノートは0..127なので音程差は±127半音 = 2^(±127/12) ≈ 1534倍 / 1/1534倍、
    // さらにSR比（最大でも192k/44.1k ≈ 4.4倍）が掛かる。ここを狭くすると
    // 「離れた音程で 2^((pitch-rootNote)/12) にならない」＝仕様と食い違う
    voice.rate = juce::jlimit (1.0e-6, 8192.0, rate);
    voice.voiceGain = gain.load() * (float) velocity / 127.0f;
    voice.followMode = follow;
    voice.releasing = false;
    voice.sounded = false;
    voice.releaseLeft = 0;
    voice.order = nextOrder++;
}

void SamplerEngine::handleNoteOff (int pitch)
{
    // Monoが先に切ったノートのオフはここで飲む。飲まないと、古いノートのオフが
    // 「今鳴っている新しいボイス」を止めてしまう（MIDIのnoteOffにノート個体のIDがないため、
    // 対応関係をこちら側で持つ必要がある）
    if (pendingNoteOffs[pitch] > 0)
    {
        --pendingNoteOffs[pitch];
        return;
    }

    // 固定モード（One Shot）で鳴り始めたボイスは noteOff を無視して最後まで鳴る。
    // 追従モードのボイスは同ピッチの未リリースのうち最古を1本だけリリースする
    // （同ピッチ連打で後の発音まで切らないため）
    int target = -1;
    for (int i = 0; i < maxVoices; ++i)
    {
        auto& voice = voices[i];
        if (! voice.active || voice.releasing || ! voice.followMode || voice.pitch != pitch)
            continue;
        if (target < 0 || voice.order < voices[target].order)
            target = i;
    }
    if (target >= 0)
    {
        voices[target].releasing = true;
        voices[target].releaseLeft = releaseSamples;
    }
}

void SamplerEngine::releaseAll (bool swallowFollowOffs)
{
    for (auto& voice : voices)
    {
        if (! voice.active || voice.releasing)
            continue;

        // 追従ボイスは対応する noteOff が後から届く。Mono由来の停止ではそれを飲む必要がある
        if (swallowFollowOffs && voice.followMode && pendingNoteOffs[voice.pitch] < 255)
            ++pendingNoteOffs[voice.pitch];

        if (! voice.sounded)
        {
            // 一度も出力していないボイス（同じ位置に複数の noteOn が来た場合）は即停止。
            // フェードさせても振幅を足すだけで音楽的な中身がなく、尾が積み上がるだけ
            voice.active = false;
            continue;
        }
        voice.releasing = true;
        voice.releaseLeft = releaseSamples;
    }
}

void SamplerEngine::renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    float* out[2] = { buffer.getWritePointer (0), nullptr };
    if (buffer.getNumChannels() >= 2)
        out[1] = buffer.getWritePointer (1);

    for (auto& voice : voices)
    {
        if (! voice.active)
            continue;

        for (int i = 0; i < numSamples; ++i)
        {
            if (voice.position >= (double) sourceLength)
            {
                voice.active = false; // サンプル末尾に到達
                break;
            }

            // 線形補間（ワンショットのピッチシフト用途では十分。品質が要れば4点Hermiteへ）
            const auto index = (juce::int64) voice.position;
            const auto next = juce::jmin (index + 1, sourceLength - 1);
            const float frac = (float) (voice.position - (double) index);
            const float l = sourceL[index] + (sourceL[next] - sourceL[index]) * frac;
            const float r = sourceR[index] + (sourceR[next] - sourceR[index]) * frac;

            float env = voice.voiceGain;
            bool finished = false;
            if (voice.releasing)
            {
                env *= (float) voice.releaseLeft / (float) releaseSamples;
                finished = --voice.releaseLeft <= 0;
            }

            out[0][startSample + i] += l * env;
            if (out[1] != nullptr)
                out[1][startSample + i] += r * env;
            voice.sounded = true;

            voice.position += voice.rate;
            if (finished)
            {
                voice.active = false;
                break;
            }
        }
    }
}
