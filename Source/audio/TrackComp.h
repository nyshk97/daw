#pragma once

#include <limits>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../shared/CompParams.h"

// トラックCompのDSP本体（透明コンプ・2ch）。
// plan: docs/plans/2026-08-15-1650-track-comp.md
//
// 定式化は Giannoulis, Massberg & Reiss (JAES 2012): feedforward・dBドメイン検波・
// ゲイン計算後のGRをdB単位で平滑化。平滑化器は smooth branching（attack/release時定数の
// 分岐型ワンポール。論文の第一推奨は smooth decoupled だが、branching は時定数が
// 設定値どおりになる＝滑らかさより予測可能性を優先する選択。切替点で不連続になるのは
// GRの勾配であってGR自体は連続）。
// 時定数の定義: α = exp(-1/(τ·fs))・ステップ入力でτ後に最終GRの63.2%へ到達。
//
// ステレオリンク: L/R別々に検波HPF（ONのとき）→ 大きい方（max）で検波 →
// 両chに同量のゲインを適用（別処理だと大きい側だけ下がり定位が動く）。
//
// 置き場所と寿命はTrackEqと同じ:
// - リアルタイム再生用は TrackParams::rtComp（オーディオスレッド専用の実行状態）
// - バウンスは BounceRenderer::TrackRender が独立インスタンスを持つ（開始時に snapTo）
//
// RT安全性: 全状態は固定サイズ（ヒープ確保なし）。パラメータ平滑化は全てサンプル単位の
// getNextValue()（skip方式はブロックサイズで掛かり方が変わる）。
// ON/OFF切替は Comp段全体の dry/wet クロスフェード。検波HPFのトグルは検波信号だけの
// 不連続で、GR目標の跳びはattack/release平滑化を通るためクロスフェード不要
class TrackComp
{
public:
    // 高速パス（既存経路の完全素通し）でよいか。コンプには「保証された中立設定」が
    // 無いため（floatは0dBFSを超えうる）、判定は compEnabled のみ。
    // false のあいだは呼び出し側が active経路で process() を呼び続けること
    //（OFFへのクロスフェード完了までは active に留まる）
    bool needsActivePath (bool compEnabled) const
    {
        return compEnabled || ! settled;
    }

    // インプレース処理（left は必須・right は nullptr でモノ）。
    // serial / timelineJumped の意味は TrackEq::process と同じ（EQと共通のFX連番を渡す）。
    // 再進入（バイパス→active）では平滑化GRと検波HPF履歴をリセットしてから
    // dryフェードインする＝バイパス中に凍結した古いGRからwet信号が復帰する事故を防ぐ
    void process (float* left, float* right, int numSamples, double sampleRate,
                  juce::uint64 serial, bool timelineJumped,
                  bool compEnabled, const Comp::Values& targets);

    // 平滑を挟まず即座に目標状態にする（バウンスの開始前に1回呼ぶ。以降 process の serial は
    // 1 から連番で渡す）。RT側は使わない（再進入時は dry からのフェードインで繋ぐ）
    void snapTo (double sampleRate, bool compEnabled, const Comp::Values& targets);

    // ---- UI向け（直近の process 1回分。バウンス側インスタンスでは読まない）----
    float blockMaxGainReductionDb() const { return blockMaxGr; }   // 正の減衰量dB
    float blockMaxDetectorPeak() const { return blockMaxDetector; } // 検波入力ピーク（線形振幅）

    // 現在の平滑化GR（正のdB）。テスト用
    float currentGainReductionDb() const { return grEnvDb; }

private:
    // 検波HPF用 Direct Form II transposed（TrackEqと同じ形。主信号には掛けない）
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
    void snapAllToTargets (bool compEnabled, const Comp::Values& targets);
    void resetDynamicsState(); // 平滑化GR＋検波HPF履歴
    void updateHpfCoefficients();
    void updateAlphas (const Comp::Values& targets);

    juce::SmoothedValue<float> thresholdDb, ratio, makeupDb;
    juce::SmoothedValue<float> chainMix; // 0..1（dry→wet。ON/OFFクロスフェード）

    float grEnvDb = 0.0f; // 平滑化済みGR（正のdB）
    Biquad hpf;
    float alphaAttack = 0.0f, alphaRelease = 0.0f;
    float lastAttackMs = -1.0f, lastReleaseMs = -1.0f; // α再計算判定（-1 = 未計算）
    bool lastHpfOn = false;

    float blockMaxGr = 0.0f, blockMaxDetector = 0.0f;

    double preparedRate = 0.0;
    // 番兵: どんな serial が最初に来ても「連続」と誤判定しない値（+1 が実現不可能な領域）
    juce::uint64 lastSerial = std::numeric_limits<juce::uint64>::max() - 1;
    bool settled = true; // 「出力がdry相当＆平滑進行なし」＝高速パスへ移ってよい状態か
};
