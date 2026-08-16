#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>

#include "PlaybackSnapshot.h"
#include "Ppq.h"

// メッセージスレッドが所有するデータモデル。オーディオスレッドへは
// buildSnapshot() で作った PlaybackSnapshot を SnapshotExchange 経由で渡す。

// ループ回数の上限（手編集JSONの極端値で展開量が暴走しないための安全弁。実用上は十分な回数）
inline constexpr int maxLoopCount = 999;

// クリップはソースWAVへの非破壊参照（offsetSamples から lengthSamples 分）。
// 分割・複製で複数クリップが同じ audio（と fileName）を共有する。
// 不変条件: 0 <= offsetSamples / offsetSamples + lengthSamples <= バッファ全長 / lengthSamples >= 1。
// 読込時のクランプと splitClip がこれを保つ
struct Clip
{
    static constexpr int samplesPerPeak = 512; // 描画用ピークキャッシュの集約単位

    juce::String fileName;      // プロジェクトフォルダ相対（例: clip-001.wav）
    juce::String name;          // 表示名。取り込みクリップのみ元ファイル名が入る（録音クリップは空=無ラベル）
    // 採用ループ由来のクリップの出自（ライブラリ相対パス。v14）。**表示専用**で、
    // アンカー（Project::loopAnchor）のライフサイクルには関与しない — 分割・複製・ペーストで
    // 構造体コピーにより継承されるが、それで挙動が変わることはない（設計は loop-track plan）
    juce::String loopSource;
    juce::int64 startSample = 0;
    juce::int64 offsetSamples = 0;  // ソースWAV内の読み出し開始位置
    juce::int64 lengthSamples = 0;  // 再生長（サンプル）
    bool muted = false;         // リージョン単位のミュート（再生スナップショットから除外）
    // 本体の後ろに何回繰り返すか（0 = ループなし）。元を編集すると繰り返し全部に反映される
    // ＝実体は1つ。長さは本体長の整数倍のみ（中途半端な終端を作らない）
    int loopCount = 0;
    // リージョン単位のゲイン（線形倍率。値域は GainScale = ±12dB 相当の約0.251..3.981）。
    // 素材の一部だけを均すトリムで、Track::sampleGain と同じスケール・同じ流儀。
    // 波形の描画振幅にも掛ける（「見た目＝出る音」を保つため）
    float gain = 1.0f;
    // フェードイン/アウト長（サンプル単位・絶対時間。BPM変更の影響を受けない）。
    // 掛かるのは「ループの連なり全体」の先頭と末尾だけで、繰り返しの間はシームレス
    // （各反復に掛けると繰り返すたびに音量が抜けてポンプするため）。カーブはリニア固定。
    // 不変条件: 0 <= fadeInSamples, 0 <= fadeOutSamples,
    //           fadeInSamples + fadeOutSamples <= totalLengthSamples()（clampFades が強制する）
    juce::int64 fadeInSamples = 0;
    juce::int64 fadeOutSamples = 0;
    std::shared_ptr<juce::AudioBuffer<float>> audio; // 1ch=モノ（録音）/ 2ch=ステレオ（取り込み）。メモリ常駐
    std::vector<float> peakCache;                    // samplesPerPeak ごとの絶対値ピーク（参照範囲のみ。ステレオはL/Rのmax合成）

    // ループを含む総再生長（描画・ヒットテスト・終端計算はこちらを見る）
    juce::int64 totalLengthSamples() const { return lengthSamples * (1 + juce::jmax (0, loopCount)); }

    // フェードの不変条件をモデル層で強制する（MidiRegion::clampNote と同じ立ち位置）。
    // 規則は「fadeIn を先に頭打ち → 残りで fadeOut を頭打ち」。読込・splitClip・
    // ループ解除・ループのドラッグから通す。
    //
    // 比率維持にしないのは、フェード長がユーザーが絶対時間（ms）で決めた値であり、
    // 全長が変わるたびに勝手に伸縮すると意図が壊れるため。単純な頭打ちなら決定的で、
    // どの経路からでも同じ関数を通せる。
    //
    // ⚠️ フェード自体のドラッグはここを直接通さないこと。この関数は fadeIn 優先なので
    // fadeIn を伸ばすと fadeOut が押し縮められる（＝「相手を押しのけない」規則に反する）。
    // ドラッグ側で先に `total - 相手のフェード` へ制限してから通す（通した時点で no-op になる）
    void clampFades()
    {
        const auto total = totalLengthSamples();
        fadeInSamples = juce::jlimit ((juce::int64) 0, total, fadeInSamples);
        fadeOutSamples = juce::jlimit ((juce::int64) 0, total - fadeInSamples, fadeOutSamples);
    }

