#include "PianoRollView.h"

#include <algorithm>
#include <cmath>

#include "../shared/GmInstruments.h"
#include "Fonts.h"
#include "Theme.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

bool isBlackKey (int pitch)
{
    switch (pitch % 12)
    {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}
} // namespace

// ---- 内部コンポーネント -------------------------------------------------

// 左端の鍵盤カラム。クリックでそのピッチをプレビュー発音する
class PianoRollView::KeyboardContent : public juce::Component
{
public:
    explicit KeyboardContent (PianoRollView& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        const auto clip = g.getClipBounds();
        for (int pitch = 0; pitch < 128; ++pitch)
        {
            const int y = (127 - pitch) * rowHeight;
            if (y > clip.getBottom() || y + rowHeight < clip.getY())
                continue;

            g.setColour (isBlackKey (pitch) ? Theme::prBlackKey : Theme::prWhiteKey);
            g.fillRect (0, y, getWidth(), rowHeight);
            g.setColour (Theme::prKeyOutline);
            g.drawHorizontalLine (y, 0.0f, (float) getWidth());

            // ドラムキット選択時はGMドラム名、それ以外はCだけ音名（C3 = 60 のLogic式表記）
            auto* track = owner.findTrack();
            const bool drums = track != nullptr && track->drums;
            const char* drumName = drums ? gmDrumName (pitch) : nullptr;
            if (drumName != nullptr)
            {
                g.setColour (isBlackKey (pitch) ? Theme::prKeyLabelLight : Theme::prKeyLabelDark);
                g.setFont (Fonts::small());
                g.drawText (drumName, 2, y, getWidth() - 6, rowHeight, juce::Justification::centredRight);
            }
            else if (! drums && pitch % 12 == 0)
            {
                g.setColour (Theme::prKeyLabelDark);
                g.setFont (Fonts::small());
                g.drawText (juce::MidiMessage::getMidiNoteName (pitch, true, true, 3),
                            2, y, getWidth() - 6, rowHeight, juce::Justification::centredRight);
            }
        }
        g.setColour (Theme::prKeyboardBorder);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int pitch = juce::jlimit (0, 127, 127 - e.y / rowHeight);
        owner.preview (pitch, 100);
    }

private:
    PianoRollView& owner;
};

class PianoRollView::GridViewport : public juce::Viewport
{
public:
    explicit GridViewport (PianoRollView& o) : owner (o) {}

    void visibleAreaChanged (const juce::Rectangle<int>&) override
    {
        owner.keyboard->setTopLeftPosition (0, -getViewPositionY());
    }

private:
    PianoRollView& owner;
};

// ノートグリッド本体
class PianoRollView::GridContent : public juce::Component
{
public:
    explicit GridContent (PianoRollView& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::timelineBg);
        const auto clip = g.getClipBounds();
        auto* region = owner.findRegion();
        if (region == nullptr)
            return;

        // 行背景（黒鍵行を暗く・固定ピッチ打楽器はその行をハイライト）と行区切り
        const int forced = owner.forcedPitch();
        for (int pitch = 0; pitch < 128; ++pitch)
        {
            const int y = (127 - pitch) * rowHeight;
            if (y > clip.getBottom() || y + rowHeight < clip.getY())
                continue;
            if (pitch == forced)
            {
                g.setColour (Theme::prRegionTint);
                g.fillRect (clip.getX(), y, clip.getWidth(), rowHeight);
            }
            else if (isBlackKey (pitch))
            {
                g.setColour (Theme::prRowDark);
                g.fillRect (clip.getX(), y, clip.getWidth(), rowHeight);
            }
            g.setColour (pitch % 12 == 11 ? Theme::prLineStrong   // B/Cの境界を強調
                                          : Theme::prLineFaint);
            g.drawHorizontalLine (y, (float) clip.getX(), (float) clip.getRight());
        }

