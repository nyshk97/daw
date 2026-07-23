// daw_tests — CTest から実行するコンソールテスト。
// GUIなしで動くもの（データモデル・保存/読込・DLSMusicDeviceのオフラインレンダリング）だけを検証する。
// テストは一時ディレクトリのみを使い、~/Music/daw には一切触れない。

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "audio/BounceRenderer.h"
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

// ---- v2プロジェクト（offset/length無し）の読込 → v3保存 → 再読込 ----
void testClipOffsetsV2Migration()
{
    beginTest ("clip offsets v2 migration");
    const auto dir = makeTempDir();

    constexpr int wavLength = 4410;
    expect (writeTestWav (dir.getChildFile ("clip-001.wav"), wavLength), "テストWAVを書けること");

    const char* v2json = R"({
        "version": 2, "bpm": 120.0, "sampleRate": 44100.0, "nextId": 5,
        "tracks": [
            { "id": 1, "type": "audio", "name": "Vocal",
              "clips": [ { "file": "clip-001.wav", "startSample": 100 } ] }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (v2json);

    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr && project->tracks.size() == 1 && project->tracks[0].clips.size() == 1,
            "v2を読込めること");
    if (project == nullptr || project->tracks.empty() || project->tracks[0].clips.empty())
        { dir.deleteRecursively(); return; }

    expect (project->tracks[0].clips[0].offsetSamples == 0, "v2読込時はoffset=0");
    expect (project->tracks[0].clips[0].lengthSamples == wavLength, "v2読込時はlength=WAV全長");

    // 参照範囲を狭めて保存 → v3として再読込で維持される
    project->tracks[0].clips[0].offsetSamples = 1000;
    project->tracks[0].clips[0].lengthSamples = 2000;
    expect (project->save (error), "v3で保存できること");

    const auto saved = juce::JSON::parse (dir.getChildFile ("project.json").loadFileAsString());
    expect ((int) saved.getProperty ("version", 0) == Project::currentVersion,
            "現行バージョンで保存されること");

    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr && reloaded->tracks.size() == 1 && reloaded->tracks[0].clips.size() == 1,
            "v3を再読込できること");
    if (reloaded != nullptr && ! reloaded->tracks.empty() && ! reloaded->tracks[0].clips.empty())
    {
        expect (reloaded->tracks[0].clips[0].offsetSamples == 1000, "offset維持");
        expect (reloaded->tracks[0].clips[0].lengthSamples == 2000, "length維持");
        expect (reloaded->tracks[0].clips[0].startSample == 100, "startSample維持");
    }

    dir.deleteRecursively();
}

// ---- v3の不正なoffset/lengthのクランプ（オーバーフロー誘発値を含む）----
void testClipOffsetClamp()
{
    beginTest ("clip offset/length clamp");
    const auto dir = makeTempDir();

    constexpr int wavLength = 4410;
    expect (writeTestWav (dir.getChildFile ("clip-001.wav"), wavLength), "テストWAVを書けること");

    // clip1: int64最大値（offset+lengthを先に足すとオーバーフローする）→ 範囲ゼロでスキップ
    // clip2: 負のoffset → 0 に。lengthはそのまま収まる
    // clip3: length省略 → offset以降の残り全部
    // clip4: length過大 → 残り範囲へクランプ
    // clip5: length 0 → スキップ
    const char* v3json = R"({
        "version": 3, "bpm": 120.0, "sampleRate": 44100.0, "nextId": 5,
        "tracks": [
            { "id": 1, "type": "audio", "name": "X",
              "clips": [
                { "file": "clip-001.wav", "startSample": 0,
                  "offsetSamples": 9223372036854775807, "lengthSamples": 9223372036854775807 },
                { "file": "clip-001.wav", "startSample": 0, "offsetSamples": -100, "lengthSamples": 200 },
                { "file": "clip-001.wav", "startSample": 0, "offsetSamples": 400 },
                { "file": "clip-001.wav", "startSample": 0, "offsetSamples": 4000, "lengthSamples": 99999 },
                { "file": "clip-001.wav", "startSample": 0, "offsetSamples": 0, "lengthSamples": 0 }
              ] }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (v3json);

    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr && project->tracks.size() == 1, "読込めること（クラッシュしない）");
    if (project == nullptr || project->tracks.empty())
        { dir.deleteRecursively(); return; }

    const auto& clips = project->tracks[0].clips;
    expect (clips.size() == 3, "範囲ゼロの2クリップはスキップされること");
    expect (warnings.size() == 2, "スキップ分の警告が出ること");
    if (clips.size() == 3)
    {
        expect (clips[0].offsetSamples == 0 && clips[0].lengthSamples == 200, "負offsetは0へ");
        expect (clips[1].offsetSamples == 400 && clips[1].lengthSamples == wavLength - 400,
                "length省略はoffset以降の全部");
        expect (clips[2].offsetSamples == 4000 && clips[2].lengthSamples == wavLength - 4000,
                "過大lengthは残り範囲へクランプ");
    }

    dir.deleteRecursively();
}

