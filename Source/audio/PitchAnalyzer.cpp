#include "PitchAnalyzer.h"

#include <cmath>
#include <juce_dsp/juce_dsp.h>

namespace
{
// 2048 フレーム・tauMax ≤ 1024 なら 4096 点で線形相関が巡回しない（frame + W ≤ 4096）
constexpr int fftOrder = 12;
constexpr int fftSize = 1 << fftOrder;

struct FftScratch
{
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> x, h;
    FftScratch() : x ((size_t) fftSize * 2, 0.0f), h ((size_t) fftSize * 2, 0.0f) {}
};

// x と head（先頭 W サンプルだけ残した x）の相互相関 r(τ) = Σ_{n<W} x[n]·x[n+τ] を FFT で求める
void correlate (FftScratch& s, const float* frame, int length, int w, int tauMax, std::vector<double>& corr)
{
    std::fill (s.x.begin(), s.x.end(), 0.0f);
    std::fill (s.h.begin(), s.h.end(), 0.0f);
    std::copy (frame, frame + length, s.x.begin());
    std::copy (frame, frame + w, s.h.begin());
    s.fft.performRealOnlyForwardTransform (s.x.data(), true);
    s.fft.performRealOnlyForwardTransform (s.h.data(), true);
    // conj(H) · X（interleaved complex）
    for (int k = 0; k <= fftSize / 2; ++k)
    {
        const float xr = s.x[(size_t) k * 2], xi = s.x[(size_t) k * 2 + 1];
        const float hr = s.h[(size_t) k * 2], hi = s.h[(size_t) k * 2 + 1];
        s.x[(size_t) k * 2] = hr * xr + hi * xi;
        s.x[(size_t) k * 2 + 1] = hr * xi - hi * xr;
    }
    s.fft.performRealOnlyInverseTransform (s.x.data());
    corr.resize ((size_t) tauMax + 1);
    for (int t = 0; t <= tauMax; ++t)
        corr[(size_t) t] = s.x[(size_t) t];
}

std::vector<double> cmndfImpl (FftScratch& s, const float* frame, int length, int tauMax)
{
    const int w = length - tauMax;
    std::vector<double> corr;
    correlate (s, frame, length, w, tauMax, corr);

    // 累積二乗和で E0 = Σ_{n<W} x²、Eτ = Σ_{n<W} x[n+τ]²
    std::vector<double> sq ((size_t) length + 1, 0.0);
    for (int n = 0; n < length; ++n)
        sq[(size_t) n + 1] = sq[(size_t) n] + (double) frame[n] * frame[n];
    const double e0 = sq[(size_t) w];

    std::vector<double> d ((size_t) tauMax + 1, 0.0), cm ((size_t) tauMax + 1, 1.0);
    for (int t = 1; t <= tauMax; ++t)
    {
        const double et = sq[(size_t) (t + w)] - sq[(size_t) t];
        d[(size_t) t] = juce::jmax (0.0, e0 + et - 2.0 * corr[(size_t) t]);
    }
    double run = 0.0;
    for (int t = 1; t <= tauMax; ++t)
    {
        run += d[(size_t) t];
        cm[(size_t) t] = run > 0.0 ? d[(size_t) t] * t / run : 1.0;
    }
    return cm;
}
} // namespace

std::vector<double> PitchAnalyzer::cumulativeMeanNormalizedDifference (const float* frame, int length, int tauMax)
{
    FftScratch s;
    return cmndfImpl (s, frame, length, tauMax);
}

