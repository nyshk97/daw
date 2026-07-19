#pragma once

#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/WaveformFifo.h"

// 入力波形のスクロール表示。GOTCHAS.mdの原則どおり、
// Timer（pull型）でFIFOから取り出してピーク値に集約し、repaint()する。
class WaveformDisplay : public juce::Component,
                        private juce::Timer
{
public:
    explicit WaveformDisplay (WaveformFifo& fifo);

    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;

    WaveformFifo& fifo;

    static constexpr int samplesPerPeak = 256; // 48kHzで約5.3ms分を1本のピークに集約
    static constexpr int numPeaks = 1024;      // 約5.5秒分の履歴

    std::vector<float> pullBuffer;             // FIFOからの取り出し用（事前確保）
    std::vector<float> peaks;                  // リングバッファ
    int writeIndex = 0;
    float accum = 0.0f;
    int accumCount = 0;
};
