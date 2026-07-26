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
    juce::String name;          // 表示名。取り込みクリップのみ元ファイル名が入る（録音クリップは空=無ラベル）
    juce::int64 startSample = 0;
    juce::int64 offsetSamples = 0;  // ソースWAV内の読み出し開始位置
    juce::int64 lengthSamples = 0;  // 再生長（サンプル）
    bool muted = false;         // リージョン単位のミュート（再生スナップショットから除外）
    std::shared_ptr<juce::AudioBuffer<float>> audio; // 1ch=モノ（録音）/ 2ch=ステレオ（取り込み）。メモリ常駐
    std::vector<float> peakCache;                    // samplesPerPeak ごとの絶対値ピーク（参照範囲のみ。ステレオはL/Rのmax合成）

    void buildPeakCache();
};

// バッファ全長の描画用ピークキャッシュ（Clip::samplesPerPeak 単位・ステレオはL/Rのmax合成）。
// サンプル音源の波形表示用（クリップは参照範囲だけを見る Clip::buildPeakCache を使う）
std::vector<float> buildFullPeakCache (const juce::AudioBuffer<float>& audio);

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

// send用固定バスの表示名（固定文言は英語）。並びは TrackParams::sends / project.json の "buses" と対応
namespace SendBuses
{
    inline constexpr const char* names[numSendBuses] = { "Reverb A", "Reverb B", "Delay" };
    inline constexpr const char* shortNames[numSendBuses] = { "A", "B", "D" }; // sendノブの豆ラベル用
}

// セクションマーカー（ルーラー下のラベル帯）。区間方式: 終端は持たず、
// 次のマーカーの開始（最後は曲末）までが自分の区間。最初のマーカーより前は無ラベル。
// Project::markers は常に startBar 昇順・同一barなしを保つ（下のヘルパー経由で編集すること）
enum class SectionType { intro, verse, hook, bridge, outro, other };

struct SectionMarker
{
    // 曲頭からの拍数（0始まり・4/4固定で4拍=1小節）。上限なし。
    // 拍より細かくはしない（セクションは曲構造のラベルであり、2/4小節が挟まる曲の
    // 「半小節ずれ」に追従できれば十分。1/16等はただの誤操作リスクになる）
    int startBeats = 0;
    SectionType type = SectionType::other;

    int bar() const { return startBeats / 4 + 1; }  // 1始まりの小節番号（JSON・ログ表記用）
    int beat() const { return startBeats % 4; }     // 小節内の拍 0..3
};

namespace SectionMarkers
{
    inline constexpr SectionType allTypes[] = { SectionType::intro,  SectionType::verse,
                                                SectionType::hook,   SectionType::bridge,
                                                SectionType::outro,  SectionType::other };

    juce::String typeName (SectionType type);                       // "intro" 等（JSONと表示名の共通ベース）
    bool typeFromName (const juce::String& name, SectionType& out); // 未知名は false

    // 挿入（同一位置へは種別変更として働く）。昇順を保つ
    void set (std::vector<SectionMarker>& markers, int startBeats, SectionType type);
    void removeAt (std::vector<SectionMarker>& markers, int index);

    // index のマーカーを newStartBeats へ動かすときの移動先（隣のマーカーの手前・>=0 にクランプ。適用はしない）
    int clampStartBeats (const std::vector<SectionMarker>& markers, int index, int newStartBeats);

    // 自動採番済み表示名。同種別が2個以上あるときだけ出現順に verse1, verse2... と番号を付ける
    juce::String displayName (const std::vector<SectionMarker>& markers, int index);
}

// MIDIトラックの音源種別。gm = macOS内蔵GM音源（DLSMusicDevice）/ sample = 外部ワンショット
enum class InstrumentKind { gm, sample };

struct Track
{
    juce::uint64 id = 0; // プロジェクト内で一意。永続化される。0 = 未採番（読込時に採番）
    TrackType type = TrackType::audio;
    juce::String name;
    std::shared_ptr<TrackParams> params = std::make_shared<TrackParams>();

    // type == audio のとき
    std::vector<Clip> clips;

    // type == midi のとき
    InstrumentKind instrument = InstrumentKind::gm;
    // GM用（instrument == sample の間も保持し続ける。楽器プルダウンでGMへ戻したとき元の楽器が復元される）
    int gmProgram = 0;   // GMプログラム番号 0..127
    bool drums = false;  // true = ch10で発音（gmProgramは無視）
    int drumPitch = -1;  // >=0: 固定ピッチ打楽器（Kick等）。再生時ノートのピッチをこの値に置き換える（GM専用の規則）
    std::vector<MidiRegion> midiRegions;

