#include "VocalResynth.h"

#include <cmath>
#include <limits>
#include <vector>

#include <world/cheaptrick.h>
#include <world/d4c.h>
#include <world/synthesis.h>

#include "PitchAnalyzer.h"
#include "../shared/Log.h"
#include "../shared/RenderedDomain.h"

#include <mutex>

namespace
{
constexpr int marginFrames = 8; // 解析範囲を domain の前後 40ms 広げる（端のフレームの窓が欠けないように）

struct Spectrogram
{
    std::vector<double> data;
    std::vector<double*> rows;
    Spectrogram (size_t frames, size_t bins) : data (frames * bins, 0.0), rows (frames)
    {
        for (size_t i = 0; i < frames; ++i)
            rows[i] = data.data() + i * bins;
    }
};

// 分解（CheapTrick/D4C）の結果キャッシュ。分解は原音・解析カーブ・範囲にしか依存せず目標音に依存しないので、
// ブロブを動かすたびに全体を分解し直す必要がない（再レンダーの大半は分解だった）。
// 1 エントリ（直前のクリップ）だけ保持。原音は weak_ptr で持ち、解放されたら無効
struct AnalysisCache
{
    std::weak_ptr<const juce::AudioBuffer<float>> source;
    const void* sourcePtr = nullptr;
    ContentDigest curveDigest;
    int ka = 0, kb = 0, fftSize = 0, channels = 0;
    double sampleRate = 0.0;
    std::vector<std::shared_ptr<Spectrogram>> sp, ap; // チャンネルごと
};
std::mutex analysisCacheMutex;
AnalysisCache analysisCache;
} // namespace

juce::int64 VocalResynth::maxAnalysisBytes() { return ClipStretchLimits::maxRenderBytes; }

