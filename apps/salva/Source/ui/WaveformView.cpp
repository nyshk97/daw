#include "WaveformView.h"

namespace
{
// 配色はLaLaのThemeに揃える（Salvaは別アプリだが、目の慣れを共有する）
const juce::Colour waveBg { 0xff1e1e22 };
const juce::Colour waveColour { 0xff5b7bb0 };
const juce::Colour selectionFill { juce::Colour (0xff4a6ea9).withAlpha (0.25f) };
const juce::Colour selectionEdge { 0xff4a6ea9 };
const juce::Colour playheadColour { 0xffffffff };
const juce::Colour timeLabelColour { 0xff8a8a90 };
} // namespace

WaveformView::WaveformView()
{
    formatManager.registerBasicFormats();
    thumbnail.addChangeListener (this);
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

WaveformView::~WaveformView()
{
    thumbnail.removeChangeListener (this);
}

void WaveformView::setFile (const juce::File& file, double sampleRate, juce::int64 newLengthSamples)
{
    sourceSampleRate = sampleRate;
    lengthSamples = newLengthSamples;
    thumbnail.setSource (new juce::FileInputSource (file)); // 所有権はthumbnailへ。背景スレッドで漸進生成
    viewStart = 0.0;
    viewLength = (double) newLengthSamples;
    selStart = selEnd = -1;
    playheadSample = 0;
    repaint();
}

void WaveformView::clearFile()
{
    thumbnail.clear();
    sourceSampleRate = 0.0;
    lengthSamples = 0;
    selStart = selEnd = -1;
    repaint();
}

void WaveformView::setSelection (juce::int64 start, juce::int64 end)
{
    selStart = juce::jlimit ((juce::int64) 0, lengthSamples, start);
    selEnd = juce::jlimit ((juce::int64) 0, lengthSamples, end);
    repaint();
}

void WaveformView::clearSelection()
{
    if (selStart < 0 && selEnd < 0)
        return;
    selStart = selEnd = -1;
    repaint();
}

void WaveformView::setPlayhead (juce::int64 sample, bool visible)
{
    if (playheadSample == sample && playheadVisible == visible)
        return;
    playheadSample = sample;
    playheadVisible = visible;
    repaint();
}

double WaveformView::xToSample (float x) const
{
    return viewStart + (double) x / (double) juce::jmax (1, getWidth()) * viewLength;
}

float WaveformView::sampleToX (juce::int64 sample) const
{
    return (float) (((double) sample - viewStart) / juce::jmax (1.0, viewLength) * getWidth());
}

juce::int64 WaveformView::clampToLength (double sample) const
{
    return juce::jlimit ((juce::int64) 0, lengthSamples, (juce::int64) std::llround (sample));
}

void WaveformView::paint (juce::Graphics& g)
{
    g.fillAll (waveBg);
    if (lengthSamples <= 0 || sourceSampleRate <= 0.0)
        return;

    const auto bounds = getLocalBounds();
    const double startSec = viewStart / sourceSampleRate;
    const double endSec = (viewStart + viewLength) / sourceSampleRate;

    g.setColour (waveColour);
    thumbnail.drawChannels (g, bounds, startSec, endSec, 0.95f);

    // 選択区間
    if (hasSelection())
    {
        const float x1 = sampleToX (selStart);
        const float x2 = sampleToX (selEnd);
        if (x2 > 0.0f && x1 < (float) getWidth())
        {
            g.setColour (selectionFill);
            g.fillRect (juce::Rectangle<float> (x1, 0.0f, x2 - x1, (float) getHeight()));
            g.setColour (selectionEdge);
            g.fillRect (juce::Rectangle<float> (x1, 0.0f, 1.5f, (float) getHeight()));
            g.fillRect (juce::Rectangle<float> (x2 - 1.5f, 0.0f, 1.5f, (float) getHeight()));
        }
    }

    // 再生ヘッド
    if (playheadVisible)
    {
        const float px = sampleToX (playheadSample);
        if (px >= 0.0f && px <= (float) getWidth())
        {
            g.setColour (playheadColour);
            g.fillRect (juce::Rectangle<float> (px, 0.0f, 1.0f, (float) getHeight()));
        }
    }

    // 表示窓の両端時刻（小さく）
    g.setColour (timeLabelColour);
    g.setFont (10.0f);
    auto fmt = [] (double sec)
    {
        const int m = (int) sec / 60;
        const double s = sec - m * 60;
        return juce::String (m) + ":" + juce::String (s, 1).paddedLeft ('0', 4);
    };
    g.drawText (fmt (startSec), 6, getHeight() - 16, 80, 12, juce::Justification::left);
    g.drawText (fmt (endSec), getWidth() - 86, getHeight() - 16, 80, 12, juce::Justification::right);
}

void WaveformView::resized() {}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    if (lengthSamples <= 0)
        return;

    if (hasSelection())
    {
        const float x1 = sampleToX (selStart);
        const float x2 = sampleToX (selEnd);
        if (std::abs (e.position.x - x1) <= edgeGrabPx)
        {
            dragMode = Drag::leftEdge;
            return;
        }
        if (std::abs (e.position.x - x2) <= edgeGrabPx)
        {
            dragMode = Drag::rightEdge;
            return;
        }
    }
    dragMode = Drag::newSelection;
    dragAnchor = clampToLength (xToSample (e.position.x));
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (lengthSamples <= 0 || dragMode == Drag::none)
        return;

    const auto sample = clampToLength (xToSample (e.position.x));
    switch (dragMode)
    {
        case Drag::newSelection:
            if (e.getDistanceFromDragStartX() == 0 && ! hasSelection())
                return; // クリック判定はmouseUpで
            selStart = juce::jmin (dragAnchor, sample);
            selEnd = juce::jmax (dragAnchor, sample);
            break;
        case Drag::leftEdge:
            selStart = juce::jmin (sample, selEnd - 1);
            break;
        case Drag::rightEdge:
            selEnd = juce::jmax (sample, selStart + 1);
            break;
        case Drag::none:
            return;
    }
    repaint();
    notifySelection();
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    const auto mode = dragMode;
    dragMode = Drag::none;

    if (lengthSamples <= 0)
        return;

    // ドラッグにならなかったクリック = シーク（選択があれば解除）
    if (mode == Drag::newSelection && e.getDistanceFromDragStart() < 3)
    {
        const bool hadSelection = hasSelection();
        clearSelection();
        if (hadSelection && onSelectionCleared)
            onSelectionCleared();
        if (onSeek)
            onSeek (clampToLength (xToSample (e.position.x)));
        return;
    }

    if (mode != Drag::none)
        notifySelection();
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    if (hasSelection()
        && (std::abs (e.position.x - sampleToX (selStart)) <= edgeGrabPx
            || std::abs (e.position.x - sampleToX (selEnd)) <= edgeGrabPx))
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else
        setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

void WaveformView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (lengthSamples <= 0)
        return;
    // 横スクロールでパン（トラックパッド）。縦ホイールもパンに使う
    const double delta = (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY);
    viewStart -= delta * viewLength * 0.5;
    clampView();
    repaint();
}

void WaveformView::zoom (double factor)
{
    if (lengthSamples <= 0)
        return;
    // アンカー: 選択中心 > 再生ヘッド（可視時） > 表示窓中心
    double anchor = viewStart + viewLength / 2.0;
    if (hasSelection())
        anchor = (double) (selStart + selEnd) / 2.0;
    else if (playheadVisible)
        anchor = (double) playheadSample;

    const double minLength = sourceSampleRate > 0.0 ? sourceSampleRate * 0.5 : 1024.0; // 最小0.5秒幅
    viewLength = juce::jlimit (minLength, (double) lengthSamples, viewLength * factor);
    viewStart = anchor - viewLength / 2.0;
    clampView();
    repaint();
}

void WaveformView::clampView()
{
    viewLength = juce::jmin (viewLength, (double) lengthSamples);
    viewStart = juce::jlimit (0.0, (double) lengthSamples - viewLength, viewStart);
}

void WaveformView::notifySelection()
{
    if (hasSelection() && onSelectionChanged)
        onSelectionChanged (selStart, selEnd);
}