// ---- 同一WAVを参照するクリップ間のバッファ共有 ----
void testSharedWavBufferOnLoad()
{
    beginTest ("shared wav buffer on load");
    const auto dir = makeTempDir();

    expect (writeTestWav (dir.getChildFile ("clip-001.wav"), 4410), "テストWAVを書けること");

    // 分割後相当: 同じWAVを参照する2クリップ（別トラックにも1つ）
    const char* v3json = R"({
        "version": 3, "bpm": 120.0, "sampleRate": 44100.0, "nextId": 5,
        "tracks": [
            { "id": 1, "type": "audio", "name": "A",
              "clips": [
                { "file": "clip-001.wav", "startSample": 0, "offsetSamples": 0, "lengthSamples": 2000 },
                { "file": "clip-001.wav", "startSample": 2000, "offsetSamples": 2000, "lengthSamples": 2410 }
              ] },
            { "id": 2, "type": "audio", "name": "B",
              "clips": [ { "file": "clip-001.wav", "startSample": 0 } ] }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (v3json);

    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr && project->tracks.size() == 2, "読込めること");
    if (project == nullptr || project->tracks.size() != 2
        || project->tracks[0].clips.size() != 2 || project->tracks[1].clips.size() != 1)
        { dir.deleteRecursively(); return; }

    expect (project->tracks[0].clips[0].audio.get() == project->tracks[0].clips[1].audio.get(),
            "同一トラック内の同一WAV参照はバッファ共有");
    expect (project->tracks[0].clips[0].audio.get() == project->tracks[1].clips[0].audio.get(),
            "トラックを跨いでもバッファ共有");

    // 保存 → 再読込でも共有が保たれる
    expect (project->save (error), "保存できること");
    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr, "再読込できること");
    if (reloaded != nullptr && reloaded->tracks.size() == 2
        && reloaded->tracks[0].clips.size() == 2 && reloaded->tracks[1].clips.size() == 1)
    {
        expect (reloaded->tracks[0].clips[0].audio.get() == reloaded->tracks[0].clips[1].audio.get()
                    && reloaded->tracks[0].clips[0].audio.get() == reloaded->tracks[1].clips[0].audio.get(),
                "再読込後もバッファ共有");
    }

    dir.deleteRecursively();
}

// ---- buildSnapshot が offset/length を ClipPlayback へ伝播すること ----
void testBuildSnapshotClipOffsets()
{
    beginTest ("buildSnapshot clip offsets");

    Project project;
    Track track;
    track.id = 1;

    Clip clip;
    clip.startSample = 500;
    clip.offsetSamples = 128;
    clip.lengthSamples = 256;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, 1024);
    track.clips.push_back (clip);

    // 範囲外のoffset/lengthはオーディオスレッドに渡る前に除外/クランプされる
    Clip broken = clip;
    broken.offsetSamples = 2000;
    track.clips.push_back (broken);
    project.tracks.push_back (std::move (track));

    auto snapshot = project.buildSnapshot();
    expect (snapshot->tracks.size() == 1 && snapshot->tracks[0].clips.size() == 1,
            "範囲ゼロのクリップはスナップショットに載らないこと");
    if (snapshot->tracks.size() == 1 && snapshot->tracks[0].clips.size() == 1)
    {
        const auto& playback = snapshot->tracks[0].clips[0];
        expect (playback.startSample == 500, "startSample伝播");
        expect (playback.offsetSamples == 128, "offsetSamples伝播");
        expect (playback.lengthSamples == 256, "lengthSamples伝播");
    }
}

// ---- PlaybackEngine が offset付きの左右クリップを連続した元音源として読むこと ----
void testEngineReadsClipOffsets()
{
    beginTest ("engine reads clip offsets");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;
    constexpr int totalSamples = blockSize * 4;
    constexpr int splitAt = blockSize + 100; // ブロック境界とズラした分割点

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // ソース: サンプル値 = 位置に比例するランプ波（読み出し位置のズレを1サンプル単位で検出できる）
    auto source = std::make_shared<juce::AudioBuffer<float>> (1, totalSamples);
    for (int i = 0; i < totalSamples; ++i)
        source->setSample (0, i, (float) i / (float) totalSamples);

    Project project;
    Track track;
    track.id = 1;
    track.params->gain.store (1.0f);
    Clip left;
    left.startSample = 0;
    left.offsetSamples = 0;
    left.lengthSamples = splitAt;
    left.audio = source;
    Clip right;
    right.startSample = splitAt;
    right.offsetSamples = splitAt;
    right.lengthSamples = totalSamples - splitAt;
    right.audio = source;
    track.clips.push_back (std::move (left));
    track.clips.push_back (std::move (right));
    project.tracks.push_back (std::move (track));
    snapshots.push (project.buildSnapshot());

    juce::AudioBuffer<float> buffer (2, blockSize);
    engine.play();
    int mismatches = 0;
    for (int block = 0; block < 4; ++block)
    {
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        for (int i = 0; i < blockSize; ++i)
            if (std::abs (buffer.getSample (0, i) - source->getSample (0, block * blockSize + i)) > 1.0e-6f)
                ++mismatches;
    }
    engine.stop();

    expect (mismatches == 0, "分割された左右クリップの出力が元音源と全サンプル一致すること");
    snapshots.deleteRetired();
}

