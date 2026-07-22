#include "TimelineView.h"

#include <cmath>

#include "Fonts.h"
#include "Shortcuts.h"
#include "Theme.h"
#include "../shared/Log.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

// セクション種別ごとの固定色（ユーザー選択なし）。5種は有彩色、otherは「特定の種別ではない」ことが
// 一目でわかる無彩色。ダーク背景上で黒文字が読める明度に揃える
juce::Colour sectionColour (SectionType type)
{
    switch (type)
    {
        case SectionType::intro:  return Theme::sectionIntro;
        case SectionType::verse:  return Theme::sectionVerse;
        case SectionType::hook:   return Theme::sectionHook;
        case SectionType::bridge: return Theme::sectionBridge;
        case SectionType::outro:  return Theme::sectionOutro;
        case SectionType::other:  return Theme::sectionOther;
    }
    return Theme::sectionOther;
}
} // namespace

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
        g.fillAll (Theme::rulerBg);
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
                    g.setColour (Theme::rulerTickBar);
                    g.drawVerticalLine (x, 8.0f, (float) getHeight());
                }
                else if ((i * 4) % div == 0)   // 拍（1/2表示時はその線も同格に）
                {
                    g.setColour (Theme::rulerTickBeat);
                    g.drawVerticalLine (x, 14.0f, (float) getHeight());
                }
                else
                {
                    g.setColour (Theme::rulerTickSub);
                    g.drawVerticalLine (x, 19.0f, (float) getHeight());
                }
            }

            if (bar % labelStep == 0)
            {
                g.setColour (Theme::rulerLabel);
                g.setFont (Fonts::mono (11.0f));
                g.drawText (juce::String (bar + 1),
                            (int) std::llround (bar * barWidth) + 4, 0,
                            (int) (labelStep * barWidth) - 6, getHeight(),
                            juce::Justification::centredLeft);
            }
        }

        const int playheadX = owner.sampleToX (owner.transport.playheadSamplePos.load());
        g.setColour (Theme::playhead);
        g.drawVerticalLine (playheadX, 0.0f, (float) getHeight());

        // 三角の頭（下向き）。線1本だけでは広い画面で見失うため、
        // ルーラー上端に頭を付けて現在位置へ視線誘導する（Logic/Cubase等の定番）
        juce::Path head;
        const float cx = (float) playheadX + 0.5f;
        head.addTriangle (cx - 5.0f, 0.0f, cx + 5.0f, 0.0f, cx, 7.0f);
        g.fillPath (head.createPathWithRoundedCorners (1.5f));
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

class TimelineView::MarkerLaneContent : public juce::Component
{
public:
    explicit MarkerLaneContent (TimelineView& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::markerLaneBg);
        const auto clip = g.getClipBounds();

        if (auto* proj = owner.project)
        {
            const auto& markers = proj->markers;
            for (int i = 0; i < (int) markers.size(); ++i)
            {
                const int x0 = owner.beatToX (markers[(size_t) i].startBeats);
                const int x1 = i + 1 < (int) markers.size()
                                   ? owner.beatToX (markers[(size_t) (i + 1)].startBeats)
                                   : getWidth(); // 最後のセクションはコンテンツ右端（曲末）まで
                if (x1 <= clip.getX() || x0 >= clip.getRight())
                    continue;

                // ベタ塗りにせず「薄い色帯＋開始位置の色線＋色文字」で描く。マーカーは常時見る
                // 情報ではないので、プレイヘッドやリージョンより視覚的に沈める（Logic/Ableton式）。
                // 開始位置の縦線を残すことで、テキストが出ない狭いセクションでも境界が分かる
                const auto colour = sectionColour (markers[(size_t) i].type);
                g.setColour (colour.withAlpha (0.12f));
                g.fillRect (x0, 1, x1 - x0, getHeight() - 2);
                g.setColour (colour);
                g.fillRect (x0, 1, 3, getHeight() - 2);

                const int textW = x1 - x0 - 10;
                if (textW > 16)
                {
                    g.setColour (colour.brighter (0.4f));
                    g.setFont (Fonts::mono (11.0f));
                    g.drawText (SectionMarkers::displayName (markers, i),
                                x0 + 7, 0, textW, getHeight(), juce::Justification::centredLeft);
                }
            }
        }