    // ハンドルのドラッグで狙った長さ（samples）を、**相手のフェードを押しのけない**範囲へ収める。
    // 適用はしない（呼び出し側が代入する）ので、ドラッグ中の「値が実際に変わったか」判定にも使える。
    // ここが clampFades() と別に要るのは、clampFades が fadeIn 優先だから
    // （fadeIn を伸ばすと fadeOut が押し縮められ、ドラッグの規則に反する）
    juce::int64 clampedFadeIn (juce::int64 samples) const
    {
        const auto limit = juce::jmax ((juce::int64) 0, totalLengthSamples() - fadeOutSamples);
        return juce::jlimit ((juce::int64) 0, limit, samples);
    }

    juce::int64 clampedFadeOut (juce::int64 samples) const
    {
        const auto limit = juce::jmax ((juce::int64) 0, totalLengthSamples() - fadeInSamples);
        return juce::jlimit ((juce::int64) 0, limit, samples);
    }

    void buildPeakCache();
};

// 区間の下端/上端（signed）。絶対値1本にしないのは、512サンプルが1周期に満たない低音
// （48kHzなら93.75Hz未満。808はここに入る）だと片側にしか振れない区間が生まれ、
// 絶対値を±で描くと実際にはない側へ波形が伸びてしまうため
struct PeakRange
{
    float lo = 0.0f;
    float hi = 0.0f;
};

// バッファ全長の描画用ピークキャッシュ（Clip::samplesPerPeak 単位・ステレオはL/Rを合成）。
// サンプル音源の波形表示用（クリップは参照範囲だけを見る Clip::buildPeakCache を使う）
std::vector<PeakRange> buildFullPeakCache (const juce::AudioBuffer<float>& audio);

// クリップを splitSample（絶対サンプル位置）で左右に分ける。左右は同じソースWAVを共有参照する。
// 分割点が内側（開始 < 分割点 < 終端）にないときは false（境界ちょうどは分割しない）。
// フェードは外側だけを継承する（左: fadeIn / 右: fadeOut）。内側を0にしないと
// 分割点にフェードが移動してしまう。
// ループは解除して返す（左右どちらに繰り返しを引き継ぐか自明でないため）。解除はフェードの
// クランプより先に行うので、返ってきた左右は不変条件を満たしている
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
    // 本体の後ろに何回繰り返すか（0 = ループなし）。Clip::loopCount と同じ規則
    int loopCount = 0;
    std::vector<MidiNote> notes;

    // ループを含む総再生長（描画・ヒットテスト・終端計算はこちらを見る）
    juce::int64 totalLengthPpq() const { return lengthPpq * (1 + juce::jmax (0, loopCount)); }

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

// ---- 再生用の展開（ループ回数ぶん繰り返す）----
// 通常再生・⌘B（Project::buildSnapshot）と ⌘E（BounceRenderer::buildItemRender）の両方から呼ぶ。
// 展開規則を1箇所に集めるためのヘルパーなので、変えると両方の経路に効く。
//
// ミュートは判断しない。⌘Eは「明示選択が優先」でリージョンのミュートを無視する既存仕様なので、
// muted による除外は呼び出し側（buildSnapshot）の責任にしている。

// ノートを絶対PPQへフラット化して out へ足す。**各反復の末尾で境界マスクをかける**ので、
// 境界をまたぐロングノートは次の反復へ持ち越さない（最終ループ終端だけで切るのでは足りない）。
// fixedPitch >= 0 なら全ノートのピッチを置き換える（GMドラムの固定ピッチ規則。-1で置換なし）
void appendRegionNotes (const MidiRegion& region, int fixedPitch, std::vector<MidiNotePlayback>& out);

// クリップを ClipPlayback 列へ展開して out へ足す。開始位置だけが本体長ずつ進み、
// ソース参照範囲（offset/length）は全反復で共通。範囲外読みの最終防衛線もここで掛ける
void appendClipPlaybacks (const Clip& clip, std::vector<ClipPlayback>& out);

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

// プロジェクトのキー（曲の調）。生成MIDI（ベースガチャ等）は常にこのキーで作る。
// キーは「曲がどの音を中心に回るか」の宣言で、BPMと同格の基本情報として扱う
// （BPMがグリッドの座標系なら、キーは音高の座標系）。拍子4/4固定と同じ引き算で
// モードは major / minor の2択（教会旋法は扱わない）
enum class KeyMode { major, minor };

