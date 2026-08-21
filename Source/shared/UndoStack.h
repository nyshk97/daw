#pragma once

#include <functional>
#include <vector>

#include "Project.h"

// 構造編集（ノート・リージョン・クリップ・トラック・セクションマーカーの追加/削除/変更）のUndo/Redo。
// メッセージスレッド専用。
//
// 方式: 編集の直前に Project::tracks とセクションマーカーの構造コピーを積む。
// - Clip::audio / TrackParams は shared_ptr の共有なのでコピーは安価
// - TrackParams を共有したまま持つため、音量・ミュート・ソロはundo対象外（仕様どおり）
// - nextId は巻き戻さない（ID再利用による衝突を避けるため常に単調増加）
//
// ⚠️ Project直下の値を新たにundo対象にするときは、ここの State に足して begin/undo/redo の
// 3箇所すべてで往復させること。EditKind は「復元後にどう再pushするか」を選ぶだけで、
// **保存範囲は広げない**（曲末フェードを足したときに実際に踏んだ）
class UndoStack
{
public:
    static constexpr int maxDepth = 100;

    // 復元後に必要な同期の種類。呼び出し側（begin）が「これから行う編集」を宣言し、
    // undo/redo がその種類を返す。
    // - sampleValue: サンプル音源の値だけ（音量・ルート音・頭カット）。ノート・クリップ・トラック構成は
    //   変わらないので**スナップショットの再pushが不要**＝発音中の音を切らずに戻せる
    //   （再pushすると差し替え検出で全ノートオフ＋跨ぎノート再発音が走り、頭から鳴り直す）
    // - clipValue: オーディオクリップの値だけ（リージョンゲイン）。sampleValue と違い atomic ミラーでなく
    //   スナップショット経由で鳴るので**再pushは必要**。ただしMIDI構成は変わらないので
    //   「MIDI世代を据え置く push」（MainComponent::pushAudioValueSnapshot）を使って発音を乱さない
    // - structure: それ以外。音程モードの変更もこちら（停止要求＋resoundの経路が必要なため）
    enum class EditKind { structure, sampleValue, clipValue };

    // 編集開始の通知（ガチャの仮配置撤去用）。begin() が状態を保存する**前**に呼ばれるので、
    // ここで仮リージョンを取り除けば undo 履歴に仮配置が混入しない。
    // フック内から begin()/undo()/redo() を呼び返さないこと（再入は想定しない）
    std::function<void()> willBegin;

    // begin() の識別トークン。不成立だった編集の取り消し（abandon）に使う。0 = 無効
    using BeginToken = juce::uint64;

    // 編集操作の直前に呼ぶ。redo履歴は破棄される（直後に abandon() で取り消すまでは退避しておく）。
    // 戻り値のトークンを呼び出し側が持ち、編集が不成立ならそのトークンで abandon() する
    BeginToken begin (const Project& project, EditKind kind = EditKind::structure)
    {
        if (willBegin != nullptr)
            willBegin();
        dropAbandonable();
        undoStates.push_back ({ project.tracks, project.markers,
                                project.fadeOutStartSixteenths, project.fadeOutEndSixteenths,
                                project.bpm, project.key, project.loopAnchor, kind });
        stripClipDomains (undoStates.back().tracks);
        if ((int) undoStates.size() > maxDepth)
        {
            evicted = std::move (undoStates.front()); // abandon で戻せるように退避
            undoStates.erase (undoStates.begin());
        }
        abandonedRedo = std::move (redoStates);
        redoStates.clear();
        return lastToken = ++tokenCounter;
    }

    // 直前の begin() の取り消し。begin は変更**前**の状態を積むしかないので、その後に編集が不成立だった
    //（往復ドラッグで同じ段に戻った・クランプで動かなかった・split/merge が false）経路は後からこれで捨てる。
    // 呼び出し側が「モデルは begin 時点から変わっていない」ことを保証する。対象はトークンで確認し
    //（深さの一致だと無関係な undo を消す）、退避した redo 履歴と maxDepth で押し出した 1 件も戻す
    void abandon (BeginToken token)
    {
        if (token == 0 || token != lastToken || undoStates.empty())
            return; // 間に別の begin / undo / redo が入っていたら触らない
        undoStates.pop_back();
        if (evicted.has_value())
            undoStates.insert (undoStates.begin(), std::move (*evicted));
        redoStates = std::move (abandonedRedo);
        dropAbandonable();
    }

    // ガチャ「残す」の確定: 呼び出し側が保持していた**仮配置前の tracks / BPM / キー / アンカー**を
    // before として1件積む（begin と違い現在の project の値は使わない — 現在はすでに候補が
    // 置かれ、ループ採用なら BPM/キー/アンカーも書き換わった後で、それを before にすると
    // ⌘Z が仮配置前の値へ戻らない）。redo履歴は破棄される。
    // markers・フェードは仮配置中に変わらない（全編集入口が先に撤去する）ので現在値でよい
    void pushCommitted (std::vector<Track> beforeTracks, const Project& project,
                        double beforeBpm, const std::optional<ProjectKey>& beforeKey,
                        const std::optional<LoopAnchor>& beforeAnchor)
    {
        undoStates.push_back ({ std::move (beforeTracks), project.markers,
                                project.fadeOutStartSixteenths, project.fadeOutEndSixteenths,
                                beforeBpm, beforeKey, beforeAnchor, EditKind::structure });
        stripClipDomains (undoStates.back().tracks);
        if ((int) undoStates.size() > maxDepth)
            undoStates.erase (undoStates.begin());
        redoStates.clear();
        dropAbandonable();
    }

