#include "TrackLofi.h"

#include <cmath>
#include <juce_dsp/juce_dsp.h>

namespace
{
constexpr double paramRampSeconds = 0.05; // Wow深さ等の平滑（跳びを短いピッチベンドに変える長さ）
// 不規則成分は控えめに（位相オフセット±0.05rad・時定数1s）。ドリフトの位相速度 φ' が
// ピッチ偏差に (1 + φ'/ω) 倍で乗るため、この値で寄与≈1.4%＝「わずかな不規則」に留まり、
// Wow偏差の実測±10%テストとも干渉しない
constexpr float driftMaxRad = 0.05f;
constexpr double driftUpdateSeconds = 2.0; // ドリフト目標の更新間隔
constexpr double driftSmoothSeconds = 1.0; // ドリフトのワンポール時定数
constexpr float noiseLpHz = 7000.0f;      // ノイズ整形（ヒスの角を取る）
constexpr double cracklePerSecond = 25.0; // クラックルの平均発生数/秒
constexpr float crackleAmp = 4.0f;        // クラックルのヒス比の強さ

// 乱数seed（リセット契約: fxResetHistory で必ずこの値へ戻す＝出力が決定的）
constexpr juce::uint32 driftSeed = 0x9e3779b9;
constexpr juce::uint32 noiseSeed = 0x1234abcd;

inline juce::uint32 xorshift (juce::uint32& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// [-1, 1) の一様乱数（整数演算のみ＝Debug/Releaseで決定的）
inline float uniform (juce::uint32& state)
{
    return (float) (juce::int32) xorshift (state) * (1.0f / 2147483648.0f);
}
} // namespace

void TrackLofi::fxResetSmootherRates (double sampleRate)
{
    wowAmp.reset (sampleRate, paramRampSeconds);
    crushKnob.reset (sampleRate, paramRampSeconds);
    toneKnob.reset (sampleRate, paramRampSeconds);
    toneMix.reset (sampleRate, paramRampSeconds);
    noiseGain.reset (sampleRate, paramRampSeconds);
}

void TrackLofi::fxSnapToTargets (const Lofi::Values& targets)
{
    wowAmp.setCurrentAndTargetValue (
        Lofi::wowDelayAmpSeconds (Lofi::wowDepthRatio (targets.wow)) * (float) preparedRate);
    crushKnob.setCurrentAndTargetValue (targets.crush);
    toneKnob.setCurrentAndTargetValue (targets.tone);
    toneMix.setCurrentAndTargetValue (targets.tone > 0.0f ? 1.0f : 0.0f);
    noiseGain.setCurrentAndTargetValue (targets.noise * targets.noise * Lofi::noiseMaxGain);
}

void TrackLofi::fxResetHistory()
{
    for (auto& channel : ring)
        std::fill (std::begin (channel), std::end (channel), 0.0f);
    ringWrite = 0;
    lfoPhase = 0.0;
    driftPhase = driftTarget = 0.0f;
    driftCountdown = 0;
    driftRng = driftSeed;
    crushPhase = 1.0;
    heldSample[0] = heldSample[1] = 0.0f;
    toneFilter.resetState();
    toneWasActive = false;
    noiseRng = noiseSeed;
    noiseLp = 0.0f;
    envValue = 0.0f;
}

void TrackLofi::fxSampleRateChanged()
{
    lfoInc = juce::MathConstants<double>::twoPi * Lofi::wowRateHz / preparedRate;
    driftCoeff = (float) (1.0 - std::exp (-1.0 / (driftSmoothSeconds * preparedRate)));
    noiseLpCoeff = (float) (1.0
                            - std::exp (-juce::MathConstants<double>::twoPi * noiseLpHz
                                        / preparedRate));
    envAttack = (float) std::exp (-1.0 / (Lofi::noiseEnvAttackMs * 0.001 * preparedRate));
    envRelease = (float) std::exp (-1.0 / (Lofi::noiseEnvReleaseMs * 0.001 * preparedRate));
    lastToneCutoff = -1.0f; // 係数はfsに依存する
}

void TrackLofi::updateToneCoefficients (float cutoffHz)
{
    // ArrayCoefficients は確保なし（Coefficients::make*() は内部で new するため使わない）
    const float f = juce::jmin (cutoffHz, (float) (preparedRate * 0.49));
    toneFilter.setCoefficients (
        juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass (preparedRate, f, 0.70710678f));
}

void TrackLofi::snapTo (double sampleRate, bool lofiEnabled, const Lofi::Values& targets)
{
    snapToBase (sampleRate, lofiEnabled, targets);
    settled = ! (lofiEnabled && ! Lofi::isNeutral (targets));
}

void TrackLofi::process (float* left, float* right, int numSamples, double sampleRate,
                         juce::uint64 serial, bool timelineJumped,
                         bool lofiEnabled, const Lofi::Values& targets)
{
    juce::ScopedNoDenormals noDenormals;

    if (! beginBlock (left, numSamples, sampleRate, serial, timelineJumped, lofiEnabled, targets))
        return;

    // 目標値の更新（同値なら SmoothedValue 側で no-op。chainMix は beginBlock が更新済み）
    wowAmp.setTargetValue (
        Lofi::wowDelayAmpSeconds (Lofi::wowDepthRatio (targets.wow)) * (float) preparedRate);
    crushKnob.setTargetValue (targets.crush);
    toneKnob.setTargetValue (targets.tone);
    toneMix.setTargetValue (targets.tone > 0.0f ? 1.0f : 0.0f);
    noiseGain.setTargetValue (targets.noise * targets.noise * Lofi::noiseMaxGain);

    // Crush・Tone はブロック頭更新（契約: レート比/ビット深度/カットオフの係数階段はブロック単位）
    const float crushNow = crushKnob.skip (numSamples);
    const bool crushActive = crushNow > 0.0f || targets.crush > 0.0f;
    const float crushBits = Lofi::crushBits (crushNow);
    const double crushRatio = (double) Lofi::crushRateRatio (crushNow);

    const float toneNow = toneKnob.skip (numSamples);
    // LPFはtoneMixが0に収束しきるまで回す（クロスフェードの受け皿。0で完全スキップ＝素通し）
    const bool toneActive = targets.tone > 0.0f || toneMix.getCurrentValue() > 0.0f
                            || toneMix.isSmoothing();
    if (toneActive)
    {
        if (! toneWasActive)
            toneFilter.resetState(); // スキップ中の凍結履歴（古い音）を復活させない
        const float cutoff = Lofi::toneCutoffHz (toneNow);
        if (cutoff != lastToneCutoff)
        {
            updateToneCoefficients (cutoff);
            lastToneCutoff = cutoff;
        }
    }
    toneWasActive = toneActive;

    const int numChannels = right != nullptr ? 2 : 1;
    const int driftInterval = juce::jmax (1, (int) (driftUpdateSeconds * preparedRate));
    // リング容量を超えない安全クランプ（>192kHzデバイスでは深さ側が制限される）
    const float maxAmp = (float) (ringCapacity - 8) * 0.5f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float amp = juce::jmin (wowAmp.getNextValue(), maxAmp);
        const float noiseAmp = noiseGain.getNextValue();
        const float toneAmount = toneMix.getNextValue();
        const float chain = chainMix.getNextValue();

        // ---- 入力エンベロープ（ノイズ追従用。劣化前のdryから取る）----
        const float inL = left[i];
        const float inR = right != nullptr ? right[i] : inL;
        const float level = juce::jmax (std::abs (inL), std::abs (inR));
        const float envAlpha = level > envValue ? envAttack : envRelease;
        envValue = envAlpha * envValue + (1.0f - envAlpha) * level;

        // ---- Wow: LFO＋ドリフト → ディレイ量（L/R共通）----
        if (--driftCountdown <= 0)
        {
            driftCountdown = driftInterval;
            driftTarget = uniform (driftRng) * driftMaxRad;
        }
        driftPhase += driftCoeff * (driftTarget - driftPhase);
        const float delay = amp > 0.0f
                                ? amp * (1.0f + (float) std::sin (lfoPhase + (double) driftPhase))
                                : 0.0f;
        lfoPhase += lfoInc;
        if (lfoPhase > juce::MathConstants<double>::twoPi)
            lfoPhase -= juce::MathConstants<double>::twoPi;

        // ---- Crush のホールドクロック（L/R共通・位相はレート変更でも連続）----
        bool takeSample = false;
        if (crushActive)
        {
            crushPhase += crushRatio;
            if (crushPhase >= 1.0)
            {
                crushPhase -= std::floor (crushPhase);
                takeSample = true;
            }
        }

        // ---- ノイズ合成（モノ1系統・両chへ同じベッドを加算）----
        float noiseSample = 0.0f;
        if (noiseAmp > 0.0f || targets.noise > 0.0f)
        {
            float raw = uniform (noiseRng) * 0.5f; // ヒス
            // クラックル: まれなインパルス（ポアソン的発生）
            const float gate = (float) xorshift (noiseRng) * (1.0f / 4294967296.0f);
            if (gate < (float) (cracklePerSecond / preparedRate))
                raw += uniform (noiseRng) * crackleAmp;
            noiseLp += noiseLpCoeff * (raw - noiseLp);
            noiseSample = noiseLp * noiseAmp * juce::jmin (1.0f, envValue);
        }

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* buffer = ch == 0 ? left : right;
            const float dry = buffer[i];

            // Wow: 書き込みは常時（スキップ中もリングを温める＝再有効化で古い音を読まない）
            ring[ch][ringWrite] = dry;
            float wet;
            if (delay >= 2.0f)
            {
                // 4点エルミート（Catmull-Rom・整数中心）。d≥2 で参照4点が全て過去に収まる
                const int di = (int) delay;
                const float frac = delay - (float) di;
                const float ym1 = ring[ch][(ringWrite - di + 1) & ringMask];
                const float y0 = ring[ch][(ringWrite - di) & ringMask];
                const float y1 = ring[ch][(ringWrite - di - 1) & ringMask];
                const float y2 = ring[ch][(ringWrite - di - 2) & ringMask];
                // 注意: リングは新しいほどインデックスが大きい＝時間軸が逆なので、
                // 補間は「y0=遅延di・y1=遅延di+1」の時間順で組む
                const float c1 = 0.5f * (y1 - ym1);
                const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
                const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
                wet = ((c3 * frac + c2) * frac + c1) * frac + y0;
            }
            else if (delay > 0.0f)
            {
                // 深さ→0 の連続収束域は線形補間（ディレイ0で恒等＝素通しと不連続を作らない）
                const int di = (int) delay;
                const float frac = delay - (float) di;
                const float y0 = ring[ch][(ringWrite - di) & ringMask];
                const float y1 = ring[ch][(ringWrite - di - 1) & ringMask];
                wet = y0 + frac * (y1 - y0);
            }
            else
            {
                wet = dry;
            }

            // Crush: S&H（takeSample時に量子化して取り込む）
            if (crushActive)
            {
                if (takeSample)
                    heldSample[ch] = Lofi::quantize (wet, crushBits);
                wet = heldSample[ch];
            }

            // Tone: LPF（dry↔LPFのクロスフェード。開放20kHzでも恒等ではないため、
            // 0境界の出入りは toneAmount の平滑で繋ぐ）
            if (toneActive)
            {
                const float filtered = toneFilter.processSample (ch, wet);
                wet += toneAmount * (filtered - wet);
            }

            // Noise 加算 → full wet（Mixなし）を chainMix（ON/OFFクロスフェード）で合流
            wet += noiseSample;
            buffer[i] = dry + chain * (wet - dry);
        }

        ringWrite = (ringWrite + 1) & ringMask;
    }

    // denormal掃除
    toneFilter.snapStatesToZero();
    if (std::abs (noiseLp) < 1.0e-20f)
        noiseLp = 0.0f;
    if (envValue < 1.0e-9f)
        envValue = 0.0f;

    // 高速パスへ移れる条件: 目標が中立/OFF相当 かつ 平滑が全て完了
    const bool smoothing = chainMix.isSmoothing() || wowAmp.isSmoothing()
                           || crushKnob.isSmoothing() || toneKnob.isSmoothing()
                           || toneMix.isSmoothing() || noiseGain.isSmoothing();
    settled = ! (lofiEnabled && ! Lofi::isNeutral (targets)) && ! smoothing;
}