// ---- splitClip: 左右のoffset/length・バッファ共有・境界no-op ----
void testSplitClip()
{
    beginTest ("splitClip");

    Clip clip;
    clip.fileName = "clip-001.wav";
    clip.startSample = 1000;
    clip.offsetSamples = 50;
    clip.lengthSamples = 400;
    clip.muted = true;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, 1000);
    for (int i = 0; i < 1000; ++i)
        clip.audio->setSample (0, i, (float) i);
    clip.buildPeakCache();

    Clip left, right;
    expect (splitClip (clip, 1100, left, right), "内側の分割点で分割できること");
    expect (left.startSample == 1000 && left.offsetSamples == 50 && left.lengthSamples == 100,
            "左: start/offset維持・length=分割点まで");
    expect (right.startSample == 1100 && right.offsetSamples == 150 && right.lengthSamples == 300,
            "右: 分割点から開始・offsetが左のぶん進む");
    expect (left.audio.get() == clip.audio.get() && right.audio.get() == clip.audio.get(),
            "左右ともソースバッファを共有すること");
    expect (left.fileName == clip.fileName && right.fileName == clip.fileName, "fileName共有");
    expect (left.muted && right.muted, "mutedが両方に引き継がれること");

    // peakCacheは参照範囲のみ: 右の先頭ピークはソースのoffset位置以降の値になる
    expect (left.peakCache.size() == 1 && right.peakCache.size() == 1, "peakCacheが再構築されること");
    if (! right.peakCache.empty())
        expect (juce::approximatelyEqual (right.peakCache[0], 449.0f), // max(|150..449|)
                "右のpeakCacheが自分の参照範囲から作られること");

    Clip unused1, unused2;
    expect (! splitClip (clip, 1000, unused1, unused2), "開始境界ちょうどはno-op");
    expect (! splitClip (clip, 1400, unused1, unused2), "終端境界ちょうどはno-op");
    expect (! splitClip (clip, 999, unused1, unused2), "範囲外はno-op");
}

// ---- splitMidiRegion: またぎノートKeep・右への相対シフト移動・境界no-op ----
void testSplitMidiRegion()
{
    beginTest ("splitMidiRegion");

    MidiRegion region;
    region.id = 10;
    region.startPpq = 3840; // 2小節目
    region.lengthPpq = 3840;
    region.muted = true;
    region.notes.push_back ({ 1, 60, 0, 100, 100 });      // 左に残る
    region.notes.push_back ({ 2, 62, 1900, 400, 90 });    // 分割点(相対1920)をまたぐ → Keep
    region.notes.push_back ({ 3, 64, 1920, 10, 80 });     // 分割点ちょうどから → 右へ
    region.notes.push_back ({ 4, 65, 3000, 100, 70 });    // 右へ

    MidiRegion left, right;
    expect (splitMidiRegion (region, 3840 + 1920, left, right), "内側の分割点で分割できること");
    expect (left.id == 10 && left.startPpq == 3840 && left.lengthPpq == 1920, "左: id/start維持");
    expect (right.id == 0, "右のidは未採番（呼び出し側で採番）");
    expect (right.startPpq == 3840 + 1920 && right.lengthPpq == 1920, "右: 分割点から残り");
    expect (left.muted && right.muted, "mutedが両方に引き継がれること");

    expect (left.notes.size() == 2, "左に2ノート");
    if (left.notes.size() == 2)
    {
        expect (left.notes[0].id == 1 && left.notes[0].startPpq == 0, "左ノート1維持");
        expect (left.notes[1].id == 2 && left.notes[1].startPpq == 1900 && left.notes[1].lengthPpq == 400,
                "またぎノートはフル長のまま左に残ること（Keep）");
    }
    expect (right.notes.size() == 2, "右に2ノート");
    if (right.notes.size() == 2)
    {
        expect (right.notes[0].id == 3 && right.notes[0].startPpq == 0 && right.notes[0].lengthPpq == 10,
                "分割点ちょうどのノートは右の先頭へ");
        expect (right.notes[1].id == 4 && right.notes[1].startPpq == 1080 && right.notes[1].velocity == 70,
                "右ノートは相対シフトされること");
    }

    MidiRegion unused1, unused2;
    expect (! splitMidiRegion (region, 3840, unused1, unused2), "開始境界ちょうどはno-op");
    expect (! splitMidiRegion (region, 3840 + 3840, unused1, unused2), "終端境界ちょうどはno-op");
    expect (! splitMidiRegion (region, 0, unused1, unused2), "範囲外はno-op");
}

