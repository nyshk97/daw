#include "VocalNoteAudition.h"

#include <cmath>

#include <world/cheaptrick.h>
#include <world/d4c.h>
#include <world/synthesis.h>

#include "PitchAnalyzer.h"

struct VocalNoteAudition::Analysis
{
    int fs = 0, fftSize = 0, hop = 0;
    int ka = 0, kb = 0;           // 解析フレーム範囲（カーブの絶対フレーム）
    juce::int64 a0 = 0;           // 解析バッファ先頭の原音サンプル
    std::vector<double> f0In, temporal;
    std::vector<double> spData, apData;
    std::vector<double*> sp, ap;
};

VocalNoteAudition::VocalNoteAudition() = default;
VocalNoteAudition::~VocalNoteAudition() = default;

void VocalNoteAudition::reset()
{
    analysis.reset();
    prepared = false;
}

bool VocalNoteAudition::prepare (const juce::AudioBuffer<float>& source, const PitchCurve& curve, double sampleRate,
                                 juce::int64 startSample, juce::int64 endSample, double marginMs)
{
    reset();
    const auto total = (juce::int64) source.getNumSamples();
    if (source.getNumChannels() < 1 || curve.hopSamples <= 0 || curve.numFrames() == 0 || endSample <= startSample
        || startSample < 0 || endSample > total || ! juce::exactlyEqual (curve.sampleRate, sampleRate))
        return false;
    auto a = std::make_unique<Analysis>();
    a->fs = (int) std::llround (sampleRate);
    a->hop = curve.hopSamples;
    const int margin = (int) std::llround (marginMs / 1000.0 * sampleRate / a->hop);
    a->ka = juce::jmax (0, (int) (startSample / a->hop) - margin);
    a->kb = juce::jmin (curve.numFrames(), (int) ((endSample + a->hop - 1) / a->hop) + margin);
    const int frames = a->kb - a->ka;
    if (frames <= 0)
        return false;
    a->a0 = (juce::int64) a->ka * a->hop;
    const juce::int64 a1 = juce::jmin (total, (juce::int64) a->kb * a->hop + a->hop);
    const int length = (int) (a1 - a->a0);
    if (length <= 0)
        return false;

    CheapTrickOption ct;
    InitializeCheapTrickOption (a->fs, &ct);
    ct.f0_floor = PitchAnalyzer::fMin;
    ct.fft_size = GetFFTSizeForCheapTrick (a->fs, &ct);
    a->fftSize = ct.fft_size;
    D4COption d4c;
    InitializeD4COption (&d4c);
    const size_t bins = (size_t) a->fftSize / 2 + 1;

    a->f0In.resize ((size_t) frames);
    a->temporal.resize ((size_t) frames);
    for (int i = 0; i < frames; ++i)
    {
        a->temporal[(size_t) i] = ((double) (a->ka + i) * a->hop - (double) a->a0) / sampleRate;
        a->f0In[(size_t) i] = curve.f0[(size_t) (a->ka + i)];
    }
    a->spData.assign ((size_t) frames * bins, 0.0);
    a->apData.assign ((size_t) frames * bins, 0.0);
    a->sp.resize ((size_t) frames);
    a->ap.resize ((size_t) frames);
    for (size_t i = 0; i < (size_t) frames; ++i)
    {
        a->sp[i] = a->spData.data() + i * bins;
        a->ap[i] = a->apData.data() + i * bins;
    }
    // Mid 化して分解
    std::vector<double> x ((size_t) length, 0.0);
    const int channels = juce::jmin (2, source.getNumChannels());
    for (int ch = 0; ch < channels; ++ch)
    {
        const float* in = source.getReadPointer (ch);
        for (int i = 0; i < length; ++i)
            x[(size_t) i] += in[a->a0 + i] / (double) channels;
    }
    CheapTrick (x.data(), length, a->fs, a->temporal.data(), a->f0In.data(), frames, &ct, a->sp.data());
    D4C (x.data(), length, a->fs, a->temporal.data(), a->f0In.data(), frames, a->fftSize, &d4c, a->ap.data());

    analysis = std::move (a);
    prepared = true;
    noteStart = startSample;
    noteEnd = endSample;
    sr = sampleRate;
    return true;
}

std::shared_ptr<const juce::AudioBuffer<float>> VocalNoteAudition::render (const PitchCorrection& correction, const PitchCurve& curve,
                                                                           juce::int64 domainOffset, juce::int64 domainLength,
                                                                           int transposeSemitones)
{
    if (! prepared || analysis == nullptr)
        return nullptr;
    const auto& a = *analysis;
    const auto target = PitchCorrections::targetCurve (correction, curve, domainOffset, domainLength, transposeSemitones);
    auto shiftAt = [&] (int k)
    {
        const int i = k - target.firstFrame;
        return i >= 0 && i < (int) target.shiftSemitones.size() ? target.shiftSemitones[(size_t) i] : (float) transposeSemitones;
    };
    // ノート範囲 [noteStart, noteEnd) だけ合成（時間写像なし）
    const int outLength = (int) (noteEnd - noteStart);
    const int outFrames = outLength / a.hop + 2;
    std::vector<double> f0Out ((size_t) outFrames);
    std::vector<const double*> spOut ((size_t) outFrames), apOut ((size_t) outFrames);
    for (int j = 0; j < outFrames; ++j)
    {
        const auto srcPos = noteStart + (juce::int64) j * a.hop;
        const int k = juce::jlimit (a.ka, a.kb - 1, (int) ((srcPos + a.hop / 2) / a.hop));
        const double f0 = curve.f0[(size_t) k];
        f0Out[(size_t) j] = f0 > 0.0 ? f0 * std::pow (2.0, (double) shiftAt (k) / 12.0) : 0.0;
        spOut[(size_t) j] = a.sp[(size_t) (k - a.ka)];
        apOut[(size_t) j] = a.ap[(size_t) (k - a.ka)];
    }
    std::vector<double> y ((size_t) outLength, 0.0);
    Synthesis (f0Out.data(), outFrames, spOut.data(), apOut.data(), a.fftSize, a.hop * 1000.0 / sr, a.fs, outLength, y.data());
    auto out = std::make_shared<juce::AudioBuffer<float>> (1, outLength);
    float* dst = out->getWritePointer (0);
    // 端のクリック防止に 5ms のフェード
    const int fade = juce::jmin (outLength / 2, (int) (sr * 0.005));
    for (int i = 0; i < outLength; ++i)
    {
        float gain = 1.0f;
        if (i < fade) gain = (float) i / (float) fade;
        else if (i >= outLength - fade) gain = (float) (outLength - 1 - i) / (float) fade;
        const double v = y[(size_t) i];
        dst[i] = std::isfinite (v) ? (float) v * gain : 0.0f;
    }
    return out;
}