        // 再生ヘッド（ルーラー・レーンの縦線と繋がって見えるように同じ白）
        const int playheadX = owner.sampleToX (owner.transport.playheadSamplePos.load());
        g.setColour (Theme::playhead);
        g.drawVerticalLine (playheadX, 0.0f, (float) getHeight());
    }

    void mouseDown (const juce::MouseEvent& e) override { owner.handleMarkerLaneMouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override { owner.handleMarkerLaneMouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override { owner.handleMarkerLaneMouseUp (e); }
    void mouseMove (const juce::MouseEvent& e) override { owner.handleMarkerLaneMouseMove (e); }

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
        g.fillAll (Theme::timelineBg);
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
                g.setColour (Theme::laneSelectedRowBg);
                g.fillRect (clip.getX(), y, clip.getWidth(), trackHeight);
            }
            g.setColour (Theme::panelBorder);
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
                g.setColour (i == 0             ? Theme::gridLineBar
                             : (i * 4) % div == 0 ? Theme::gridLineBeat
                                                  : Theme::gridLineSub);
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
            g.setColour (Theme::playhead.withAlpha (0.8f));
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
        const int w = juce::jmax (2, (int) ((double) clip.lengthSamples / spp));
        if (x > clipRegion.getRight() || x + w < clipRegion.getX())
            return;

        const auto rect = juce::Rectangle<int> (x, y + 4, w, trackHeight - 8);
        // ミュート中はグレー減光（Logic準拠）
        g.setColour (clip.muted ? (isSelected ? Theme::clipMutedSelected : Theme::clipMuted)
                                : (isSelected ? Theme::accent : Theme::clipAudio));
        g.fillRoundedRectangle (rect.toFloat(), 4.0f);
        if (isSelected)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (rect.toFloat().reduced (0.5f), 4.0f, 1.5f);
        }

        // 波形（ロード時に作ったピークキャッシュから描く）
        g.setColour (juce::Colours::white.withAlpha (clip.muted ? 0.3f : 0.75f));
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
        // ミュート中はグレー減光（Logic準拠）
        g.setColour (region.muted ? (isSelected ? Theme::clipMutedSelected : Theme::clipMuted)
                                  : (isSelected ? Theme::regionMidiSelected : Theme::regionMidi));
        g.fillRoundedRectangle (rect.toFloat(), 4.0f);
        if (isSelected)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (rect.toFloat().reduced (0.5f), 4.0f, 1.5f);
        }

        // ノートのミニチュア（ピッチ範囲 C1..C7 に射影。範囲外はクランプ）
        g.setColour (juce::Colours::white.withAlpha (region.muted ? 0.3f : 0.8f));
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
    markerLane = std::make_unique<MarkerLaneContent> (*this);
    lanes = std::make_unique<LaneContent> (*this);
    viewport = std::make_unique<LaneViewport> (*this);

    viewport->setViewedComponent (lanes.get(), false);
    viewport->setScrollBarsShown (true, true);

    addAndMakeVisible (*viewport);
    addAndMakeVisible (*ruler);
    addAndMakeVisible (*markerLane);

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
    markerLane->repaint();
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
    markerLane->repaint();
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

juce::int64 TimelineView::beatStartSample (int beats) const
{
    return (juce::int64) std::llround ((double) juce::jmax (0, beats) * barLengthSamples() / 4.0);
}

int TimelineView::beatToX (int beats) const
{
    return (int) std::llround ((double) juce::jmax (0, beats) * pxPerBar / 4.0);
}

int TimelineView::snapUnitBeats() const
{
    // 表示中グリッドと同じ刻みで置けるが、上限は拍（セクションは曲構造のラベルなので
    // 1/8以下の細かさは誤操作リスクにしかならない）。ズームアウト中は小節頭のまま
    return 4 / juce::jmin (gridDivisionsPerBar(), 4);
}

int TimelineView::xToMarkerBeats (int x) const
{
    const int unit = snapUnitBeats();
    const double unitPx = pxPerBar / 4.0 * unit;
    return juce::jmax (0, (int) std::floor ((double) x / unitPx)) * unit;
}

void TimelineView::resized()
{
    viewport->setBounds (0, topHeight, getWidth(), getHeight() - topHeight);
    updateContentSize();
    syncScroll();
}