// ---- セクションマーカー: ヘルパー・保存/読込ラウンドトリップ・不正値除外 ----
void testSectionMarkers()
{
    beginTest ("section markers");

    // set は昇順を保ち、同一位置への追加は種別変更として働く（位置は曲頭からの拍数・4拍=1小節）
    std::vector<SectionMarker> markers;
    SectionMarkers::set (markers, 32, SectionType::verse);   // bar 9
    SectionMarkers::set (markers, 0, SectionType::intro);    // bar 1
    SectionMarkers::set (markers, 64, SectionType::hook);    // bar 17
    expect (markers.size() == 3, "3個追加されること");
    expect (markers[0].startBeats == 0 && markers[1].startBeats == 32 && markers[2].startBeats == 64,
            "startBeats昇順を保つこと");
    expect (markers[1].bar() == 9 && markers[1].beat() == 0, "bar/beat換算が正しいこと");
    SectionMarkers::set (markers, 32, SectionType::bridge);
    expect (markers.size() == 3 && markers[1].type == SectionType::bridge,
            "同一位置へのsetは種別変更になること");

    // 自動採番: 1個だけなら番号なし、2個以上で出現順
    SectionMarkers::set (markers, 32, SectionType::verse); // 戻す
    SectionMarkers::set (markers, 96, SectionType::verse); // bar 25
    expect (SectionMarkers::displayName (markers, 0) == "intro", "1個だけの種別は番号なし");
    expect (SectionMarkers::displayName (markers, 1) == "verse1", "2個以上は出現順に採番(1)");
    expect (SectionMarkers::displayName (markers, 3) == "verse2", "2個以上は出現順に採番(2)");
    SectionMarkers::removeAt (markers, 1); // verse1を削除 → 残りが繰り上がる
    expect (SectionMarkers::displayName (markers, 2) == "verse", "1個に戻ったら番号が消えること");

    // clampStartBeats: 隣のマーカーの手前・>=0にクランプ（適用はしない）
    // markers = [0:intro, 64:hook, 96:verse]
    expect (SectionMarkers::clampStartBeats (markers, 1, 120) == 95, "次のマーカーの手前まで");
    expect (SectionMarkers::clampStartBeats (markers, 1, 0) == 1, "前のマーカーの直後まで");
    expect (SectionMarkers::clampStartBeats (markers, 0, -5) == 0, "先頭は曲頭まで");
    expect (SectionMarkers::clampStartBeats (markers, 2, 9999) == 9999, "最後のマーカーは上限なし");

    // 全6種＋拍オフセット付きの保存→読込ラウンドトリップ
    const auto dir = makeTempDir();
    juce::String error;
    auto project = Project::createNew (dir.getChildFile ("proj"), error);
    expect (project != nullptr, "createNewできること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    int beats = 0;
    for (auto type : SectionMarkers::allTypes)
        SectionMarkers::set (project->markers, beats += 32, type);
    SectionMarkers::set (project->markers, 34, SectionType::other); // bar 9 beat 2（拍オフセット）
    expect (project->save (error), "マーカー付きで保存できること");

    juce::StringArray warnings;
    auto reloaded = Project::load (project->directory, warnings, error);
    expect (reloaded != nullptr && warnings.isEmpty(), "警告なく再読込できること");
    if (reloaded != nullptr)
    {
        expect (reloaded->markers.size() == 7, "全6種＋拍オフセットが読み戻せること");
        for (size_t i = 0; i < reloaded->markers.size(); ++i)
        {
            expect (reloaded->markers[i].startBeats == project->markers[i].startBeats,
                    "startBeats（bar+beat）が維持されること");
            expect (reloaded->markers[i].type == project->markers[i].type,
                    "typeが維持されること");
        }
    }
    dir.deleteRecursively();
}

void testSectionMarkersInvalidLoad()
{
    beginTest ("section markers invalid load");
    const auto dir = makeTempDir();

    // bar 0・beat範囲外・未知type・重複位置 は警告付きで捨てる。
    // beat省略=0（旧形式）、同barでもbeat違いは別位置、bar 999（上限なし）は残す
    const char* json = R"({
        "version": 3, "bpm": 120.0, "sampleRate": 44100.0, "tracks": [],
        "markers": [
            { "bar": 0, "type": "intro" },
            { "bar": 5, "type": "chorus" },
            { "bar": 9, "type": "verse" },
            { "bar": 9, "type": "hook" },
            { "bar": 9, "beat": 2, "type": "bridge" },
            { "bar": 2, "beat": 4, "type": "intro" },
            { "bar": 2, "beat": -1, "type": "intro" },
            { "bar": 999, "type": "outro" }
        ]
    })";
    dir.getChildFile ("project.json").replaceWithText (json);

    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr, "読込自体は成功すること");
    if (project == nullptr)
        { dir.deleteRecursively(); return; }

    expect (warnings.size() == 5, "不正マーカー5件の警告が出ること");
    expect (project->markers.size() == 3, "有効なマーカーだけ残ること");
    if (project->markers.size() == 3)
    {
        expect (project->markers[0].startBeats == 32 && project->markers[0].type == SectionType::verse,
                "重複位置は先勝ちであること（beat省略=0）");
        expect (project->markers[1].startBeats == 34 && project->markers[1].type == SectionType::bridge,
                "同barでもbeat違いは別位置として残ること");
        expect (project->markers[2].startBeats == 3992 && project->markers[2].type == SectionType::outro,
                "barの上限がないこと");
    }
    dir.deleteRecursively();
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

    // セクションマーカーの追加/種別変更/移動/削除のundo/redo
    undo.begin (project);
    SectionMarkers::set (project.markers, 0, SectionType::intro);
    expect (undo.undo (project) && project.markers.empty(), "マーカー追加をundoできること");
    expect (undo.redo (project) && project.markers.size() == 1, "マーカー追加をredoできること");

    undo.begin (project);
    SectionMarkers::set (project.markers, 0, SectionType::verse); // 同一位置 = 種別変更
    undo.undo (project);
    expect (project.markers.size() == 1 && project.markers[0].type == SectionType::intro,
            "種別変更をundoできること");

    undo.begin (project);
    project.markers[0].startBeats = 18; // 移動（クランプ済みの値を直接書くUI側の操作と同じ）
    undo.undo (project);
    expect (project.markers[0].startBeats == 0, "移動をundoできること");

    undo.begin (project);
    SectionMarkers::removeAt (project.markers, 0);
    expect (undo.undo (project) && project.markers.size() == 1, "削除をundoできること");

    // マーカーを含むスナップショット化後もトラック編集のundoが維持されること（markersは巻き戻らない）
    undo.begin (project);
    Track third;
    third.id = 4;
    project.tracks.push_back (std::move (third));
    undo.undo (project);
    expect (project.tracks.size() == 2, "トラック編集undoが維持されること");
    expect (project.markers.size() == 1 && project.markers[0].type == SectionType::intro,
            "トラック編集undoでマーカーが壊れないこと");
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
    clip.lengthSamples = clip.audio != nullptr ? clip.audio->getNumSamples() : 0;
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
        clip.lengthSamples = numSamples;
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

    const float peak0 = juce::jmax (project.tracks[0].params->peakL.exchange (0.0f),
                                    project.tracks[0].params->peakR.exchange (0.0f));
    const float peak1 = juce::jmax (project.tracks[1].params->peakL.exchange (0.0f),
                                    project.tracks[1].params->peakR.exchange (0.0f));
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
// ---- バウンス: 完了までポーリング（ワーカースレッドの終了待ち）----
bool waitForBounce (BounceRenderer& renderer, int timeoutMs = 30000)
{
    const auto start = juce::Time::getMillisecondCounter();
    while (renderer.status() == BounceRenderer::Status::running)
    {
        if (juce::Time::getMillisecondCounter() - start > (juce::uint32) timeoutMs)
            return false;
        juce::Thread::sleep (10);
    }
    return true;
}

