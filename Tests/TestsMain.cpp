// daw_tests — CTest から実行するコンソールテスト。
// GUIなしで動くもの（データモデル・保存/読込・DLSMusicDeviceのオフラインレンダリング）だけを検証する。
// テストは一時ディレクトリのみを使い、~/Music/daw には一切触れない。

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "audio/PlaybackEngine.h"
#include "shared/Project.h"
#include "shared/SynthBank.h"
#include "shared/UndoStack.h"

namespace
{
int failureCount = 0;
juce::String currentTest;

void expect (bool condition, const char* description)
{
    if (! condition)
    {
        ++failureCount;
        std::cout << "FAIL [" << currentTest << "] " << description << std::endl;
    }
}

void beginTest (const char* name)
{
    currentTest = name;
    std::cout << "---- " << name << std::endl;
}

// 一時ディレクトリ（テストごとに一意）。~/Music/daw には一切触れない
juce::File makeTempDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("daw-tests-" + juce::Uuid().toString());
    dir.createDirectory();
    return dir;
}

// テスト用の小さなモノラルWAVを書く
bool writeTestWav (const juce::File& file, int numSamples)
{
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;

    using Opts = juce::AudioFormatWriterOptions;
    auto writer = wavFormat.createWriterFor (stream,
        Opts{}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer (1, numSamples);
    for (int i = 0; i < numSamples; ++i)
        buffer.setSample (0, i, std::sin ((float) i * 0.1f) * 0.5f);
    return writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
}

// ---- v1プロジェクト読込 → v2保存 → 再読込のラウンドトリップ ----
void testV1ToV2Roundtrip()
{
    beginTest ("v1 -> v2 roundtrip");
    const auto dir = makeTempDir();

    expect (writeTestWav (dir.getChildFile ("clip-001.wav"), 4410), "テストWAVを書けること");

    // v1形式のJSON（ID・type・nextId無し）
    const char* v1json = R"({
        "version": 1, "bpm": 95.5, "sampleRate": 44100.0,
        "tracks": [
            { "type": "audio", "name": "Vocal", "mute": true, "solo": false, "volume": 0.6,
              "clips": [ { "file": "clip-001.wav", "startSample": 12345 } ] },
            { "type": "audio", "name": "Chorus", "mute": false, "solo": true, "volume": 0.9, "clips": [] }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (v1json);

    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr, "v1を読込めること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    expect (warnings.isEmpty(), "警告が出ないこと");
    expect (project->tracks.size() == 2, "トラック数2");
    expect (juce::approximatelyEqual (project->bpm, 95.5), "bpm維持");
    expect (project->tracks[0].id != 0 && project->tracks[1].id != 0, "IDが採番されること");
    expect (project->tracks[0].id != project->tracks[1].id, "IDが一意であること");
    expect (project->tracks[0].type == TrackType::audio, "v1トラックはaudio種別");
    expect (project->tracks[0].clips.size() == 1, "クリップが読めること");
    expect (project->tracks[0].clips[0].startSample == 12345, "startSample維持");
    expect (project->tracks[0].params->mute.load(), "mute維持");
    expect (project->tracks[1].params->solo.load(), "solo維持");

    const auto id0 = project->tracks[0].id;
    const auto id1 = project->tracks[1].id;

    expect (project->save (error), "v2で保存できること");

    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr, "v2を再読込できること");
    if (reloaded == nullptr)
        { dir.deleteRecursively(); return; }

    expect (reloaded->tracks.size() == 2, "再読込後のトラック数2");
    expect (reloaded->tracks[0].id == id0 && reloaded->tracks[1].id == id1,
            "保存→再読込でIDが安定していること");
    expect (reloaded->tracks[0].clips.size() == 1, "再読込後もクリップが読めること");
    expect (juce::approximatelyEqual (reloaded->bpm, 95.5), "再読込後のbpm維持");

    dir.deleteRecursively();
}

// ---- MIDIトラックの保存/読込ラウンドトリップ ----
void testMidiRoundtrip()
{
    beginTest ("midi track roundtrip");
    const auto dir = makeTempDir();

    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "createNewできること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    Track midiTrack;
    midiTrack.id = project->allocateId();
    midiTrack.type = TrackType::midi;
    midiTrack.name = "Drums";
    midiTrack.gmProgram = 33;
    midiTrack.drums = true;
    midiTrack.drumPitch = 36; // 固定ピッチ打楽器（Kick）

    MidiRegion region;
    region.id = project->allocateId();
    region.startPpq = Ppq::ticksPerBar * 2;
    region.lengthPpq = Ppq::ticksPerBar;

    MidiNote note1 { project->allocateId(), 36, 0, Ppq::ticksPerQuarter / 8, 127 };
    MidiNote note2 { project->allocateId(), 38, Ppq::ticksPerQuarter, 80, 1 }; // 1/32三連符=80tick, velocity最小
    region.notes.push_back (note1);
    region.notes.push_back (note2);
    midiTrack.midiRegions.push_back (region);
    project->tracks.push_back (std::move (midiTrack));

    expect (project->save (error), "保存できること");

    juce::StringArray warnings;
    auto reloaded = Project::load (project->directory, warnings, error);
    expect (reloaded != nullptr, "再読込できること");
    if (reloaded == nullptr)
        { dir.deleteRecursively(); return; }

    expect (reloaded->tracks.size() == 2, "トラック数2");
    const auto& t = reloaded->tracks[1];
    expect (t.type == TrackType::midi, "midi種別維持");
    expect (t.gmProgram == 33 && t.drums, "gmProgram/drums維持");
    expect (t.drumPitch == 36, "drumPitch維持");
    expect (t.midiRegions.size() == 1, "リージョン数1");
    const auto& r = t.midiRegions[0];
    expect (r.id == region.id, "リージョンID維持");
    expect (r.startPpq == Ppq::ticksPerBar * 2 && r.lengthPpq == Ppq::ticksPerBar, "リージョン位置維持");
    expect (r.notes.size() == 2, "ノート数2");
    expect (r.notes[0].id == note1.id && r.notes[0].pitch == 36
                && r.notes[0].lengthPpq == Ppq::ticksPerQuarter / 8 && r.notes[0].velocity == 127,
            "ノート1維持");
    expect (r.notes[1].id == note2.id && r.notes[1].startPpq == Ppq::ticksPerQuarter
                && r.notes[1].lengthPpq == 80 && r.notes[1].velocity == 1,
            "ノート2維持（1/32三連符・velocity境界）");

    dir.deleteRecursively();
}

// ---- 不正なJSONへの防御 ----
void testInvalidJson()
{
    beginTest ("invalid json");
    const auto dir = makeTempDir();
    juce::StringArray warnings;
    juce::String error;

    // 壊れたJSON → エラーで nullptr
    dir.getChildFile ("project.json").replaceWithText ("{ not valid json !!");
    expect (Project::load (dir, warnings, error) == nullptr, "壊れたJSONはnullptr");
    expect (error.isNotEmpty(), "エラーメッセージが入ること");

    // 型違い・欠損・範囲外 → クラッシュせずデフォルト/クランプで読む
    const char* weird = R"({
        "version": 2, "bpm": 99999, "sampleRate": -5,
        "tracks": [
            42,
            { "type": "midi", "name": "X", "gmProgram": 999, "drums": false,
              "regions": [
                  "not an object",
                  { "id": 7, "startPpq": -100, "lengthPpq": 0,
                    "notes": [ { "id": 7, "pitch": 200, "startPpq": -5, "lengthPpq": 0, "velocity": 0 } ] }
              ] },
            { "type": "audio", "name": "Y", "clips": "nope" }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (weird);

    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr, "型違い混在でも読込めること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    expect (project->tracks.size() == 2, "非オブジェクトのトラックは読み飛ばす");
    expect (project->bpm <= 300.0, "bpmがクランプされること");
    expect (project->sampleRate >= 0.0, "sampleRateがクランプされること");

    const auto& t = project->tracks[0];
    expect (t.gmProgram == 127, "gmProgramクランプ");
    expect (t.midiRegions.size() == 1, "非オブジェクトのリージョンは読み飛ばす");
    const auto& r = t.midiRegions[0];
    expect (r.startPpq == 0 && r.lengthPpq == 1, "リージョンの負開始・長さ0がクランプされること");
    expect (r.notes.size() == 1, "ノートが読めること");
    expect (r.notes[0].pitch == 127 && r.notes[0].velocity == 1
                && r.notes[0].startPpq == 0 && r.notes[0].lengthPpq == 1,
            "ノートの範囲外値がクランプされること");
    // id重複 (region 7 / note 7) はどちらかが振り直される
    expect (r.id != r.notes[0].id, "重複IDが振り直されること");

    dir.deleteRecursively();
}

// ---- clampNote の境界規則 ----
void testClampNoteBoundaries()
{
    beginTest ("clampNote boundaries");

    MidiRegion region;
    region.lengthPpq = Ppq::ticksPerBar; // 3840

    MidiNote note;
    note.startPpq = Ppq::ticksPerBar + 100; // リージョン外
    note.lengthPpq = 0;
    note.pitch = -3;
    note.velocity = 300;
    region.clampNote (note);

    expect (note.startPpq == Ppq::ticksPerBar - 1, "開始はリージョン末尾-1へクランプ");
    expect (note.lengthPpq == 1, "長さ最小1tick");
    expect (note.pitch == 0, "pitch下限0");
    expect (note.velocity == 127, "velocity上限127");

    // リージョン端を越えて伸びるノートは許容（再生時マスク）
    MidiNote longNote;
    longNote.startPpq = Ppq::ticksPerBar - 10;
    longNote.lengthPpq = Ppq::ticksPerQuarter * 8;
    region.clampNote (longNote);
    expect (longNote.startPpq == Ppq::ticksPerBar - 10 && longNote.lengthPpq == Ppq::ticksPerQuarter * 8,
            "リージョン端を越える長さは維持されること");
}

// ---- UndoStack: 構造編集の巻き戻し・ミキサー値の非対象・WAV GC保護 ----
void testUndoStack()
{
    beginTest ("UndoStack");

    Project project;
    Track first;
    first.id = 1;
    first.name = "A";
    project.tracks.push_back (std::move (first));

    UndoStack undo;
    expect (! undo.canUndo() && ! undo.canRedo(), "初期状態は履歴なし");

    // トラック追加 → undo → redo
    undo.begin (project);
    Track second;
    second.id = 2;
    second.name = "B";
    project.tracks.push_back (std::move (second));

    expect (undo.undo (project), "undoできること");
    expect (project.tracks.size() == 1 && project.tracks[0].name == "A", "追加前に戻ること");
    expect (undo.redo (project), "redoできること");
    expect (project.tracks.size() == 2 && project.tracks[1].name == "B", "redoで復元されること");

    // MIDIリージョン編集のundo
    project.tracks[1].type = TrackType::midi;
    undo.begin (project);
    MidiRegion region;
    region.id = 3;
    project.tracks[1].midiRegions.push_back (region);
    expect (undo.undo (project), "リージョン追加をundoできること");
    expect (project.tracks[1].midiRegions.empty(), "リージョンが消えること");

    // ミキサー値（TrackParams）はundo対象外: begin後の変更がundoで巻き戻らない
    undo.begin (project);
    project.tracks.pop_back();
    project.tracks[0].params->mute.store (true);
    undo.undo (project);
    expect (project.tracks.size() == 2, "構造は戻ること");
    expect (project.tracks[0].params->mute.load(), "ミキサー値はundoで巻き戻らないこと");
}

void testSaveGcProtectsUndoWavs()
{
    beginTest ("save GC protects undo-referenced wavs");
    const auto dir = makeTempDir();

    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "createNewできること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    const auto wavFile = project->directory.getChildFile ("clip-001.wav");
    expect (writeTestWav (wavFile, 4410), "テストWAVを書けること");

    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.audio = Project::loadWavMono (wavFile);
    project->tracks[0].clips.push_back (std::move (clip));

    UndoStack undo;
    undo.begin (*project);
    project->tracks[0].clips.clear(); // クリップ削除（undo履歴には残っている）

    expect (project->save (error, undo.referencedWavs()), "保存できること");
    expect (wavFile.existsAsFile(), "undo履歴が参照するWAVはGCされないこと");

    // 履歴なしで保存すればGCされる（従来どおり）
    UndoStack emptyUndo;
    expect (project->save (error, emptyUndo.referencedWavs()), "再保存できること");
    expect (! wavFile.existsAsFile(), "未参照WAVはGCされること");

    dir.deleteRecursively();
}

// ---- buildSnapshot のノートフラット化（絶対PPQ変換・リージョン境界マスク・ソート）----
void testBuildSnapshotFlattensNotes()
{
    beginTest ("buildSnapshot flattens midi notes");

    Project project;
    Track track;
    track.id = 1;
    track.type = TrackType::midi;

    MidiRegion region;
    region.id = 2;
    region.startPpq = Ppq::ticksPerBar;      // 2小節目
    region.lengthPpq = Ppq::ticksPerBar;

    // リージョン相対: 3拍目から2小節分（リージョン端を大きくはみ出す→マスクされる）
    region.notes.push_back ({ 3, 60, Ppq::ticksPerQuarter * 2, Ppq::ticksPerBar * 2, 100 });
    // 1拍目（後ろのノートより先に鳴る。ソート確認用に後から追加）
    region.notes.push_back ({ 4, 64, 0, Ppq::ticksPerQuarter, 90 });
    track.midiRegions.push_back (region);
    project.tracks.push_back (std::move (track));

    auto snapshot = project.buildSnapshot();
    expect (snapshot->tracks.size() == 1, "トラック数1");
    const auto& notes = snapshot->tracks[0].notes;
    expect (notes.size() == 2, "ノート2つ");
    if (notes.size() != 2)
        return;

    expect (notes[0].startPpq == Ppq::ticksPerBar && notes[0].pitch == 64,
            "startPpq昇順にソートされること");
    expect (notes[0].endPpq == Ppq::ticksPerBar + Ppq::ticksPerQuarter, "絶対PPQ変換");
    expect (notes[1].startPpq == Ppq::ticksPerBar + Ppq::ticksPerQuarter * 2, "絶対PPQ変換2");
    expect (notes[1].endPpq == Ppq::ticksPerBar * 2, "リージョン端でマスクされること");

    // 固定ピッチ打楽器（Kick等）は再生時に全ノートのピッチが置き換わる
    project.tracks[0].drums = true;
    project.tracks[0].drumPitch = 36;
    auto drumSnapshot = project.buildSnapshot();
    expect (drumSnapshot->tracks[0].notes.size() == 2, "固定ピッチでもノート数は同じ");
    for (auto& note : drumSnapshot->tracks[0].notes)
        expect (note.pitch == 36, "固定ピッチに置き換わること");
}

// ---- SynthBank: プロジェクトのMIDIトラックに応じた生成・差し替え・破棄と発音 ----
void testSynthBank()
{
    beginTest ("SynthBank lifecycle and sound");

    Project project;
    Track track;
    track.id = 10;
    track.type = TrackType::midi;
    track.gmProgram = 0;
    project.tracks.push_back (std::move (track));

    SynthBank bank;
    expect (! bank.sync (project, 0.0, 512), "sampleRate未確定の間は何もしないこと");
    expect (bank.get (10) == nullptr, "未確定中はsynthなし");

    expect (bank.sync (project, 44100.0, 512), "レート確定後の初回syncで生成されること");
    auto synth = bank.get (10);
    expect (synth != nullptr && synth->plugin != nullptr, "synthが生成されること");
    expect (! bank.sync (project, 44100.0, 512), "変更がなければsyncはfalse");

    if (synth != nullptr && synth->plugin != nullptr)
    {
        expect (synth->midiChannel == 1, "非ドラムはch1");
        expect (synth->totalOutputChannels >= 2, "出力チャンネル数が記録されること");

        juce::AudioBuffer<float> buffer (synth->totalOutputChannels, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (synth->midiChannel, 60, (juce::uint8) 100), 0);
        float magnitude = 0.0f;
        for (int i = 0; i < 20; ++i)
        {
            buffer.clear();
            synth->plugin->processBlock (buffer, midi);
            midi.clear();
            magnitude = juce::jmax (magnitude, buffer.getMagnitude (0, buffer.getNumSamples()));
        }
        expect (magnitude > 0.001f, "SynthBank生成のsynthで音が出ること");
    }

    // 楽器変更 → インスタンス差し替え
    project.tracks[0].gmProgram = 48;
    expect (bank.sync (project, 44100.0, 512), "楽器変更でsyncがtrue");
    auto replaced = bank.get (10);
    expect (replaced != nullptr && replaced != synth, "別インスタンスに差し替わること");

    // ドラム指定 → ch10
    project.tracks[0].drums = true;
    bank.sync (project, 44100.0, 512);
    auto drumSynth = bank.get (10);
    expect (drumSynth != nullptr && drumSynth->midiChannel == 10, "ドラムはch10");

    // トラック削除 → エントリ破棄
    project.tracks.clear();
    expect (bank.sync (project, 44100.0, 512), "トラック削除でsyncがtrue");
    expect (bank.get (10) == nullptr, "削除後はsynthなし");
}

// ---- PlaybackEngine: MIDIトラックの再生・シーク再発音・停止消音・ミュート時のイベント継続 ----
void testPlaybackEngineMidi()
{
    beginTest ("PlaybackEngine midi rendering");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // bpm 120 / Strings（持続音で判定しやすい）。ノートは曲頭から8小節伸ばす
    Project project;
    Track track;
    track.id = 20;
    track.type = TrackType::midi;
    track.gmProgram = 48;
    MidiRegion region;
    region.id = 21;
    region.startPpq = 0;
    region.lengthPpq = Ppq::ticksPerBar * 8;
    region.notes.push_back ({ 22, 60, 0, Ppq::ticksPerBar * 8, 100 });
    track.midiRegions.push_back (region);
    project.tracks.push_back (std::move (track));

    SynthBank bank;
    bank.sync (project, sr, blockSize);
    auto pushSnapshot = [&]
    {
        auto snapshot = project.buildSnapshot();
        snapshot->tracks[0].synth = bank.get (20);
        snapshots.push (std::move (snapshot));
    };
    pushSnapshot();

    juce::AudioBuffer<float> buffer (2, blockSize);
    auto processBlocks = [&] (int count)
    {
        float magnitude = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            magnitude = juce::jmax (magnitude, buffer.getMagnitude (0, 0, blockSize));
        }
        return magnitude;
    };

    // 停止中は無音
    expect (processBlocks (5) < 0.0001f, "停止中は無音");

    // 再生でノートが鳴る
    engine.play();
    expect (processBlocks (20) > 0.001f, "再生でMIDIノートが鳴ること");

    // 一旦止めて減衰させ、ノートの途中（2小節目）へシーク → 跨ぎノートが再発音される
    engine.stop();
    processBlocks (100); // 停止エッジの消音＋リリース減衰
    transport.seekRequest.store ((juce::int64) (sr * 2.0)); // 2秒 = 2小節目頭
    engine.play();
    expect (processBlocks (20) > 0.001f, "シーク途中の持続音が再発音されること");

    // 停止で鳴り止む（リリース減衰後にほぼ無音）
    engine.stop();
    processBlocks (150);
    expect (processBlocks (5) < 0.01f, "停止後は減衰して静かになること");

    // ミュート中もイベントは処理される（ミックスだけ0）: ミュートで再生開始→無音、
    // 途中でミュート解除→ノートオンを取りこぼしていなければ即musicが聞こえる
    project.tracks[0].params->mute.store (true);
    transport.seekRequest.store (0);
    engine.play();
    expect (processBlocks (20) < 0.0001f, "ミュート中は完全に無音（加算ゲイン0）");
    project.tracks[0].params->mute.store (false);
    expect (processBlocks (10) > 0.001f, "ミュート解除直後から鳴ること（イベント送信は止まっていない）");
    engine.stop();
    processBlocks (200); // 消音＋リリース減衰

    // プレビュー発音: 停止中にFIFO経由でノートオン → 音が出る → 固定発音長（0.5秒）後に自動オフ
    auto synth = bank.get (20);
    expect (synth != nullptr, "synth取得");
    const auto countActive = [&synth] (int pitch)
    {
        int count = 0;
        if (synth != nullptr)
            for (int i = 0; i < synth->numActiveNotes; ++i)
                if (synth->activeNotes[i].pitch == pitch)
                    ++count;
        return count;
    };

    previewFifo.push ({ PreviewFifo::Command::Type::noteOn, 20, 72, 100 });
    expect (processBlocks (10) > 0.001f, "停止中のプレビュー発音が鳴ること");
    expect (countActive (72) == 1, "プレビュー中はactiveNotesに載ること");
    processBlocks (60); // 0.5秒（約43ブロック）を超えて回す → 自動ノートオフ
    expect (countActive (72) == 0, "固定発音長の経過で自動ノートオフされること");

    // ペインを閉じたときの打ち消し（allNotesOff）
    previewFifo.push ({ PreviewFifo::Command::Type::noteOn, 20, 60, 100 });
    processBlocks (3);
    previewFifo.push ({ PreviewFifo::Command::Type::allNotesOff, 20, 0, 0 });
    processBlocks (3);
    expect (countActive (60) == 0, "allNotesOffでプレビューが打ち消されること");

    // 再生中はプレビューコマンドを破棄する
    engine.play();
    processBlocks (2);
    previewFifo.push ({ PreviewFifo::Command::Type::noteOn, 20, 84, 100 });
    processBlocks (3);
    expect (countActive (84) == 0, "再生中のプレビューは破棄されること");
    engine.stop();
    processBlocks (10);

    snapshots.deleteRetired();
}

// ---- PlaybackEngine: トラックメーターは重なるクリップの「合算後」ピークを測る（混入なし）----
void testTrackLevelMeter()
{
    beginTest ("track level meter");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    auto makeClip = [] (float amplitude, int numSamples)
    {
        Clip clip;
        clip.startSample = 0;
        clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            clip.audio->setSample (0, i, amplitude);
        return clip;
    };

    Project project;
    {
        Track track; // 0.6+0.6の重なり → 合算後ピークは約1.2（個別maxの0.6では0.9を超えない）
        track.id = 1;
        track.name = "overlap";
        track.params->gain.store (1.0f);
        track.clips.push_back (makeClip (0.6f, blockSize * 8));
        track.clips.push_back (makeClip (0.6f, blockSize * 8));
        project.tracks.push_back (std::move (track));
    }
    {
        Track track; // 0.3単独 → trackScratchのclear漏れがあると前トラックの1.2が混入する
        track.id = 2;
        track.name = "single";
        track.params->gain.store (1.0f);
        track.clips.push_back (makeClip (0.3f, blockSize * 8));
        project.tracks.push_back (std::move (track));
    }
    snapshots.push (project.buildSnapshot());

    juce::AudioBuffer<float> buffer (2, blockSize);
    engine.play();
    for (int i = 0; i < 4; ++i)
    {
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
    }
    engine.stop();

    const float peak0 = project.tracks[0].params->peakLevel.exchange (0.0f);
    const float peak1 = project.tracks[1].params->peakLevel.exchange (0.0f);
    expect (peak0 > 0.9f, "重なったクリップは合算後ピークで測ること（0.6+0.6 > 0.9）");
    expect (peak0 > 1.15f && peak0 < 1.25f, "合算後ピークが約1.2であること");
    expect (peak1 > 0.25f && peak1 < 0.35f, "他トラックの音が混入しないこと（clear漏れ検知）");

    // 出力自体も従来どおり両トラック合算で鳴っていること（スクラッチ経由への置き換えで無音化していない）
    buffer.clear();
    engine.play();
    juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
    engine.process (info);
    engine.stop();
    expect (buffer.getMagnitude (0, 0, blockSize) > 1.4f, "出力は全トラック合算（1.2+0.3）で鳴ること");

    snapshots.deleteRetired();
}

// ---- 再生中のノート編集: スナップショット差し替え時に消音→跨ぎノート再発音（鳴りっぱなし防止）----
void testSnapshotSwapDuringPlayback()
{
    beginTest ("snapshot swap during playback");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    Project project;
    Track track;
    track.id = 30;
    track.type = TrackType::midi;
    track.gmProgram = 48; // Strings（持続音）
    MidiRegion region;
    region.id = 31;
    region.startPpq = 0;
    region.lengthPpq = Ppq::ticksPerBar * 16;
    region.notes.push_back ({ 32, 60, 0, Ppq::ticksPerBar * 16, 100 });
    track.midiRegions.push_back (region);
    project.tracks.push_back (std::move (track));

    SynthBank bank;
    bank.sync (project, sr, blockSize);
    auto synth = bank.get (30);
    expect (synth != nullptr, "synth取得");
    const auto countActive = [&synth] (int pitch)
    {
        int count = 0;
        if (synth != nullptr)
            for (int i = 0; i < synth->numActiveNotes; ++i)
                if (synth->activeNotes[i].pitch == pitch)
                    ++count;
        return count;
    };
    auto pushSnapshot = [&]
    {
        auto snapshot = project.buildSnapshot();
        snapshot->tracks[0].synth = bank.get (30);
        snapshots.push (std::move (snapshot));
    };
    pushSnapshot();

    juce::AudioBuffer<float> buffer (2, blockSize);
    auto processBlocks = [&] (int count)
    {
        float magnitude = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
            magnitude = juce::jmax (magnitude, buffer.getMagnitude (0, 0, blockSize));
        }
        return magnitude;
    };

    engine.play();
    expect (processBlocks (20) > 0.001f, "ロングノートが鳴ること");
    expect (countActive (60) == 1, "発音中はactiveNotesに載ること");

    // 再生中にノートを削除（新スナップショットへ差し替え）→ 鳴りっぱなしにならない
    project.tracks[0].midiRegions[0].notes.clear();
    snapshots.deleteRetired();
    pushSnapshot();
    processBlocks (5);
    expect (countActive (60) == 0, "再生中の削除で発音が止まること（鳴りっぱなし防止）");
    processBlocks (200); // リリース減衰
    expect (processBlocks (5) < 0.01f, "削除後は減衰して静かになること");

    // 再生中にノートを戻す → 跨ぎノートとして再発音される
    project.tracks[0].midiRegions[0].notes.push_back ({ 33, 60, 0, Ppq::ticksPerBar * 16, 100 });
    snapshots.deleteRetired();
    pushSnapshot();
    expect (processBlocks (10) > 0.001f, "再生中の追加で跨ぎノートが再発音されること");
    expect (countActive (60) == 1, "再発音後はactiveNotesに載ること");

    engine.stop();
    processBlocks (10);
    snapshots.deleteRetired();
}

