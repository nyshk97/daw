#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "Project.h"
#include "UndoStack.h"

// SMF（.mid）→ LaLa の MidiRegion への変換と Project への適用。
// 判断ロジック（PPQ換算・ch10分離・note on/off 対応付け・トラック生成・undo 1件）は
// 全部ここに置き、UI（D&D・ファイルブラウザ・Fileメニュー）は呼ぶだけにする
// （daw_tests は UI の .cpp を含まないため、ここに置かないと回帰テストで固定できない）。
//
// 変換仕様（docs/plans/2026-08-04-1546-reference-drums-in-lala.md で確定）:
// - type 0/1 両対応。SMF トラックはチャンネル群（ch10 / それ以外）の中でマージする
// - PPQ 換算: ファイルの ticks_per_beat → LaLa の 960（Ppq::ticksPerQuarter）。丸めは round
// - SMPTE 形式（getTimeFormat() が負値）は明示的にエラー（tpq として換算すると位置が壊れる）
// - note_on/note_off の対応付けキーは（MIDIチャンネル, pitch）。その中で最も古い未クローズ
//   note_on と組にする（FIFO）。velocity 0 の note_on は note_off 扱い。孤立 note_off は無視、
//   クローズされない note_on はファイル末尾（最終イベント時刻）で閉じる
// - テンポトラックは無視（プロジェクト BPM が真実。PPQ ベースなので相対配置は保たれる）
namespace MidiImport
{
// 変換結果。チャンネル情報は分離にだけ使い、ノートには持たせない
struct Result
{
    std::vector<MidiNote> drumNotes;   // ch10 → Drum Kit（drums=true）トラックへ。id は 0（apply が採番）
    std::vector<MidiNote> otherNotes;  // それ以外のチャンネル → GM トラックへ
    juce::int64 drumRegionLengthPpq = 0;  // 最終ノート終端の小節切り上げ（該当ノートが無ければ 0）
    juce::int64 otherRegionLengthPpq = 0;
};

// 失敗（読めない・SMPTE・ノートなし）は false ＋ error に理由
bool parse (juce::InputStream& stream, Result& out, juce::String& error);
bool parseFile (const juce::File& file, Result& out, juce::String& error);

struct ApplyOutcome
{
    int firstTrackIndex = -1; // 作成したトラックの先頭（呼び出し側の選択用）
    int numTracksCreated = 0;
};

// Result を Project へ反映する。ch10 → Drum Kit トラック / 他チャンネル → GM トラック。
// 混在 SMF は2トラックになり、両リージョンは同じ startPpq に置く。undo は全体で1件
// （ここで undoStack.begin を1回だけ呼ぶ）。プロジェクトのサンプルレートには触れない。
// name はファイル名由来（拡張子なし）。startPpq は呼び出し側で小節頭へ丸め済みであること
ApplyOutcome apply (Project& project, UndoStack& undoStack, const Result& result,
                    const juce::String& name, juce::int64 startPpq);
}
