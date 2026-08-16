#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "../shared/LofiParams.h"
#include "TrackFxBase.h"
#include "BiquadFilter.h"

// トラックLo-fiのDSP本体（劣化処理の束・2ch）。
// plan: docs/plans/2026-08-16-2058-fx-batch3-saturation-lofi.md Phase 5
// 状態機械（serial・リセット分岐・chainMix・snapTo骨格）は TrackFxBase に集約済み。
// ノブ→物理量のマッピングは LofiParams.h（UIと共有）。
//
// 内部の成分順（固定）: Wow → Crush → Tone → ノイズ加算
//（Tone が Crush の折り返しの効かせ具合も握る＝実機サンプラーの出力フィルタと同じ構図）
//
// - Wow: 変調ディレイ。d[n] = A_sm[n]·(1 + sin θ[n])（0〜2A を振れる・θ は 0.55Hz固定＋
//   ゆっくりした乱数ドリフト）。深さ変更はディレイ時間そのもの（A）のサンプル単位平滑＝
//   跳びはクリックでなく短いピッチベンドに化ける。L/Rは同一の揺れ（別々に揺らすと
//   広がり系の別エフェクトになる）。補間は d≥2 で4点エルミート（整数中心）・d<2 は線形
//   （深さ→0 の連続収束用。ディレイ0＝恒等で素通しと不連続を作らない）
// - Crush: S&H レート低減（アンチエイリアスなし＝折り返しがキャラクター）＋振幅量子化。
//   L/Rは同一ホールドクロック。レート・ビットはブロック頭更新・S&H位相はレート変更でも連続
// - Tone: biquad LPF（Q=0.707固定）。ノブ0は処理スキップ（開放）
// - Noise: ヒス＋クラックルのモノ合成を両chへ加算。入力エンベロープ追従（無音では鳴らない）。
//   乱数は xorshift（整数演算＝決定的。rand()禁止）
//
// 置き場所と寿命はTrackEqと同じ（RT = TrackParams::rtLofi・バウンス = TrackRender が独立所有）。
// RT安全性: 全状態固定サイズ（リングは2048×2ch事前確保）・確保なし
class TrackLofi : public TrackFxBase<TrackLofi>
{
public:
    bool needsActivePath (bool lofiEnabled, const Lofi::Values& targets) const
    {
        if (lofiEnabled && ! Lofi::isNeutral (targets))
            return true;
        return ! settled;
    }

    // インプレース処理（left は必須・right は nullptr でモノ）。
    // serial / timelineJumped の意味は TrackEq::process と同じ（共通のFX連番を渡す）
    void process (float* left, float* right, int numSamples, double sampleRate,
                  juce::uint64 serial, bool timelineJumped,
                  bool lofiEnabled, const Lofi::Values& targets);

    // 平滑を挟まず即座に目標状態にする（バウンスの開始前に1回呼ぶ。以降 process の serial は
    // 1 から連番で渡す）。リセット契約: LFO位相=0・乱数seed=固定値・S&H位相=0・
    // ディレイ履歴/エンベロープ=クリア → 同一入力なら出力が決定的（ハッシュゲート前提）
    void snapTo (double sampleRate, bool lofiEnabled, const Lofi::Values& targets);

private:
    friend class TrackFxBase<TrackLofi>;

    // FxBaseフック
    void fxResetSmootherRates (double sampleRate);
    void fxSnapToTargets (const Lofi::Values& targets);
    void fxResetHistory();
    void fxSampleRateChanged();

    void updateToneCoefficients (float cutoffHz);

    // ---- Wow ----
    // 192kHzの最大ディレイ 2A≈8.68ms（≈1667サンプル）＋補間マージンが収まる2の冪
    static constexpr int ringCapacity = 2048;
    static constexpr int ringMask = ringCapacity - 1;
    float ring[2][ringCapacity] {};
    int ringWrite = 0;
    double lfoPhase = 0.0;   // rad
    double lfoInc = 0.0;     // rad/sample（fs依存）
    float driftPhase = 0.0f, driftTarget = 0.0f; // 不規則成分（位相オフセットrad）
    float driftCoeff = 0.0f; // ドリフトのワンポール係数（fs依存）
    int driftCountdown = 0;  // 次のドリフト目標更新までのサンプル数
    juce::uint32 driftRng = 0;
    juce::SmoothedValue<float> wowAmp; // A（サンプル数）

    // ---- Crush ----
    juce::SmoothedValue<float> crushKnob;
    double crushPhase = 1.0; // 1以上で次のサンプルを取り込む（初回に必ずホールド）
    float heldSample[2] {};

    // ---- Tone ----
    juce::SmoothedValue<float> toneKnob;
    // Tone成分のdry/LPFクロスフェード（0..1）: 開放側の20kHz LPFは恒等ではないため、
    // ノブ0との境界を即時切替にすると（他成分の動作中に）高域の段差・クリックになる。
    // toneMix=0 に収束した時点で処理スキップ＝素通しとビット一致で連続
    juce::SmoothedValue<float> toneMix;
    Biquad toneFilter;
    float lastToneCutoff = -1.0f; // 係数再計算判定（-1 = 未計算）
    bool toneWasActive = false;   // スキップ→処理の遷移で履歴をリセットする

    // ---- Noise ----
    juce::SmoothedValue<float> noiseGain;
    juce::uint32 noiseRng = 0;
    float noiseLp = 0.0f;      // ノイズ整形ワンポールLPFの状態
    float noiseLpCoeff = 0.0f; // fs依存
    float envValue = 0.0f;     // 入力エンベロープ（線形振幅）
    float envAttack = 0.0f, envRelease = 0.0f; // fs依存
};
