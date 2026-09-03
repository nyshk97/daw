#include "WaveformView.h"

#include <cmath>

#include "ui/StemColours.h"

namespace
{
// 配色はLaLaのThemeに揃える（Salvaは別アプリだが、目の慣れを共有する）
// アイコン基準パレット（波形=レーベルのオレンジ、選択=クリームで対比を取る）
const juce::Colour waveBg { 0xff17161a };
const juce::Colour waveColour { 0xffef8a3f };
const juce::Colour selectionFill { juce::Colour (0xfff2e8d5).withAlpha (0.12f) };
const juce::Colour selectionEdge { 0xfff2e8d5 };
const juce::Colour playheadColour { 0xffffffff };
const juce::Colour timeLabelColour { 0xff97908a };
// 分離中の走査表示（案C）: 走査線＝アクセントのオレンジ、ラベル箱はヘッダーと同じ地色
const juce::Colour scanColour { 0xffff7a2e };
const juce::Colour boxFill { 0xeb242328 };
const juce::Colour boxBorder { 0xff46444e };
const juce::Colour boxText { 0xfff2e8d5 };
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

    // 分離中は元波形を沈めて、走査済み区間のステム色の積み重ねを主役にする
    g.setColour (separating ? waveColour.withAlpha (0.3f) : waveColour);
    thumbnail.drawChannels (g, bounds, startSec, endSec, 0.95f);

    if (separating)
        paintSeparation (g);

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

    // 表示窓の両端時刻（小さく）。分離中は凡例に譲る（操作できないので位置情報も要らない）
    if (separating)
        return;
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
    if (lengthSamples <= 0 || separating)
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
    if (lengthSamples <= 0 || dragMode == Drag::none || separating)
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

    if (lengthSamples <= 0 || separating)
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
    if (separating)
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }
    if (hasSelection()
        && (std::abs (e.position.x - sampleToX (selStart)) <= edgeGrabPx
            || std::abs (e.position.x - sampleToX (selEnd)) <= edgeGrabPx))
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else
        setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

void WaveformView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (lengthSamples <= 0 || separating)
        return;
    // 横スクロールでパン（トラックパッド）。縦ホイールもパンに使う
    const double delta = (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY);
    viewStart -= delta * viewLength * 0.5;
    clampView();
    repaint();
}