    // サンプル音源用（instrument == sample のとき有効。GMへ戻しても保持する）。
    // undo対象にするため TrackParams（atomic共有）でなく Track のフィールドとして持ち、
    // オーディオスレッドへは SynthBank::sync() が SamplerEngine の atomic へミラーする（真実の源はここ）
    juce::String sampleFile;            // プロジェクトフォルダ相対（instr-NNN.wav）
    juce::String sampleName;            // 表示名（元ファイル名・拡張子なし）
    bool samplePitchFollow = false;     // false=固定（One Shot）/ true=音程追従（ノート長ゲート）
    // true = 新しい打点で前の音を切る（Logicの Quick Sampler「Polyphony: 1」相当・実機TR-808と同じ）。
    // 長い音（808等）を連打すると重なって濁る・出力がクリップするため、その回避用。既定OFF＝重ねる
    bool sampleMono = false;
    int sampleRootNote = 60;            // 追従時の基準ノート（既定 C3=60）
    float sampleGain = 1.0f;            // サンプル音量 0..2
    juce::int64 sampleStartOffset = 0;  // 頭の無音カット位置（バッファ内サンプル）
    double sampleSourceRate = 0.0;      // ファイル自体のSR（再生比率 sourceRate/deviceRate の計算に必要）
    std::shared_ptr<juce::AudioBuffer<float>> sampleAudio; // メモリ常駐（Clip::audio と同じ寿命規則）
    std::vector<float> samplePeakCache;                    // 波形描画用（Clip::samplesPerPeak 単位・全長）

    bool hasSample() const { return sampleAudio != nullptr && sampleSourceRate > 0.0; }
    bool usesSampler() const { return type == TrackType::midi && instrument == InstrumentKind::sample; }
};

class Project
{
public:
    // v2: MIDIトラック・ID追加 / v3: クリップのoffsetSamples・lengthSamples /
    // v4: pan・sends・固定バス3本・Master / v5: サイクル（ループ範囲）/
    // v6: ステレオクリップ（ch数はJSONに持たずWAV自体から判定）・クリップ表示名（取り込み用）/
    // v7: プロジェクトメモ / v8: サンプル音源（instrument・sample*）
    static constexpr int currentVersion = 8;

    juce::File directory;
    double bpm = 120.0;
    double sampleRate = 0.0; // 0 = 未確定（最初の録音時にデバイスレートで確定）
    juce::String memo;       // プロジェクトごとの自由記述メモ（v7。旧形式は空文字）
    std::vector<Track> tracks;
    std::vector<SectionMarker> markers; // 常にstartBar昇順・同一barなし（SectionMarkersヘルパーで編集する）

    // サイクル（ループ）範囲。16分音符単位・0始まり（タイムラインの最小グリッド1/16と一致。
    // BPM変更後も音楽的位置を維持する）。範囲が有効なのは start < end のときだけ。
    // 音量・ミュートと同じくundo対象外（Logicもサイクル操作はundoしない）
    int cycleStartSixteenths = 0;
    int cycleEndSixteenths = 0;
    bool cycleEnabled = false;
    bool hasCycleRange() const { return cycleStartSixteenths < cycleEndSixteenths; }

    // send用固定バス3本（gain=リターン量・mute使用）とMaster（gain使用）。
    // gainの既定はトラック（0.8）と違いユニティ1.0（unityParams()が保証。
    // v3以前の読込・新規作成でもこの初期値のまま）
    std::shared_ptr<TrackParams> busParams[numSendBuses] { unityParams(), unityParams(), unityParams() };
    std::shared_ptr<TrackParams> masterParams { unityParams() };

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

    juce::File nextClipFile() const;       // clip-NNN.wav の空き連番
    juce::File nextInstrumentFile() const; // instr-NNN.wav の空き連番（サンプル音源）
    std::unique_ptr<PlaybackSnapshot> buildSnapshot() const;

    static juce::File projectsRoot(); // ~/Music/daw

    // 1chはモノ、2ch以上は先頭2chをL/Rとして読む（3ch以上の余剰chは捨てる）。
    // sourceSampleRate != nullptr ならファイル自体のSRも返す（サンプル音源は元SRのまま保存し、
    // 再生比率の計算に使うため必須。クリップ読込はプロジェクトSRへ変換済みなので不要）
    static std::shared_ptr<juce::AudioBuffer<float>> loadWav (const juce::File& file,
                                                             double* sourceSampleRate = nullptr);

private:
    juce::uint64 nextId = 1; // 永続化される採番カウンタ

    static std::shared_ptr<TrackParams> unityParams()
    {
        auto params = std::make_shared<TrackParams>();
        params->gain.store (1.0f); // TrackParamsの既定0.8fを引き継がない（バス/Masterはユニティ）
        return params;
    }

    void ensureUniqueIds(); // 読込後に未採番(0)・重複IDを振り直す
};
