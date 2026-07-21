#pragma once

#include <memory>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>

#include "PlaybackSnapshot.h"
#include "Ppq.h"

// メッセージスレッドが所有するデータモデル。オーディオスレッドへは
// buildSnapshot() で作った PlaybackSnapshot を SnapshotExchange 経由で渡す。

// クリップはソースWAVへの非破壊参照（offsetSamples から lengthSamples 分）。
// 分割・複製で複数クリップが同じ audio（と fileName）を共有する。
// 不変条件: 0 <= offsetSamples / offsetSamples + lengthSamples <= バッファ全長 / lengthSamples >= 1。
// 読込時のクランプと splitClip がこれを保つ
struct Clip
{
    static constexpr int samplesPerPeak = 512; // 描画用ピークキャッシュの集約単位

    juce::String fileName;      // プロジェクトフォルダ相対（例: clip-001.wav）
    juce::int64 startSample = 0;
    juce::int64 offsetSamples = 0;  // ソースWAV内の読み出し開始位置
    juce::int64 lengthSamples = 0;  // 再生長（サンプル）
    bool muted = false;         // リージョン単位のミュート（再生スナップショットから除外）
    std::shared_ptr<juce::AudioBuffer<float>> audio; // モノラル・メモリ常駐
    std::vector<float> peakCache;                    // samplesPerPeak ごとの絶対値ピーク（参照範囲のみ）

    void buildPeakCache();
};

// クリップを splitSample（絶対サンプル位置）で左右に分ける。左右は同じソースWAVを共有参照する。
// 分割点が内側（開始 < 分割点 < 終端）にないときは false（境界ちょうどは分割しない）
bool splitClip (const Clip& clip, juce::int64 splitSample, Clip& left, Clip& right);

// MIDIノート。startPpq はリージョン相対。
// 不変条件: pitch 0..127 / velocity 1..127 / startPpq >= 0（リージョン内）/ lengthPpq >= 1。
// リージョン端を越えて伸びるノートは許容し、再生時にリージョン境界でノートオフ（マスク）する。
struct MidiNote
{
    juce::uint64 id = 0;
    int pitch = 60;
    juce::int64 startPpq = 0;
    juce::int64 lengthPpq = Ppq::ticksPerQuarter;
    int velocity = 100;
};

struct MidiRegion
{
    juce::uint64 id = 0;
    juce::int64 startPpq = 0;                  // 曲頭からの絶対位置（>= 0）
    juce::int64 lengthPpq = Ppq::ticksPerBar;  // >= 1
    bool muted = false;                        // リージョン単位のミュート（再生スナップショットから除外）
    std::vector<MidiNote> notes;

    // 不変条件をモデル層で強制する。ノートの追加・移動・リサイズ後に必ず通すこと
    void clampNote (MidiNote& note) const
    {
        note.pitch = juce::jlimit (0, 127, note.pitch);
        note.velocity = juce::jlimit (1, 127, note.velocity);
        note.startPpq = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, lengthPpq - 1), note.startPpq);
        note.lengthPpq = juce::jmax ((juce::int64) 1, note.lengthPpq);
    }
};

// リージョンを splitPpq（絶対PPQ）で左右に分ける。分割点をまたぐノートは左にフル長のまま残し（Keep）、
// 分割点以降に始まるノートは相対シフトして右へ移す。右の id は 0 のまま返す（呼び出し側で採番する）。
// 分割点が内側にないときは false
bool splitMidiRegion (const MidiRegion& region, juce::int64 splitPpq, MidiRegion& left, MidiRegion& right);

enum class TrackType { audio, midi };

struct Track
{
    juce::uint64 id = 0; // プロジェクト内で一意。永続化される。0 = 未採番（読込時に採番）
    TrackType type = TrackType::audio;
    juce::String name;
    std::shared_ptr<TrackParams> params = std::make_shared<TrackParams>();

    // type == audio のとき
    std::vector<Clip> clips;

    // type == midi のとき
    int gmProgram = 0;   // GMプログラム番号 0..127
    bool drums = false;  // true = ch10で発音（gmProgramは無視）
    int drumPitch = -1;  // >=0: 固定ピッチ打楽器（Kick等）。再生時ノートのピッチをこの値に置き換える
    std::vector<MidiRegion> midiRegions;
};

class Project
{
public:
    static constexpr int currentVersion = 3; // v2: MIDIトラック・ID追加 / v3: クリップのoffsetSamples・lengthSamples

    juce::File directory;
    double bpm = 120.0;
    double sampleRate = 0.0; // 0 = 未確定（最初の録音時にデバイスレートで確定）
    std::vector<Track> tracks;

    juce::String name() const { return directory.getFileName(); }

    // トラック・リージョン・ノートのIDを採番する（メッセージスレッド専用）
    juce::uint64 allocateId() { return nextId++; }

    // project.json 書き出し＋未参照 clip-*.wav のGC。失敗時は error にメッセージ。
    // keepReferencedWavs: undo履歴が参照するファイル名（GCから保護する。Phase 3で使用）
    bool save (juce::String& error, const juce::StringArray& keepReferencedWavs = {});

    static std::unique_ptr<Project> load (const juce::File& dir,
                                          juce::StringArray& warnings,
                                          juce::String& error);
    static std::unique_ptr<Project> createNew (const juce::File& dir, juce::String& error);

    juce::File nextClipFile() const; // clip-NNN.wav の空き連番
    std::unique_ptr<PlaybackSnapshot> buildSnapshot() const;

    static juce::File projectsRoot(); // ~/Music/daw
    static std::shared_ptr<juce::AudioBuffer<float>> loadWavMono (const juce::File& file);

private:
    juce::uint64 nextId = 1; // 永続化される採番カウンタ

    void ensureUniqueIds(); // 読込後に未採番(0)・重複IDを振り直す
};