struct ProjectKey
{
    int root = 0;                    // ピッチクラス 0..11（0=C）
    KeyMode mode = KeyMode::minor;

    bool operator== (const ProjectKey& other) const { return root == other.root && mode == other.mode; }
    bool operator!= (const ProjectKey& other) const { return ! (*this == other); }
};

namespace ProjectKeys
{
    juce::String modeName (KeyMode mode);                        // "major" / "minor"（JSON・CLI用）
    bool modeFromName (const juce::String& name, KeyMode& out);  // 未知名は false

    juce::String rootName (int root);                            // 表示用 "C" "C♯" …（♯表記固定）
    bool rootFromName (const juce::String& name, int& out);      // "C#"/"Db"/"F♯" 等（カードの値）を受ける

    // 表示名。Logic等の慣例に合わせ major は素の音名、minor は m を付ける（例: "F♯" / "F♯m"）
    juce::String displayName (const ProjectKey& key);

    // bass.py の --key 形式（ASCII。例: "F#:minor"）
    juce::String cliText (const ProjectKey& key);

    // カードの表示テキスト "F# major"（ReferenceAnalyzer::Result::keyText）→ ProjectKey。
    // 読めなければ nullopt（キーのゲート落ちで空のことがある）
    std::optional<ProjectKey> fromCardText (const juce::String& text);
}

// 採用ループ（ハーモニーのアンカー。v14）。上モノのループが進行とキーを決め、ベースガチャは
// これに従う（docs/design/reference-beat.md「音色の調達」）。
// - **Project 所有でクリップとは独立**: 由来クリップを全部消しても残り、消えるのは Loops タブ
//   での差し替えか明示解除のみ。同時に持てるのは1本だけ
// - roots は採用時に looproots.py が1回検出した値の永続化（ベースガチャは保存済みの
//   この値を読む。契約の真実の源は tools/library/looproots.py の docstring）
struct LoopAnchor
{
    juce::String libraryPath;      // ライブラリ相対パス（出自の表示・差し替え判定用）
    double bpm = 0.0;              // ループの表記BPM（逆コピーの源）
    ProjectKey key;                // ループのキー（逆コピーの源）
    int loopBars = 0;              // ループ小節数（4/4）
    int slotsPerBar = 0;           // 1小節あたりのハーモニースロット数（1/2/4/8/16）
    std::vector<int> roots;        // ルート列 0..11。長さ = loopBars × slotsPerBar
    std::vector<float> confidence; // スロット別の検出信頼度 0..1（roots と同長）
    bool degraded = false;         // 低信頼でトニック連打に退化した契約か

    // 契約（looproots.py）と同じ規則。読込時の検証と、書き込み側の事故防止の両方で使う
    bool isValid() const
    {
        const bool slotsOk = slotsPerBar == 1 || slotsPerBar == 2 || slotsPerBar == 4
                          || slotsPerBar == 8 || slotsPerBar == 16;
        // BPM は bass.py の実効 BPM と同じ 30..300（NaN/inf は範囲比較が false になり弾かれる）
        if (! slotsOk || loopBars < 1 || ! (bpm >= 30.0 && bpm <= 300.0))
            return false;
        // アンカー自身のキーも契約どおりに（root 12 等を save が書き込むと、次回 load で
        // アンカーごと消える「時限破損」になる。mode は enum 外の値のキャスト混入に対する防御）
        if (key.root < 0 || key.root > 11
            || (key.mode != KeyMode::major && key.mode != KeyMode::minor))
            return false;
        // ライブラリ**相対**パスの契約（index.json と同じ）。空・絶対パス・.. 上り は不正。
        // ".." は**パス要素として**のみ拒否する（"foo..bar.wav" のような正常なファイル名は通す）
        if (libraryPath.isEmpty() || libraryPath.startsWithChar ('/'))
            return false;
        for (const auto& segment : juce::StringArray::fromTokens (libraryPath, "/", ""))
            if (segment == "..")
                return false;
        // 長さ比較は size_t で行う（int の loopBars × slotsPerBar は巨大値で符号付きオーバーフロー）
        if (roots.size() != (size_t) loopBars * (size_t) slotsPerBar
            || confidence.size() != roots.size())
            return false;
        for (int r : roots)
            if (r < 0 || r > 11)
                return false;
        for (float c : confidence)
            if (! (c >= 0.0f && c <= 1.0f))
                return false;
        return true;
    }
};

