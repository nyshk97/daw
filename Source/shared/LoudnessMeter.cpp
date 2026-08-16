#include "LoudnessMeter.h"

#include <cmath>

#include "TruePeakDetector.h"

namespace Loudness
{
void kWeightingCoefficients (double sampleRate, Biquad& shelf, Biquad& highpass)
{
    // 段1: 高域シェルフ（頭部の音響効果の近似 = 高域が大きく聴こえる分を持ち上げて測る）。
    // BS.1770 は48kHzの係数表しか持たないため、libebur128 と同じ解析パラメータ
    // （f0/G/Q）からRBJ型で任意SRへ再設計する（48kHzで規格表と一致する）
    {
        const double f0 = 1681.974450955533;
        const double gainDb = 3.999843853973347;
        const double q = 0.7071752369554196;
        const double k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const double vh = std::pow (10.0, gainDb / 20.0);
        const double vb = std::pow (vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf.setCoefficients ({ (float) ((vh + vb * k / q + k * k) / a0),
                                 (float) (2.0 * (k * k - vh) / a0),
                                 (float) ((vh - vb * k / q + k * k) / a0),
                                 1.0f,
                                 (float) (2.0 * (k * k - 1.0) / a0),
                                 (float) ((1.0 - k / q + k * k) / a0) });
    }

    // 段2: RLBハイパス（超低域は音量感に寄与しないので測定から外す）
    {
        const double f0 = 38.13547087602444;
        const double q = 0.5003270373238773;
        const double k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const double a0 = 1.0 + k / q + k * k;
        highpass.setCoefficients ({ 1.0f, -2.0f, 1.0f,
                                    1.0f,
                                    (float) (2.0 * (k * k - 1.0) / a0),
                                    (float) ((1.0 - k / q + k * k) / a0) });
    }
}

std::optional<double> computeIntegratedLufs (const std::vector<float>& subPowers)
{
    const int count = (int) subPowers.size();
    const int numBlocks = count - subBlocksPerGatingBlock + 1; // 400ms窓を100msずつスライド
    if (numBlocks <= 0)
        return std::nullopt;

    // 各400msブロックのパワー（線形エネルギーの平均。dBで平均してはいけない）
    std::vector<double> blockPowers ((size_t) numBlocks);
    for (int j = 0; j < numBlocks; ++j)
    {
        double sum = 0.0;
        for (int k = 0; k < subBlocksPerGatingBlock; ++k)
            sum += subPowers[(size_t) (j + k)];
        blockPowers[(size_t) j] = sum / subBlocksPerGatingBlock;
    }

    // 絶対ゲート: -70 LUFS 以下のブロック（≒無音）を除外して仮平均を作る
    double sumAbs = 0.0;
    int numAbs = 0;
    for (const double power : blockPowers)
        if (energyToLufs (power) > absoluteGateLufs)
        {
            sumAbs += power;
            ++numAbs;
        }
    if (numAbs == 0)
        return std::nullopt;

    // 相対ゲート: 仮平均-10LUより静かなブロックを除外して本平均（静かな余白で薄まるのを防ぐ）
    const double relativeThreshold = energyToLufs (sumAbs / numAbs) + relativeGateLu;
    double sumRel = 0.0;
    int numRel = 0;
    for (const double power : blockPowers)
    {
        const double lufs = energyToLufs (power);
        if (lufs > absoluteGateLufs && lufs > relativeThreshold)
        {
            sumRel += power;
            ++numRel;
        }
    }
    if (numRel == 0)
        return std::nullopt;
    return energyToLufs (sumRel / numRel);
}

void MasterMeterAggregator::reset()
{
    haveGeneration = false;
    overflowed = false;
    haveTaint = false;
    subPowers.clear();
    recent.clear();
    maxTruePeak = 0.0f;
    current = {};
}

void MasterMeterAggregator::resetSession (juce::uint32 newGeneration)
{
    generation = newGeneration;
    haveGeneration = true;
    subPowers.clear();
    recent.clear();
    maxTruePeak = 0.0f;
}

void MasterMeterAggregator::consume (MasterMeterRing& ring)
{
    bool changed = false;
    MasterMeterBlock block;
    while (ring.pop (block))
    {
        changed = true;

        if (! haveGeneration || block.generation != generation)
            resetSession (block.generation); // 再生開始（世代切替）で仕切り直す

        // integrated は完全な公称長のサブブロックのみ（末尾の部分ブロックは規格どおり除外）
        const bool complete = block.numSamples == block.nominalSamples && block.numSamples > 0;
        if (complete)
            subPowers.push_back ((block.kwSumSqL + block.kwSumSqR) / (float) block.numSamples);

        recent.push_back ({ (double) block.kwSumSqL + block.kwSumSqR,
                            (double) block.rawSumLL, (double) block.rawSumRR,
                            (double) block.rawSumLR, block.numSamples, complete });
        if ((int) recent.size() > shortTermSubBlocks + 1)
            recent.erase (recent.begin(), recent.begin() + ((int) recent.size() - shortTermSubBlocks - 1));

        maxTruePeak = juce::jmax (maxTruePeak, block.maxTruePeak);
    }

    // あふれ（書き手が新規エントリを破棄した）: 計測を無効にする。
    // 汚染世代は「最後に破棄された世代」（世代は単調増加なので最大値）で、
    // **それ以前の世代はすべて無効**（リング満杯中に複数世代がまとめて破棄されると、
    // リングに残った古い世代だけを読んで「有効」に見えてしまうため ==比較では足りない）。
    // 汚染世代より後の世代（次の再生セッション）が始まれば回復する
    juce::uint32 droppedGen = 0;
    if (ring.takeDropped (droppedGen))
    {
        taintedGeneration = droppedGen;
        haveTaint = true;
        changed = true;
    }
    const bool overflowedNow = haveTaint && haveGeneration && generation <= taintedGeneration;
    if (overflowedNow != overflowed)
    {
        overflowed = overflowedNow;
        changed = true;
    }

    if (changed)
        rebuildFeed();
}

void MasterMeterAggregator::rebuildFeed()
{
    MasterMeterFeed feed;
    feed.measurementValid = ! overflowed;

    // short-term: 直近の**完全な100msブロック30個**（=3秒窓）のエネルギー合算 → LUFS。
    // 3秒窓が揃うまでは出さない（揃う前に出すと実質Momentaryに近い短窓の値を誤読させる）。
    // 停止時の部分ブロックは窓に**入れない**（入れると判定と計算範囲がずれて
    // 「完全29個＋部分1個」の3秒未満窓で表示してしまう。部分は相関/TPにだけ使う）
    {
        double sumSq = 0.0;
        juce::int64 samples = 0;
        int used = 0;
        for (int i = (int) recent.size() - 1; i >= 0 && used < shortTermSubBlocks; --i)
        {
            if (! recent[(size_t) i].complete)
                continue;
            sumSq += recent[(size_t) i].kwSumSq;
            samples += recent[(size_t) i].numSamples;
            ++used;
        }
        if (samples > 0 && used >= shortTermSubBlocks)
        {
            feed.shortTermLufs = (float) energyToLufs (sumSq / (double) samples);
            feed.hasShortTerm = true;
        }
    }

    if (const auto integrated = computeIntegratedLufs (subPowers))
    {
        feed.integratedLufs = (float) *integrated;
        feed.hasIntegrated = true;
    }

    // 相関: 直近約300ms。無音（平均二乗が-100dB未満）は表示しない
    {
        double ll = 0.0, rr = 0.0, lr = 0.0;
        juce::int64 samples = 0;
        const int start = juce::jmax (0, (int) recent.size() - correlationSubBlocks);
        for (int i = start; i < (int) recent.size(); ++i)
        {
            ll += recent[(size_t) i].rawLL;
            rr += recent[(size_t) i].rawRR;
            lr += recent[(size_t) i].rawLR;
            samples += recent[(size_t) i].numSamples;
        }
        if (samples > 0 && (ll + rr) / (double) samples > 1.0e-10)
        {
            const double denominator = std::sqrt (ll * rr);
            feed.correlation = denominator > 0.0
                                   ? (float) juce::jlimit (-1.0, 1.0, lr / denominator)
                                   : 0.0f;
            feed.hasCorrelation = true;
        }
    }

    if (maxTruePeak > 1.0e-5f)
    {
        feed.maxTruePeakDb = juce::Decibels::gainToDecibels (maxTruePeak);
        feed.hasTruePeak = true;
    }

    current = feed;
}

bool measureFile (const juce::File& file, double& integratedLufsOut, double& truePeakDbOut)
{
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return false;

    const int numChannels = juce::jmin (2, (int) reader->numChannels);
    const double sr = reader->sampleRate;
    const int subBlockLen = (int) std::lround (subBlockSeconds * sr);

    Biquad shelf, highpass;
    kWeightingCoefficients (sr, shelf, highpass);
    TruePeakDetector truePeak;
    truePeak.reset();

    std::vector<float> subPowers;
    double blockSumSq = 0.0;
    int blockCount = 0;
    float maxTp = 0.0f;

    constexpr int readBlock = 8192;
    juce::AudioBuffer<float> buffer (numChannels, readBlock);
    for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += readBlock)
    {
        const int n = (int) juce::jmin ((juce::int64) readBlock, reader->lengthInSamples - pos);
        if (! reader->read (&buffer, 0, n, pos, true, true))
            return false;

        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float x = buffer.getSample (ch, i);
                const float weighted = highpass.processSample (ch, shelf.processSample (ch, x));
                blockSumSq += (double) weighted * weighted;
                maxTp = juce::jmax (maxTp, truePeak.processSample (ch, x));
            }
            if (++blockCount == subBlockLen)
            {
                subPowers.push_back ((float) (blockSumSq / subBlockLen));
                blockSumSq = 0.0;
                blockCount = 0;
            }
        }
    }
    for (int ch = 0; ch < numChannels; ++ch)
        maxTp = juce::jmax (maxTp, truePeak.flush (ch));

    const auto integrated = computeIntegratedLufs (subPowers);
    integratedLufsOut = integrated.value_or (-120.0);
    truePeakDbOut = juce::Decibels::gainToDecibels ((double) maxTp, -120.0);
    return true;
}
} // namespace Loudness
