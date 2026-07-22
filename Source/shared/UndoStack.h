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

    // 編集操作の直前に呼ぶ。redo履歴は破棄される
    void begin (const Project& project)
    {
        undoStates.push_back ({ project.tracks, project.markers });
        if ((int) undoStates.size() > maxDepth)
            undoStates.erase (undoStates.begin());
        redoStates.clear();
    }

    bool undo (Project& project)
    {
        if (undoStates.empty())
            return false;
        redoStates.push_back ({ std::move (project.tracks), std::move (project.markers) });
        project.tracks = std::move (undoStates.back().tracks);
        project.markers = std::move (undoStates.back().markers);
        undoStates.pop_back();
        return true;
    }

    bool redo (Project& project)
    {
        if (redoStates.empty())
            return false;
        undoStates.push_back ({ std::move (project.tracks), std::move (project.markers) });
        project.tracks = std::move (redoStates.back().tracks);
        project.markers = std::move (redoStates.back().markers);
        redoStates.pop_back();
        return true;
    }

    bool canUndo() const { return ! undoStates.empty(); }
    bool canRedo() const { return ! redoStates.empty(); }

    // undo/redo履歴が参照する録音WAVのファイル名。
    // 保存時のGCから保護する（redoでクリップを復元したときにWAVが消えている事故を防ぐ）
    juce::StringArray referencedWavs() const
    {
        juce::StringArray files;
        for (const auto* states : { &undoStates, &redoStates })
            for (const auto& state : *states)
                for (const auto& track : state.tracks)
                    for (const auto& clip : track.clips)
                        files.addIfNotAlreadyThere (clip.fileName);
        return files;
    }

private:
    struct State
    {
        std::vector<Track> tracks;
        std::vector<SectionMarker> markers;
    };
    std::vector<State> undoStates, redoStates;
};