void TimelineView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::timelineBg);
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
    markerLane->repaint();
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
                maxSample = juce::jmax (maxSample, clip.startSample + clip.lengthSamples);
            for (auto& region : track.midiRegions)
                maxSample = juce::jmax (maxSample,
                                        (juce::int64) std::llround ((double) (region.startPpq + region.lengthPpq) * spt));
        }
        // 後方のマーカー（素材より先の小節）もコンテンツ幅に含める（見えない・操作できないを防ぐ）
        if (! project->markers.empty())
            maxSample = juce::jmax (maxSample,
                                    beatStartSample (project->markers.back().startBeats + 4));
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
    if (contentWidth != markerLane->getWidth() || markerLane->getHeight() != markerLaneHeight)
        markerLane->setSize (contentWidth, markerLaneHeight);
}

void TimelineView::syncScroll()
{
    ruler->setTopLeftPosition (-viewport->getViewPositionX(), 0);
    markerLane->setTopLeftPosition (-viewport->getViewPositionX(), rulerHeight);
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

    // 右クリック: リージョン/クリップ上ならまず選択してからメニュー表示（ドラッグ・シークはしない）。
    // リージョン外の右クリックは何もしない
    if (e.mods.isPopupMenu())
    {
        if (row >= 0 && row < numTracks)
        {
            const auto& track = project->tracks[(size_t) row];
            const int item = track.type == TrackType::midi ? hitTestRegion (row, e.x)
                                                           : hitTestClip (row, e.x);
            if (item >= 0)
            {
                if (track.type == TrackType::midi)
                {
                    selection.clear();
                    regionSelection = { row, item };
                }
                else
                {
                    selection = { row, item };
                    regionSelection.clear();
                }
                if (onSelectionChanged)
                    onSelectionChanged();
                lanes->repaint();
                showItemMenu (row, item);
            }
        }
        return;
    }

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
            const int ci = hitTestClip (row, e.x);
            if (ci >= 0)
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

int TimelineView::hitTestClip (int trackIndex, int x) const
{
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return -1;
    const auto& track = project->tracks[(size_t) trackIndex];
    if (track.type != TrackType::audio)
        return -1;

    // 重なりは後勝ち＝後から録ったものを優先
    const auto samplePos = xToSample (x);
    for (int ci = (int) track.clips.size() - 1; ci >= 0; --ci)
    {
        const auto& clip = track.clips[(size_t) ci];
        if (samplePos >= clip.startSample && samplePos < clip.startSample + clip.lengthSamples)
            return ci;
    }
    return -1;
}

void TimelineView::showItemMenu (int trackIndex, int itemIndex)
{
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    const auto& track = project->tracks[(size_t) trackIndex];
    const bool isMidi = track.type == TrackType::midi;
    if (itemIndex < 0 || itemIndex >= (int) (isMidi ? track.midiRegions.size() : track.clips.size()))
        return;
    const bool muted = isMidi ? track.midiRegions[(size_t) itemIndex].muted
                              : track.clips[(size_t) itemIndex].muted;

    // 分割は再生ヘッドが対象の内側（境界を除く）にあるときだけ有効
    bool canSplit = false;
    if (isMidi)
    {
        const auto& region = track.midiRegions[(size_t) itemIndex];
        const auto splitPpq = playheadPpq();
        canSplit = splitPpq > region.startPpq && splitPpq < region.startPpq + region.lengthPpq;
    }
    else
    {
        const auto& clip = track.clips[(size_t) itemIndex];
        const auto playhead = transport.playheadSamplePos.load();
        canSplit = playhead > clip.startSample && playhead < clip.startSample + clip.lengthSamples;
    }

    // ショートカット持ちの項目は表記を横に出す（shortcutKeyDescriptionはsetterのない公開フィールド）
    const auto itemWithKey = [] (int id, const juce::String& text, Shortcuts::ID shortcutId,
                                 bool enabled = true)
    {
        juce::PopupMenu::Item item (text);
        item.itemID = id;
        item.isEnabled = enabled;
        item.shortcutKeyDescription = Shortcuts::keyText (shortcutId);
        return item;
    };

    juce::PopupMenu menu;
    menu.addItem (itemWithKey (1, muted ? jp (u8"ミュート解除") : jp (u8"ミュート"),
                               Shortcuts::ID::muteRegion));
    menu.addItem (2, jp (u8"複製"));
    menu.addItem (itemWithKey (4, jp (u8"再生ヘッド位置で分割"), Shortcuts::ID::split, canSplit));
    menu.addItem (itemWithKey (3, jp (u8"削除"), Shortcuts::ID::deleteItem));

    // コールバックは後から呼ばれるためSafePointerで寿命を確認し、右クリック時点の対象を捕捉して渡す。
    // メニュー表示中はモーダルで他の編集操作が発生せずインデックスは変化しない前提（各操作側でも範囲チェックする）
    juce::Component::SafePointer<TimelineView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options(),
                        [safe, trackIndex, itemIndex] (int result)
                        {
                            if (safe == nullptr)
                                return;
                            if (result == 1)
                                safe->toggleMuteAt (trackIndex, itemIndex);
                            else if (result == 2)
                                safe->duplicateAt (trackIndex, itemIndex);
                            else if (result == 3 && safe->onDeleteItemRequested)
                                safe->onDeleteItemRequested (trackIndex, itemIndex);
                            else if (result == 4)
                                safe->splitAtPlayhead (trackIndex, itemIndex);
                        });
}

