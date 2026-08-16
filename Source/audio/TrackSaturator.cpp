#include "TrackSaturator.h"

#include <cmath>

namespace
{
constexpr double paramRampSeconds = 0.02; // Drive/Mix/補償ゲインの平滑（ジッパーノイズ対策）
constexpr float dcBlockerHz = 5.0f;       // DC除去の一次ブロッカ（可聴域に触らない低さ）
constexpr float adaaEpsilon = 1.0e-4f;    // ADAA差分の中点評価フォールバック閾値
} // namespace

void TrackSaturator::fxResetSmootherRates (double sampleRate)
{
    drive.reset (sampleRate, paramRampSeconds);
    mix.reset (sampleRate, paramRampSeconds);
    compGain.reset (sampleRate, paramRampSeconds);
    neutralFade.reset (sampleRate, paramRampSeconds);
}

void TrackSaturator::fxSnapToTargets (const Sat::Values& targets)
{
    drive.setCurrentAndTargetValue (targets.drive);
    mix.setCurrentAndTargetValue (targets.mix);
    compGain.setCurrentAndTargetValue (Sat::compensationGain (targets.drive));
    neutralFade.setCurrentAndTargetValue (targets.drive > 0.0f ? 1.0f : 0.0f);
    lastCompDrive = targets.drive;
}

void TrackSaturator::fxResetHistory()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        prevInput[ch] = 0.0f;
        dcPrevIn[ch] = 0.0f;
        dcPrevOut[ch] = 0.0f;
    }
}

void TrackSaturator::fxSampleRateChanged()
{
    dcCoeff = 1.0f - (float) (juce::MathConstants<double>::twoPi * dcBlockerHz / preparedRate);
}

void TrackSaturator::snapTo (double sampleRate, bool satEnabled, const Sat::Values& targets)
{
    snapToBase (sampleRate, satEnabled, targets);
    settled = ! (satEnabled && ! Sat::isNeutral (targets));
}

void TrackSaturator::process (float* left, float* right, int numSamples, double sampleRate,
                              juce::uint64 serial, bool timelineJumped,
                              bool satEnabled, const Sat::Values& targets)
{
    juce::ScopedNoDenormals noDenormals;

    if (! beginBlock (left, numSamples, sampleRate, serial, timelineJumped, satEnabled, targets))
        return;

    // 補償ゲインの目標更新（driveのターゲットが変わったブロック頭でのみ64点求積を回す）
    if (targets.drive != lastCompDrive)
    {
        compGain.setTargetValue (Sat::compensationGain (targets.drive));
        lastCompDrive = targets.drive;
    }

    // 目標値の更新（同値なら SmoothedValue 側で no-op。chainMix は beginBlock が更新済み）
    drive.setTargetValue (targets.drive);
    mix.setTargetValue (targets.mix);
    neutralFade.setTargetValue (targets.drive > 0.0f ? 1.0f : 0.0f);

    const int numChannels = right != nullptr ? 2 : 1;
    const float dcR = dcCoeff;

    for (int i = 0; i < numSamples; ++i)
    {
        // パラメータはサンプル単位で進める（EQ/Compと同じ流儀。カーブの前計算は
        // tanh 1回なのでサンプル単位でも軽い）
        const float driveNow = drive.getNextValue();
        const float mixNow = mix.getNextValue();
        const float compNow = compGain.getNextValue();
        const float fade = neutralFade.getNextValue();
        const float chain = chainMix.getNextValue();
        const auto curve = Sat::Curve::fromDriveGain (Sat::driveGain (driveNow));

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* buffer = ch == 0 ? left : right;
            const float dry = buffer[i];

            // 1次ADAA（平滑中は F・f とも現在サンプルの曲線で評価）。
            // 線形域（g<ε）は恒等にする: 線形カーブへのADAAは中点平均（半サンプルLPF）に
            // なってしまい、高速パスの完全素通しとの間に高域差が残るため
            const float x0 = prevInput[ch];
            const double dx = (double) dry - (double) x0;
            float wet;
            if (curve.isLinear())
                wet = dry;
            else if (std::abs (dx) < adaaEpsilon)
                wet = Sat::transfer (curve, 0.5f * (dry + x0));
            else
                wet = (float) ((Sat::transferAD (curve, dry) - Sat::transferAD (curve, x0)) / dx);
            prevInput[ch] = dry;

            // DCブロッカ: y[n] = x[n] − x[n−1] + R·y[n−1]（非対称カーブのDCを除去）。
            // 線形域（drive→0の平滑中）でも掛けて経路を切り替えない（連続性優先。
            // 完全素通しの保証は呼び出し側の高速パス＝isNeutral判定が担う）
            const float dcOut = wet - dcPrevIn[ch] + dcR * dcPrevOut[ch];
            dcPrevIn[ch] = wet;
            dcPrevOut[ch] = dcOut;

            // 中立フェード込みのdry/wet: fade=0（Drive目標0で平滑完了）のとき出力は
            // dryとビット一致＝高速パスへの切替が不連続にならない
            const float mixed = dry + mixNow * fade * (compNow * dcOut - dry);
            buffer[i] = dry + chain * (mixed - dry);
        }
    }

    // denormal掃除（DCブロッカ履歴は無音入力で0へ漸近する）
    for (int ch = 0; ch < 2; ++ch)
    {
        if (std::abs (dcPrevOut[ch]) < 1.0e-20f)
            dcPrevOut[ch] = 0.0f;
        if (std::abs (dcPrevIn[ch]) < 1.0e-20f)
            dcPrevIn[ch] = 0.0f;
    }

    // 高速パスへ移れる条件: 目標が中立/OFF相当 かつ 平滑（中立フェード含む）が全て完了
    const bool smoothing = chainMix.isSmoothing() || drive.isSmoothing() || mix.isSmoothing()
                           || compGain.isSmoothing() || neutralFade.isSmoothing();
    settled = ! (satEnabled && ! Sat::isNeutral (targets)) && ! smoothing;
}