// ---- バウンス基本: クリップ×gainのミックス・24bit/2ch/レート・原子的置換・一時ファイル掃除 ----
void testBounceRendererBasic()
{
    beginTest ("BounceRenderer basic render");
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce.wav");
    target.replaceWithText ("stale junk"); // 既存ファイルが置換されることの確認用

    // クリップ: サンプル値0.5×1000サンプルを位置500へ、gain 0.5 → 出力は0.25
    auto audio = std::make_shared<juce::AudioBuffer<float>> (1, 1000);
    for (int i = 0; i < 1000; ++i)
        audio->setSample (0, i, 0.5f);

    BounceRenderer::Request request;
    request.sampleRate = 44100.0;
    request.bpm = 120.0;
    request.endSample = 2000;
    request.targetFile = target;
    BounceRenderer::TrackRender track;
    track.gain = 0.5f;
    track.clips.push_back ({ audio, 500, 0, 1000 });
    request.tracks.push_back (std::move (track));

    BounceRenderer renderer;
    expect (renderer.start (std::move (request)), "startできること");
    expect (waitForBounce (renderer), "タイムアウトせず完了すること");
    const auto result = renderer.takeResult();
    expect (result.status == BounceRenderer::Status::success, "successで終わること");
    expect (! result.scaled, "ピーク1.0以下ならスケールしないこと");
    expect (result.writtenSamples == 2000, "テールなし=endSampleちょうどの長さ");

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::FileInputStream (target), true));
    expect (reader != nullptr, "書き出したWAVを読めること（junkが置換されている）");
    if (reader != nullptr)
    {
        expect ((int) reader->numChannels == 2, "ステレオであること");
        expect ((int) reader->bitsPerSample == 24, "24bitであること");
        expect (juce::approximatelyEqual (reader->sampleRate, 44100.0), "サンプルレートが一致すること");
        expect (reader->lengthInSamples == 2000, "長さがendSampleと一致すること");

        juce::AudioBuffer<float> readBack (2, 2000);
        reader->read (&readBack, 0, 2000, 0, true, true);
        expect (std::abs (readBack.getSample (0, 800) - 0.25f) < 0.001f, "クリップ区間はgain適用値（L）");
        expect (std::abs (readBack.getSample (1, 800) - 0.25f) < 0.001f, "クリップ区間はgain適用値（R）");
        expect (std::abs (readBack.getSample (0, 100)) < 0.0001f, "クリップ前は無音");
        expect (std::abs (readBack.getSample (0, 1800)) < 0.0001f, "クリップ後は無音");
    }

    expect (dir.getNumberOfChildFiles (juce::File::findFiles, "*.tmp") == 0
                && dir.getNumberOfChildFiles (juce::File::findFiles, ".*") == 0,
            "一時ファイルが残らないこと");
    dir.deleteRecursively();
}

// ---- バウンス: ピーク>1.0のときだけ全体スケールダウン（オーバーロード保護）----
void testBounceRendererClippingProtection()
{
    beginTest ("BounceRenderer clipping protection");
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce.wav");

    // 0.8のクリップを同位置に2枚重ねて加算1.6 → 0.999/1.6にスケールされる
    auto audio = std::make_shared<juce::AudioBuffer<float>> (1, 500);
    for (int i = 0; i < 500; ++i)
        audio->setSample (0, i, 0.8f);

    BounceRenderer::Request request;
    request.sampleRate = 44100.0;
    request.endSample = 500;
    request.targetFile = target;
    BounceRenderer::TrackRender track;
    track.gain = 1.0f;
    track.clips.push_back ({ audio, 0, 0, 500 });
    track.clips.push_back ({ audio, 0, 0, 500 });
    request.tracks.push_back (std::move (track));

    BounceRenderer renderer;
    expect (renderer.start (std::move (request)), "startできること");
    expect (waitForBounce (renderer), "タイムアウトせず完了すること");
    const auto result = renderer.takeResult();
    expect (result.status == BounceRenderer::Status::success, "successで終わること");
    expect (result.scaled, "ピーク>1.0でスケールされること");
    expect (std::abs (result.peak - 1.6f) < 0.001f, "スケール前ピークが記録されること");

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::FileInputStream (target), true));
    expect (reader != nullptr, "書き出したWAVを読めること");
    if (reader != nullptr)
    {
        juce::AudioBuffer<float> readBack (2, 500);
        reader->read (&readBack, 0, 500, 0, true, true);
        const float peak = readBack.getMagnitude (0, 500);
        expect (peak <= 1.0f, "出力ピークが1.0以下に収まること");
        expect (std::abs (peak - 0.999f) < 0.005f, "0.999へ正規化されること");
    }
    dir.deleteRecursively();
}

