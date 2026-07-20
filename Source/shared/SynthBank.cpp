#include "SynthBank.h"

namespace
{
// DLSMusicDevice（macOS内蔵GM音源）のJUCE形式識別子
const char* const dlsIdentifier = "AudioUnit:Synths/aumu,dls ,appl";

} // namespace

bool SynthBank::sync (const Project& project, double sampleRate, int deviceBlockSize)
{
    if (sampleRate <= 0.0)
        return false;

    // AUには余裕を持ったブロックサイズで prepareToPlay しておき、
    // 実コールバックの numSamples がこれを超えた場合はエンジン側が安全にスキップする
    const int blockSize = juce::jmax (4096, deviceBlockSize);

    bool changed = false;

    // 現存するMIDIトラックに対応するインスタンスを用意する
    for (const auto& track : project.tracks)
    {
        if (track.type != TrackType::midi)
            continue;

        auto it = entries.find (track.id);
        const bool needsCreate =
            it == entries.end()
            || it->second.gmProgram != track.gmProgram
            || it->second.drums != track.drums
            || (it->second.synth != nullptr
                && (! juce::approximatelyEqual (it->second.synth->preparedSampleRate, sampleRate)
                    || it->second.synth->preparedBlockSize < blockSize));

        if (needsCreate)
        {
            Entry entry;
            entry.gmProgram = track.gmProgram;
            entry.drums = track.drums;
            entry.synth = createSynth (track.gmProgram, track.drums, sampleRate, blockSize);
            entries[track.id] = std::move (entry); // 旧synthへの参照はここで手放す（破棄はスナップショット退役後）
            changed = true;
        }
    }

    // 削除されたトラックのインスタンスを手放す
    for (auto it = entries.begin(); it != entries.end();)
    {
        const auto trackId = it->first;
        const bool exists = std::any_of (project.tracks.begin(), project.tracks.end(),
                                         [trackId] (const Track& t)
                                         { return t.id == trackId && t.type == TrackType::midi; });
        if (! exists)
        {
            it = entries.erase (it);
            changed = true;
        }
        else
        {
            ++it;
        }
    }

    return changed;
}

std::shared_ptr<SynthInstance> SynthBank::get (juce::uint64 trackId) const
{
    const auto it = entries.find (trackId);
    return it != entries.end() ? it->second.synth : nullptr;
}

std::shared_ptr<SynthInstance> SynthBank::createSynth (int gmProgram, bool drums,
                                                       double sampleRate, int blockSize)
{
    // 生成は稀（トラック追加・楽器変更・レート変更時のみ）なので、キャッシュせず毎回引く
    juce::AudioUnitPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, dlsIdentifier);
    if (found.isEmpty())
    {
        jassertfalse; // macOSなら常に見つかるはず
        return nullptr;
    }

    juce::String error;
    auto plugin = format.createInstanceFromDescription (*found.getFirst(), sampleRate, blockSize, error);
    if (plugin == nullptr)
        return nullptr;

    auto synth = std::make_shared<SynthInstance>();
    synth->midiChannel = drums ? 10 : 1; // GM: ch10はドラムキット固定
    synth->preparedSampleRate = sampleRate;
    synth->preparedBlockSize = blockSize;

    // DLSは出力バスを2本（計4ch）報告し、disableNonMainBuses() でも減らせない。
    // processBlock には全チャンネル分のバッファを渡す必要がある（不足するとチャンネル範囲外アクセス）。
    // ミックスに使うのはメインバスの ch0/1 だけ
    synth->totalOutputChannels = juce::jmax (2, plugin->getTotalNumOutputChannels());

    plugin->prepareToPlay (sampleRate, blockSize);

    // 楽器（プログラム）の適用。まだこのインスタンスは自分しか参照していないので、
    // メッセージスレッドから直接1ブロック流してよい（スナップショットに載せる前）
    {
        juce::AudioBuffer<float> warmup (juce::jmax (2, plugin->getTotalNumOutputChannels()), 64);
        warmup.clear();
        juce::MidiBuffer midi;
        if (! drums)
            midi.addEvent (juce::MidiMessage::programChange (synth->midiChannel, gmProgram), 0);
        plugin->processBlock (warmup, midi);
    }

    synth->plugin = std::move (plugin);
    return synth;
}