        // 縦グリッド（スナップ解像度で描き、拍・小節を強調）
        const auto snap = owner.snapTicks();
        for (juce::int64 tick = 0; tick <= region->lengthPpq; tick += snap)
        {
            const int x = owner.ppqToX (tick);
            if (x < clip.getX() - 1 || x > clip.getRight() + 1)
                continue;
            g.setColour (tick % Ppq::ticksPerBar == 0       ? Theme::prGridBar
                         : tick % Ppq::ticksPerQuarter == 0 ? Theme::prLineStrong
                                                            : Theme::prLineFaint);
            g.drawVerticalLine (x, (float) clip.getY(), (float) clip.getBottom());
        }

        // ノート
        for (const auto& note : region->notes)
        {
            const auto rect = owner_noteRect (note);
            if (rect.getX() > clip.getRight() || rect.getRight() < clip.getX())
                continue;
            const bool isSelected = owner.isSelected (note.id);
            // ベロシティで彩度を変える（弱=暗め）
            const float bright = 0.45f + 0.55f * (float) note.velocity / 127.0f;
            g.setColour (isSelected ? juce::Colours::white
                                    : juce::Colour::fromFloatRGBA (0.30f * bright, 0.62f * bright, 0.42f * bright, 1.0f));
            g.fillRoundedRectangle (rect.toFloat(), 2.0f);
            if (isSelected)
            {
                g.setColour (Theme::regionMidi);
                g.drawRoundedRectangle (rect.toFloat().reduced (0.5f), 2.0f, 1.0f);
            }
        }

        // ラバーバンド選択の矩形
        if (owner.noteDrag.mode == NoteDrag::Mode::rubberBand)
        {
            g.setColour (juce::Colours::white.withAlpha (0.12f));
            g.fillRect (owner.noteDrag.rubberRect);
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.drawRect (owner.noteDrag.rubberRect);
        }

        // 再生ヘッド（リージョン内にあるときだけ）
        const auto playheadPpq = owner.playheadRegionPpq();
        if (playheadPpq >= 0 && playheadPpq <= region->lengthPpq)
        {
            g.setColour (juce::Colours::white.withAlpha (0.8f));
            g.drawVerticalLine (owner.ppqToX (playheadPpq), (float) clip.getY(), (float) clip.getBottom());
        }
    }

    juce::Rectangle<int> owner_noteRect (const MidiNote& note) const
    {
        const int x = owner.ppqToX (note.startPpq);
        const int w = juce::jmax (3, owner.ppqToX (note.startPpq + note.lengthPpq) - x);
        return { x, (127 - note.pitch) * rowHeight + 1, w, rowHeight - 2 };
    }

    void mouseDown (const juce::MouseEvent& e) override { owner.handleGridMouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override { owner.handleGridMouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override { owner.handleGridMouseUp (e); }
    void mouseDoubleClick (const juce::MouseEvent& e) override { owner.handleGridDoubleClick (e); }

    void mouseMagnify (const juce::MouseEvent& e, float scaleFactor) override
    {
        owner.pxPerBar = juce::jlimit (80.0, 2560.0, owner.pxPerBar * (double) scaleFactor);
        juce::ignoreUnused (e);
        owner.updateContentSize();
        repaint();
    }

private:
    PianoRollView& owner;
};

// ---- PianoRollView 本体 --------------------------------------------------

PianoRollView::PianoRollView (TransportState& transportState)
    : transport (transportState)
{
    keyboard = std::make_unique<KeyboardContent> (*this);
    grid = std::make_unique<GridContent> (*this);
    viewport = std::make_unique<GridViewport> (*this);

    viewport->setViewedComponent (grid.get(), false);
    viewport->setScrollBarsShown (true, true);

    addAndMakeVisible (titleLabel);
    addAndMakeVisible (gridBox);
    addAndMakeVisible (velocityCaption);
    addAndMakeVisible (velocitySlider);
    addAndMakeVisible (closeButton);
    addAndMakeVisible (keyboardHolder);
    keyboardHolder.addAndMakeVisible (keyboard.get());
    addAndMakeVisible (*viewport);

    titleLabel.setFont (Fonts::body());

    velocityCaption.setText ("Vel", juce::dontSendNotification);
    velocityCaption.setFont (Fonts::body());
    velocityCaption.setJustificationType (juce::Justification::centredRight);
    velocitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    velocitySlider.setRange (1.0, 127.0, 1.0);
    velocitySlider.setValue (100.0, juce::dontSendNotification);
    velocitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 34, 18);
    velocitySlider.onDragStart = [this]
    {
        if (! selectedIds.empty())
        {
            if (onWillEditModel)
                onWillEditModel();
            velocityDragActive = true;
        }
    };
    velocitySlider.onValueChange = [this]
    {
        if (! velocityDragActive)
            return; // bind時の setValue では発火させない
        if (auto* region = findRegion())
        {
            for (auto& note : region->notes)
                if (isSelected (note.id))
                {
                    note.velocity = (int) velocitySlider.getValue();
                    region->clampNote (note);
                }
            grid->repaint();
        }
    };
    velocitySlider.onDragEnd = [this]
    {
        if (velocityDragActive)
        {
            velocityDragActive = false;
            if (onModelEdited)
                onModelEdited();
        }
    };

    gridBox.addItem (jp (u8"グリッド: 自動"), 1);
    gridBox.addItem ("1/8", 2);
    gridBox.addItem ("1/16", 3);
    gridBox.addItem ("1/32", 4);
    gridBox.addItem (jp (u8"1/8 三連"), 5);
    gridBox.addItem (jp (u8"1/16 三連"), 6);
    gridBox.addItem (jp (u8"1/32 三連"), 7);
    gridBox.setSelectedId (1, juce::dontSendNotification);
    gridBox.onChange = [this] { grid->repaint(); };

    closeButton.onClick = [this]
    {
        if (onCloseRequested)
            onCloseRequested();
    };

    for (auto* c : std::initializer_list<juce::Component*> { &gridBox, &closeButton, &velocitySlider })
    {
        c->setWantsKeyboardFocus (false);
        c->setMouseClickGrabsKeyboardFocus (false);
    }

    startTimerHz (30);
}