void TimelineView::toggleMuteAt (int trackIndex, int itemIndex)
{
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    const bool isMidi = track.type == TrackType::midi;
    if (itemIndex < 0 || itemIndex >= (int) (isMidi ? track.midiRegions.size() : track.clips.size()))
        return;

    if (onWillEditModel)
        onWillEditModel();
    bool& muted = isMidi ? track.midiRegions[(size_t) itemIndex].muted
                         : track.clips[(size_t) itemIndex].muted;
    muted = ! muted;
    Log::info ("region.mute", "track=" + juce::String (trackIndex)
                                  + " item=" + juce::String (itemIndex)
                                  + " muted=" + (muted ? "1" : "0"));
    if (onModelEdited)
        onModelEdited();
    lanes->repaint();
}

void TimelineView::duplicateAt (int trackIndex, int itemIndex)
{
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];

    if (track.type == TrackType::midi)
    {
        if (itemIndex < 0 || itemIndex >= (int) track.midiRegions.size())
            return;
        if (onWillEditModel)
            onWillEditModel();
        MidiRegion copy = track.midiRegions[(size_t) itemIndex];
        copy.id = project->allocateId();
        for (auto& note : copy.notes)
            note.id = project->allocateId();
        copy.startPpq += copy.lengthPpq; // 元の終端直後（Logicのリピート相当）
        track.midiRegions.push_back (std::move (copy));
        selection.clear();
        regionSelection = { trackIndex, (int) track.midiRegions.size() - 1 };
    }
    else
    {
        if (itemIndex < 0 || itemIndex >= (int) track.clips.size())
            return;
        if (onWillEditModel)
            onWillEditModel();
        Clip copy = track.clips[(size_t) itemIndex]; // fileName/audioは共有、peakCacheは値コピー
        copy.startSample += copy.lengthSamples;      // 元の終端直後（Logicのリピート相当）
        track.clips.push_back (std::move (copy));
        selection = { trackIndex, (int) track.clips.size() - 1 };
        regionSelection.clear();
    }

    Log::info ("region.duplicate", "track=" + juce::String (trackIndex)
                                       + " item=" + juce::String (itemIndex));
    updateContentSize();
    if (onSelectionChanged)
        onSelectionChanged();
    if (onModelEdited)
        onModelEdited();
    lanes->repaint();
}

void TimelineView::splitAtPlayhead (int trackIndex, int itemIndex)
{
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    const auto playhead = transport.playheadSamplePos.load();

    // 左は元のindexを上書き、右は末尾に追加する（既存indexが動かないので選択の維持が単純になる。
    // 左右は重ならないため描画順が変わっても見た目に影響しない。duplicateAtと同じ方針）
    if (track.type == TrackType::midi)
    {
        if (itemIndex < 0 || itemIndex >= (int) track.midiRegions.size())
            return;
        MidiRegion left, right;
        if (! splitMidiRegion (track.midiRegions[(size_t) itemIndex], playheadPpq(), left, right))
            return;
        if (onWillEditModel)
            onWillEditModel();
        right.id = project->allocateId();
        track.midiRegions[(size_t) itemIndex] = std::move (left);
        track.midiRegions.push_back (std::move (right));
        selection.clear();
        regionSelection = { trackIndex, itemIndex }; // 左側を選択したまま
    }
    else
    {
        if (itemIndex < 0 || itemIndex >= (int) track.clips.size())
            return;
        Clip left, right;
        if (! splitClip (track.clips[(size_t) itemIndex], playhead, left, right))
            return;
        if (onWillEditModel)
            onWillEditModel();
        track.clips[(size_t) itemIndex] = std::move (left);
        track.clips.push_back (std::move (right));
        selection = { trackIndex, itemIndex };
        regionSelection.clear();
    }

    Log::info ("region.split", "track=" + juce::String (trackIndex)
                                   + " item=" + juce::String (itemIndex)
                                   + " pos=" + juce::String (playhead));
    if (onSelectionChanged)
        onSelectionChanged();
    if (onModelEdited)
        onModelEdited();
    lanes->repaint();
}

