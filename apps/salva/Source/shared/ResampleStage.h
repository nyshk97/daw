#pragma once

#include <cmath>
#include <cstring>

#include <juce_audio_basics/juce_audio_basics.h>

// 再生用リサンプル段（ステレオ固定）。PlaybackCallbackから切り出したテスト可能な純ロジック。
//
// - 補間は LagrangeInterpolator（ロック・確保なし）
// - **帯域制限**: ダウンサンプリング時は補間の前、アップサンプリング時は補間の後に
//   2次ローパス（バイリニアButterworth）を適用する。JUCE ResamplingAudioSource と同じ設計
//   （juce_ResamplingAudioSource.cpp の createLowPass/applyFilter を移植）。
//   4点Lagrangeだけでは96kHz→48kHz等で24kHz超の成分が可聴帯域へ折り返すため
// - 入力の消費端数は内部キャリーで持ち越して時間連続性を保つ
// - **シーク等で入力が不連続になったら reset() を呼ぶのは呼び出し側の契約**
//   （キャリー・補間履歴・フィルタ状態に旧位置のサンプルが残るため。
//    juce_Interpolators.h の reset() と同じ契約）
//
// スレッド: prepare はコールバック外（確保する）。process / reset はオーディオスレッド安全
class ResampleStage
{
public:
    // maxOutputSamples はコールバックの最大ブロック、maxRatioToSupport は比率上限
    void prepare (int maxOutputSamples, double maxRatioToSupport)
    {
        maxRatio = maxRatioToSupport;
        inputCapacity = (int) std::ceil (maxOutputSamples * maxRatioToSupport) + 64;
        inputCarry.setSize (2, inputCapacity);
        inputCarry.clear();
        reset();
        lastRatio = 0.0; // フィルタ再設計を強制
    }

    bool canHandle (double ratio) const { return inputCapacity > 0 && ratio <= maxRatio; }

    // 不連続（シーク・ファイル変更・構成切替）時に呼ぶ。旧位置のサンプルを完全に破棄する
    void reset()
    {
        carrySamples = 0;
        for (auto& interp : interpolators)
            interp.reset();
        for (auto& fs : filterStates)
            fs = {};
    }

    // readSource(float* l, float* r, int n): ソースSR基準で n サンプル読み込む関数。
    // outL/outR へ numOut サンプル生成する（outR==outL のモノラル出力も可）
    template <typename ReadFn>
    void process (double ratio, ReadFn&& readSource, float* outL, float* outR, int numOut)
    {
        // 同一SRは補間もフィルタも通さない（ビット劣化なし・最頻ケース）
        if (std::abs (ratio - 1.0) < 1.0e-9)
        {
            readSource (outL, outR, numOut);
            return;
        }

        if (! juce::exactlyEqual (ratio, lastRatio))
        {
            designLowPass (ratio); // 算術のみ（コールバック内で安全）。状態のresetは不連続時のみ
            lastRatio = ratio;
        }

        const int needed = (int) std::ceil (numOut * ratio) + 8;
        const int total = juce::jmin (inputCapacity, juce::jmax (needed, carrySamples));
        float* inL = inputCarry.getWritePointer (0);
        float* inR = inputCarry.getWritePointer (1);
        if (total > carrySamples)
        {
            readSource (inL + carrySamples, inR + carrySamples, total - carrySamples);
            // ダウンサンプリング: 折り返す帯域を間引きの前に落とす（新規到着分だけ。既存キャリーは適用済み）
            if (ratio > 1.0001)
            {
                applyFilter (inL + carrySamples, total - carrySamples, filterStates[0]);
                applyFilter (inR + carrySamples, total - carrySamples, filterStates[1]);
            }
        }

        int used = 0;
        used = interpolators[0].process (ratio, inL, outL, numOut, total, 0);
        if (outR != outL)
            interpolators[1].process (ratio, inR, outR, numOut, total, 0);

        // アップサンプリング: 補間後にイメージング成分を落とす
        if (ratio < 0.9999)
        {
            applyFilter (outL, numOut, filterStates[0]);
            if (outR != outL)
                applyFilter (outR, numOut, filterStates[1]);
        }

        // 未消費分を先頭へ寄せる（数十サンプル程度・有界）
        carrySamples = juce::jmax (0, total - used);
        if (carrySamples > 0)
        {
            std::memmove (inL, inL + used, (size_t) carrySamples * sizeof (float));
            std::memmove (inR, inR + used, (size_t) carrySamples * sizeof (float));
        }
    }

private:
    struct FilterState
    {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };

    // JUCE ResamplingAudioSource::createLowPass の移植（2次バイリニアButterworth。
    // カットオフはダウン時=出力Nyquist相当・アップ時=入力Nyquist相当）
    void designLowPass (double frequencyRatio)
    {
        const double proportionalRate = (frequencyRatio > 1.0) ? 0.5 / frequencyRatio
                                                               : 0.5 * frequencyRatio;
        const double n = 1.0 / std::tan (juce::MathConstants<double>::pi * juce::jmax (0.001, proportionalRate));
        const double nSquared = n * n;
        const double c1 = 1.0 / (1.0 + juce::MathConstants<double>::sqrt2 * n + nSquared);

        coefficients[0] = c1;
        coefficients[1] = c1 * 2.0;
        coefficients[2] = c1;
        coefficients[3] = 1.0;
        coefficients[4] = c1 * 2.0 * (1.0 - nSquared);
        coefficients[5] = c1 * (1.0 - juce::MathConstants<double>::sqrt2 * n + nSquared);
    }

    void applyFilter (float* samples, int num, FilterState& fs)
    {
        while (--num >= 0)
        {
            const double in = *samples;
            double out = coefficients[0] * in
                         + coefficients[1] * fs.x1
                         + coefficients[2] * fs.x2
                         - coefficients[4] * fs.y1
                         - coefficients[5] * fs.y2;
            // デノーマル対策（JUCEと同じ）
            if (! (out < -1.0e-8 || out > 1.0e-8))
                out = 0.0;
            fs.x2 = fs.x1;
            fs.x1 = in;
            fs.y2 = fs.y1;
            fs.y1 = out;
            *samples++ = (float) out;
        }
    }

    double coefficients[6] {};
    FilterState filterStates[2];
    juce::LagrangeInterpolator interpolators[2];
    juce::AudioBuffer<float> inputCarry;
    int carrySamples = 0;
    int inputCapacity = 0;
    double maxRatio = 0.0;
    double lastRatio = 0.0;
};