PianoRollView::~PianoRollView() = default;

void PianoRollView::setProject (Project* p)
{
    project = p;
    close();
}

void PianoRollView::openRegion (juce::uint64 trackId, juce::uint64 regionId)
{
    shownTrackId = trackId;
    shownRegionId = regionId;
    selectedIds.clear();
    open = true;
    setVisible (true);

    auto* track = findTrack();
    auto* region = findRegion();
    if (track != nullptr && region != nullptr)
    {
        titleLabel.setText (track->name + " — " + jp (u8"小節 ")
                                + juce::String (region->startPpq / Ppq::ticksPerBar + 1),
                            juce::dontSendNotification);
        // 最初のノート（無ければ固定ピッチ or C3=60）が見える位置へスクロール
        scrollToPitch (! region->notes.empty() ? region->notes.front().pitch
                       : forcedPitch() >= 0    ? forcedPitch()
                                               : 60);
    }
    updateContentSize();
    grid->repaint();
}

void PianoRollView::close()
{
    open = false;
    setVisible (false);
    selectedIds.clear();
    noteDrag = {};
}

void PianoRollView::refreshFromModel()
{
    if (! open)
        return;
    if (findRegion() == nullptr)
    {
        if (onCloseRequested)
            onCloseRequested(); // 対象が消えた（undo・削除）ので閉じてもらう
        return;
    }
    pruneSelection();
    updateContentSize();
    grid->repaint();
}

// ---- 選択 ----

bool PianoRollView::isSelected (juce::uint64 noteId) const
{
    return std::find (selectedIds.begin(), selectedIds.end(), noteId) != selectedIds.end();
}

int PianoRollView::forcedPitch() const
{
    auto* track = findTrack();
    return (track != nullptr && track->drums && track->drumPitch >= 0) ? track->drumPitch : -1;
}

