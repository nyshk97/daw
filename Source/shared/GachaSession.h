#pragma once

#include <array>
#include <juce_core/juce_core.h>

#include "MidiImport.h"
#include "Project.h"
#include "UndoStack.h"

// ガチャの候補管理と仮配置（右パネル第3モードの判断ロジック側）。
// パーツ（Drums / Bass）ごとに独立した仮配置を同時に保持できる — パーツを行き来して
// 「ベースを聴いてからドラムを振り直す」を成立させるため（将来 Keys 等が +1 される）。
// UI（RightPanel のガチャモード）は候補一覧の表示とクリックだけを担当し、
// モデルへの仮配置・差し替え・撤去・「残す」確定はすべてここを通す
// （daw_tests は UI の .cpp を含まないため、ここに置いて CTest で固定する）。
//
// undo との整合（docs/plans/2026-08-04-1546 / 2026-08-06-1809 で確定）:
// - 候補の切り替えは UndoStack を経由せず Project を直接書き換える（呼び出し側が
//   スナップショットを再pushする）。仮配置を undo 履歴に混入させないため
// - baseline（仮配置前の tracks）は**セッション全体で1回だけ**、最初の仮配置時に保存する。
//   パーツごとに持つと、後から始めたパーツの before に未確定の他パーツが混ざる
// - 「残す」は**全パーツをまとめて**、baseline を before として UndoStack::pushCommitted に
//   1件積む（begin 方式だと「残した後の状態」が before になり undo が no-op になる）
// - キャンセルは2層: cancelPart はそのパーツだけ外科的に撤去（他パーツ維持。最後の1件が
//   消えたら baseline 破棄）/ cancelPreview は全パーツ撤去（通常編集・undo・保存・モード
//   離脱の入口用）。このセッションで自動作成したトラックはトラックごと撤去する
class GachaSession
{
public:
    // パーツ。candidates / previews の添字にも使う。
    // loops は音声（ループ素材の採用）で、候補は MIDI でなくクリップとして敷く
    enum class Part { drums = 0, bass = 1, loops = 2 };
    static constexpr int numParts = 3;

    // パターン・ミニチュア（ドラム候補一覧の1行に描く1小節ぶんのドット譜）。
    // レーンは [kick, snare, hat]、スロットは16分×16。値: 0=なし / 1=装飾（ゴースト）/ 2=骨格
    static constexpr int patternLanes = 3;
    static constexpr int patternSlots = 16;
    using Pattern = std::array<std::array<int, patternSlots>, patternLanes>;

    // ベース候補のミニチュア（音高ストリップ）。x = パターン内位置 0..1 / y = 音高 0..1
    // （ハード範囲 MIDI 28..51 を正規化。y は上が高音になるよう描画側で反転する）
    struct BassDot
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    // porcelain の1行ぶん（候補一覧の1件）。ドラムは kick/snare/hat、ベースは prog/rhythm の
    // seed を持つ（使わない側は空のまま）
    struct Candidate
    {
        juce::String base;                         // ファイル名の共通部（.mid/.wav/.json が並ぶ）
        juce::String kickSeed, snareSeed, hatSeed; // 8桁hex（drums の --lock に渡す形式そのまま）
        juce::String progSeed, rhythmSeed;         // 8桁hex（bass の --lock に渡す形式そのまま）
        juce::String status;                       // generated / regenerated / skipped
        Pattern pattern {};                        // ドラムのミニチュア（hasPattern=false なら未取得）
        bool hasPattern = false;
        std::vector<BassDot> bassDots;             // ベースのミニチュア（空なら未取得）
    };

    // 取り込み済みノート列（PPQ 960）から1小節ぶんのドラム・ミニチュアを作る。
    // - スロットはスウィング・ジッター込みの位置を最近傍の16分へ丸める（生成側のクリップ幅
    //   ±40%×16分 < 半スロットなので取り違えない）
    // - 濃淡は velocity で分ける: 生成器は vel ≈ 20 + 強度×96 なので、骨格閾値 0.35 に相当する
    //   ≈53 を境に 骨格(2)/装飾(1) とみなす（表示専用の近似。境界の±ノイズは許容）
    // - 2小節目以降のノートは無視する（ドラムガチャは1小節パターンの繰り返し）
    static Pattern patternFromDrumNotes (const std::vector<MidiNote>& notes);