juce::int64 TimelineView::playheadPpq() const
{
    return (juce::int64) std::llround ((double) transport.playheadSamplePos.load() / samplesPerTick());
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

// ---- セクションマーカー -------------------------------------------------

int TimelineView::hitTestMarker (int x) const
{
    if (project == nullptr)
        return -1;

    // 区間 = 自分の開始x〜次のマーカーの開始x。最初のマーカーより前は無ラベル（-1）
    int found = -1;
    for (int i = 0; i < (int) project->markers.size(); ++i)
    {
        if (beatToX (project->markers[(size_t) i].startBeats) > x)
            break;
        found = i;
    }
    return found;
}

int TimelineView::hitTestMarkerEdge (int x) const
{
    if (project == nullptr)
        return -1;

    for (int i = 0; i < (int) project->markers.size(); ++i)
        if (std::abs (x - beatToX (project->markers[(size_t) i].startBeats)) <= 4)
            return i;
    return -1;
}

void TimelineView::handleMarkerLaneMouseDown (const juce::MouseEvent& e)
{
    markerDrag = {};
    if (project == nullptr)
        return;

    const int hit = hitTestMarker (e.x);
    if (e.mods.isPopupMenu())
    {
        if (hit >= 0)
            showMarkerMenu (hit, xToMarkerBeats (e.x));
        else
            showAddMarkerMenu (xToMarkerBeats (e.x));
        return;
    }

    // 境界・本体どちらを掴んでもドラッグで開始位置を移動できる。
    // 動かさず離したときだけシーク（mouseUpで判定）。空白は追加メニュー
    const int edge = hitTestMarkerEdge (e.x);
    const int target = edge >= 0 ? edge : hit;
    if (target >= 0)
    {
        markerDrag.index = target;
        markerDrag.origStartBeats = project->markers[(size_t) target].startBeats;
        markerDrag.startX = e.x;
        markerDrag.fromEdge = edge >= 0;
        return;
    }
    showAddMarkerMenu (xToMarkerBeats (e.x));
}

void TimelineView::handleMarkerLaneMouseDrag (const juce::MouseEvent& e)
{
    if (project == nullptr || markerDrag.index < 0
        || markerDrag.index >= (int) project->markers.size())
        return;

    // 境界掴みは最近傍のスナップ位置へ吸着、本体掴みは掴んだ位置からの相対移動。
    // 刻みは表示グリッド準拠（上限=拍）。どちらも隣のマーカーを越えない範囲にクランプ
    const int unit = snapUnitBeats();
    const double unitPx = pxPerBar / 4.0 * unit;
    const int wantedBeats = markerDrag.fromEdge
                                ? juce::jmax (0, (int) std::llround ((double) e.x / unitPx)) * unit
                                : markerDrag.origStartBeats
                                      + (int) std::llround ((double) (e.x - markerDrag.startX) / unitPx) * unit;
    const int targetBeats = SectionMarkers::clampStartBeats (project->markers, markerDrag.index, wantedBeats);

    auto& marker = project->markers[(size_t) markerDrag.index];
    if (targetBeats == marker.startBeats)
        return;

    if (! markerDrag.edited)
    {
        if (onWillEditModel)
            onWillEditModel();
        markerDrag.edited = true;
    }
    marker.startBeats = targetBeats;
    markerLane->repaint();
}

void TimelineView::handleMarkerLaneMouseUp (const juce::MouseEvent&)
{
    if (project != nullptr && markerDrag.index >= 0
        && markerDrag.index < (int) project->markers.size())
    {
        if (markerDrag.edited)
        {
            const auto& moved = project->markers[(size_t) markerDrag.index];
            Log::info ("marker.move", "index=" + juce::String (markerDrag.index)
                                          + " bar=" + juce::String (moved.bar())
                                          + " beat=" + juce::String (moved.beat()));
            updateContentSize();
            if (onModelEdited)
                onModelEdited();
        }
        else
        {
            // 動かさず離した＝クリック → セクション頭へシーク（「hookの頭からもう一回」の動線）。
            // 境界クリックも「境界から始まるセクション」の頭へのシークとして扱う。
            // 録音中のガードはonSeek側（MainComponent）が行う
            if (onSeek)
                onSeek (beatStartSample (project->markers[(size_t) markerDrag.index].startBeats));
        }
    }
    markerDrag = {};
}

void TimelineView::handleMarkerLaneMouseMove (const juce::MouseEvent& e)
{
    markerLane->setMouseCursor (hitTestMarkerEdge (e.x) >= 0
                                    ? juce::MouseCursor::LeftRightResizeCursor
                                    : juce::MouseCursor::NormalCursor);
}

void TimelineView::showAddMarkerMenu (int beats)
{
    if (project == nullptr || beats < 0)
        return;

    juce::PopupMenu menu;
    for (int i = 0; i < (int) std::size (SectionMarkers::allTypes); ++i)
        menu.addItem (i + 1, SectionMarkers::typeName (SectionMarkers::allTypes[i]));

    // コールバックは後から呼ばれるためSafePointerで寿命を確認する（showItemMenuと同じ方針）
    juce::Component::SafePointer<TimelineView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options(),
                        [safe, beats] (int result)
                        {
                            if (safe != nullptr && result > 0)
                                safe->addMarkerAt (beats, SectionMarkers::allTypes[result - 1]);
                        });
}