void WaveformView::zoom (double factor)
{
    if (lengthSamples <= 0 || separating)
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

//==============================================================================
// 分離中の表示（案C）

void WaveformView::setSeparation (bool active, const SeparationProgress& progress, double remainingSeconds)
{
    const bool wasSeparating = separating;
    const auto previousStage = separation.stage;
    separating = active;
    separation = progress;
    separationRemainingSec = remainingSeconds;

    if (! active)
    {
        scanShownX = 0.0f;
        if (wasSeparating)
            repaint();
        return;
    }

    // 走査線の目標位置: 段階内の進捗×幅（準備中は左端・書き出し中は右端）。
    // 段階が切り替わったら左端から再出発（4 STEMS → 6 STEMS で2周目に入る）
    using Stage = SeparationProgress::Stage;
    const float w = (float) getWidth();
    const float target = progress.stage == Stage::preparing ? 0.0f
                       : progress.stage == Stage::exporting ? w
                                                            : w * juce::jlimit (0.0f, 1.0f, progress.local);
    if (! wasSeparating || previousStage != progress.stage)
        scanShownX = 0.0f;
    scanShownX += (target - scanShownX) * 0.15f; // 30Hzで約0.5秒かけて寄る（tqdmの刻みを滑らかに）
    if (std::abs (target - scanShownX) < 0.5f)
        scanShownX = target;
    repaint();
}

void WaveformView::paintStemStack (juce::Graphics& g, float fromX, float toX, int numStems, float alpha)
{
    const int numCh = thumbnail.getNumChannels();
    if (numCh <= 0 || toX <= fromX || sourceSampleRate <= 0.0)
        return;

    const float laneH = (float) getHeight() / (float) numCh;
    const double samplesPerPx = viewLength / (double) juce::jmax (1, getWidth());
    const int x0 = juce::jmax (0, (int) std::floor (fromX));
    const int x1 = juce::jmin (getWidth(), (int) std::ceil (toX));

    for (int x = x0; x < x1; ++x)
    {
        const double t0 = (viewStart + x * samplesPerPx) / sourceSampleRate;
        const double t1 = t0 + samplesPerPx / sourceSampleRate;
        const double tc = (t0 + t1) * 0.5;

        // 積み重ねの比率は装飾（時間でゆっくり揺れるテクスチャ。実際のステム配分ではない）
        float r[StemColours::count] = {
            0.28f + 0.16f * std::abs ((float) std::sin (tc * 0.9)),   // drums
            0.20f + 0.08f * (float) std::sin (tc * 0.37),             // bass
            0.22f + 0.10f * (float) std::sin (tc * 1.3 + 1.0),        // other
            0.30f * juce::jmax (0.0f, (float) std::sin (tc * 0.27)),  // vocals
            0.14f * (0.5f + 0.5f * (float) std::sin (tc * 0.9 + 2.0)), // guitar
            0.12f * (0.5f + 0.5f * (float) std::cos (tc * 0.55)),     // piano
        };
        float sum = 0.0f;
        for (int k = 0; k < numStems; ++k)
            sum += r[k];
        if (sum <= 0.0f)
            continue;

        for (int ch = 0; ch < numCh; ++ch)
        {
            float mn = 0.0f, mx = 0.0f;
            thumbnail.getApproximateMinMax (t0, t1, ch, mn, mx);
            const float centre = laneH * ((float) ch + 0.5f);
            const float half = laneH * 0.5f * 0.95f; // drawChannels の verticalZoom と同じ
            const float top = centre - mx * half;
            const float bottom = centre - mn * half;
            const float span = bottom - top;
            if (span < 0.5f)
                continue;
            float y = bottom;
            for (int k = 0; k < numStems; ++k)
            {
                const float hk = span * r[k] / sum;
                g.setColour (StemColours::swatch (k).withAlpha (alpha));
                g.fillRect (juce::Rectangle<float> ((float) x, y - hk, 1.0f, hk));
                y -= hk;
            }
        }
    }
}

void WaveformView::paintSeparation (juce::Graphics& g)
{
    using Stage = SeparationProgress::Stage;
    const float w = (float) getWidth(), h = (float) getHeight();
    const auto stage = separation.stage;
    const float scanX = juce::jlimit (0.0f, w, scanShownX);

    // 積み重ね: 4 STEMS 段階は走査済みだけ。6 STEMS 段階は走査済み=6色・未走査=1周目の4色を薄く残す。
    // 書き出し中は全幅6色
    if (stage == Stage::stems4)
    {
        paintStemStack (g, 0.0f, scanX, 4, 0.85f);
    }
    else if (stage == Stage::stems6)
    {
        paintStemStack (g, scanX, w, 4, 0.35f);
        paintStemStack (g, 0.0f, scanX, 6, 0.95f);
    }
    else if (stage == Stage::exporting)
    {
        paintStemStack (g, 0.0f, w, 6, 0.95f);
    }

    // 走査線＋グロー（準備中は左端で待つ。書き出し中は無し）
    if (stage != Stage::exporting)
    {
        juce::ColourGradient glow (scanColour.withAlpha (0.0f), scanX - 60.0f, 0.0f,
                                   scanColour.withAlpha (0.22f), scanX, 0.0f, false);
        g.setGradientFill (glow);
        g.fillRect (juce::Rectangle<float> (scanX - 60.0f, 0.0f, 60.0f, h));
        g.setColour (scanColour);
        g.fillRect (juce::Rectangle<float> (scanX - 1.0f, 0.0f, 2.0f, h));
    }

    // ラベル（段階・%・残り時間）: 走査線のそばに置き、端では内側へ寄せる
    const auto jp = [] (const char* text) { return juce::String::fromUTF8 (text); };
    juce::String line1 = stage == Stage::preparing ? jp (u8"準備中")
                       : stage == Stage::stems4    ? jp (u8"4 STEMS を分離中")
                       : stage == Stage::stems6    ? jp (u8"6 STEMS を分離中")
                                                   : jp (u8"書き出し中");
    if (stage == Stage::stems4 || stage == Stage::stems6)
        line1 += jp (u8"　") + juce::String (juce::roundToInt (separation.overall() * 100.0f)) + "%";
    juce::String line2;
    if (separationRemainingSec >= 0.0)
    {
        const int total = (int) std::lround (separationRemainingSec);
        line2 = jp (u8"残り ") + juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
    }
    const juce::Font font1 (juce::FontOptions (13.0f).withStyle ("Bold"));
    const juce::Font font2 (juce::FontOptions (11.0f));
    const float boxW = juce::jmax (juce::GlyphArrangement::getStringWidth (font1, line1),
                                   juce::GlyphArrangement::getStringWidth (font2, line2)) + 28.0f;
    const float boxH = line2.isEmpty() ? 30.0f : 46.0f;
    const float boxX = juce::jlimit (8.0f, juce::jmax (8.0f, w - boxW - 8.0f), scanX - boxW * 0.5f);
    const juce::Rectangle<float> box (boxX, 10.0f, boxW, boxH);
    g.setColour (boxFill);
    g.fillRoundedRectangle (box, 7.0f);
    g.setColour (boxBorder);
    g.drawRoundedRectangle (box, 7.0f, 1.0f);
    g.setColour (boxText);
    g.setFont (font1);
    g.drawText (line1, box.reduced (14.0f, 0.0f).removeFromTop (30.0f), juce::Justification::centredLeft);
    if (line2.isNotEmpty())
    {
        g.setColour (timeLabelColour);
        g.setFont (font2);
        g.drawText (line2, box.reduced (14.0f, 0.0f).removeFromBottom (22.0f).withTrimmedBottom (4.0f),
                    juce::Justification::centredLeft);
    }

    // 凡例（左下）: 積み重ねの色の意味。段階に応じて4色/6色
    const int legendStems = stage == Stage::stems6 || stage == Stage::exporting ? 6 : 4;
    g.setFont (juce::FontOptions (10.0f));
    float legendW = 8.0f;
    for (int k = 0; k < legendStems; ++k)
        legendW += 12.0f + juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), StemColours::names[k]) + 14.0f;
    const float ly = h - 16.0f;
    g.setColour (boxFill.withAlpha (0.8f)); // 波形の上でも読めるように薄い下地
    g.fillRoundedRectangle (6.0f, ly - 10.0f, legendW, 20.0f, 6.0f);
    float lx = 12.0f;
    for (int k = 0; k < legendStems; ++k)
    {
        g.setColour (StemColours::swatch (k));
        g.fillRoundedRectangle (lx, ly - 4.0f, 8.0f, 8.0f, 2.0f);
        g.setColour (timeLabelColour);
        const juce::String name (StemColours::names[k]);
        g.drawText (name, juce::Rectangle<float> (lx + 12.0f, ly - 7.0f, 60.0f, 14.0f), juce::Justification::centredLeft);
        lx += 12.0f + juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), name) + 14.0f;
    }
}