    // ベース候補のミニチュア。patternTicks = パターン長（LaLa PPQ。loop_bars × ticksPerBar）。
    // パターン1周ぶん（patternTicks 未満）のノートだけを x 0..1 へ正規化する
    static std::vector<BassDot> bassDotsFromNotes (const std::vector<MidiNote>& notes,
                                                   juce::int64 patternTicks);

    // porcelain 1行をパースする。JSON でない・キー欠損は false（呼び出し側は行を捨てずエラー扱いに）
    static bool parsePorcelainLine (const juce::String& line, Candidate& out);     // drums（kick/snare/hat）
    static bool parseBassPorcelainLine (const juce::String& line, Candidate& out); // bass（prog/rhythm）

    // ベース振り直しの実行計画（試聴長・キック抽出）。判断をUIから切り離してテスト可能にする。
    // - previewBars = max(ドラム長, loopBars) を loopBars の倍数へ切り上げ（bass.py は倍数のみ受ける）
    // - kickTicks = ドラムリージョン先頭基準の相対 tick 列（**bass.py の PPQ 480**。LaLa の 960 から
    //   換算済み）。ループ反復は previewBars ぶんまで展開する。固定ピッチ打楽器トラック
    //   （drumPitch >= 0）は実効ピッチで判定する
    struct BassRollPlan
    {
        int previewBars = 1;
        int drumsBars = 0;           // 0 = ドラムソース無し
        juce::StringArray kickTicks; // 空 = キックブーストなし
    };
    static BassRollPlan planBassRoll (const Project& project, int drumsTrackIndex,
                                      int drumsRegionIndex, int loopBars);

    // ---- ループ（音声）仮配置 ----
    // ループ候補は MIDI と違い**クリップ**として敷く。クリップは ID を持たないため、
    // fileName（マーカー → keep 時に clip-NNN.wav へ実体化）で識別する。
    // audio は**プロジェクトSRへ変換済み**のバッファを渡す契約（取り込みと同じ）。keep は
    // このバッファを 24bit WAV として書き出す — ライブラリ原本のコピーにしないのは、
    // 原本の SR がプロジェクトと違うと再読込で再生速度が狂うため
    static constexpr const char* loopPreviewMarker = "gacha-loop-preview"; // 実体化前の仮 fileName

    struct LoopPreviewInput
    {
        LoopAnchor anchor;                               // 採用ループのメタデータ（isValid 前提）
        std::shared_ptr<juce::AudioBuffer<float>> audio; // プロジェクトSR変換済み
        juce::String displayName;                        // クリップ表示名（ファイル名の basename）
        // 配置位置。**差し替えでも毎回使う**（BPM が変わると同じ絶対サンプルは小節頭で
        // なくなるため、呼び出し側が「逆コピー→新BPMで小節頭換算」の順で毎回計算して渡す）
        juce::int64 startSample = 0;
        // audio を変換したレート。SR 未確定プロジェクトでは keep 時にこの値で確定する
        // （プレビュー段階で SR を確定させない — キャンセルだけで dirty が残るため。
        // ダイアログ中にデバイス SR が変わっても、変換に使った値がここに残るので狂わない）
        double audioSampleRate = 0.0;
        int loopCount = 1;           // Clip::loopCount（0 = 1周のみ）
        bool applyKeyBpm = true;     // 逆コピー（「設定して敷く」）。false = 敷くだけ
    };

    // ループ候補を仮配置する（2回目以降は差し替え・同じ場所）。アンカーは常に更新し、
    // applyKeyBpm ならプロジェクトの BPM・キーもループの値になる（undo はセッション baseline が
    // 受け持つ）。BPM/キーが実際に変わるときは**ベースの仮配置だけ撤去**する
    // （進行がループ由来のため。ドラムは維持 — plan の決定）。
    // 敷き先は**常に専用の新規トラック**（名前=ループ名・キャンセルでトラックごと撤去）。
    // 既存トラックへ敷くと前に採用した旧ループと同位置に重なり見えない二重再生になるため、
    // 旧ループは自分のトラックに残して M/S で比べる設計にした（2026-08-08 実地の混乱から変更）
    bool previewLoopCandidate (Project& project, const LoopPreviewInput& input);

