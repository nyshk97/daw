#include "SpectrumAnalyzer.h"

#include <cmath>

SpectrumAnalyzer::SpectrumAnalyzer()
{
    window.resize (fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi
                                                      * (float) i / (float) (fftSize - 1)));
    ringL.resize (fftSize, 0.0f);
    ringR.resize (fftSize, 0.0f);
    popL.resize (AnalyzerTap::blockSamples);
    popR.resize (AnalyzerTap::blockSamples);
    workL.resize ((size_t) fftSize * 2);
    workR.resize ((size_t) fftSize * 2);
    displayDb.fill (floorDb);
}

void SpectrumAnalyzer::reset()
{
    std::fill (ringL.begin(), ringL.end(), 0.0f);
    std::fill (ringR.begin(), ringR.end(), 0.0f);
    writePos = 0;
    filled = 0;
    samplesSinceFrame = 0;
    displayDb.fill (floorDb);
}

float SpectrumAnalyzer::binFrequency (int bin)
{
    return 20.0f * std::pow (1000.0f, (float) bin / (float) (numBins - 1)); // 20 × 1000^t = 20〜20k
}

bool SpectrumAnalyzer::update (AnalyzerTap& tap, double sampleRate)
{
    const auto generation = tap.generation();
    const auto trackId = tap.targetId();
    if (generation != lastGeneration)
    {
        reset(); // 切替直後: 旧トラックの蓄積を混ぜない
        lastGeneration = generation;
    }

    AnalyzerTap::Header header;
    bool producedFrame = false;
    while (tap.popBlock (header, popL.data(), popR.data()))
    {
        // (trackId, 世代) の両方が現在値と一致するブロックだけ採用（キューに残った旧データを弾く）
        if (header.generation != generation || header.trackId != trackId)
            continue;

        for (int i = 0; i < AnalyzerTap::blockSamples; ++i)
        {
            ringL[(size_t) writePos] = popL[(size_t) i];
            ringR[(size_t) writePos] = popR[(size_t) i];
            writePos = (writePos + 1) % fftSize;
        }
        filled = juce::jmin (fftSize, filled + AnalyzerTap::blockSamples);
        samplesSinceFrame += AnalyzerTap::blockSamples;

        if (filled >= fftSize && samplesSinceFrame >= hopSize && sampleRate > 0.0)
        {
            computeFrame (sampleRate);
            samplesSinceFrame = 0;
            producedFrame = true;
        }
    }
    return producedFrame;
}

void SpectrumAnalyzer::computeFrame (double sampleRate)
{
    // リングを時系列順に展開して窓掛け（後半は performFrequencyOnlyForwardTransform の作業領域）
    std::fill (workL.begin(), workL.end(), 0.0f);
    std::fill (workR.begin(), workR.end(), 0.0f);
    for (int i = 0; i < fftSize; ++i)
    {
        const int src = (writePos + i) % fftSize; // writePos = 最古のサンプル位置
        workL[(size_t) i] = ringL[(size_t) src] * window[(size_t) i];
        workR[(size_t) i] = ringR[(size_t) src] * window[(size_t) i];
    }
    fft.performFrequencyOnlyForwardTransform (workL.data()); // 先頭 fftSize/2 に |X| が入る
    fft.performFrequencyOnlyForwardTransform (workR.data());

    // |X| → 振幅（片側スペクトル×窓補正）: 2/(N×0.5) = 4/N
    const float amplitudeScale = 4.0f / (float) fftSize;
    const double binHz = sampleRate / fftSize;

    for (int bin = 0; bin < numBins; ++bin)
    {
        // 表示ビンの境界（隣とは幾何平均で接する）に含まれるFFTビンのパワー最大値を採る。
        // 低域はFFT分解能の方が粗いので、境界内にFFTビンが無ければ中心周波数で線形補間する
        const float f0 = bin > 0 ? std::sqrt (binFrequency (bin - 1) * binFrequency (bin))
                                 : binFrequency (0);
        const float f1 = bin < numBins - 1 ? std::sqrt (binFrequency (bin) * binFrequency (bin + 1))
                                           : binFrequency (numBins - 1);
        const int k0 = (int) std::ceil ((double) f0 / binHz);
        const int k1 = juce::jmin (fftSize / 2 - 1, (int) std::floor ((double) f1 / binHz));

        double power = 0.0;
        if (k0 <= k1)
        {
            for (int k = juce::jmax (1, k0); k <= k1; ++k)
            {
                const double ampL = (double) workL[(size_t) k] * amplitudeScale;
                const double ampR = (double) workR[(size_t) k] * amplitudeScale;
                power = juce::jmax (power, (ampL * ampL + ampR * ampR) * 0.5); // L/Rパワー平均
            }
        }
        else
        {
            const double exactBin = juce::jlimit (1.0, (double) (fftSize / 2 - 2),
                                                  (double) binFrequency (bin) / binHz);
            const int k = (int) exactBin;
            const double t = exactBin - k;
            const double ampL = ((1.0 - t) * workL[(size_t) k] + t * workL[(size_t) (k + 1)])
                                * amplitudeScale;
            const double ampR = ((1.0 - t) * workR[(size_t) k] + t * workR[(size_t) (k + 1)])
                                * amplitudeScale;
            power = (ampL * ampL + ampR * ampR) * 0.5;
        }

        const double db = power > 0.0 ? 10.0 * std::log10 (power) : (double) floorDb;
        displayDb[(size_t) bin] = juce::jlimit (floorDb, 0.0f, (float) db);
    }
}