    // kind には「復元した編集の種類」が入る（往復で同じ値。呼び出し側の同期範囲の判断に使う）
    bool undo (Project& project, EditKind& kind)
    {
        dropAbandonable(); // 以後 abandon は効かない
        if (undoStates.empty())
            return false;
        kind = undoStates.back().kind;
        redoStates.push_back ({ std::move (project.tracks), std::move (project.markers),
                                project.fadeOutStartSixteenths, project.fadeOutEndSixteenths,
                                project.bpm, project.key, project.loopAnchor, kind });
        stripClipDomains (redoStates.back().tracks);
        restoreFrom (undoStates, project);
        return true;
    }

    bool redo (Project& project, EditKind& kind)
    {
        dropAbandonable();
        if (redoStates.empty())
            return false;
        kind = redoStates.back().kind;
        undoStates.push_back ({ std::move (project.tracks), std::move (project.markers),
                                project.fadeOutStartSixteenths, project.fadeOutEndSixteenths,
                                project.bpm, project.key, project.loopAnchor, kind });
        stripClipDomains (undoStates.back().tracks);
        restoreFrom (redoStates, project);
        return true;
    }

    bool canUndo() const { return ! undoStates.empty(); }
    bool canRedo() const { return ! redoStates.empty(); }

    // undo/redo履歴が参照するWAV（録音・取り込みクリップ＋サンプル音源）のファイル名。
    // 保存時のGCから保護する（undo/redoで復元したときにWAVが消えている事故を防ぐ）
    // undo/redo 履歴が参照するピッチ解析サイドカーのファイル名（保存時の GC で残す世代）
    juce::StringArray referencedSidecars() const
    {
        juce::StringArray files;
        for (const auto* states : { &undoStates, &redoStates })
            for (const auto& state : *states)
                for (const auto& track : state.tracks)
                    for (const auto& clip : track.clips)
                        if (clip.pitchCorrection.has_value())
                            files.addIfNotAlreadyThere (
                                PitchSidecar::fileNameFor (clip.fileName, clip.pitchCorrection->curveDigest));
        return files;
    }

    juce::StringArray referencedWavs() const
    {
        juce::StringArray files;
        for (const auto* states : { &undoStates, &redoStates })
            for (const auto& state : *states)
                for (const auto& track : state.tracks)
                {
                    for (const auto& clip : track.clips)
                        files.addIfNotAlreadyThere (clip.fileName);
                    if (track.sampleFile.isNotEmpty())
                        files.addIfNotAlreadyThere (track.sampleFile);
                }
        return files;
    }

private:
    struct State
    {
        std::vector<Track> tracks;
        std::vector<SectionMarker> markers;
        // 曲末フェード（Project直下の値。tracks/markers と違い移動できないので値コピー）
        int fadeOutStartSixteenths = 0;
        int fadeOutEndSixteenths = 0;
        double bpm = 120.0; // BPM変更（LCD・「BPMをプロジェクトに設定」）もundo対象
        std::optional<ProjectKey> key; // キー変更（LCD・「BPMとキーを設定」）もundo対象
        // 採用ループ（v14）。明示解除・採用・差し替えを undo 1件で戻すため State に持つ
        // （解除は tracks に触らないので、ここに無いと ⌘Z がアンカーだけ取り残す）
        std::optional<LoopAnchor> loopAnchor;
        EditKind kind = EditKind::structure;
    };

    // undo state に activeDomain（レンダー済みバッファ）を入れない。maxDepth = 100 ×
    // 3分ステレオ約69MB で数GBを抱え込むため。undo/redo 後は ClipDomains::reconcile と
    // RenderCache が（多くはキャッシュから）引き直す
    static void stripClipDomains (std::vector<Track>& tracks)
    {
        for (auto& track : tracks)
            for (auto& clip : track.clips)
            {
                clip.activeDomain = nullptr;
                clip.previewDomain = nullptr; // プレビューも積まない（エディタが現在の状態から作り直す）
            }
    }

    // states 末尾 → project の復元（undo/redo 共通。State のフィールドを増やしたらここも足す）
    static void restoreFrom (std::vector<State>& states, Project& project)
    {
        project.tracks = std::move (states.back().tracks);
        project.markers = std::move (states.back().markers);
        project.fadeOutStartSixteenths = states.back().fadeOutStartSixteenths;
        project.fadeOutEndSixteenths = states.back().fadeOutEndSixteenths;
        project.bpm = states.back().bpm;
        project.key = states.back().key;
        project.loopAnchor = std::move (states.back().loopAnchor);
        states.pop_back();
    }

    std::vector<State> undoStates, redoStates;

    std::vector<State> abandonedRedo; // 直前の begin で捨てた redo（abandon で戻す）
    std::optional<State> evicted;     // 直前の begin が maxDepth で押し出した 1 件（abandon で戻す）
    BeginToken lastToken = 0, tokenCounter = 0;
    // abandon 可能な状態を捨てる（退避分のメモリを次の begin まで抱えない）
    void dropAbandonable()
    {
        lastToken = 0;
        abandonedRedo.clear();
        evicted.reset();
    }
};