void PianoRollView::pruneSelection()
{
    auto* region = findRegion();
    selectedIds.erase (std::remove_if (selectedIds.begin(), selectedIds.end(),
                                       [region] (juce::uint64 id)
                                       {
                                           if (region == nullptr)
                                               return true;
                                           for (auto& note : region->notes)
                                               if (note.id == id)
                                                   return false;
                                           return true;
                                       }),
                       selectedIds.end());
    syncVelocitySlider();
}

void PianoRollView::syncVelocitySlider()
{
    if (auto* region = findRegion())
        for (auto& note : region->notes)
            if (isSelected (note.id))
            {
                velocitySlider.setValue (note.velocity, juce::dontSendNotification);
                return;
            }
}

// ---- キーボード操作 ----

bool PianoRollView::deleteSelectedNotes()
{
    if (! open || selectedIds.empty())
        return false;
    auto* region = findRegion();
    if (region == nullptr)
        return false;

    if (onWillEditModel)
        onWillEditModel();
    region->notes.erase (std::remove_if (region->notes.begin(), region->notes.end(),
                                         [this] (const MidiNote& n) { return isSelected (n.id); }),
                         region->notes.end());
    selectedIds.clear();
    if (onModelEdited)
        onModelEdited();
    grid->repaint();
    return true;
}

bool PianoRollView::transposeSelection (int semitones)
{
    if (! open || selectedIds.empty())
        return false;
    auto* region = findRegion();
    if (region == nullptr)
        return false;

    if (onWillEditModel)
        onWillEditModel();
    const int forced = forcedPitch();
    int previewPitch = -1;
    for (auto& note : region->notes)
        if (isSelected (note.id))
        {
            note.pitch = forced >= 0 ? forced : note.pitch + semitones;
            region->clampNote (note);
            previewPitch = juce::jmax (previewPitch, note.pitch);
        }
    if (previewPitch >= 0)
        preview (previewPitch, 100);
    if (onModelEdited)
        onModelEdited();
    grid->repaint();
    return true;
}

bool PianoRollView::copySelection()
{
    if (! open || selectedIds.empty())
        return false;
    auto* region = findRegion();
    if (region == nullptr)
        return false;

    clipboard.clear();
    juce::int64 minStart = std::numeric_limits<juce::int64>::max();
    for (auto& note : region->notes)
        if (isSelected (note.id))
        {
            clipboard.push_back (note);
            minStart = juce::jmin (minStart, note.startPpq);
        }
    for (auto& note : clipboard)
        note.startPpq -= minStart; // 先頭ノート基準の相対位置で保持
    return ! clipboard.empty();
}

bool PianoRollView::pasteAtPlayhead()
{
    if (! open || clipboard.empty() || project == nullptr)
        return false;
    auto* region = findRegion();
    if (region == nullptr)
        return false;

    if (onWillEditModel)
        onWillEditModel();

    // 貼り付け位置 = 再生ヘッド（リージョン外なら先頭へ）をグリッドにスナップ
    const auto snap = snapTicks();
    auto basePpq = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, region->lengthPpq - 1),
                                 playheadRegionPpq());
    basePpq = (basePpq / snap) * snap;

    const int forced = forcedPitch();
    selectedIds.clear();
    for (auto note : clipboard)
    {
        note.id = project->allocateId();
        note.startPpq += basePpq;
        if (forced >= 0)
            note.pitch = forced;
        region->clampNote (note);
        region->notes.push_back (note);
        selectedIds.push_back (note.id);
    }
    syncVelocitySlider();
    if (onModelEdited)
        onModelEdited();
    grid->repaint();
    return true;
}

juce::int64 PianoRollView::playheadRegionPpq() const
{
    auto* region = findRegion();
    if (region == nullptr)
        return -1;
    const double bpm = juce::jlimit (20.0, 400.0, transport.bpm.load());
    const double sr = project != nullptr && project->sampleRate > 0.0
                          ? project->sampleRate
                          : (transport.sampleRate.load() > 0.0 ? transport.sampleRate.load() : 48000.0);
    const auto absPpq = (juce::int64) std::llround (
        (double) transport.playheadSamplePos.load() * Ppq::ticksPerSample (bpm, sr));
    return absPpq - region->startPpq;
}

