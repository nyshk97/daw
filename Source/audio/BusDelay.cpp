#include "BusDelay.h"

#include <cmath>

namespace
{
constexpr double feedbackRampSeconds = 0.05; // Feedbackドラッグの平滑（急変のジッパー回避）
constexpr double toneRampSeconds = 0.05;
constexpr double wetRampSeconds = 0.01;      // Ping-pong切替の出力ランプ
constexpr double xfadeSeconds = 0.05;        // ディレイ長変更のクロスフェード
} // namespace

void BusDelay::prepare (double sampleRate)
{
    preparedRate = sampleRate;
    lineL.prepare (sampleRate, Delay::maxDelaySeconds);
    lineR.prepare (sampleRate, Delay::maxDelaySeconds);
    feedback.reset (sampleRate, feedbackRampSeconds);
    toneKnob.reset (sampleRate, toneRampSeconds);
    wetGain.reset (sampleRate, wetRampSeconds);
    xfadeTotal = juce::jmax (1, (int) std::lround (xfadeSeconds * sampleRate));
    snapTo (Delay::defaults, 120.0);
}

int BusDelay::delaySamplesFor (double bpm, int timeIndex) const
{
    const auto samples = (int) std::lround (Delay::timeSeconds (bpm, timeIndex) * preparedRate);
    // capacityは契約（4秒×SR）どおりなら常に足りる。SR未prepareの縮退だけ1へ潰す
    return juce::jlimit (1, juce::jmax (1, lineL.capacity() - 1), samples);
}

void BusDelay::snapTo (const Delay::Values& targets, double bpm)
{
    lineL.clear();
    lineR.clear();
    lpL = lpR = 0.0f;
    lastCutoffHz = -1.0f;
    feedback.setCurrentAndTargetValue (targets.feedback);
    toneKnob.setCurrentAndTargetValue (targets.tone);
    wetGain.setCurrentAndTargetValue (1.0f);
    currentDelay = oldDelay = delaySamplesFor (bpm, targets.timeIndex);
    xfadeLeft = 0;
    activePingPong = targets.pingPong;
}

void BusDelay::process (float* left, float* right, int numSamples, double bpm,
                        const Delay::Values& targets)
{
    juce::ScopedNoDenormals noDenormals;
    if (preparedRate <= 0.0 || numSamples <= 0)
        return;

    // ---- ブロック頭の状態遷移 ----
    feedback.setTargetValue (targets.feedback);
    toneKnob.setTargetValue (targets.tone);

    // ディレイ長（Time・BPM）の変更 → クロスフェード開始（遷移中の再変更は完了後に拾う）
    const int desired = delaySamplesFor (bpm, targets.timeIndex);
    if (desired != currentDelay && xfadeLeft <= 0)
    {
        oldDelay = currentDelay;
        currentDelay = desired;
        xfadeLeft = xfadeTotal;
    }

    // Ping-pong切替: 出力を落としきってからトポロジを入れ替え、リングも組み直す。
    // クリアするのは、書き込み式が変わった点の内容不連続が**Dサンプル後**（＝出力ランプが
    // 戻った後）に段差として現れるため。エコーは切替時に一度消えて新モードで積み直される。
    // クリア（最大4秒×SRのfill）は切替時のみ＝ブロック予算（10ms超）に対して十分軽い
    if (targets.pingPong != activePingPong)
    {
        wetGain.setTargetValue (0.0f);
        if (! wetGain.isSmoothing() && wetGain.getCurrentValue() <= 0.0f)
        {
            activePingPong = targets.pingPong;
            lineL.clear();
            lineR.clear();
            lpL = lpR = 0.0f;
            wetGain.setTargetValue (1.0f);
        }
    }
    else
    {
        wetGain.setTargetValue (1.0f);
    }

    // Tone: 係数はブロック頭更新（Lo-fi Toneと同じ契約）
    const float cutoff = Delay::toneCutoffHz (toneKnob.skip (numSamples));
    if (cutoff != lastCutoffHz)
    {
        const double limited = juce::jmin ((double) cutoff, preparedRate * 0.49);
        lpCoeff = (float) (1.0
                           - std::exp (-juce::MathConstants<double>::twoPi * limited
                                       / preparedRate));
        lastCutoffHz = cutoff;
    }

    // ---- サンプルループ ----
    for (int i = 0; i < numSamples; ++i)
    {
        const float fb = feedback.getNextValue();
        const float wet = wetGain.getNextValue();

        // ディレイ長のクロスフェード読み（旧→新。x: 1→0）
        float rL, rR;
        if (xfadeLeft > 0)
        {
            const float x = (float) xfadeLeft / (float) xfadeTotal;
            rL = x * lineL.read (oldDelay) + (1.0f - x) * lineL.read (currentDelay);
            rR = x * lineR.read (oldDelay) + (1.0f - x) * lineR.read (currentDelay);
            --xfadeLeft;
        }
        else
        {
            rL = lineL.read (currentDelay);
            rR = lineR.read (currentDelay);
        }

        // ループ内ローパス（書き戻す前に掛ける＝繰り返すたびに暗くなる）
        lpL += lpCoeff * (rL - lpL);
        lpR += lpCoeff * (rR - lpR);

        if (activePingPong)
        {
            // 入力モノ化→Lへ。LのタップがRのラインへ渡り、RのタップがLへ戻る（交互）。
            // fbは受け渡しごとに掛ける＝1タップごとの減衰がストレートと同じ fb になる
            const float mono = 0.5f * (left[i] + right[i]);
            lineL.write (mono + fb * lpR);
            lineR.write (fb * lpL);
        }
        else
        {
            lineL.write (left[i] + fb * lpL);
            lineR.write (right[i] + fb * lpR);
        }

        left[i] = lpL * wet;
        right[i] = lpR * wet;
    }

    // denormal掃除（保存状態に残る微小値。ScopedNoDenormalsは処理中のみ）
    if (std::abs (lpL) < 1.0e-20f)
        lpL = 0.0f;
    if (std::abs (lpR) < 1.0e-20f)
        lpR = 0.0f;
}
