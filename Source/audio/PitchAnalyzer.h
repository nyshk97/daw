#pragma once

#include <functional>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../shared/PitchCurve.h"

// ボーカルのピッチ検出（YIN）。**オフライン専用**（ClipStretcher と同じ但し書き: ワーカースレッドから
// 呼ぶ。オーディオコールバックからは呼ばない）。
//
// アルゴリズムは de Cheveigné & Kawahara (2002) の YIN をそのまま:
//  1. 差分関数 d(τ) = Σ_{n<W} (x[n] − x[n+τ])²（FFT の自己相関で計算）
//  2. 累積平均正規化 d'(τ)（τ=0 で 1・周期で谷）
//  3. 閾値 0.15 を最初に下回った谷 → 放物線補間で小数 τ
//  4. 谷の深さ d'min を非周期性とし、有声度 = 1 − d'min。0.6 未満は無声
// 定数と挙動は tools/pitchlab/yin.py（Python 原型）と一致させる。lab の結果は
// docs/labs/pitch-correction.md（合成素材 GPE 0%・P 0.992/R 0.994。実声で pYIN/CREPE と音程 1% 台で一致）。
// pYIN の HMM 後処理は入れていない（同 lab の「留保」参照: オクターブ飛びが目立つ素材が出たら YIN の上に足す）
namespace PitchAnalyzer
{
inline const char* const algoId = "yin-1"; // 定数を変えたら上げる（サイドカーの世代が分かれる）

inline constexpr int frameLength = 2048;        // 48kHz で 42ms（60Hz の周期 800 サンプルが窓に収まる）
inline constexpr double fMin = 60.0;            // ラップの低い語尾
inline constexpr double fMax = 1000.0;          // サビの高音まで
inline constexpr double threshold = 0.15;       // 絶対閾値法の閾値（YIN 論文の推奨 0.1〜0.15）
inline constexpr double voicingMin = 0.6;       // 有声度の下限（lab で掃引。0.8 だと実声の recall 0.63）
inline constexpr double rmsGate = 1e-3;         // これ未満のフレームは解析せず無声（-60dBFS）

// ステレオは Mid（平均）で解析する。shouldCancel が true を返した時点で空のカーブ（0 フレーム）を返す。
// onProgress は 0..1（ワーカースレッドから呼ばれる。UI は atomic に写すだけにする）
PitchCurve analyze (const juce::AudioBuffer<float>& source, double sampleRate,
                    const std::function<bool()>& shouldCancel = {},
                    const std::function<void (float)>& onProgress = {});

// 1フレームぶんの累積平均正規化差分関数（テストで総当たり計算と照合するために公開）。
// frame は窓掛け済み frameLength サンプル。戻り値の長さは tauMax + 1
std::vector<double> cumulativeMeanNormalizedDifference (const float* frame, int length, int tauMax);
} // namespace PitchAnalyzer