// ---- イベント上限超過: 捨てたノートの終端で同ピッチの別ノートを誤って止めない ----
void testOverflowDoesNotKillOtherNotes()
{
    beginTest ("note-on overflow does not kill other notes");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // 1ブロック ≈ 22.3 tick（bpm120/44.1kHz）。全ノートをtick 0に置き、
    // [A: pitch60ロング] → [フィラー1023個: pitch62・10tickの短ノート] → [C: pitch60・50tick]
    // の順で上限1024を消費させ、Cのノートオンだけを捨てさせる（stable_sortで並び順は維持される）
    Project project;
    Track track;
    track.id = 40;
    track.type = TrackType::midi;
    track.gmProgram = 48;
    MidiRegion region;
    region.id = 41;
    region.startPpq = 0;
    region.lengthPpq = Ppq::ticksPerBar * 16;
    juce::uint64 nextNoteId = 100;
    region.notes.push_back ({ nextNoteId++, 60, 0, Ppq::ticksPerBar * 8, 100 }); // A
    for (int i = 0; i < 1023; ++i)
        region.notes.push_back ({ nextNoteId++, 62, 0, 10, 100 });               // フィラー（ブロック内で完結）
    region.notes.push_back ({ nextNoteId++, 60, 0, 50, 100 });                   // C（捨てられる・終端はブロック3）
    track.midiRegions.push_back (std::move (region));
    project.tracks.push_back (std::move (track));

    SynthBank bank;
    bank.sync (project, sr, blockSize);
    auto synth = bank.get (40);
    expect (synth != nullptr, "synth取得");
    {
        auto snapshot = project.buildSnapshot();
        snapshot->tracks[0].synth = bank.get (40);
        snapshots.push (std::move (snapshot));
    }

    juce::AudioBuffer<float> buffer (2, blockSize);
    auto processBlocks = [&] (int count)
    {
        for (int i = 0; i < count; ++i)
        {
            buffer.clear();
            juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
            engine.process (info);
        }
    };

    engine.play();
    processBlocks (10); // Cの終端（50tick ≈ ブロック3）を確実に通過させる

    expect (transport.midiDroppedNoteOns.load() > 0, "上限超過で捨てたノートオンが計上されること");
    int activeAt60 = 0;
    for (int i = 0; i < synth->numActiveNotes; ++i)
        if (synth->activeNotes[i].pitch == 60)
            ++activeAt60;
    expect (activeAt60 == 1, "捨てたノートの終端で、送信済みの同ピッチノートが止められないこと");

    engine.stop();
    processBlocks (5);
    snapshots.deleteRetired();
}

