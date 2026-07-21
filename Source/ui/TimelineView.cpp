#include "TimelineView.h"

#include <cmath>

#include "Fonts.h"

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
                g.setFont (Fonts::mono (11.0f));
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

        // クリップ（オーディオ）とMIDIリージョン
        for (int t = 0; t < numTracks; ++t)
        {
            const int y = t * trackHeight;
            if (y > clip.getBottom() || y + trackHeight < clip.getY())
                continue;

            auto& track = proj->tracks[(size_t) t];
            if (track.type == TrackType::audio)
            {
                for (int ci = 0; ci < (int) track.clips.size(); ++ci)
                {
                    const bool isSelected = (owner.selection.track == t && owner.selection.clip == ci);
                    drawClip (g, track.clips[(size_t) ci], y, isSelected, clip);
                }
            }
            else
            {
                for (int ri = 0; ri < (int) track.midiRegions.size(); ++ri)
                {
                    const bool isSelected = (owner.regionSelection.track == t
                                             && owner.regionSelection.region == ri);
                    drawMidiRegion (g, track.midiRegions[(size_t) ri], y, isSelected, clip);
                }
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

    void mouseDrag (const juce::MouseEvent& e) override
    {
        owner.handleLaneMouseDrag (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        owner.handleLaneMouseUp (e);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        owner.handleLaneDoubleClick (e);
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

    void drawMidiRegion (juce::Graphics& g, const MidiRegion& region, int y, bool isSelected,
                         const juce::Rectangle<int>& clipRegion)
    {
        const int x = owner.ppqToX (region.startPpq);
        const int w = juce::jmax (2, owner.ppqToX (region.startPpq + region.lengthPpq) - x);
        if (x > clipRegion.getRight() || x + w < clipRegion.getX())
            return;

        const auto rect = juce::Rectangle<int> (x, y + 4, w, trackHeight - 8);
        g.setColour (isSelected ? juce::Colour (0xff4a9968) : juce::Colour (0xff3a7350));
        g.fillRoundedRectangle (rect.toFloat(), 4.0f);
        if (isSelected)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (rect.toFloat().reduced (0.5f), 4.0f, 1.5f);
        }

        // ノートのミニチュア（ピッチ範囲 C1..C7 に射影。範囲外はクランプ）
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        constexpr int loPitch = 24, hiPitch = 96;
        const auto inner = rect.reduced (1, 3);
        const double tickW = (double) w / (double) juce::jmax ((juce::int64) 1, region.lengthPpq);

        for (const auto& note : region.notes)
        {
            const int nx = rect.getX() + (int) std::llround ((double) note.startPpq * tickW);
            const int nw = juce::jmax (2, (int) std::llround ((double) note.lengthPpq * tickW));
            const int clamped = juce::jlimit (loPitch, hiPitch, note.pitch);
            const float rel = (float) (hiPitch - clamped) / (float) (hiPitch - loPitch);
            const int ny = inner.getY() + (int) (rel * (float) (inner.getHeight() - 2));
            g.fillRect (juce::jmax (nx, rect.getX()), ny,
                        juce::jmin (nw, rect.getRight() - nx), 2);
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
    if (! selection.isValid() && ! regionSelection.isValid())
        return;
    selection.clear();
    regionSelection.clear();
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

double TimelineView::samplesPerTick() const
{
    const double bpm = juce::jlimit (20.0, 400.0, transport.bpm.load());
    return 1.0 / Ppq::ticksPerSample (bpm, effectiveSampleRate());
}

int TimelineView::ppqToX (juce::int64 ppq) const
{
    return (int) std::llround ((double) ppq * samplesPerTick() / samplesPerPixel());
}

juce::int64 TimelineView::xToPpq (int x) const
{
    return (juce::int64) std::llround ((double) x * samplesPerPixel() / samplesPerTick());
}

juce::int64 TimelineView::gridPpq() const
{
    return Ppq::ticksPerBar / gridDivisionsPerBar();
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
    {
        const double spt = samplesPerTick();
        for (auto& track : project->tracks)
        {
            for (auto& clip : track.clips)
                maxSample = juce::jmax (maxSample, clip.startSample + clip.lengthSamples());
            for (auto& region : track.midiRegions)
                maxSample = juce::jmax (maxSample,
                                        (juce::int64) std::llround ((double) (region.startPpq + region.lengthPpq) * spt));
        }
    }
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
    regionDrag = {};
    if (project == nullptr)
        return;

    const int numTracks = (int) project->tracks.size();
    const int row = e.y / trackHeight;
    if (row >= 0 && row < numTracks && onTrackSelected)
        onTrackSelected (row);

    if (row >= 0 && row < numTracks)
    {
        auto& track = project->tracks[(size_t) row];

        if (track.type == TrackType::midi)
        {
            const int ri = hitTestRegion (row, e.x);
            if (ri >= 0)
            {
                selection.clear();
                regionSelection = { row, ri };
                auto& region = track.midiRegions[(size_t) ri];

                // 右端8px以内はリサイズ（⌥ドラッグは常に移動＝複製）
                const int rightX = ppqToX (region.startPpq + region.lengthPpq);
                regionDrag.mode = (! e.mods.isAltDown() && e.x >= rightX - 8)
                                      ? RegionDrag::Mode::resize
                                      : RegionDrag::Mode::move;
                regionDrag.track = row;
                regionDrag.region = ri;
                regionDrag.origStartPpq = region.startPpq;
                regionDrag.origLengthPpq = region.lengthPpq;
                regionDrag.startX = e.x;
                regionDrag.duplicateOnDrag = e.mods.isAltDown(); // 複製は実際に動いた時点で作る

                if (onSelectionChanged)
                    onSelectionChanged();
                lanes->repaint();
                return;
            }
        }
        else
        {
            // クリップのヒット判定（重なりは後勝ち＝後から録ったものを優先）
            const auto samplePos = xToSample (e.x);
            auto& clips = track.clips;
            for (int ci = (int) clips.size() - 1; ci >= 0; --ci)
            {
                auto& clip = clips[(size_t) ci];
                if (samplePos >= clip.startSample && samplePos < clip.startSample + clip.lengthSamples())
                {
                    selection = { row, ci };
                    regionSelection.clear();
                    if (onSelectionChanged)
                        onSelectionChanged();
                    lanes->repaint();
                    return;
                }
            }
        }
    }

    // 空白クリック → 選択解除＋シーク（表示グリッドにスナップ）
    if (selection.isValid() || regionSelection.isValid())
    {
        selection.clear();
        regionSelection.clear();
        if (onSelectionChanged)
            onSelectionChanged();
    }
    seekFromX (e.x);
    lanes->repaint();
    ruler->repaint();
}

void TimelineView::handleLaneMouseDrag (const juce::MouseEvent& e)
{
    if (regionDrag.mode == RegionDrag::Mode::none || project == nullptr)
        return;
    if (regionDrag.track >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) regionDrag.track];
    if (regionDrag.region >= (int) track.midiRegions.size())
        return;

    const auto deltaPpq = xToPpq (e.x) - xToPpq (regionDrag.startX);
    if (! regionDrag.edited)
    {
        if (deltaPpq == 0)
            return;
        if (onWillEditModel)
            onWillEditModel();
        regionDrag.edited = true;

        // ⌥ドラッグ: 動き始めた今、複製を作ってドラッグ対象を差し替える
        if (regionDrag.duplicateOnDrag && project != nullptr)
        {
            MidiRegion copy = track.midiRegions[(size_t) regionDrag.region];
            copy.id = project->allocateId();
            for (auto& note : copy.notes)
                note.id = project->allocateId();
            track.midiRegions.push_back (std::move (copy));
            regionDrag.region = (int) track.midiRegions.size() - 1;
            regionSelection = { regionDrag.track, regionDrag.region };
            if (onSelectionChanged)
                onSelectionChanged();
        }
    }

    auto& region = track.midiRegions[(size_t) regionDrag.region];
    const auto grid = juce::jmax ((juce::int64) 1, gridPpq());

    if (regionDrag.mode == RegionDrag::Mode::move)
    {
        const auto snapped = (juce::int64) std::llround ((double) (regionDrag.origStartPpq + deltaPpq)
                                                         / (double) grid) * grid;
        region.startPpq = juce::jmax ((juce::int64) 0, snapped);
    }
    else
    {
        const auto snapped = (juce::int64) std::llround ((double) (regionDrag.origLengthPpq + deltaPpq)
                                                         / (double) grid) * grid;
        region.lengthPpq = juce::jmax (grid, snapped);
    }
    lanes->repaint();
}

void TimelineView::handleLaneMouseUp (const juce::MouseEvent&)
{
    if (regionDrag.mode != RegionDrag::Mode::none && regionDrag.edited)
    {
        updateContentSize();
        if (onModelEdited)
            onModelEdited();
    }
    regionDrag = {};
}

void TimelineView::handleLaneDoubleClick (const juce::MouseEvent& e)
{
    if (project == nullptr)
        return;
    const int row = e.y / trackHeight;
    if (row < 0 || row >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) row];
    if (track.type != TrackType::midi)
        return;

    const int ri = hitTestRegion (row, e.x);
    if (ri >= 0)
    {
        if (onOpenRegion)
            onOpenRegion (row, ri); // ピアノロールを開く（Phase 4）
        return;
    }

    // 空エリアのダブルクリック → 1小節のリージョンを作成（小節頭にスナップ）
    if (onWillEditModel)
        onWillEditModel();

    MidiRegion region;
    region.id = project->allocateId();
    region.startPpq = juce::jmax ((juce::int64) 0,
                                  (xToPpq (e.x) / Ppq::ticksPerBar) * Ppq::ticksPerBar);
    region.lengthPpq = Ppq::ticksPerBar;
    track.midiRegions.push_back (std::move (region));

    selection.clear();
    regionSelection = { row, (int) track.midiRegions.size() - 1 };
    updateContentSize();
    if (onSelectionChanged)
        onSelectionChanged();
    if (onModelEdited)
        onModelEdited();
    lanes->repaint();
}

int TimelineView::hitTestRegion (int trackIndex, int x) const
{
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return -1;
    const auto& track = project->tracks[(size_t) trackIndex];
    if (track.type != TrackType::midi)
        return -1;

    for (int ri = (int) track.midiRegions.size() - 1; ri >= 0; --ri)
    {
        const auto& region = track.midiRegions[(size_t) ri];
        const int x0 = ppqToX (region.startPpq);
        const int x1 = ppqToX (region.startPpq + region.lengthPpq);
        if (x >= x0 && x <= x1)
            return ri;
    }
    return -1;
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
