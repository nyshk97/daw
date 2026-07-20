#include "TimelineView.h"

#include <cmath>

// ---- 内部コンポーネント -------------------------------------------------

class TimelineView::LaneViewport : public juce::Viewport
{
public:
    explicit LaneViewport (TimelineView& o) : owner (o) {}

    void visibleAreaChanged (const juce::Rectangle<int>&) override
    {
        owner.syncScroll();
    }

private:
    TimelineView& owner;
};

class TimelineView::RulerContent : public juce::Component
{
public:
    explicit RulerContent (TimelineView& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff2a2a2e));
        const auto clip = g.getClipBounds();
        const double barWidth = owner.pxPerBar;

        const int firstBar = juce::jmax (0, (int) std::floor (clip.getX() / barWidth) - 1);
        const int lastBar = (int) std::floor (clip.getRight() / barWidth) + 1;
        const int div = owner.gridDivisionsPerBar();

        // ラベルが重ならないよう、ズームアウト時は2の冪の間隔で間引く
        int labelStep = 1;
        while (labelStep * barWidth < 48.0)
            labelStep *= 2;

        for (int bar = firstBar; bar <= lastBar; ++bar)
        {
            for (int i = 0; i < div; ++i)
            {
                const int x = (int) std::llround ((bar + i / (double) div) * barWidth);
                if (i == 0)
                {
                    g.setColour (juce::Colour (0xff55555a));
                    g.drawVerticalLine (x, 8.0f, (float) getHeight());
                }
                else if ((i * 4) % div == 0)   // 拍（1/2表示時はその線も同格に）
                {
                    g.setColour (juce::Colour (0xff4a4a4f));
                    g.drawVerticalLine (x, 14.0f, (float) getHeight());
                }
                else
                {
                    g.setColour (juce::Colour (0xff3c3c41));
                    g.drawVerticalLine (x, 19.0f, (float) getHeight());
                }
            }

            if (bar % labelStep == 0)
            {
                g.setColour (juce::Colours::lightgrey);
                g.setFont (11.0f);
                g.drawText (juce::String (bar + 1),
                            (int) std::llround (bar * barWidth) + 4, 0,
                            (int) (labelStep * barWidth) - 6, getHeight(),
                            juce::Justification::centredLeft);
            }
        }

        const int playheadX = owner.sampleToX (owner.transport.playheadSamplePos.load());
        g.setColour (juce::Colours::white);
        g.drawVerticalLine (playheadX, 0.0f, (float) getHeight());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        owner.seekFromX (e.x);
    }

    void mouseMagnify (const juce::MouseEvent& e, float scaleFactor) override
    {
        owner.zoomAroundContentX ((double) scaleFactor, e.x);
    }

private:
    TimelineView& owner;
};