// ---- モデル参照の解決 ----

Track* PianoRollView::findTrack() const
{
    if (project == nullptr)
        return nullptr;
    for (auto& track : project->tracks)
        if (track.id == shownTrackId && track.type == TrackType::midi)
            return &track;
    return nullptr;
}

MidiRegion* PianoRollView::findRegion() const
{
    if (auto* track = findTrack())
        for (auto& region : track->midiRegions)
            if (region.id == shownRegionId)
                return &region;
    return nullptr;
}

// ---- 換算 ----

double PianoRollView::pxPerTick() const
{
    return pxPerBar / (double) Ppq::ticksPerBar;
}

int PianoRollView::ppqToX (juce::int64 ppq) const
{
    return (int) std::llround ((double) ppq * pxPerTick());
}

juce::int64 PianoRollView::xToPpq (int x) const
{
    return (juce::int64) std::llround ((double) x / pxPerTick());
}

juce::int64 PianoRollView::snapTicks() const
{
    switch (gridBox.getSelectedId())
    {
        case 2: return Ppq::ticksPerQuarter / 2;      // 1/8 = 480
        case 3: return Ppq::ticksPerQuarter / 4;      // 1/16 = 240
        case 4: return Ppq::ticksPerQuarter / 8;      // 1/32 = 120
        case 5: return Ppq::ticksPerQuarter / 3;      // 1/8三連 = 320
        case 6: return Ppq::ticksPerQuarter / 6;      // 1/16三連 = 160
        case 7: return Ppq::ticksPerQuarter / 12;     // 1/32三連 = 80
        default: break;
    }
    // 自動: 線の間隔10px以上を確保できる最も細かい2分割系（1/4〜1/32）
    juce::int64 best = Ppq::ticksPerQuarter;
    for (juce::int64 t : { Ppq::ticksPerQuarter / 2, Ppq::ticksPerQuarter / 4, Ppq::ticksPerQuarter / 8 })
        if ((double) t * pxPerTick() >= 10.0)
            best = t;
    return best;
}

void PianoRollView::updateContentSize()
{
    auto* region = findRegion();
    const int contentWidth = region != nullptr
                                 ? juce::jmax (viewport->getMaximumVisibleWidth(), ppqToX (region->lengthPpq))
                                 : viewport->getMaximumVisibleWidth();
    const int contentHeight = 128 * rowHeight;
    if (grid->getWidth() != contentWidth || grid->getHeight() != contentHeight)
        grid->setSize (contentWidth, contentHeight);
    if (keyboard->getWidth() != keyboardWidth || keyboard->getHeight() != contentHeight)
        keyboard->setSize (keyboardWidth, contentHeight);
}

void PianoRollView::scrollToPitch (int pitch)
{
    const int y = (127 - pitch) * rowHeight - viewport->getMaximumVisibleHeight() / 2;
    viewport->setViewPosition (viewport->getViewPositionX(), juce::jmax (0, y));
}

void PianoRollView::preview (int pitch, int velocity)
{
    if (onPreviewNote && ! transport.isPlaying.load())
        onPreviewNote (shownTrackId, pitch, velocity);
}

// ---- マウス編集 ----

MidiNote* PianoRollView::hitTestNote (int x, int y) const
{
    auto* region = findRegion();
    if (region == nullptr)
        return nullptr;
    const int pitch = 127 - y / rowHeight;
    for (int i = (int) region->notes.size() - 1; i >= 0; --i)
    {
        auto& note = region->notes[(size_t) i];
        if (note.pitch != pitch)
            continue;
        const int x0 = ppqToX (note.startPpq);
        const int x1 = juce::jmax (x0 + 3, ppqToX (note.startPpq + note.lengthPpq));
        if (x >= x0 && x <= x1)
            return &note;
    }
    return nullptr;
}

