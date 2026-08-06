#pragma once

#include <juce_core/juce_core.h>

#include "Project.h"
#include "UndoStack.h"

// リファレンス原曲クリップの頭出し: クリップ内の最初の小節頭（分析が検出した位相）を
// LaLa の小節線に合わせる。テンポ（BPM設定）と位相の両方が揃って初めて
// 「原曲を小さめ＋生成ドラムを大きめで重ねて聴く」が成立する。
//
// 判断ロジック（位相の読み込み・信頼性ゲート・クリップ同定・no-op判定・undo 1件の適用）は
// 全部ここに置き、MainComponent は transport/LCD/再描画の同期だけを担当する
// （daw_tests は UI の .cpp を含まないため。MidiImport::apply と同じ構成）。
//
// 位相データの出典（docs/plans/2026-08-05-1458 で確定）:
// - `analysis/groove.json` の `first_downbeat_sec` を使う。basics.json の同名値は前段の推定で、
//   groove.py がドラムステムのアタックで原点を再調整した後の値と数十msズレる（フラムに聞こえる）。
//   精度の劣る値へのフォールバックはしない
// - `analysis/gates.json` の `downbeat.ok` が true のときだけ提供する。分析は信頼できないときも
//   最尤の小節頭を常に出すため、値の存在では信頼性を判定できない
namespace ReferenceAlign
{
// 元クリップの同定情報（track.wav 書き出し時のクリップ）。複製クリップは3値が同一になるため、
// 一致が「タイムライン全体でちょうど1件」のときだけ操作を提供する（別クリップを黙って動かさない）
struct ClipDescriptor
{
    juce::String fileName;          // プロジェクト相対（clip-NNN.wav）
    juce::int64 offsetSamples = 0;
    juce::int64 lengthSamples = 0;

    bool isValid() const { return fileName.isNotEmpty() && lengthSamples > 0; }
};

// 頭出し情報（groove.json ＋ gates.json）。available=false のとき reason に理由
struct Info
{
    bool available = false;
    juce::String reason;
    double firstDownbeatSec = 0.0;
};

Info readInfo (const juce::File& referenceFolder);

// source.json（書き出し時に ReferenceExport が置く元クリップのメモ）の読み書き。
// 読めない・欠損は isValid()==false の記述子を返す
bool writeSourceDescriptor (const juce::File& referenceFolder, const ClipDescriptor& descriptor);
ClipDescriptor readSourceDescriptor (const juce::File& referenceFolder);

// 記述子に一致するクリップを探す。matches はタイムライン全体での一致数
//（trackIndex/clipIndex は matches==1 のときだけ有効）
struct Located
{
    int trackIndex = -1;
    int clipIndex = -1;
    int matches = 0;
};

Located locateClip (const Project& project, const ClipDescriptor& descriptor);

// 頭出し後の startSample。現在位置から最も近い整合位置（クリップ内の最初の小節頭が
// 小節線に乗る位置）へ動かす。startSample >= 0 を保つ
juce::int64 alignedClipStart (juce::int64 currentStart, juce::int64 fdSamples, double barLenSamples);

enum class ApplyResult
{
    applied,  // BPM設定＋クリップ移動を undo 1件で適用した
    noChange, // すでに整合済み（begin していない = no-op の undo 履歴を積まない）
    notFound  // クリップを同定できない・前提が壊れている（何も変更していない）
};

// BPM設定（＋キー設定）＋クリップ頭出しの複合適用。undo は全体で1件。
// key は nullopt = キーに触らない（現行の「BPMを設定して頭出し」と同じ）。
// no-op 判定は対象項目すべて（BPM・key・移動先）で行い、全同値なら begin() せず noChange
ApplyResult apply (Project& project, UndoStack& undoStack, const ClipDescriptor& descriptor,
                   double bpm, double firstDownbeatSec,
                   const std::optional<ProjectKey>& key = std::nullopt);

// BPM＋キーのみの複合設定（キーあり・頭出し不可の経路）。undo は全体で1件。
// 両方同値なら noChange / BPM が範囲外なら notFound。
// 分析完了ダイアログの undo 粒度（経路別に1件）の判断をUIから切り離すためここに置く
ApplyResult applyBpmAndKey (Project& project, UndoStack& undoStack,
                            double bpm, const std::optional<ProjectKey>& key);
}
