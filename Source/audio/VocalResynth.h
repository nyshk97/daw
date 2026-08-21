#pragma once

#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../shared/RenderRecipe.h"

// ボーカルのピッチ補正の再合成（WORLD ボコーダー）。**オフライン専用**（RenderCache のワーカーから呼ぶ。
// オーディオコールバックからは呼ばない）。Phase 0 の lab で採用（docs/labs/pitch-correction.md）。
//
// 仕組み: 声を「音程（f0）」「声道の形（スペクトル包絡 = CheapTrick）」「息成分（非周期性 = D4C）」に分解し、
// f0 だけを目標カーブへ書き換えて合成し直す。包絡を触らないのでフォルマントは構造上保たれる（つまみ不要）。
// f0 の入力は WORLD 自身の検出（harvest）でなく自作 YIN のカーブ（解析は1回で済ませる）。
// 時間写像（ノートの横移動・v20 の stretchRatio）は出力フレームごとに入力フレームを最近傍参照して表現する。
// 無声フレーム（f0 = 0）はそのまま無声として合成される＝子音・息は移調でも動かない
// （signalsmith の移調は子音も動く。声では動かない方が自然）。
//
// 補正なしのクリップ（移調・伸縮だけ）はこの経路を通らない（ClipStretcher のまま。無加工ビット一致の原則）
namespace VocalResynth
{
// 失敗は nullptr（原音でごまかさない）。要求値の範囲外・カーブ不整合・メモリ上限超過も nullptr
std::unique_ptr<juce::AudioBuffer<float>> render (const RenderRecipe& recipe);

// 解析フレーム数の上限（CheapTrick/D4C のスペクトログラムは fft/2+1 doubles × 2 本/フレーム。
// 48kHz・f0_floor 60Hz で fft 4096 → 32KB/フレーム。768MB で約 2 分。超える domain は失敗にする）
juce::int64 maxAnalysisBytes();
} // namespace VocalResynth