void PianoRollView::handleGridMouseDown (const juce::MouseEvent& e)
{
    noteDrag = {};
    auto* region = findRegion();
    if (region == nullptr)
        return;

    if (auto* note = hitTestNote (e.x, e.y))
    {
        if (e.mods.isShiftDown())
        {
            // Shiftクリック: 選択のトグル（ドラッグは開始しない）
            if (isSelected (note->id))
                selectedIds.erase (std::remove (selectedIds.begin(), selectedIds.end(), note->id),
                                   selectedIds.end());
            else
                selectedIds.push_back (note->id);
            syncVelocitySlider();
            grid->repaint();
            return;
        }

        // 未選択ノートをクリックしたら単独選択に切り替え。選択済みならそのまま（複数移動できる）
        if (! isSelected (note->id))
        {
            selectedIds.clear();
            selectedIds.push_back (note->id);
        }
        syncVelocitySlider();

        const int rightX = ppqToX (note->startPpq + note->lengthPpq);
        noteDrag.mode = (e.x >= rightX - 6) ? NoteDrag::Mode::resize : NoteDrag::Mode::move;
        noteDrag.noteId = note->id;
        noteDrag.origLengthPpq = note->lengthPpq;
        noteDrag.startX = e.x;
        noteDrag.startY = e.y;
        noteDrag.lastPreviewedPitch = note->pitch;
        for (auto& n : region->notes)
            if (isSelected (n.id))
                noteDrag.origPositions.push_back ({ n.id, n.startPpq, n.pitch });
        preview (note->pitch, note->velocity);
        grid->repaint();
        return;
    }

    // 空エリア: ラバーバンド選択を開始（Shiftなしなら選択解除から）
    if (! e.mods.isShiftDown())
        selectedIds.clear();
    noteDrag.mode = NoteDrag::Mode::rubberBand;
    noteDrag.startX = e.x;
    noteDrag.startY = e.y;
    noteDrag.rubberRect = { e.x, e.y, 0, 0 };
    grid->repaint();
}

void PianoRollView::handleGridMouseDrag (const juce::MouseEvent& e)
{
    if (noteDrag.mode == NoteDrag::Mode::none)
        return;
    auto* region = findRegion();
    if (region == nullptr)
        return;

    if (noteDrag.mode == NoteDrag::Mode::rubberBand)
    {
        noteDrag.rubberRect = juce::Rectangle<int>::leftTopRightBottom (
            juce::jmin (noteDrag.startX, e.x), juce::jmin (noteDrag.startY, e.y),
            juce::jmax (noteDrag.startX, e.x), juce::jmax (noteDrag.startY, e.y));

        // 矩形に重なるノートを選択（Shift開始時の既存選択には足すだけ）
        for (auto& note : region->notes)
        {
            const int x0 = ppqToX (note.startPpq);
            const int x1 = juce::jmax (x0 + 3, ppqToX (note.startPpq + note.lengthPpq));
            const int y0 = (127 - note.pitch) * rowHeight;
            const juce::Rectangle<int> noteRect (x0, y0, x1 - x0, rowHeight);
            if (noteDrag.rubberRect.intersects (noteRect))
            {
                if (! isSelected (note.id))
                    selectedIds.push_back (note.id);
            }
            else if (! e.mods.isShiftDown() && isSelected (note.id))
            {
                selectedIds.erase (std::remove (selectedIds.begin(), selectedIds.end(), note.id),
                                   selectedIds.end());
            }
        }
        grid->repaint();
        return;
    }

    const auto deltaPpq = xToPpq (e.x) - xToPpq (noteDrag.startX);
    const int deltaPitch = -(e.y / rowHeight - noteDrag.startY / rowHeight);
    if (! noteDrag.edited)
    {
        if (deltaPpq == 0 && deltaPitch == 0)
            return;
        if (onWillEditModel)
            onWillEditModel();
        noteDrag.edited = true;
    }

    const auto snap = snapTicks();
    if (noteDrag.mode == NoteDrag::Mode::move)
    {
        // 選択ノート全部を同じデルタで動かす（固定ピッチ打楽器は縦移動しない）
        const int forced = forcedPitch();
        for (const auto& orig : noteDrag.origPositions)
        {
            for (auto& note : region->notes)
            {
                if (note.id != orig.id)
                    continue;
                note.startPpq = (juce::int64) std::llround ((double) (orig.startPpq + deltaPpq)
                                                            / (double) snap) * snap;
                note.pitch = forced >= 0 ? forced : orig.pitch + deltaPitch;
                region->clampNote (note);
                if (note.id == noteDrag.noteId && note.pitch != noteDrag.lastPreviewedPitch)
                {
                    noteDrag.lastPreviewedPitch = note.pitch;
                    preview (note.pitch, note.velocity);
                }
                break;
            }
        }
    }
    else // resize: 掴んだノートのみ
    {
        for (auto& note : region->notes)
        {
            if (note.id != noteDrag.noteId)
                continue;
            const auto snapped = (juce::int64) std::llround ((double) (noteDrag.origLengthPpq + deltaPpq)
                                                             / (double) snap) * snap;
            note.lengthPpq = juce::jmax (snap, snapped);
            region->clampNote (note);
            break;
        }
    }
    grid->repaint();
}