// ---- バウンス: MIDIトラック（DLS専用インスタンス）のレンダリングと余韻テール ----
void testBounceRendererMidiTail()
{
    beginTest ("BounceRenderer midi + tail");
    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("bounce.wav");

    SynthBank bank;
    auto synth = bank.createIndependent (0, false, 44100.0, BounceRenderer::renderBlockSize);
    expect (synth != nullptr, "バウンス専用DLSインスタンスを作れること");
    if (synth == nullptr)
    {
        dir.deleteRecursively();
        return;
    }

    // 1拍のノート（120BPMで0.5秒=22050サンプル）。endSampleはノート終端ちょうど
    BounceRenderer::Request request;
    request.sampleRate = 44100.0;
    request.bpm = 120.0;
    request.endSample = 22050;
    request.wantTail = true;
    request.targetFile = target;
    BounceRenderer::TrackRender track;
    track.gain = 0.8f;
    track.synth = synth;
    track.notes.push_back ({ 0, Ppq::ticksPerQuarter, 60, 100 });
    request.tracks.push_back (std::move (track));

    BounceRenderer renderer;
    expect (renderer.start (std::move (request)), "startできること");
    expect (waitForBounce (renderer), "タイムアウトせず完了すること");
    const auto result = renderer.takeResult();
    expect (result.status == BounceRenderer::Status::success, "successで終わること");
    expect (result.peak > 0.001f, "無音でないこと");
    expect (result.writtenSamples >= 22050, "テールで曲末より長くなること（最低でも切り捨てない）");
    expect (result.writtenSamples <= 22050 + (juce::int64) (44100 * 5.0) + BounceRenderer::renderBlockSize,
            "テール上限（5秒）を超えないこと");

    dir.deleteRecursively();
}

// ---- v4: pan/sends/バス/Masterの保存・読込と、v3以前のデフォルト補完 ----
void testMixerParamsRoundtrip()
{
    beginTest ("mixer params roundtrip");

    // 新規Projectのデフォルト: バス・Masterはユニティ（TrackParamsの既定0.8を引き継がない）
    Project fresh;
    for (int b = 0; b < numSendBuses; ++b)
        expect (juce::approximatelyEqual (fresh.busParams[b]->gain.load(), 1.0f),
                "新規Projectのバスgainは1.0");
    expect (juce::approximatelyEqual (fresh.masterParams->gain.load(), 1.0f),
            "新規ProjectのMaster gainは1.0");

    // v3形式（pan/sends/buses/masterなし）の読込 → デフォルト補完
    auto dir = makeTempDir();
    dir.getChildFile ("project.json").replaceWithText (R"({
        "version": 3, "bpm": 120.0, "sampleRate": 0.0, "nextId": 2,
        "tracks": [ { "id": 1, "type": "audio", "name": "t",
                      "mute": false, "solo": false, "volume": 0.5, "clips": [] } ]
    })");
    juce::StringArray warnings;
    juce::String error;
    auto project = Project::load (dir, warnings, error);
    expect (project != nullptr && project->tracks.size() == 1, "v3を読込めること");
    if (project == nullptr || project->tracks.empty())
    {
        dir.deleteRecursively();
        return;
    }
    auto& params = *project->tracks[0].params;
    expect (juce::approximatelyEqual (params.pan.load(), 0.0f), "v3読込: pan=0");
    expect (params.eqEnabled.load() && params.compEnabled.load(), "v3読込: FXはON補完");
    for (int b = 0; b < numSendBuses; ++b)
        expect (juce::approximatelyEqual (params.sends[b].load(), 0.0f), "v3読込: send=0");
    for (int b = 0; b < numSendBuses; ++b)
        expect (juce::approximatelyEqual (project->busParams[b]->gain.load(), 1.0f)
                    && ! project->busParams[b]->mute.load(),
                "v3読込: バスgain=1.0・mute=false");
    expect (juce::approximatelyEqual (project->masterParams->gain.load(), 1.0f),
            "v3読込: Master gain=1.0");

    // 値を入れて保存 → v4になり、再読込で維持される
    params.pan.store (-0.5f);
    params.sends[0].store (0.3f);
    params.sends[2].store (1.0f);
    params.eqEnabled.store (false);
    project->busParams[1]->gain.store (0.7f);
    project->busParams[1]->mute.store (true);
    project->masterParams->gain.store (0.9f);
    expect (project->save (error), "v4で保存できること");

    const auto parsed = juce::JSON::parse (dir.getChildFile ("project.json").loadFileAsString());
    expect ((int) parsed.getProperty ("version", 0) == 4, "version=4で保存されること");

    auto reloaded = Project::load (dir, warnings, error);
    expect (reloaded != nullptr && reloaded->tracks.size() == 1, "v4を再読込できること");
    if (reloaded != nullptr && ! reloaded->tracks.empty())
    {
        auto& p = *reloaded->tracks[0].params;
        expect (juce::approximatelyEqual (p.pan.load(), -0.5f), "pan維持");
        expect (juce::approximatelyEqual (p.sends[0].load(), 0.3f)
                    && juce::approximatelyEqual (p.sends[1].load(), 0.0f)
                    && juce::approximatelyEqual (p.sends[2].load(), 1.0f),
                "sends維持");
        expect (! p.eqEnabled.load() && p.compEnabled.load(), "FXのON/OFF維持（eq=OFF/comp=ON）");
        expect (juce::approximatelyEqual (reloaded->busParams[1]->gain.load(), 0.7f)
                    && reloaded->busParams[1]->mute.load(),
                "バスgain/mute維持");
        expect (juce::approximatelyEqual (reloaded->masterParams->gain.load(), 0.9f), "Master gain維持");

        // buildSnapshotにバス・Masterが載ること
        auto snapshot = reloaded->buildSnapshot();
        expect (snapshot->busParams[0] == reloaded->busParams[0]
                    && snapshot->masterParams == reloaded->masterParams,
                "スナップショットがバス/Masterのparamsを共有すること");
    }
    dir.deleteRecursively();
}

