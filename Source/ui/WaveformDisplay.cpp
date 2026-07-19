#include "WaveformDisplay.h"

WaveformDisplay::WaveformDisplay (WaveformFifo& f)
    : fifo (f)
{
    pullBuffer.resize (4096);
    peaks.resize ((size_t) numPeaks, 0.0f);
    startTimerHz (25);
}

void WaveformDisplay::timerCallback()
{
    int numPulled = 0;
    while ((numPulled = fifo.pull (pullBuffer.data(), (int) pullBuffer.size())) > 0)
    {
        for (int i = 0; i < numPulled; ++i)
        {
            accum = juce::jmax (accum, std::abs (pullBuffer[(size_t) i]));
            if (++accumCount >= samplesPerPeak)
            {
                peaks[(size_t) writeIndex] = accum;
                writeIndex = (writeIndex + 1) % numPeaks;
                accum = 0.0f;
                accumCount = 0;
            }
        }
    }
    repaint();
}

void WaveformDisplay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    const auto bounds = getLocalBounds().toFloat();
    const float midY = bounds.getCentreY();
    const int width = juce::jmax (1, getWidth());

    g.setColour (juce::Colours::limegreen);

    for (int x = 0; x < width; ++x)
    {
        // 左端が最古（= writeIndex の次）、右端が最新になるようマッピング
        const int offset = x * numPeaks / width;
        const float peak = peaks[(size_t) ((writeIndex + offset) % numPeaks)];
        const float halfHeight = juce::jmax (0.5f, peak * midY);
        g.drawVerticalLine (x, midY - halfHeight, midY + halfHeight);
    }
}