PitchCurve PitchAnalyzer::analyze (const juce::AudioBuffer<float>& source, double sampleRate,
                                   const std::function<bool()>& shouldCancel,
                                   const std::function<void (float)>& onProgress)
{
    PitchCurve curve;
    curve.algoId = algoId;
    curve.sampleRate = sampleRate;
    curve.hopSamples = PitchCurve::hopSamplesFor (sampleRate);
    curve.source = SourceIdentity::of (source, sampleRate);
    const int numSamples = source.getNumSamples();
    const int channels = source.getNumChannels();
    if (numSamples <= 0 || channels <= 0 || curve.hopSamples <= 0)
        return curve;

    // Mid 化（ステレオは平均）
    std::vector<float> mono ((size_t) numSamples, 0.0f);
    for (int ch = 0; ch < channels; ++ch)
    {
        const float* p = source.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            mono[(size_t) i] += p[i] / (float) channels;
    }

    const int hop = curve.hopSamples;
    const int numFrames = (numSamples + hop - 1) / hop;
    const int tauMin = (int) (sampleRate / fMax);
    const int tauMax = juce::jmin ((int) (sampleRate / fMin), frameLength / 2);
    const int half = frameLength / 2;
    curve.f0.assign ((size_t) numFrames, 0.0f);
    curve.voicing.assign ((size_t) numFrames, 0.0f);
    curve.rms.assign ((size_t) numFrames, 0.0f);

    std::vector<float> window ((size_t) frameLength);
    for (int i = 0; i < frameLength; ++i)
        window[(size_t) i] = 0.5f - 0.5f * (float) std::cos (juce::MathConstants<double>::twoPi * i / (frameLength - 1));

    FftScratch scratch;
    std::vector<float> frame ((size_t) frameLength);
    auto sampleAt = [&] (juce::int64 i) { return i >= 0 && i < numSamples ? mono[(size_t) i] : 0.0f; };

    for (int k = 0; k < numFrames; ++k)
    {
        if (shouldCancel && (k % 64) == 0 && shouldCancel())
            return PitchCurve {};
        if (onProgress && (k % 200) == 0)
            onProgress ((float) k / (float) numFrames);

        const juce::int64 center = (juce::int64) k * hop;
        // RMS（中心 ±hop）
        double sum = 0.0;
        for (juce::int64 i = center - hop; i < center + hop; ++i)
        {
            const double v = sampleAt (i);
            sum += v * v;
        }
        const float rms = (float) std::sqrt (sum / (2.0 * hop));
        curve.rms[(size_t) k] = rms;
        if (rms < rmsGate)
            continue;

        for (int i = 0; i < frameLength; ++i)
            frame[(size_t) i] = sampleAt (center - half + i) * window[(size_t) i];
        const auto cm = cmndfImpl (scratch, frame.data(), frameLength, tauMax);

        // 絶対閾値法: 閾値を下回った最初の谷。無ければ全体の最小
        int tau = -1;
        for (int t = tauMin; t <= tauMax; ++t)
        {
            if (cm[(size_t) t] < threshold)
            {
                tau = t;
                while (tau + 1 <= tauMax && cm[(size_t) tau + 1] < cm[(size_t) tau])
                    ++tau;
                break;
            }
        }
        if (tau < 0)
        {
            tau = tauMin;
            for (int t = tauMin; t <= tauMax; ++t)
                if (cm[(size_t) t] < cm[(size_t) tau])
                    tau = t;
        }
        double tauF = tau;
        if (tau >= 1 && tau < tauMax)
        {
            const double a = cm[(size_t) tau - 1], b = cm[(size_t) tau], c = cm[(size_t) tau + 1];
            const double denom = a - 2.0 * b + c;
            if (denom != 0.0)
                tauF = tau + 0.5 * (a - c) / denom;
        }
        const double prob = 1.0 - juce::jlimit (0.0, 1.0, cm[(size_t) tau]);
        curve.voicing[(size_t) k] = (float) prob;
        if (prob >= voicingMin && tauF > 0.0)
            curve.f0[(size_t) k] = (float) (sampleRate / tauF);
    }

    // 孤立フレーム（前後が無声の1フレームだけ有声）はノイズ扱い
    for (int k = 1; k + 1 < numFrames; ++k)
        if (curve.f0[(size_t) k] > 0.0f && curve.f0[(size_t) k - 1] == 0.0f && curve.f0[(size_t) k + 1] == 0.0f)
            curve.f0[(size_t) k] = 0.0f;

    if (onProgress)
        onProgress (1.0f);
    return curve;
}