std::unique_ptr<juce::AudioBuffer<float>> VocalResynth::render (const RenderRecipe& recipe)
{
    using namespace ClipStretchLimits;
    const auto& src = recipe.sourceAudio;
    const auto& curve = recipe.curve;
    if (src == nullptr || curve == nullptr)
        return nullptr;
    const auto sourceLength = (juce::int64) src->getNumSamples();
    const int channels = juce::jmin (2, src->getNumChannels());
    if (channels < 1 || sourceLength <= 0)
        return nullptr;
    if (recipe.domainOffset < 0 || recipe.domainLength < 1 || recipe.domainLength > sourceLength
        || recipe.domainOffset > sourceLength - recipe.domainLength)
        return nullptr;
    if (! std::isfinite (recipe.stretchRatio) || recipe.stretchRatio < minRatio || recipe.stretchRatio > maxRatio)
        return nullptr;
    if (recipe.transposeSemitones < -maxSemitones || recipe.transposeSemitones > maxSemitones)
        return nullptr;
    if (curve->hopSamples <= 0 || curve->numFrames() == 0
        || ! juce::exactlyEqual (curve->sampleRate, recipe.sampleRate)
        || curve->source.frames != sourceLength)
        return nullptr;
    if (! recipe.correction.validate (recipe.domainOffset, recipe.domainLength))
        return nullptr;

    const double sr = recipe.sampleRate;
    const int fs = (int) std::llround (sr);
    const int hop = curve->hopSamples;
    const double framePeriodMs = hop * 1000.0 / sr;

    const auto timeMap = recipe.timeMap();
    const auto outLength64 = timeMap.outputLength();
    if (outLength64 < 1 || outLength64 > (juce::int64) (std::numeric_limits<int>::max() / 2)
        || outLength64 * channels * (juce::int64) sizeof (float) > maxRenderBytes)
        return nullptr;
    const int outLength = (int) outLength64;

    const auto target = recipe.target();
    const int k0 = target.firstFrame;
    const int k1 = k0 + (int) target.shiftSemitones.size();
    if (k1 <= k0)
        return nullptr;

    // 解析範囲（フレーム・サンプル）
    const int ka = juce::jmax (0, k0 - marginFrames);
    const int kb = juce::jmin (curve->numFrames(), k1 + marginFrames);
    const int analysisFrames = kb - ka;
    const juce::int64 a0 = (juce::int64) ka * hop;
    const juce::int64 a1 = juce::jmin (sourceLength, (juce::int64) kb * hop + hop);
    const int analysisLength = (int) (a1 - a0);
    if (analysisFrames <= 0 || analysisLength <= 0)
        return nullptr;

    CheapTrickOption ctOption;
    InitializeCheapTrickOption (fs, &ctOption);
    ctOption.f0_floor = PitchAnalyzer::fMin;
    ctOption.fft_size = GetFFTSizeForCheapTrick (fs, &ctOption);
    const int fftSize = ctOption.fft_size;
    const size_t bins = (size_t) fftSize / 2 + 1;
    const auto analysisBytes = (juce::int64) analysisFrames * (juce::int64) bins * 2 * (juce::int64) sizeof (double);
    if (analysisBytes > maxAnalysisBytes())
        return nullptr;
    D4COption d4cOption;
    InitializeD4COption (&d4cOption);

    std::vector<double> temporal ((size_t) analysisFrames), f0In ((size_t) analysisFrames);
    for (int i = 0; i < analysisFrames; ++i)
    {
        temporal[(size_t) i] = ((double) (ka + i) * hop - (double) a0) / sr;
        f0In[(size_t) i] = curve->f0[(size_t) (ka + i)];
    }

    // 出力フレーム列: 出力位置 → 入力位置（timeMap の逆）→ 最近傍の解析フレーム
    const int outFrames = outLength / hop + 2;
    std::vector<int> srcFrame ((size_t) outFrames);
    std::vector<double> f0Out ((size_t) outFrames);
    for (int j = 0; j < outFrames; ++j)
    {
        const auto outPos = (juce::int64) j * hop;
        const auto srcPos = timeMap.inverse (juce::jmin (outPos, outLength64));
        const int k = juce::jlimit (ka, kb - 1, (int) ((srcPos + hop / 2) / hop));
        srcFrame[(size_t) j] = k - ka;
        const float shift = target.shiftSemitones[(size_t) juce::jlimit (0, k1 - k0 - 1, k - k0)];
        const double f0 = curve->f0[(size_t) k];
        f0Out[(size_t) j] = f0 > 0.0 ? f0 * std::pow (2.0, (double) shift / 12.0) : 0.0;
    }

    // 分解（キャッシュが効けば飛ばす）
    const auto tStart = juce::Time::getMillisecondCounterHiRes();
    std::vector<std::shared_ptr<Spectrogram>> sp, ap;
    bool cacheHit = false;
    {
        std::lock_guard<std::mutex> lock (analysisCacheMutex);
        auto& c = analysisCache;
        if (c.sourcePtr == src.get() && ! c.source.expired() && c.curveDigest == curve->digest() && c.ka == ka && c.kb == kb
            && c.fftSize == fftSize && c.channels == channels && juce::exactlyEqual (c.sampleRate, sr))
        {
            sp = c.sp; ap = c.ap; cacheHit = true;
        }
    }
    if (! cacheHit)
    {
        std::vector<double> x ((size_t) analysisLength);
        for (int ch = 0; ch < channels; ++ch)
        {
            auto s = std::make_shared<Spectrogram> ((size_t) analysisFrames, bins);
            auto a = std::make_shared<Spectrogram> ((size_t) analysisFrames, bins);
            const float* in = src->getReadPointer (ch);
            for (int i = 0; i < analysisLength; ++i)
                x[(size_t) i] = in[a0 + i];
            CheapTrick (x.data(), analysisLength, fs, temporal.data(), f0In.data(), analysisFrames, &ctOption, s->rows.data());
            D4C (x.data(), analysisLength, fs, temporal.data(), f0In.data(), analysisFrames, fftSize, &d4cOption, a->rows.data());
            sp.push_back (std::move (s)); ap.push_back (std::move (a));
        }
        std::lock_guard<std::mutex> lock (analysisCacheMutex);
        analysisCache = { src, src.get(), curve->digest(), ka, kb, fftSize, channels, sr, sp, ap };
    }
    const auto tAnalysed = juce::Time::getMillisecondCounterHiRes();

    // 合成（目標音が変わるたびに走るのはここだけ）
    auto result = std::make_unique<juce::AudioBuffer<float>> (channels, outLength);
    std::vector<double> y ((size_t) outLength);
    std::vector<const double*> spOut ((size_t) outFrames), apOut ((size_t) outFrames);
    for (int ch = 0; ch < channels; ++ch)
    {
        for (int j = 0; j < outFrames; ++j)
        {
            spOut[(size_t) j] = sp[(size_t) ch]->rows[(size_t) srcFrame[(size_t) j]];
            apOut[(size_t) j] = ap[(size_t) ch]->rows[(size_t) srcFrame[(size_t) j]];
        }
        std::fill (y.begin(), y.end(), 0.0);
        Synthesis (f0Out.data(), outFrames, spOut.data(), apOut.data(), fftSize, framePeriodMs, fs, outLength, y.data());
        float* out = result->getWritePointer (ch);
        for (int i = 0; i < outLength; ++i)
        {
            const double v = y[(size_t) i];
            out[i] = std::isfinite (v) ? (float) v : 0.0f;
        }
    }
    Log::info ("pitch.render", "frames=" + juce::String (analysisFrames) + " cache=" + juce::String ((int) cacheHit)
                                   + " analyse_ms=" + juce::String (tAnalysed - tStart, 0)
                                   + " synth_ms=" + juce::String (juce::Time::getMillisecondCounterHiRes() - tAnalysed, 0));
    return result;
}
