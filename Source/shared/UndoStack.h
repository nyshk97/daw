#pragma once

#include <vector>

#include "Project.h"

// 構造編集（ノート・リージョン・クリップ・トラック・セクションマーカーの追加/削除/変更）のUndo/Redo。
// メッセージスレッド専用。
//
// 方式: 編集の直前に Project::tracks とセクションマーカーの構造コピーを積む。
// - Clip::audio / TrackParams は shared_ptr の共有なのでコピーは安価
// - TrackParams を共有したまま持つため、音量・ミュート・ソロはundo対象外（仕様どおり）
// - nextId は巻き戻さない（ID再利用による衝突を避けるため常に単調増加）
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

    // 編集操作の直前に呼ぶ。redo履歴は破棄される
    void begin (const Project& project, EditKind kind = EditKind::structure)
    {
        undoStates.push_back ({ project.tracks, project.markers, kind });
        if ((int) undoStates.size() > maxDepth)
            undoStates.erase (undoStates.begin());
        redoStates.clear();
    }

    // kind には「復元した編集の種類」が入る（往復で同じ値。呼び出し側の同期範囲の判断に使う）
    bool undo (Project& project, EditKind& kind)
    {
        if (undoStates.empty())
            return false;
        kind = undoStates.back().kind;
        redoStates.push_back ({ std::move (project.tracks), std::move (project.markers), kind });
        project.tracks = std::move (undoStates.back().tracks);
        project.markers = std::move (undoStates.back().markers);
        undoStates.pop_back();
        return true;
    }

    bool redo (Project& project, EditKind& kind)
    {
        if (redoStates.empty())
            return false;
        kind = redoStates.back().kind;
        undoStates.push_back ({ std::move (project.tracks), std::move (project.markers), kind });
        project.tracks = std::move (redoStates.back().tracks);
        project.markers = std::move (redoStates.back().markers);
        redoStates.pop_back();
        return true;
    }

    bool canUndo() const { return ! undoStates.empty(); }
    bool canRedo() const { return ! redoStates.empty(); }

    // undo/redo履歴が参照するWAV（録音・取り込みクリップ＋サンプル音源）のファイル名。
    // 保存時のGCから保護する（undo/redoで復元したときにWAVが消えている事故を防ぐ）
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
        EditKind kind = EditKind::structure;
    };
    std::vector<State> undoStates, redoStates;
};