void PianoRollView::handleGridMouseUp (const juce::MouseEvent&)
{
    if (noteDrag.mode == NoteDrag::Mode::rubberBand)
        syncVelocitySlider();
    else if (noteDrag.mode != NoteDrag::Mode::none && noteDrag.edited)
        if (onModelEdited)
            onModelEdited();
    noteDrag = {};
    grid->repaint();
}

void PianoRollView::handleGridDoubleClick (const juce::MouseEvent& e)
{
    auto* region = findRegion();
    if (region == nullptr || project == nullptr)
        return;

    // ノート上のダブルクリック = 削除（Logic準拠）
    if (auto* note = hitTestNote (e.x, e.y))
    {
        selectedIds.clear();
        selectedIds.push_back (note->id);
        deleteSelectedNotes();
        return;
    }

    // 空エリアのダブルクリック = ノート作成（スナップ位置・グリッド1つ分の長さ）
    if (onWillEditModel)
        onWillEditModel();

    const auto snap = snapTicks();
    const int forced = forcedPitch();
    MidiNote note;
    note.id = project->allocateId();
    note.pitch = forced >= 0 ? forced : juce::jlimit (0, 127, 127 - e.y / rowHeight);
    note.startPpq = (xToPpq (e.x) / snap) * snap;
    note.lengthPpq = snap;
    note.velocity = 100;
    region->clampNote (note);
    region->notes.push_back (note);

    selectedIds.clear();
    selectedIds.push_back (note.id);
    syncVelocitySlider();
    preview (note.pitch, note.velocity);
    if (onModelEdited)
        onModelEdited();
    grid->repaint();
}

// ---- レイアウト・描画 ----

void PianoRollView::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (headerHeight).reduced (6, 3);
    closeButton.setBounds (header.removeFromRight (24));
    header.removeFromRight (6);
    gridBox.setBounds (header.removeFromRight (140));
    header.removeFromRight (10);
    velocitySlider.setBounds (header.removeFromRight (150));
    velocityCaption.setBounds (header.removeFromRight (30));
    titleLabel.setBounds (header);

    auto body = area;
    keyboardHolder.setBounds (body.removeFromLeft (keyboardWidth));
    keyboard->setBounds (0, -viewport->getViewPositionY(), keyboardWidth, 128 * rowHeight);
    viewport->setBounds (body);
    updateContentSize();
}

void PianoRollView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::prVelocityBg);
    g.setColour (Theme::prVelocityBorder);
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
}

void PianoRollView::timerCallback()
{
    if (! open)
        return;
    const auto playhead = transport.playheadSamplePos.load();
    if (playhead == lastPaintedPlayhead && ! transport.isPlaying.load())
        return;
    lastPaintedPlayhead = playhead;
    grid->repaint();
}