namespace LoopAnchors
{
    // bass.py --roots に渡す契約 JSON（真実の源は tools/library/looproots.py の docstring）。
    // アンカーの永続化と同じ値から生成する — 採用時に1回検出した進行を、生成のたびに
    // 再検出せずここから配る
    juce::String rootsContractJson (const LoopAnchor& anchor);

    // looproots.py が出力した契約 JSON から anchor を組み立てる（**生の型まで strict に検証** —
    // 変換後の isValid だけだと roots: [9.5] のような型違反が 9 に化けて通ってしまう）。
    // bpm は契約に無いので呼び出し側が候補から設定し、libraryPath も候補のライブラリ相対パスで
    // 上書きしてから isValid を通すこと（契約の source は表示用のファイル名にすぎない）
    bool anchorFromContractJson (const juce::String& json, LoopAnchor& out);
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
    float sampleGain = 1.0f;            // サンプル音量（線形倍率。値域は GainScale = ±12dB 相当の約0.251..3.981）
    juce::int64 sampleStartOffset = 0;  // 頭の無音カット位置（バッファ内サンプル）
    double sampleSourceRate = 0.0;      // ファイル自体のSR（再生比率 sourceRate/deviceRate の計算に必要）
    std::shared_ptr<juce::AudioBuffer<float>> sampleAudio; // メモリ常駐（Clip::audio と同じ寿命規則）
    std::vector<PeakRange> samplePeakCache;                // 波形描画用（Clip::samplesPerPeak 単位・全長）

    bool hasSample() const { return sampleAudio != nullptr && sampleSourceRate > 0.0; }
    bool usesSampler() const { return type == TrackType::midi && instrument == InstrumentKind::sample; }
};

class Project
{
public:
    // v2: MIDIトラック・ID追加 / v3: クリップのoffsetSamples・lengthSamples /
    // v4: pan・sends・固定バス3本・Master / v5: サイクル（ループ範囲）/
    // v6: ステレオクリップ（ch数はJSONに持たずWAV自体から判定）・クリップ表示名（取り込み用）/
    // v7: プロジェクトメモ / v8: サンプル音源（instrument・sample*）/
    // v9: リージョン/クリップのループ（loopCount。欠損＝ループなし）/
    // v10: オーディオリージョンのゲイン（clips[].gain。線形倍率・欠損＝1.0）/
    // v11: オーディオリージョンのフェード（clips[].fadeInSamples / fadeOutSamples。欠損＝0）/
    // v12: 曲末フェードアウト（fadeOut.start / fadeOut.end。16分音符単位・欠損＝未設定）/
    // v13: キー（key.root 0..11 / key.mode major|minor。欠損＝未設定。旧LaLaがキー付き
    //      projectを開いて保存するとキーを黙って消すため、追加時に版を上げている）/
    // v14: 採用ループのアンカー（loopAnchor。欠損＝未採用）とクリップの出自（clips[].loopSource。
    //      欠損＝ループ由来でない）。旧LaLaが開いて保存するとアンカーが黙って消えるため版を上げる
    // v15: トラックEQの4バンド（fx.eq.bands 配列。欠損＝既定値。旧LaLaが開いて保存すると
    //      バンド設定が黙って消えるため版を上げる）
    // v16: トラックCompのパラメータ（fx.comp.threshold/ratio/attack/release/makeup/detectorHpf）。
    //      v15以前の fx.comp.enabled はDSPが無かった頃の値で無意味なため、読込時に一律OFFへ
    //      リセットする（既定もOFF: コンプに保証された中立設定が無く、ピルが真のバイパスを担う）
    // v17: Master Limiter（master.limiter.gain/ceiling/release。欠損＝既定値 Gain 0 /
    //      Ceiling -1.0 / Release 60ms。常在なので enabled は持たない）
    static constexpr int currentVersion = 17;

    juce::File directory;
    double bpm = 120.0;
    std::optional<ProjectKey> key; // 未設定 = nullopt（キーを決めていない曲を壊さない）
    std::optional<LoopAnchor> loopAnchor; // 採用ループ（v14）。未採用 = nullopt
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

    // 曲末フェードアウト（マスターフェード）。プロジェクトに1本だけ。単位・流儀はサイクルと同じ
    // （16分音符・0始まり・有効なのは start < end のときだけ）。
    // サイクルと違い**undo対象**にする（範囲の繰り返しと違って鳴る音そのものが変わるため）。
    // 終端以降は無音（フェード後にゲインが戻ることはない＝「曲そのものの終端」）
    int fadeOutStartSixteenths = 0;
    int fadeOutEndSixteenths = 0;
    bool hasFadeOut() const { return fadeOutStartSixteenths < fadeOutEndSixteenths; }

