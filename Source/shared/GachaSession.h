#pragma once

#include <array>
#include <juce_core/juce_core.h>

#include "MidiImport.h"
#include "Project.h"
#include "UndoStack.h"

// ドラムガチャの候補管理と仮配置（右パネル第3モードの判断ロジック側）。
// UI（RightPanel のガチャモード）は候補一覧の表示とクリックだけを担当し、
// モデルへの仮配置・差し替え・撤去・「残す」確定はすべてここを通す
// （daw_tests は UI の .cpp を含まないため、ここに置いて CTest で固定する）。
//
// undo との整合（docs/plans/2026-08-04-1546 で確定）:
// - 候補の切り替えは UndoStack を経由せず Project を直接書き換える（呼び出し側が
//   スナップショットを再pushする）。仮配置を undo 履歴に混入させないため
// - 「残す」は仮配置**前**の tracks を before として UndoStack::pushCommitted に1件積む
//   （begin 方式だと「残した後の状態」が before になり undo が no-op になる）
// - キャンセルは仮リージョンを外科的に取り除く（このセッションで自動作成した
//   「Drums」トラックはトラックごと撤去）
class GachaSession
{
public:
    // パターン・ミニチュア（候補一覧の1行に描く1小節ぶんのドット譜）。
    // レーンは [kick, snare, hat]、スロットは16分×16。値: 0=なし / 1=装飾（ゴースト）/ 2=骨格
    static constexpr int patternLanes = 3;
    static constexpr int patternSlots = 16;
    using Pattern = std::array<std::array<int, patternSlots>, patternLanes>;

    // drums.py --porcelain の1行ぶん（候補一覧の1件）
    struct Candidate
    {
        juce::String base;                       // ファイル名の共通部（.mid/.wav/.json が並ぶ）
        juce::String kickSeed, snareSeed, hatSeed; // 8桁hex（--lock に渡す形式そのまま）
        juce::String status;                     // generated / regenerated / skipped
        Pattern pattern {};                      // .mid から抽出したミニチュア（hasPattern=false なら未取得）
        bool hasPattern = false;
    };

    // 取り込み済みノート列（PPQ 960）から1小節ぶんのミニチュアを作る。
    // - スロットはスウィング・ジッター込みの位置を最近傍の16分へ丸める（生成側のクリップ幅
    //   ±40%×16分 < 半スロットなので取り違えない）
    // - 濃淡は velocity で分ける: 生成器は vel ≈ 20 + 強度×96 なので、骨格閾値 0.35 に相当する
    //   ≈53 を境に 骨格(2)/装飾(1) とみなす（表示専用の近似。境界の±ノイズは許容）
    // - 2小節目以降のノートは無視する（ガチャは1小節パターンの繰り返し）
    static Pattern patternFromDrumNotes (const std::vector<MidiNote>& notes);

    // porcelain 1行をパースする。JSON でない・キー欠損は false（呼び出し側は行を捨てずエラー扱いに）
    static bool parsePorcelainLine (const juce::String& line, Candidate& out);

    // ---- 候補一覧（今回の実行で申告されたファイルだけ。gacha/ の全列挙はしない）----
    void setCandidates (std::vector<Candidate> list) { candidates = std::move (list); }
    const std::vector<Candidate>& getCandidates() const { return candidates; }

    // ---- 仮配置 ----
    bool hasPreview() const { return previewActive; }
    juce::uint64 previewRegionId() const { return previewActive ? regionId : 0; }
    juce::uint64 previewTrackId() const { return previewActive ? trackId : 0; }

    // (trackId, regionId) が仮オブジェクトか（仮リージョン・自動作成トラックへの操作を
    // 「撤去して中止」するための判定。トラックだけの判定は trackIsPreview）
    bool isPreviewObject (juce::uint64 objectTrackId, juce::uint64 objectRegionId) const
    {
        return previewActive
               && (objectRegionId == regionId
                   || (autoCreatedTrack && objectTrackId == trackId));
    }

    bool trackIsPreviewOwned (juce::uint64 objectTrackId) const
    {
        return previewActive && autoCreatedTrack && objectTrackId == trackId;
    }

    // 候補を仮配置する（2回目以降は差し替え）。
    // - 初回: 仮配置前の tracks を保存し、対象トラックを決める。preferredTrackIndex が
    //   Drum Kit（midi かつ drums=true）ならそれ、でなければ「Drums」トラックを自動作成
    // - 開始位置は初回の startPpq で固定（差し替えは同じ場所。「別の場所で聴きたい」は
    //   一度キャンセルして仮配置し直す操作になる）
    // 失敗（ドラムノートが無い等）は false（状態は変えない）
    bool previewCandidate (Project& project, const MidiImport::Result& parsed,
                           int preferredTrackIndex, juce::int64 startPpq);

    // 仮リージョン（＋自動作成トラック）を取り除く。何か取り除いたら true。
    // 全編集入口の cancelGachaPreview() から呼ばれる想定（呼び出し側が再描画・再pushする）
    bool cancelPreview (Project& project);

    // 「残す」: 仮配置前の状態を before として undo に1件積み、セッションを畳む。
    // 確定変更があれば true（呼び出し側が dirty 化・保存・再push する）
    bool keep (Project& project, UndoStack& undoStack);

private:
    int findPreviewTrack (const Project& project) const;   // trackId → index（無ければ -1）

    std::vector<Candidate> candidates;

    bool previewActive = false;
    bool autoCreatedTrack = false;   // このセッションで「Drums」を作った（キャンセルでトラックごと撤去）
    juce::uint64 trackId = 0;        // 仮配置先トラック
    juce::uint64 regionId = 0;       // 仮リージョン
    juce::int64 previewStartPpq = 0; // 初回配置位置（差し替えでも維持）
    std::vector<Track> beforeTracks; // 仮配置開始時点の tracks（「残す」の before）
};