void TimelineView::showMarkerMenu (int markerIndex, int clickedBeats)
{
    if (project == nullptr || markerIndex < 0 || markerIndex >= (int) project->markers.size())
        return;

    const auto currentType = project->markers[(size_t) markerIndex].type;
    juce::PopupMenu addSub, typeSub;
    for (int i = 0; i < (int) std::size (SectionMarkers::allTypes); ++i)
    {
        const auto name = SectionMarkers::typeName (SectionMarkers::allTypes[i]);
        addSub.addItem (100 + i, name);
        typeSub.addItem (200 + i, name, true, SectionMarkers::allTypes[i] == currentType);
    }

    // 「ここに追加」= クリック位置の小節頭に境界を1本立てる（既存セクションの分割になる）
    juce::PopupMenu menu;
    menu.addSubMenu (jp (u8"ここにセクションを追加"), addSub);
    menu.addSubMenu (jp (u8"種別を変更"), typeSub);
    menu.addItem (1, jp (u8"削除"));

    juce::Component::SafePointer<TimelineView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options(),
                        [safe, markerIndex, clickedBeats] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            if (result == 1)
                                safe->removeMarker (markerIndex);
                            else if (result >= 200)
                                safe->changeMarkerType (markerIndex,
                                                        SectionMarkers::allTypes[result - 200]);
                            else if (result >= 100)
                                safe->addMarkerAt (clickedBeats,
                                                   SectionMarkers::allTypes[result - 100]);
                        });
}

void TimelineView::addMarkerAt (int beats, SectionType type)
{
    if (project == nullptr || beats < 0)
        return;
    // 同一位置への同種別は完全な変化なし → undo履歴を積まない
    for (const auto& marker : project->markers)
        if (marker.startBeats == beats && marker.type == type)
            return;

    if (onWillEditModel)
        onWillEditModel();
    SectionMarkers::set (project->markers, beats, type);
    Log::info ("marker.add", "bar=" + juce::String (beats / 4 + 1)
                                 + " beat=" + juce::String (beats % 4)
                                 + " type=" + SectionMarkers::typeName (type));
    updateContentSize();
    if (onModelEdited)
        onModelEdited();
    markerLane->repaint();
}

void TimelineView::changeMarkerType (int index, SectionType type)
{
    if (project == nullptr || index < 0 || index >= (int) project->markers.size())
        return;
    if (project->markers[(size_t) index].type == type)
        return;

    if (onWillEditModel)
        onWillEditModel();
    project->markers[(size_t) index].type = type;
    Log::info ("marker.type", "index=" + juce::String (index)
                                  + " type=" + SectionMarkers::typeName (type));
    if (onModelEdited)
        onModelEdited();
    markerLane->repaint();
}

void TimelineView::removeMarker (int index)
{
    if (project == nullptr || index < 0 || index >= (int) project->markers.size())
        return;

    if (onWillEditModel)
        onWillEditModel();
    Log::info ("marker.remove", "index=" + juce::String (index)
                                    + " bar=" + juce::String (project->markers[(size_t) index].bar())
                                    + " beat=" + juce::String (project->markers[(size_t) index].beat()));
    SectionMarkers::removeAt (project->markers, index);
    updateContentSize();
    if (onModelEdited)
        onModelEdited();
    markerLane->repaint();
}