    // ループ仮配置の audio が変換されたレート（未配置は 0）。SR 未確定プロジェクトの keep 前に
    // 呼び出し側がこの値で project.sampleRate を確定する（実体化はプロジェクト SR で書くため）
    double loopPreviewSampleRate() const { return previews[(size_t) Part::loops].audioSampleRate; }

    // ---- ループ候補（recommend.py --json の1ページぶん）----
    // MIDI 候補（Candidate）と違い seed を持たない: おすすめは決定的なランキングで、
    // 「振り直し」でなく「次の5本」のページングで探索する（plan の確定仕様）
    struct LoopCandidate
    {
        juce::String path;         // ライブラリ相対パス
        double bpm = 0.0;          // ループの表記 BPM
        int loopBars = 0;          // index の尺推定（0 = 不明。採用時 looproots へ --bars で渡す）
        int keyRoot = 0;           // 0..11
        KeyMode keyMode = KeyMode::minor;
        int transposeSemitones = 0; // リファレンスのキー圏に合わせる移調量（表示用）
        double bpmRatio = 1.0;      // ループ/リファレンスの BPM 比（表示用）
        juce::String reason;        // 平易語の一行
    };
    struct LoopRecommendation
    {
        double refBpm = 0.0;
        juce::String refKeyText;   // 表示用（例 "A minor"）
        bool keyTrusted = true;    // false = カードのキーがゲート落ちで推定値
        int page = 1;
        int pageSize = 5;          // recommend.py --page-size の値（出力に無い旧形式は5）
        int total = 0;             // フィルタ通過の総数
        std::vector<LoopCandidate> candidates;
    };
    // recommend.py --json の出力をパースする。形式不備（キー欠損・型違い）は false
    static bool parseRecommendJson (const juce::String& json, LoopRecommendation& out);

    // ---- 候補一覧（今回の実行で申告されたファイルだけ。gacha/ の全列挙はしない）----
    void setCandidates (Part part, std::vector<Candidate> list)
    {
        candidates[(size_t) part] = std::move (list);
    }
    const std::vector<Candidate>& getCandidates (Part part) const
    {
        return candidates[(size_t) part];
    }

    // ---- 仮配置 ----
    bool hasPreview() const;           // どれかのパーツが仮配置中
    bool hasPreview (Part part) const { return previews[(size_t) part].active; }
    juce::uint64 previewRegionId (Part part) const;
    juce::uint64 previewTrackId (Part part) const;
    juce::int64 previewStartPpq (Part part) const;

    // (trackId, regionId) がいずれかのパーツの仮オブジェクトか（仮リージョン・自動作成トラックへの
    // 操作を「撤去して中止」するための判定。トラックだけの判定は trackIsPreviewOwned）
    bool isPreviewObject (juce::uint64 objectTrackId, juce::uint64 objectRegionId) const;
    bool trackIsPreviewOwned (juce::uint64 objectTrackId) const;
    // (trackId, クリップの fileName) が Loops の仮クリップか（クリップは ID を持たないため
    // fileName で判定する。isPreviewObject の音声クリップ版 — 編集入口のガードに使う）
    bool isPreviewClip (juce::uint64 objectTrackId, const juce::String& fileName) const;

    // 仮配置元の情報（延長再生成に使う: どのカードのどの seed から来た候補か）。
    // MainComponent が previewCandidate 成功後に setPreviewSource で記録し、
    // そのパーツの撤去で消える。bars = 生成時の書き出し小節数
    struct PreviewSource
    {
        juce::File cardFolder;
        juce::StringArray laneSeeds; // drums: kick,snare,hat / bass: prog,rhythm（--lock の順）
        int bars = 0;
        bool isValid() const { return cardFolder != juce::File() && ! laneSeeds.isEmpty(); }
    };
    void setPreviewSource (Part part, PreviewSource source);
    const PreviewSource& previewSource (Part part) const { return sources[(size_t) part]; }