class TimelineView::LaneContent : public juce::Component
{
public:
    explicit LaneContent (TimelineView& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1e1e22));
        const auto clip = g.getClipBounds();
        auto* proj = owner.project;
        if (proj == nullptr)
            return;

        const int numTracks = (int) proj->tracks.size();

        // 行背景・区切り線
        for (int t = 0; t < numTracks; ++t)
        {
            const int y = t * trackHeight;
            if (y > clip.getBottom() || y + trackHeight < clip.getY())
                continue;
            if (t == owner.selectedTrack)
            {
                g.setColour (juce::Colour (0xff26262e));
                g.fillRect (clip.getX(), y, clip.getWidth(), trackHeight);
            }
            g.setColour (juce::Colour (0xff333338));
            g.drawHorizontalLine (y + trackHeight - 1, (float) clip.getX(), (float) clip.getRight());
        }

        // 小節・拍グリッド（ズームに応じて最大1/16音符まで細分化）
        const double barWidth = owner.pxPerBar;
        const int firstBar = juce::jmax (0, (int) std::floor (clip.getX() / barWidth));
        const int lastBar = (int) std::floor (clip.getRight() / barWidth) + 1;
        const int div = owner.gridDivisionsPerBar();
        for (int bar = firstBar; bar <= lastBar; ++bar)
        {
            for (int i = 0; i < div; ++i)
            {
                const int x = (int) std::llround ((bar + i / (double) div) * barWidth);
                g.setColour (i == 0             ? juce::Colour (0xff2c2c31)
                             : (i * 4) % div == 0 ? juce::Colour (0xff28282c)
                                                  : juce::Colour (0xff242428));
                g.drawVerticalLine (x, (float) clip.getY(), (float) clip.getBottom());
            }
        }

        // クリップ
        for (int t = 0; t < numTracks; ++t)
        {
            const int y = t * trackHeight;
            if (y > clip.getBottom() || y + trackHeight < clip.getY())
                continue;

            auto& clips = proj->tracks[(size_t) t].clips;
            for (int ci = 0; ci < (int) clips.size(); ++ci)
            {
                const bool isSelected = (owner.selection.track == t && owner.selection.clip == ci);
                drawClip (g, clips[(size_t) ci], y, isSelected, clip);
            }
        }

        // 録音中の仮クリップ（赤）
        if (owner.transport.recordArmed.load()
            && owner.selectedTrack >= 0 && owner.selectedTrack < numTracks)
        {
            const auto recordedLen = owner.transport.recordedSamples.load();
            if (recordedLen > 0)
            {
                const int x = owner.sampleToX (owner.transport.punchInSample.load());
                const int w = juce::jmax (2, (int) ((double) recordedLen / owner.samplesPerPixel()));
                const int y = owner.selectedTrack * trackHeight;
                g.setColour (juce::Colours::red.withAlpha (0.45f));
                g.fillRoundedRectangle ((float) x, (float) y + 4.0f, (float) w, (float) trackHeight - 8.0f, 4.0f);
            }
        }

        // 再生ヘッド
        const int playheadX = owner.sampleToX (owner.transport.playheadSamplePos.load());
        if (playheadX >= clip.getX() - 1 && playheadX <= clip.getRight() + 1)
        {
            g.setColour (juce::Colours::white.withAlpha (0.8f));
            g.drawVerticalLine (playheadX, (float) clip.getY(), (float) clip.getBottom());
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        owner.handleLaneMouseDown (e);
    }

    void mouseMagnify (const juce::MouseEvent& e, float scaleFactor) override
    {
        owner.zoomAroundContentX ((double) scaleFactor, e.x);
    }

private:
    void drawClip (juce::Graphics& g, const Clip& clip, int y, bool isSelected,
                   const juce::Rectangle<int>& clipRegion)
    {
        const double spp = owner.samplesPerPixel();
        const int x = owner.sampleToX (clip.startSample);
        const int w = juce::jmax (2, (int) ((double) clip.lengthSamples() / spp));
        if (x > clipRegion.getRight() || x + w < clipRegion.getX())
            return;

        const auto rect = juce::Rectangle<int> (x, y + 4, w, trackHeight - 8);
        g.setColour (isSelected ? juce::Colour (0xff4a6ea9) : juce::Colour (0xff39537d));
        g.fillRoundedRectangle (rect.toFloat(), 4.0f);
        if (isSelected)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (rect.toFloat().reduced (0.5f), 4.0f, 1.5f);
        }

        // 波形（ロード時に作ったピークキャッシュから描く）
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        const int x0 = juce::jmax (rect.getX(), clipRegion.getX());
        const int x1 = juce::jmin (rect.getRight(), clipRegion.getRight());
        const float midY = (float) rect.getCentreY();
        const float halfH = (float) (rect.getHeight() / 2 - 3);

        for (int px = x0; px < x1; ++px)
        {
            const double s0 = (px - x) * spp;
            const double s1 = s0 + spp;
            const int i0 = (int) (s0 / Clip::samplesPerPeak);
            const int i1 = juce::jmax (i0 + 1, (int) (s1 / Clip::samplesPerPeak));

            float peak = 0.0f;
            for (int i = i0; i < i1 && i < (int) clip.peakCache.size(); ++i)
                peak = juce::jmax (peak, clip.peakCache[(size_t) i]);

            const float h = juce::jlimit (1.0f, halfH, peak * halfH * 1.4f);
            g.drawVerticalLine (px, midY - h, midY + h);
        }
    }

    TimelineView& owner;
};