    // 最後のアイテムの「見た目上の終端」を16分音符単位で返す（0 = 鳴るアイテムが1つもない）。
    // 曲末フェードの終端を自動で置くための値で、バウンス範囲の算出（MainComponent::startBounce）
    // とは規則が違うので共用しない:
    // - リージョンミュートは除外する（「この部分は使わない」という恒久的な意思表示）
    // - **トラックのミュート・ソロは考慮しない**（一時的なモニター状態であって曲の長さではない。
    //   ミュートして作業中に⌃Fを押したら終端が変わる、という驚きの方が大きい）
    // - One Shot のサンプル全長は含めない（バウンスが含めるのは「鳴り切るまで書き出す」ため）
    // - 端数は**切り上げる**（最近傍だと終端が手前へ吸着して最後の音を切る）
    int lastItemEndSixteenths (double sampleRate) const;

    // 「startSixteenths から曲末まで」でフェードを引くときの終端を解決する。
    // 0 = no-op（何もしない）。判断をUIから切り離してテスト可能にするためモデル側に置く。
    // 規則:
    // - 鳴るアイテムが1つも無ければ 0。**既存フェードが残っていても 0**（全アイテムを消した後に
    //   ⌃F で開始点だけ動かせてしまうのを防ぐ）
    // - 既にフェードがあるなら終端は動かさない（2本目は作らず開始点だけ移す）
    // - 開始点が終端以後なら 0（判定は呼び出し側でグリッドへ丸めた後の値で行うこと）
    int resolveSongFadeEnd (int startSixteenths, double sampleRate) const;

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
    // スナップショットに載せる PlaybackSnapshot::midiGeneration の進め方。
    // エンジンはこの世代が変わったときだけ「全ノートオフ＋跨ぎノート再発音」を行う。
    // - midiStructure（既定・安全側）: リアルタイムエンジンへ渡す。MIDI構成が変わったかもしれない → 進める
    // - audioValuesOnly: リアルタイムエンジンへ渡すが、ノート・リージョン・トラック構成・音源を
    //   一切変えていないと呼び出し側が保証できるとき（リージョンゲインの調整）→ 据え置く。
    //   誤って据え置くと「削除したノートのオフが失われて鳴りっぱなし」が起きる
    // - offlineRender: エンジンへ渡さない構築（⌘Bのバウンス用）→ 世代に触らない。
    //   ここで進めてしまうと、次の audioValuesOnly のpushが「世代が変わった」と誤認され、
    //   鳴っているMIDIが消音＋再発音される（バウンス後にゲインを動かすと起きる）
    enum class SnapshotChange { midiStructure, audioValuesOnly, offlineRender };

    std::unique_ptr<PlaybackSnapshot> buildSnapshot (SnapshotChange change = SnapshotChange::midiStructure);

    static juce::File projectsRoot(); // ~/Music/daw

    // 1chはモノ、2ch以上は先頭2chをL/Rとして読む（3ch以上の余剰chは捨てる）。
    // sourceSampleRate != nullptr ならファイル自体のSRも返す（サンプル音源は元SRのまま保存し、
    // 再生比率の計算に使うため必須。クリップ読込はプロジェクトSRへ変換済みなので不要）
    static std::shared_ptr<juce::AudioBuffer<float>> loadWav (const juce::File& file,
                                                             double* sourceSampleRate = nullptr);

    // loadWav ＋ targetRate へのリサンプル（WindowedSinc・レイテンシ補償込み。AudioImporter と
    // 同じ変換品質）。ループ採用のように「ライブラリの wav をプロジェクト SR のバッファとして
    // メモリに欲しい」用途向け — クリップの契約（audio は常にプロジェクト SR）を守るための入口。
    // SR が一致していれば変換せずそのまま返す。失敗は nullptr
    static std::shared_ptr<juce::AudioBuffer<float>> loadWavResampled (const juce::File& file,
                                                                       double targetRate);

private:
    juce::uint64 nextId = 1; // 永続化される採番カウンタ
    // MIDI構成の世代。単調増加させるだけで永続化しない（エンジン側の初期値0を「未受信」に残すため
    // 最初の buildSnapshot で1になる）
    juce::uint64 midiGeneration = 0;

    static std::shared_ptr<TrackParams> unityParams()
    {
        auto params = std::make_shared<TrackParams>();
        params->gain.store (1.0f); // TrackParamsの既定0.8fを引き継がない（バス/Masterはユニティ）
        return params;
    }

    void ensureUniqueIds(); // 読込後に未採番(0)・重複IDを振り直す
};