// ---- スパイク: DLSMusicDevice をホストしてノートオン→無音でないことを確認 ----
void testDlsMusicDeviceRendersAudio()
{
    beginTest ("DLSMusicDevice renders audio");

    juce::AudioUnitPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, "AudioUnit:Synths/aumu,dls ,appl");
    expect (found.size() > 0, "DLSMusicDevice が見つかること");
    if (found.isEmpty())
        return;

    juce::String error;
    auto instance = format.createInstanceFromDescription (*found[0], 44100.0, 512, error);
    expect (instance != nullptr, "インスタンス化できること");
    if (instance == nullptr)
    {
        std::cout << "  error: " << error << std::endl;
        return;
    }

    instance->setNonRealtime (true);
    instance->prepareToPlay (44100.0, 512);

    const int numChannels = juce::jmax (2, instance->getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer (numChannels, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);

    float magnitude = 0.0f;
    for (int block = 0; block < 20; ++block)
    {
        buffer.clear();
        instance->processBlock (buffer, midi);
        midi.clear();
        magnitude = juce::jmax (magnitude, buffer.getMagnitude (0, buffer.getNumSamples()));
    }
    expect (magnitude > 0.001f, "ノートオン後に無音でないこと");
    std::cout << "  peak magnitude: " << magnitude << std::endl;

    instance->releaseResources();
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager 初期化（AUインスタンス化に必要）

    testV1ToV2Roundtrip();
    testMidiRoundtrip();
    testInvalidJson();
    testClampNoteBoundaries();
    testUndoStack();
    testSaveGcProtectsUndoWavs();
    testBuildSnapshotFlattensNotes();
    testSynthBank();
    testPlaybackEngineMidi();
    testTrackLevelMeter();
    testSnapshotSwapDuringPlayback();
    testOverflowDoesNotKillOtherNotes();
    testDlsMusicDeviceRendersAudio();

    if (failureCount > 0)
    {
        std::cout << failureCount << " test(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "all tests passed" << std::endl;
    return 0;
}