// ---- TimelineView 本体 ---------------------------------------------------

TimelineView::TimelineView (TransportState& transportState)
    : transport (transportState)
{
    ruler = std::make_unique<RulerContent> (*this);
    lanes = std::make_unique<LaneContent> (*this);
    viewport = std::make_unique<LaneViewport> (*this);

    viewport->setViewedComponent (lanes.get(), false);
    viewport->setScrollBarsShown (true, true);

    addAndMakeVisible (*viewport);
    addAndMakeVisible (*ruler);

    startTimerHz (30); // GOTCHAS.md: 通知はpush型でなくpull型（Timerポーリング）
}

TimelineView::~TimelineView() = default;

void TimelineView::setProject (Project* p)
{
    project = p;
    selection.clear();
    refresh();
}

void TimelineView::setSelectedTrack (int index)
{
    if (selectedTrack == index)
        return;
    selectedTrack = index;
    lanes->repaint();
}

void TimelineView::refresh()
{
    updateContentSize();
    lanes->repaint();
    ruler->repaint();
}

void TimelineView::clearSelection()
{
    if (! selection.isValid())
        return;
    selection.clear();
    lanes->repaint();
}

double TimelineView::effectiveSampleRate() const
{
    if (project != nullptr && project->sampleRate > 0.0)
        return project->sampleRate;
    const auto deviceRate = transport.sampleRate.load();
    return deviceRate > 0.0 ? deviceRate : 48000.0;
}

double TimelineView::barLengthSamples() const
{
    const double bpm = juce::jlimit (20.0, 400.0, transport.bpm.load());
    return effectiveSampleRate() * 60.0 / bpm * 4.0; // 4/4固定
}

double TimelineView::samplesPerPixel() const
{
    return barLengthSamples() / pxPerBar;
}

int TimelineView::gridDivisionsPerBar() const
{
    // 線の間隔が12px以上確保できる最も細かい分割を選ぶ。上限は1/16音符
    // （Tier 1ではグリッドは表示とシークの目安のみなので、これ以上細かくしない）
    int div = 1;
    for (int candidate : { 2, 4, 8, 16 })
        if (pxPerBar / candidate >= 12.0)
            div = candidate;
    return div;
}

void TimelineView::zoomBy (double factor)
{
    const auto view = viewport->getViewArea();
    const int playheadX = sampleToX (transport.playheadSamplePos.load());
    const int anchorX = (playheadX >= view.getX() && playheadX <= view.getRight())
                            ? playheadX
                            : view.getCentreX();
    zoomAroundContentX (factor, anchorX);
}

void TimelineView::zoomAroundContentX (double factor, int contentX)
{
    const double newPxPerBar = juce::jlimit (minPxPerBar, maxPxPerBar, pxPerBar * factor);
    if (juce::approximatelyEqual (newPxPerBar, pxPerBar))
        return;

    // アンカー位置のサンプルが画面上の同じ場所に留まるようスクロールを補正する
    const auto anchorSample = xToSample (contentX);
    const int anchorOffset = contentX - viewport->getViewPositionX();
    pxPerBar = newPxPerBar;
    updateContentSize();
    viewport->setViewPosition (juce::jmax (0, sampleToX (anchorSample) - anchorOffset),
                               viewport->getViewPositionY());
    lanes->repaint();
    ruler->repaint();
}

int TimelineView::sampleToX (juce::int64 samplePos) const
{
    return (int) std::llround ((double) samplePos / samplesPerPixel());
}

juce::int64 TimelineView::xToSample (int x) const
{
    return (juce::int64) std::llround ((double) x * samplesPerPixel());
}

void TimelineView::resized()
{
    viewport->setBounds (0, rulerHeight, getWidth(), getHeight() - rulerHeight);
    updateContentSize();
    syncScroll();
}

void TimelineView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e1e22));
}