// ---- エンジン: pan法則・post-fader send（素通しバス）・busGain/mute・Masterゲイン・メーター ----
void testEnginePanSendsMaster()
{
    beginTest ("engine pan/sends/master");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    // 定数振幅0.5のクリップ（レベル検証がしやすい）
    Project project;
    Track track;
    track.id = 1;
    track.params->gain.store (1.0f);
    Clip clip;
    clip.startSample = 0;
    clip.lengthSamples = blockSize * 64;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, blockSize * 64);
    for (int i = 0; i < clip.audio->getNumSamples(); ++i)
        clip.audio->setSample (0, i, 0.5f);
    track.clips.push_back (std::move (clip));
    project.tracks.push_back (std::move (track));
    auto& params = *project.tracks[0].params;
    snapshots.push (project.buildSnapshot());

    juce::AudioBuffer<float> buffer (2, blockSize);
    auto measure = [&] (float& left, float& right)
    {
        transport.seekRequest.store (0);
        engine.play();
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        left = buffer.getMagnitude (0, 0, blockSize);
        right = buffer.getMagnitude (1, 0, blockSize);
        engine.stop();
        buffer.clear();
        engine.process (info); // 停止エッジの消化
    };

    float left = 0.0f, right = 0.0f;

    // panセンター: 両ch 0.5（等パワー補正型はセンター0dB = 既存プロジェクトの音量を変えない）
    measure (left, right);
    expect (std::abs (left - 0.5f) < 0.001f && std::abs (right - 0.5f) < 0.001f,
            "panセンターは両ch等量（0.5）");

    // pan右振り切り: 左ほぼ0・右は+3dB（0.5×√2≈0.707）
    params.peakL.exchange (0.0f); // センター測定の蓄積ピーク（CAS max）をリセット
    params.peakR.exchange (0.0f);
    params.pan.store (1.0f);
    measure (left, right);
    expect (left < 0.001f, "pan右振り切りで左chは無音");
    expect (std::abs (right - 0.7071f) < 0.005f, "pan右振り切りで右chは+3dB（約0.707）");
    expect (params.peakR.exchange (0.0f) > 0.7f, "Rメーターはpost-panピーク（約0.707）");
    expect (params.peakL.exchange (0.0f) < 0.001f, "pan右振り切りでLメーターは振れないこと");

    // send（素通しバス）: pan中央・send100% → 原音と二重加算で1.0
    params.pan.store (0.0f);
    params.sends[0].store (1.0f);
    measure (left, right);
    expect (std::abs (left - 1.0f) < 0.002f, "send100%は素通しバスで二重加算（1.0）");
    expect (project.busParams[0]->peakL.exchange (0.0f) > 0.45f, "バスメーターが振れること");

    // バスミュートでsend分が消える
    project.busParams[0]->mute.store (true);
    measure (left, right);
    expect (std::abs (left - 0.5f) < 0.002f, "バスMでsend分が消えること");
    project.busParams[0]->mute.store (false);

    // バスのリターン量（gain 0.5 → 0.5 + 0.25 = 0.75）
    project.busParams[0]->gain.store (0.5f);
    measure (left, right);
    expect (std::abs (left - 0.75f) < 0.002f, "バスgainがリターン量として効くこと");
    project.busParams[0]->gain.store (1.0f);

    // Masterゲイン（全体 1.0 → 0.5）とMasterメーター
    project.masterParams->gain.store (0.5f);
    project.masterParams->peakL.exchange (0.0f); // 前シナリオの蓄積ピーク（CAS max）をリセット
    project.masterParams->peakR.exchange (0.0f);
    measure (left, right);
    expect (std::abs (left - 0.5f) < 0.002f, "Masterゲインで全体が半減すること");
    expect (std::abs (project.masterParams->peakL.exchange (0.0f) - 0.5f) < 0.01f,
            "Masterメーターはpost-masterピーク");

    snapshots.deleteRetired();
}

