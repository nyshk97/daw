#include "BusReverb.h"

#include <cmath>
#include <juce_dsp/juce_dsp.h>

namespace
{
constexpr double xfadeSeconds = 0.05;
constexpr double maxPreDelayLineSeconds = 0.12; // 100ms＋マージン
constexpr double lowCutRampSeconds = 0.02;      // 周波数の平滑（TrackEqと同じ20ms）
} // namespace

void BusReverb::prepare (double sampleRate)
{
    preparedRate = sampleRate;
    // setSampleRateの**前**にfull wet/dry 0を設定する: juce::Reverbのコンストラクタ既定は
    // dry 0.4で、setParametersは平滑ターゲットを変えるだけ。setSampleRateが平滑値を
    // ターゲットへスナップするので、この順ならdryの混入（冒頭10msだけ原音が漏れる）が起きない
    applyParameters (Reverb::defaultsForBus (0));
    reverb.setSampleRate (sampleRate); // ヒープ確保を伴う（このためprepareはRT禁止）
    preL.prepare (sampleRate, maxPreDelayLineSeconds);
    preR.prepare (sampleRate, maxPreDelayLineSeconds);
    lowCutHz.reset (sampleRate, lowCutRampSeconds);
    xfadeTotal = juce::jmax (1, (int) std::lround (xfadeSeconds * sampleRate));
    snapTo (Reverb::defaultsForBus (0));
}

int BusReverb::preDelaySamplesFor (float preDelayMs) const
{
    const auto samples = (int) std::lround ((double) preDelayMs * 0.001 * preparedRate);
    return juce::jlimit (0, juce::jmax (1, preL.capacity() - 1), samples);
}

void BusReverb::applyParameters (const Reverb::Values& targets)
{
    juce::Reverb::Parameters parameters;
    parameters.roomSize = targets.size;
    parameters.damping = targets.damp;
    parameters.wetLevel = 1.0f; // full wet（dryはトラック本体から出ている）
    parameters.dryLevel = 0.0f;
    parameters.width = targets.width;
    parameters.freezeMode = 0.0f;
    reverb.setParameters (parameters); // 連続変化はjuce::Reverb内部のSmoothedValueが平滑する
}

void BusReverb::snapTo (const Reverb::Values& targets)
{
    applyParameters (targets);
    reverb.reset();
    preL.clear();
    preR.clear();
    lowCut.resetState();
    lowCutHz.setCurrentAndTargetValue (targets.lowCutHz);
    lastLowCutHz = -1.0f;
    currentPre = oldPre = preDelaySamplesFor (targets.preDelayMs);
    xfadeLeft = 0;
}

void BusReverb::process (float* left, float* right, int numSamples,
                         const Reverb::Values& targets)
{
    juce::ScopedNoDenormals noDenormals;
    if (preparedRate <= 0.0 || numSamples <= 0)
        return;

    // Low Cut: 周波数を20msで平滑してから係数をブロック頭更新（TrackEqと同じ流儀。
    // ドラッグの急変で係数が飛ぶとクリックになる）
    lowCutHz.setTargetValue (targets.lowCutHz);
    const float smoothedHz = lowCutHz.skip (numSamples);
    if (smoothedHz != lastLowCutHz)
    {
        const float f = juce::jlimit (Reverb::minLowCutHz, Reverb::maxLowCutHz, smoothedHz);
        lowCut.setCoefficients (
            juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (preparedRate, f, 0.70710678f));
        lastLowCutHz = smoothedHz;
    }

    // Pre-delay長の変更 → クロスフェード開始（遷移中の再変更は完了後に拾う）
    const int desired = preDelaySamplesFor (targets.preDelayMs);
    if (desired != currentPre && xfadeLeft <= 0)
    {
        oldPre = currentPre;
        currentPre = desired;
        xfadeLeft = xfadeTotal;
    }

    // Low Cut → Pre-delay をインプレースで通し、まとめて juce::Reverb へ。
    // タップの読みは書き込みの**前**（DelayLineの契約: この順で遅延がちょうどDサンプルになる。
    // 0タップは read(0) が未定義なので入力直結）
    for (int i = 0; i < numSamples; ++i)
    {
        const float inL = lowCut.processSample (0, left[i]);
        const float inR = lowCut.processSample (1, right[i]);

        const float newL = currentPre > 0 ? preL.read (currentPre) : inL;
        const float newR = currentPre > 0 ? preR.read (currentPre) : inR;
        if (xfadeLeft > 0)
        {
            const float x = (float) xfadeLeft / (float) xfadeTotal;
            const float oldTapL = oldPre > 0 ? preL.read (oldPre) : inL;
            const float oldTapR = oldPre > 0 ? preR.read (oldPre) : inR;
            left[i] = x * oldTapL + (1.0f - x) * newL;
            right[i] = x * oldTapR + (1.0f - x) * newR;
            --xfadeLeft;
        }
        else
        {
            left[i] = newL;
            right[i] = newR;
        }

        preL.write (inL);
        preR.write (inR);
    }

    applyParameters (targets);
    reverb.processStereo (left, right, numSamples);

    lowCut.snapStatesToZero();
}