    // 候補を仮配置する（2回目以降は差し替え）。
    // - セッション初回: 仮配置前の tracks を baseline として保存
    // - パーツ初回: **常に専用の新規トラックを自動作成**（「Drums」/「Bass」Finger Bass 33。
    //   同名があれば連番）。既存トラックの流用はしない — 選択駆動の流用は「回す前に正しい
    //   トラックを選んでおく」暗黙知を要求し、外すと予測できない場所に置かれるため廃止
    //   （2026-08-08 ループの新規トラック化と同時に統一。ガチャが既存トラックへ書くことはない）
    // - 開始位置はパーツ初回の startPpq で固定（差し替えは同じ場所）
    // 失敗（対象ノートが無い等）は false（状態は変えない）
    bool previewCandidate (Part part, Project& project, const MidiImport::Result& parsed,
                           juce::int64 startPpq);

    // そのパーツの仮リージョン（＋自動作成トラック）だけを取り除く。何か取り除いたら true。
    // 最後の仮配置が消えたときだけセッション baseline を破棄する（他パーツは維持）
    bool cancelPart (Part part, Project& project);

    // 全パーツの仮配置を取り除く。何か取り除いたら true。
    // 全編集入口の cancelGachaPreview() から呼ばれる想定（呼び出し側が再描画・再pushする）
    bool cancelPreview (Project& project);

    // 「残す」: baseline を before として undo に1件積み、**全パーツをまとめて確定**して
    // セッションを畳む。確定変更があれば true（呼び出し側が dirty 化・保存・再push する）
    bool keep (Project& project, UndoStack& undoStack);

private:
    struct PartPreview
    {
        bool active = false;
        bool autoCreatedTrack = false; // このセッションで作った（キャンセルでトラックごと撤去）
        juce::uint64 trackId = 0;      // 仮配置先トラック
        juce::uint64 regionId = 0;     // 仮リージョン（MIDI パーツ）
        juce::int64 startPpq = 0;      // パーツ初回の配置位置（差し替えでも維持）
        // loops パーツ用（クリップは ID を持たないため fileName で識別する）
        juce::String clipFileName;     // マーカー → keep の実体化で clip-NNN.wav へ
        juce::int64 startSample = 0;   // 配置位置（サンプル。差し替えごとに input の値で更新）
        double audioSampleRate = 0.0;  // audio の変換レート（SR 未確定プロジェクトの keep 用）
    };

    // ループ仮クリップの実体化（keep 内から呼ぶ）。バッファを 24bit WAV として
    // clip-NNN.wav へ書き出し、fileName をマーカーから実名へ差し替える。失敗は false
    bool materializeLoopClip (Project& project);

    int findTrack (const Project& project, juce::uint64 trackId) const; // id → index（無ければ -1）
    bool removePartObjects (Part part, Project& project); // 仮リージョン/自動作成トラックの撤去
    void resetPart (Part part);
    void resetSession();

    std::array<std::vector<Candidate>, numParts> candidates;
    std::array<PartPreview, numParts> previews;
    std::array<PreviewSource, numParts> sources;

    // セッション単位のトランザクション（「残す」＋⌘Z 1回＝全復元の土台）。
    // tracks だけでなく BPM・キー・アンカーも保存する — ループ採用（逆コピー）が仮配置中に
    // これらを書き換えるため。Project だけ戻しても transport が古い値を持ち直す事故は
    // 呼び出し側（MainComponent）が復元後に transport / LCD を同期して防ぐ
    bool baselineValid = false;      // 以下の before* が有効（どれかのパーツが仮配置中）
    std::vector<Track> beforeTracks; // セッション開始時点の tracks（「残す」の before）
    double beforeBpm = 120.0;
    std::optional<ProjectKey> beforeKey;
    std::optional<LoopAnchor> beforeAnchor;

    // キャンセル系で BPM・キー・アンカーを baseline へ戻す（tracks の撤去とは別枠。
    // 復元したら true — 呼び出し側が transport / LCD の同期を行う合図）
    bool restoreProjectValues (Project& project);
};
