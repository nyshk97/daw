#include "SynthBank.h"

#include "Log.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

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
        const bool rateChanged =
            it != entries.end() && it->second.synth != nullptr
            && (! juce::approximatelyEqual (it->second.synth->preparedSampleRate, sampleRate)
                || it->second.synth->preparedBlockSize < blockSize);
        const bool needsCreate =
            it == entries.end()
            || it->second.instrument != track.instrument
            || (track.instrument == InstrumentKind::gm
                && (it->second.gmProgram != track.gmProgram || it->second.drums != track.drums))
            || (track.instrument == InstrumentKind::sample && it->second.sampleFile != track.sampleFile)
            || rateChanged;

        if (needsCreate)
        {
            Entry entry;
            entry.instrument = track.instrument;
            entry.gmProgram = track.gmProgram;
            entry.drums = track.drums;
            entry.sampleFile = track.sampleFile;
            entry.pitchFollow = track.samplePitchFollow;
            entry.synth = track.instrument == InstrumentKind::sample
                              ? createSampler (track, sampleRate, blockSize)
                              : createSynth (track.gmProgram, track.drums, sampleRate, blockSize);
            entries[track.id] = std::move (entry); // 旧synthへの参照はここで手放す（破棄はスナップショット退役後）
            changed = true;
            continue;
        }

        if (track.instrument != InstrumentKind::sample)
            continue;

        // サンプルの音量・頭カット・音程モード・ルート音はインスタンスを作り直さず atomic の更新だけで
        // 反映する（作り直すと発音中の音が切れる）
        auto& entry = it->second;
        if (entry.synth == nullptr)
            continue;
        applySampleParams (*entry.synth, track);

        // 音程モードが切り替わった瞬間は全ボイスを止める（旧モードのボイスが残ったまま
        // 新モードの resound が走ると二重発音・不自然な打ち切りになる）。
        // この判定をUIハンドラでなく sync() に置くのは、undo/redo が
        // afterHistoryRestore() → pushSnapshot() → sync() の経路で復元されUIを通らないため。
        // UI側で立てると undo での切り替わりを取りこぼす
        if (entry.pitchFollow != track.samplePitchFollow)
        {
            entry.pitchFollow = track.samplePitchFollow;
            entry.synth->sampler->requestStopAll();
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

std::shared_ptr<SynthInstance> SynthBank::createIndependent (const Track& track,
                                                             double sampleRate, int blockSize)
{
    if (track.instrument == InstrumentKind::sample)
        return createSampler (track, sampleRate, blockSize); // サンプラーは実時間の概念を持たない

    auto synth = createSynth (track.gmProgram, track.drums, sampleRate, blockSize);
    if (synth != nullptr && synth->plugin != nullptr)
        synth->plugin->setNonRealtime (true); // オフラインレンダリング（実時間より速いprocessBlock連打）
    return synth;
}

std::shared_ptr<SynthInstance> SynthBank::createSampler (const Track& track,
                                                         double sampleRate, int blockSize)
{
    if (! track.hasSample())
    {
        // ファイル欠損（読込時に警告済み）。トラックは無音になる
        Log::warn ("instrument.load_fail", "file=" + track.sampleFile
                                               + " sourceSr=" + juce::String (track.sampleSourceRate, 0));
        return nullptr;
    }

    auto synth = std::make_shared<SynthInstance>();
    synth->sampler = std::make_unique<SamplerEngine> (track.sampleAudio, track.sampleSourceRate, sampleRate);
    synth->midiChannel = 1;            // サンプラーはチャンネルを見ないが、既存経路と揃える
    synth->preparedSampleRate = sampleRate;
    synth->preparedBlockSize = blockSize;
    synth->totalOutputChannels = 2;    // サンプラーの出力は常にステレオ
    applySampleParams (*synth, track);
    return synth;
}

void SynthBank::applySampleParams (SynthInstance& synth, const Track& track)
{
    if (synth.sampler == nullptr)
        return;
    synth.sampler->setGain (track.sampleGain);
    synth.sampler->setStartOffset (track.sampleStartOffset);
    synth.sampler->setRootNote (track.sampleRootNote);
    synth.sampler->setPitchFollow (track.samplePitchFollow);
    synth.sampler->setMono (track.sampleMono);
    // 固定モード（One Shot）は PlaybackEngine の resound 判定にも使う
    synth.oneShot.store (! track.samplePitchFollow);
}

juce::StringArray SynthBank::takeCreateErrors()
{
    auto errors = std::move (pendingCreateErrors);
    pendingCreateErrors.clear();
    return errors;
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
        Log::error ("synth.dls_not_found", juce::String ("identifier=") + dlsIdentifier);
        pendingCreateErrors.add (jp (u8"GM音源（DLSMusicDevice）が見つかりませんでした。"));
        jassertfalse; // macOSなら常に見つかるはず
        return nullptr;
    }

    juce::String error;
    auto plugin = format.createInstanceFromDescription (*found.getFirst(), sampleRate, blockSize, error);
    if (plugin == nullptr)
    {
        // 該当トラックは無音になる。sync() は失敗をキャッシュするので設定が変わるまで再試行されない
        Log::error ("synth.create_failed", "error=" + error + " program=" + juce::String (gmProgram)
                                               + " drums=" + juce::String ((int) drums)
                                               + " sr=" + juce::String (sampleRate, 0));
        pendingCreateErrors.add (jp (u8"GM音源を作成できませんでした（トラックは無音になります）: ") + error);
        return nullptr;
    }

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
