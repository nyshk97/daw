#pragma once

#include <array>
#include <vector>
#include <juce_dsp/juce_dsp.h>

#include "AnalyzerTap.h"

// スペクトラムアナライザの数値処理（AnalyzerTapのブロック → dBFS配列）。
// UIは magnitudesDb() を**描くだけ**にする — dBFSの値を決めるのは描画でなく判断ロジックなので
// ui/ に置かない（daw_testsで固定する）。plan: docs/plans/2026-08-15-1506-track-eq.md
//
// 担当範囲: (trackId,世代)の照合・Hann窓・FFT（4096点）・振幅正規化・片側スペクトル補正・
// L/Rパワー平均・対数bin集約・-60dB floor。
//
// **モノ和にしない**: L/Rに位相差のある音は加算すると打ち消され「聴こえているのに表示から
// 消える」誤表示になる。L/Rを別々にFFTして各binのパワー（振幅の2乗）を平均する。
//
// 正規化: フルスケール（振幅1.0）のサイン波がbin中心で 0dBFS になるように、
// |X| に 2/(N×窓のコヒーレントゲイン0.5) = 4/N を掛けて振幅へ戻す。
class SpectrumAnalyzer
{
public:
    static constexpr int fftOrder = 12;
    static constexpr int fftSize = 1 << fftOrder; // 4096点（48kHzで約11.7Hz分解能）
    static constexpr int hopSize = fftSize / 2;   // 50%オーバーラップで更新
    static constexpr int numBins = 240;           // 対数表示ビン（20Hz〜20kHz）
    static constexpr float floorDb = -60.0f;

    SpectrumAnalyzer();

    // FIFOを吸い上げて蓄積し、新しいフレームができたら true（magnitudesDbが更新される）。
    // (trackId, 世代) が現在値と一致しないブロックは破棄する。メッセージスレッド専用
    bool update (AnalyzerTap& tap, double sampleRate);

    const std::array<float, numBins>& magnitudesDb() const { return displayDb; }

    void reset(); // トラック切替時（蓄積と表示をfloorへ戻す）

    // 表示ビンの中心周波数（対数等間隔 20Hz〜20kHz）
    static float binFrequency (int bin);

private:
    void computeFrame (double sampleRate);

    juce::dsp::FFT fft { fftOrder };
    std::vector<float> window;                 // Hann（事前計算）
    std::vector<float> ringL, ringR;           // 直近fftSizeサンプルのリング
    int writePos = 0;
    int filled = 0;
    int samplesSinceFrame = 0;
    juce::uint32 lastGeneration = 0;
    std::vector<float> popL, popR;             // popBlock用の受け皿
    std::vector<float> workL, workR;           // FFT作業領域（2×fftSize）
    std::array<float, numBins> displayDb;

    JUCE_DECLARE_NON_COPYABLE (SpectrumAnalyzer)
};
