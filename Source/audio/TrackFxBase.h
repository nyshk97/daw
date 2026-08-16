#pragma once

#include <limits>
#include <juce_audio_basics/juce_audio_basics.h>

// トラックFX共通の状態機械（FxBase）。
// plan: docs/plans/2026-08-16-2058-fx-batch3-saturation-lofi.md Phase 1
//
// TrackEq / TrackComp（以降 TrackSaturator / TrackLofi も）が全く同じ手順で持っていた:
//   - serial連続性判定（番兵初期値・再進入検知）
//   - timelineJumped / SR変更 / 再進入の3分岐リセット
//   - ON/OFFの dry/wet クロスフェード（chainMix）
//   - settled（高速パスへ移ってよいかの判定材料）と snapTo（バウンス開始前の即時整定）
// を1箇所に集約する。DSP本体（検波・平滑・係数計算）はFXごとに本当に違うので共有しない。
//
// CRTP（仮想関数なし・オーディオパスでの間接呼び出しゼロ）。派生クラスは以下のフックを
// 実装し、TrackFxBase<派生> を friend にする。フックの呼び出しは全てブロック単位:
//   void fxResetSmootherRates (double sampleRate); // SmoothedValue の reset 群（chainMix以外）
//   void fxSnapToTargets (const TargetsT&);        // 平滑値を目標へスナップ（chainMix以外）
//   void fxResetHistory();                          // IIR履歴・エンベロープ等の消去
//   void fxSampleRateChanged();                     // fs依存の係数・時定数の再計算マーキング
//
// 派生側の責務（基底は触らない）:
//   - process 末尾での settled の更新（「高速パスへ移れる条件」はFXごとに違う）
//   - needsActivePath の定義（settled を材料に使う）
//   - snapTo での settled 初期化と、snapTo固有の追加リセット
template <typename Derived>
class TrackFxBase
{
protected:
    // ON/OFFクロスフェードの平滑化時間（boolの瞬時切替のクリック対策。EQ/Compで同値だった）
    static constexpr double chainMixRampSeconds = 0.01;

    // process 冒頭の共通手順。false なら引数不正＝呼び出し側は何もせず return する。
    // true のとき、SR変更・時間不連続・再進入の各リセットと chainMix の目標更新まで完了している
    template <typename TargetsT>
    bool beginBlock (const float* left, int numSamples, double sampleRate,
                     juce::uint64 serial, bool timelineJumped,
                     bool enabledTarget, const TargetsT& targets)
    {
        if (left == nullptr || numSamples <= 0 || sampleRate <= 0.0)
            return false;

        const bool firstCall = preparedRate <= 0.0;
        const bool srChanged = sampleRate != preparedRate;
        if (srChanged)
        {
            preparedRate = sampleRate;
            // reset は現在値を目標へスナップする（下の分岐でさらに整える）
            chainMix.reset (sampleRate, chainMixRampSeconds);
            derived().fxResetSmootherRates (sampleRate);
            derived().fxSampleRateChanged();
        }
        const bool reentry = serial != lastSerial + 1;
        lastSerial = serial;

        if (timelineJumped || (srChanged && ! firstCall))
        {
            // 時間不連続（シーク・SR変更）: 音自体が不連続なのでフェード不要。全て目標へスナップ
            derived().fxResetHistory();
            derived().fxSnapToTargets (targets);
            chainMix.setCurrentAndTargetValue (enabledTarget ? 1.0f : 0.0f);
        }
        else if (reentry || firstCall)
        {
            // 再進入（高速パス・ミュート・停止から復帰）: バイパス中の出力＝dryなので、
            // チェーンだけ dry(0) からフェードインして連続にする。内部履歴は凍結された
            // 古い値のままなので必ずリセットする（古い状態からwet信号が復帰する事故を防ぐ）
            derived().fxResetHistory();
            derived().fxSnapToTargets (targets);
            chainMix.setCurrentAndTargetValue (0.0f);
        }

        chainMix.setTargetValue (enabledTarget ? 1.0f : 0.0f);
        return true;
    }

    // snapTo の共通骨格（バウンス開始前の即時整定。以降 process の serial は 1 から連番で渡す）。
    // 派生はこの後に固有の再計算マーキングと settled の初期化を行う
    template <typename TargetsT>
    void snapToBase (double sampleRate, bool enabledTarget, const TargetsT& targets)
    {
        preparedRate = sampleRate;
        chainMix.reset (sampleRate, chainMixRampSeconds);
        derived().fxResetSmootherRates (sampleRate);
        derived().fxSnapToTargets (targets);
        chainMix.setCurrentAndTargetValue (enabledTarget ? 1.0f : 0.0f);
        derived().fxResetHistory();
        derived().fxSampleRateChanged();
        lastSerial = 0; // 最初のブロック（serial=1）を再進入扱いにしない
    }

    juce::SmoothedValue<float> chainMix; // 0..1（dry→wet。ON/OFFクロスフェード）

    double preparedRate = 0.0;
    bool settled = true; // 「出力がdry相当＆平滑進行なし」＝高速パスへ移ってよい状態か

private:
    Derived& derived() { return static_cast<Derived&> (*this); }

    // 番兵: どんな serial が最初に来ても「連続」と誤判定しない値（+1 が実現不可能な領域）
    juce::uint64 lastSerial = std::numeric_limits<juce::uint64>::max() - 1;
};