void TimelineView::timerCallback()
{
    const auto playhead = transport.playheadSamplePos.load();
    const bool active = transport.isPlaying.load() || transport.recordArmed.load();
    if (playhead == lastPaintedPlayhead && ! active)
        return;

    lastPaintedPlayhead = playhead;
    updateContentSize();

    // 再生ヘッドが見切れたら追従スクロール
    if (transport.isPlaying.load())
    {
        const int playheadX = sampleToX (playhead);
        const auto view = viewport->getViewArea();
        if (playheadX < view.getX() || playheadX > view.getRight() - 60)
            viewport->setViewPosition (juce::jmax (0, playheadX - 60), view.getY());
    }

    lanes->repaint();
    ruler->repaint();
}

void TimelineView::updateContentSize()
{
    const double barLen = barLengthSamples();

    juce::int64 maxSample = juce::jmax ((juce::int64) 0, transport.playheadSamplePos.load());
    if (project != nullptr)
        for (auto& track : project->tracks)
            for (auto& clip : track.clips)
                maxSample = juce::jmax (maxSample, clip.startSample + clip.lengthSamples());
    if (transport.recordArmed.load())
        maxSample = juce::jmax (maxSample,
                                transport.punchInSample.load() + transport.recordedSamples.load());

    const int numBars = juce::jmax (64, (int) std::ceil ((double) maxSample / barLen) + 8);
    const int contentWidth = (int) std::ceil (numBars * pxPerBar);
    const int numTracks = project != nullptr ? (int) project->tracks.size() : 0;
    const int contentHeight = juce::jmax (numTracks * trackHeight,
                                          viewport->getMaximumVisibleHeight());

    if (contentWidth != lanes->getWidth() || contentHeight != lanes->getHeight())
        lanes->setSize (contentWidth, contentHeight);
    if (contentWidth != ruler->getWidth() || ruler->getHeight() != rulerHeight)
        ruler->setSize (contentWidth, rulerHeight);
}

void TimelineView::syncScroll()
{
    ruler->setTopLeftPosition (-viewport->getViewPositionX(), 0);
    if (onVerticalScroll)
        onVerticalScroll (viewport->getViewPositionY());
}

void TimelineView::scrollVertically (float wheelDeltaY)
{
    viewport->setViewPosition (viewport->getViewPositionX(),
                               juce::jmax (0, viewport->getViewPositionY()
                                                  - (int) (wheelDeltaY * 100.0f)));
}

void TimelineView::handleLaneMouseDown (const juce::MouseEvent& e)
{
    if (project == nullptr)
        return;

    const int numTracks = (int) project->tracks.size();
    const int row = e.y / trackHeight;
    if (row >= 0 && row < numTracks && onTrackSelected)
        onTrackSelected (row);

    // クリップのヒット判定（重なりは後勝ち＝後から録ったものを優先）
    const auto samplePos = xToSample (e.x);
    if (row >= 0 && row < numTracks)
    {
        auto& clips = project->tracks[(size_t) row].clips;
        for (int ci = (int) clips.size() - 1; ci >= 0; --ci)
        {
            auto& clip = clips[(size_t) ci];
            if (samplePos >= clip.startSample && samplePos < clip.startSample + clip.lengthSamples())
            {
                selection = { row, ci };
                if (onSelectionChanged)
                    onSelectionChanged();
                lanes->repaint();
                return;
            }
        }
    }

    // 空白クリック → 選択解除＋シーク（表示グリッドにスナップ）
    if (selection.isValid())
    {
        selection.clear();
        if (onSelectionChanged)
            onSelectionChanged();
    }
    seekFromX (e.x);
    lanes->repaint();
    ruler->repaint();
}

void TimelineView::seekFromX (int x)
{
    // 表示中の最小グリッド単位にスナップ（ズームが深いほど細かく移動できる）
    const double gridLen = barLengthSamples() / gridDivisionsPerBar();
    const auto samplePos = juce::jmax ((juce::int64) 0, xToSample (x));
    const auto gridIndex = (juce::int64) std::floor ((double) samplePos / gridLen);
    if (onSeek)
        onSeek ((juce::int64) std::llround ((double) gridIndex * gridLen));
}
