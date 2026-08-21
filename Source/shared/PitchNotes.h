#pragma once

#include <vector>

#include "PitchCurve.h"
#include "ProjectKey.h"

// ピッチカーブ → 検出ノート列（tools/pitchlab/notes.py の移植。判断ロジックは shared/ に置き
// daw_tests で固定する）。検出ノートは「有声フレームに対する目標のまとまり」で、目標音・バイパス・
// タイミングは持たない（編集状態は PitchCorrection が唯一の持ち主）。フレームは WAV 先頭基準の絶対座標
struct DetectedPitchNote
{
    int startFrame = 0;   // 含む
    int endFrame = 0;     // 含まない
    float medianMidi = 0; // 区間の中央値（ビブラート・語尾のしゃくりに引きずられない）

    int lengthFrames() const { return endFrame - startFrame; }
};

namespace PitchNotes
{
// 分割規則の定数（lab で決めた値。docs/labs/pitch-correction.md）
inline constexpr double smoothMs = 40.0;  // 「いまの音程」= 直近 40ms の中央値
inline constexpr double holdMs = 90.0;    // ノート中央値から jumpSemitones 以上離れた状態がこれ以上続いたら切る
                                          // （ビブラート半周期 5〜6Hz = 80〜100ms より長く。40ms だと割れた）
inline constexpr double jumpSemitones = 0.5;
inline constexpr double minNoteMs = 60.0; // これ未満は隣（同じ有声区間の直前。先頭なら直後）へ吸収

std::vector<DetectedPitchNote> detect (const PitchCurve& curve);

// スケール推定（Krumhansl–Schmuckler）。ノートの長さで重み付けした音高クラス分布と 24 調のプロファイルの
// 相関係数を取り、最大の調を返す。correlation は −1..1 で、1 に近いほど「その調の音の使われ方に似ている」。
// 0.8 超なら明確、0.5 前後は曖昧（短いフレーズ・ラップの話し声的な音高では低くなる）
struct KeyEstimate
{
    ProjectKey key;
    double correlation = 0.0;
    bool valid = false; // ノートが無ければ false
};
KeyEstimate estimateKey (const std::vector<DetectedPitchNote>& notes);
} // namespace PitchNotes