// ---- エンジン: 最終出力ルール（ch0/1のみ・1chはダウンミックス・余剰chは無音。クリック含む）----
void testEngineOutputChannelRule()
{
    beginTest ("engine output channel rule");

    constexpr double sr = 44100.0;
    constexpr int blockSize = 512;

    TransportState transport;
    SnapshotExchange snapshots;
    PreviewFifo previewFifo;
    PlaybackEngine engine (transport, snapshots, previewFifo);
    engine.prepareToPlay (blockSize, sr);

    Project project;
    Track track;
    track.id = 1;
    track.params->gain.store (1.0f);
    track.params->sends[0].store (1.0f); // バス経路も通す（余剰ch漏れの検査対象に含める）
    Clip clip;
    clip.startSample = 0;
    clip.lengthSamples = blockSize * 64;
    clip.audio = std::make_shared<juce::AudioBuffer<float>> (1, blockSize * 64);
    for (int i = 0; i < clip.audio->getNumSamples(); ++i)
        clip.audio->setSample (0, i, 0.5f);
    track.clips.push_back (std::move (clip));
    project.tracks.push_back (std::move (track));
    snapshots.push (project.buildSnapshot());

    // 4ch出力: 通常音（クリップ＋バス）とクリック（曲頭=拍頭で必ず鳴る）がch0/1のみに出ること
    transport.clickEnabled.store (true);
    {
        juce::AudioBuffer<float> buffer (4, blockSize);
        transport.seekRequest.store (0);
        engine.play();
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        engine.stop();
        expect (buffer.getMagnitude (0, 0, blockSize) > 0.9f
                    && buffer.getMagnitude (1, 0, blockSize) > 0.9f,
                "4ch出力: ch0/1に音が出ること（クリップ＋バス＋クリック）");
        expect (buffer.getMagnitude (2, 0, blockSize) == 0.0f
                    && buffer.getMagnitude (3, 0, blockSize) == 0.0f,
                "4ch出力: ch2以降は完全に無音（クリックも漏れない）");
        buffer.clear();
        engine.process (info); // 停止エッジの消化
    }

    // 1ch出力: クラッシュせずL+R等分ダウンミックスになること（クリックは切って振幅を検証）
    transport.clickEnabled.store (false);
    project.tracks[0].params->sends[0].store (0.0f);
    {
        juce::AudioBuffer<float> buffer (1, blockSize);
        transport.seekRequest.store (0);
        engine.play();
        buffer.clear();
        juce::AudioSourceChannelInfo info (&buffer, 0, blockSize);
        engine.process (info);
        engine.stop();
        expect (std::abs (buffer.getMagnitude (0, 0, blockSize) - 0.5f) < 0.002f,
                "1ch出力: L+R等分ダウンミックス（panセンターの0.5が保たれる）");
        buffer.clear();
        engine.process (info);
    }

    snapshots.deleteRetired();
}

// ---- エンジン: 停止中のMIDIプレビュー発音もMaster（pan/sendバス経路）を通ること ----
void testPreviewThroughMaster()
{
    beginTest ("preview routes through master");

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
    project.tracks.push_back (std::move (track));

    SynthBank bank;
    bank.sync (project, sr, blockSize);
    auto snapshot = project.buildSnapshot();
    snapshot->tracks[0].synth = bank.get (30);
    snapshots.push (std::move (snapshot));

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

    // Master 0 でプレビュー → 無音（Masterを迂回していない証拠）。イベント自体は処理されている
    project.masterParams->gain.store (0.0f);
    previewFifo.push ({ PreviewFifo::Command::Type::noteOn, 30, 72, 100 });
    expect (processBlocks (5) < 0.0001f, "Master 0ならプレビューも無音");
    auto synth = bank.get (30);
    bool active = false;
    if (synth != nullptr)
        for (int i = 0; i < synth->numActiveNotes; ++i)
            active = active || synth->activeNotes[i].pitch == 72;
    expect (active, "無音でもプレビューのイベントは処理されていること");

    // Master 1 に戻すと（同じ発音中ノートが）聞こえる
    project.masterParams->gain.store (1.0f);
    expect (processBlocks (5) > 0.001f, "Masterを戻すとプレビューが聞こえること");

    processBlocks (60); // 発音長を消化してから片付け
    snapshots.deleteRetired();
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager 初期化（AUインスタンス化に必要）

    testV1ToV2Roundtrip();
    testMidiRoundtrip();
    testInvalidJson();
    testClampNoteBoundaries();
    testClipOffsetsV2Migration();
    testClipOffsetClamp();
    testSharedWavBufferOnLoad();
    testBuildSnapshotClipOffsets();
    testEngineReadsClipOffsets();
    testSplitClip();
    testSplitMidiRegion();
    testSectionMarkers();
    testSectionMarkersInvalidLoad();
    testUndoStack();
    testSaveGcProtectsUndoWavs();
    testBuildSnapshotFlattensNotes();
    testSynthBank();
    testPlaybackEngineMidi();
    testTrackLevelMeter();
    testSnapshotSwapDuringPlayback();
    testOverflowDoesNotKillOtherNotes();
    testDlsMusicDeviceRendersAudio();
    testBounceRendererBasic();
    testBounceRendererClippingProtection();
    testBounceRendererMidiTail();
    testMixerParamsRoundtrip();
    testEnginePanSendsMaster();
    testEngineOutputChannelRule();
    testPreviewThroughMaster();

    if (failureCount > 0)
    {
        std::cout << failureCount << " test(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "all tests passed" << std::endl;
    return 0;
}
