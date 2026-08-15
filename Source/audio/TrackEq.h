#pragma once

#include <limits>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../shared/EqParams.h"

// トラックEQのDSP本体（biquad×4バンド×2ch・RBJ cookbook準拠）。
// plan: docs/plans/2026-08-15-1506-track-eq.md
//
// 置き場所と寿命:
// - リアルタイム再生用は TrackParams::rtEq（オーディオスレッド専用の実行状態。
//   スナップショット再構築を跨いで biquad 履歴を持続させるため TrackParams に住む）
// - バウンスは BounceRenderer::TrackRender が独立インスタンスを持つ（開始時に snapTo）。
//   再生スレッドとバウンススレッドが同じIIR履歴を同時更新する競合を構造的に排除する
//
// RT安全性: 全状態は固定サイズ（ヒープ確保なし）。係数計算は juce::dsp::IIR::ArrayCoefficients
// （確保なしで std::array を返す。Coefficients::make*() はコールバック内で new するため使わない）
//
// クリック対策は2階層:
// - パラメータ変更: SmoothedValue（実処理セグメント長で skip し、戻り値から係数を再計算）
// - ON/OFF切替: eqEnabled はチェーン全体の dry/wet クロスフェード、HPの enabled は
//   HPバンド位置だけの入力/HP出力クロスフェード（1本の wet/dry だと、ベル+6dB中に
//   HPだけOFFしたときフェード中にベルまで一時的に外れる）
class TrackEq
{
public:
    // 高速パス（既存経路の完全素通し）でよいか。false のあいだは呼び出し側が
    // active経路で process() を呼び続けること（バイパスへ戻るクロスフェード・平滑化の
    // 完了までは active に留まる、という状態遷移をここが担う）
    bool needsActivePath (bool eqEnabled, const Eq::Values& targets, bool analyzerTap) const
    {
        if (analyzerTap)
            return true;
        if (eqEnabled && ! Eq::isNeutral (targets))
            return true;
        return ! settled;
    }

    // インプレース処理（left は必須・right は nullptr でモノ）。
    // serial: 呼び出し側のセグメント連番（毎セグメント+1）。飛んでいたら「処理していない期間があった」
    //         ＝再進入とみなし、履歴をリセットして dry からフェードインする（バイパス出力＝dryと連続）
    // timelineJumped: シーク・サイクルラップ等の時間不連続。履歴リセット＋全平滑値を目標へスナップ
    //         （音自体が不連続なのでフェードは不要。古い履歴が別位置の音に混ざるのを防ぐ）
    void process (float* left, float* right, int numSamples, double sampleRate,
                  juce::uint64 serial, bool timelineJumped,
                  bool eqEnabled, const Eq::Values& targets);

    // 平滑を挟まず即座に目標状態にする（バウンスの開始前に1回呼ぶ。以降 process の serial は
    // 1 から連番で渡す）。RT側は使わない（再進入時は dry からのフェードインで繋ぐ）
    void snapTo (double sampleRate, bool eqEnabled, const Eq::Values& targets);

private:
    // Direct Form II transposed。係数は a0 正規化済み
    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float s1[2] {}, s2[2] {};

        float processSample (int ch, float x) noexcept
        {
            const float y = b0 * x + s1[ch];
            s1[ch] = b1 * x - a1 * y + s2[ch];
            s2[ch] = b2 * x - a2 * y;
            return y;
        }

        void resetState() noexcept { s1[0] = s1[1] = s2[0] = s2[1] = 0.0f; }
    };

    void resetSmootherRates (double sampleRate);
    void snapAllToTargets (bool eqEnabled, const Eq::Values& targets);
    void resetFilterStates();
    void updateBandCoefficients (int band, float freqHz, float gainDbValue, float qValue);

    juce::SmoothedValue<float> freq[Eq::numBands], gainDb[Eq::numBands], q[Eq::numBands];
    juce::SmoothedValue<float> chainMix, hpMix; // 0..1（dry→wet / HP入力→HP出力）
    Biquad filters[Eq::numBands];

    // 係数の再計算判定用（平滑値が動いた帯域だけ計算し直す）。-1 = 未計算
    float lastFreq[Eq::numBands] { -1.0f, -1.0f, -1.0f, -1.0f };
    float lastGain[Eq::numBands] { -1.0f, -1.0f, -1.0f, -1.0f };
    float lastQ[Eq::numBands] { -1.0f, -1.0f, -1.0f, -1.0f };

    double preparedRate = 0.0;
    // 番兵: どんな serial が最初に来ても「連続」と誤判定しない値（+1 が実現不可能な領域）
    juce::uint64 lastSerial = std::numeric_limits<juce::uint64>::max() - 1;
    bool settled = true; // 「出力がdry相当＆平滑進行なし」＝高速パスへ移ってよい状態か
};
