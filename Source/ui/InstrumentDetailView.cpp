#include "InstrumentDetailView.h"

#include <cmath>

#include "Fonts.h"
#include "StereoMeter.h"
#include "Theme.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

constexpr int rowHeight = 40;     // 設定行の高さ
constexpr int rowGap = 10;        // 波形と設定行の間
constexpr int trimHitHalfWidth = 6; // 頭カット線のヒット領域（±6px = 13px。実マウスで狙える幅）
} // namespace

// ---- 波形＋頭カット（ドラッグ）＋クリック試聴 --------------------------------

class InstrumentDetailView::WaveDisplay : public juce::Component
{
public:
    explicit WaveDisplay (InstrumentDetailView& o) : owner (o)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds();
        g.setColour (Theme::faderSlotBg);
        g.fillRoundedRectangle (bounds.toFloat(), 5.0f);

        auto* model = owner.track;
        if (model == nullptr || ! model->hasSample() || model->samplePeakCache.empty())
        {
            g.setColour (juce::Colours::white.withAlpha (0.3f));
            g.setFont (Fonts::small());
            g.drawText (jp (u8"サンプルがありません"), bounds, juce::Justification::centred);
            return;
        }

        const auto& peaks = model->samplePeakCache;
        const float midY = (float) bounds.getCentreY();
        const float halfH = (float) (bounds.getHeight() / 2 - 6);
        const int w = juce::jmax (1, bounds.getWidth());

        // 波形（クリップの描画と同じ流儀: ピークキャッシュを1px1本の縦線で描く）
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        for (int px = 0; px < w; ++px)
        {
            const int i0 = (int) ((double) px / (double) w * (double) peaks.size());
            const int i1 = juce::jmax (i0 + 1, (int) ((double) (px + 1) / (double) w * (double) peaks.size()));
            float peak = 0.0f;
            for (int i = i0; i < i1 && i < (int) peaks.size(); ++i)
                peak = juce::jmax (peak, peaks[(size_t) i]);
            // クリップ描画の1.4倍強調は掛けない（小さなレーンで見せるための補正で、
            // この大きさでは頭打ちになって減衰の形が読めなくなる）
            const float h = juce::jlimit (1.0f, halfH, peak * halfH);
            g.drawVerticalLine (px, midY - h, midY + h);
        }

        // 中央線（振幅0の基準。波形が薄いサンプルでも位置が分かる）
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawHorizontalLine (bounds.getCentreY(), 0.0f, (float) w);

        // 頭カット区間は減光（消さずに痕跡を見せる）＋境界の縦線
        const int cutX = trimX();
        if (cutX > 0)
        {
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRect (0, 0, cutX, bounds.getHeight());
        }
        g.setColour (Theme::panArcGreen);
        g.fillRect (cutX, 0, 1, bounds.getHeight());
        // つまみ（ドラッグできることを示す。ヒット領域と同じ幅）
        g.fillRect (juce::Rectangle<int> (trimHitHalfWidth * 2 - 1, 26)
                        .withCentre ({ cutX, bounds.getCentreY() }));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* model = owner.track;
        if (model == nullptr || ! model->hasSample())
            return;

        if (std::abs (e.x - trimX()) <= trimHitHalfWidth)
        {
            // ドラッグ1回＝undo1回（mouseDownで積み、mouseDrag中は積まない）
            dragging = true;
            if (owner.onWillEdit)
                owner.onWillEdit (true); // 頭カットは値のみの編集
            return;
        }

        // 頭カット線から離れた場所のクリックは試聴（ピアノロールと排他なので音を確かめる唯一の手段）
        if (owner.onPreview)
            owner.onPreview (model->sampleRootNote);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* model = owner.track;
        if (! dragging || model == nullptr || ! model->hasSample())
            return;

        const auto length = (juce::int64) model->sampleAudio->getNumSamples();
        const auto pos = (juce::int64) std::llround ((double) juce::jlimit (0, juce::jmax (1, getWidth()), e.x)
                                                    / (double) juce::jmax (1, getWidth()) * (double) length);
        model->sampleStartOffset = juce::jlimit ((juce::int64) 0, length - 1, pos);
        owner.trimLabel.setText (owner.trimText(), juce::dontSendNotification);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! dragging)
            return;
        dragging = false;
        // 確定はドラッグ終了時に1回（SamplerEngineのatomicへの反映もここ）。
        // 頭カットは次の発音から効く値なので、鳴っている音を切らないようスナップショットは再pushしない
        if (owner.onValueEdited)
            owner.onValueEdited();
    }

private:
    int trimX() const
    {
        auto* model = owner.track;
        if (model == nullptr || ! model->hasSample())
            return 0;
        const auto length = (juce::int64) juce::jmax (1, model->sampleAudio->getNumSamples());
        return (int) std::llround ((double) model->sampleStartOffset / (double) length
                                   * (double) juce::jmax (1, getWidth()));
    }

    InstrumentDetailView& owner;
    bool dragging = false;
};

// ---- InstrumentDetailView ---------------------------------------------------

InstrumentDetailView::InstrumentDetailView()
{
    wave = std::make_unique<WaveDisplay> (*this);
    addAndMakeVisible (*wave);

    addAndMakeVisible (sampleNameLabel);
    sampleNameLabel.setFont (Fonts::bodyStrong());
    sampleNameLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.9f));

    // 音程モードは2択のセグメント、MonoはON/OFF（いずれも点灯＝accent。M/Sボタンと同じフラット角丸トグル）
    for (auto* b : std::initializer_list<juce::TextButton*> { &fixedButton, &followButton, &monoButton })
    {
        addAndMakeVisible (*b);
        b->setClickingTogglesState (false); // 状態はモデルが持つ（bindで反映）
        b->getProperties().set ("flatButton", true);
        b->setColour (juce::TextButton::buttonColourId, Theme::controlBg);
        b->setColour (juce::TextButton::buttonOnColourId, Theme::accent);
        b->setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.6f));
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        b->setWantsKeyboardFocus (false);
        b->setMouseClickGrabsKeyboardFocus (false);
    }
    fixedButton.setTooltip (jp (u8"ノート長を無視して最後まで鳴らす（ワンショット）"));
    followButton.setTooltip (jp (u8"ルート音を基準に音程を変え、ノートの長さで止める"));
    fixedButton.onClick = [this] { applyPitchMode (false); };
    followButton.onClick = [this] { applyPitchMode (true); };

    monoButton.setTooltip (jp (u8"新しい打点で前の音を切る（808ロール等。OFFは重ねて鳴らす）"));
    monoButton.onClick = [this] { applyMono (! monoButton.getToggleState()); };

    addAndMakeVisible (rootBox);
    for (int pitch = 0; pitch < 128; ++pitch) // C-2〜G8（Logic式表記。C3 = 60）
        rootBox.addItem (juce::MidiMessage::getMidiNoteName (pitch, true, true, 3), pitch + 1);
    rootBox.onChange = [this] { applyRootNote (rootBox.getSelectedId() - 1); };
    rootBox.setWantsKeyboardFocus (false);
    rootBox.setMouseClickGrabsKeyboardFocus (false);

    addAndMakeVisible (gainSlider);
    gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gainSlider.setRange (0.0, 2.0);
    gainSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    gainSlider.setDoubleClickReturnValue (true, 1.0);
    gainSlider.textFromValueFunction = [] (double v) { return Meters::dbText ((float) v) + " dB"; };
    gainSlider.setPopupDisplayEnabled (true, false, nullptr); // ドラッグ中だけdB表示（ヘッダーと同じ流儀）
    gainSlider.setWantsKeyboardFocus (false);
    gainSlider.setMouseClickGrabsKeyboardFocus (false);
    // undoの粒度: ドラッグ開始で1回積み、値変更ごとには積まない（確定はドラッグ終了時）
    gainSlider.onDragStart = [this] { if (onWillEdit) onWillEdit (true); };
    gainSlider.onValueChange = [this]
    {
        if (track != nullptr)
            track->sampleGain = (float) gainSlider.getValue();
    };
    gainSlider.onDragEnd = [this] { if (onValueEdited) onValueEdited(); };

    addAndMakeVisible (trimLabel);
    trimLabel.setFont (Fonts::small());
    trimLabel.setColour (juce::Label::textColourId, Theme::lcdLabel);
    trimLabel.setJustificationType (juce::Justification::centredRight);

    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

InstrumentDetailView::~InstrumentDetailView() = default;

void InstrumentDetailView::setTrack (Track* trackToShow)
{
    track = trackToShow;
    refreshFromModel();
}

void InstrumentDetailView::refreshFromModel()
{
    const bool hasSample = track != nullptr && track->usesSampler();
    const bool follow = hasSample && track->samplePitchFollow;

    sampleNameLabel.setText (hasSample ? track->sampleName : juce::String(),
                             juce::dontSendNotification);
    sampleNameLabel.setFont (Fonts::forText (Fonts::bodyStrong(),
                                             hasSample ? track->sampleName : juce::String()));

    fixedButton.setToggleState (hasSample && ! follow, juce::dontSendNotification);
    followButton.setToggleState (follow, juce::dontSendNotification);
    fixedButton.setEnabled (hasSample);
    followButton.setEnabled (hasSample);

    monoButton.setToggleState (hasSample && track->sampleMono, juce::dontSendNotification);
    monoButton.setEnabled (hasSample);

    rootBox.setSelectedId (hasSample ? track->sampleRootNote + 1 : 61, juce::dontSendNotification);
    rootBox.setEnabled (follow); // ルート音は追従モードのときだけ効く
    rootBox.setAlpha (follow ? 1.0f : 0.4f);

    gainSlider.setValue (hasSample ? track->sampleGain : 1.0f, juce::dontSendNotification);
    gainSlider.setEnabled (hasSample);

    trimLabel.setText (trimText(), juce::dontSendNotification);
    wave->repaint();
    repaint();
}

// 頭カットの表示（ミリ秒。元SR基準）
juce::String InstrumentDetailView::trimText() const
{
    if (track == nullptr || ! track->hasSample())
        return {};
    const double ms = (double) track->sampleStartOffset / track->sampleSourceRate * 1000.0;
    return jp (u8"頭カット ") + juce::String (ms, ms < 10.0 ? 1 : 0) + " ms";
}

void InstrumentDetailView::applyPitchMode (bool follow)
{
    if (track == nullptr || ! track->usesSampler() || track->samplePitchFollow == follow)
        return;

    if (onWillEdit)
        onWillEdit (false); // 音程モードは停止要求＋resoundが要るので値のみ扱いにしない
    track->samplePitchFollow = follow;
    refreshFromModel();
    // 停止要求（発音中ボイスの打ち切り）と oneShot の更新は onPitchModeEdited 内で走る
    // SynthBank::sync() の責務。ここから requestStopAll()/sync() を直接呼ばないのは、
    // undo/redo での切り替わりも同じ一本の経路（pushSnapshot → sync）に通すため
    if (onPitchModeEdited)
        onPitchModeEdited();
}

void InstrumentDetailView::applyMono (bool mono)
{
    if (track == nullptr || ! track->usesSampler() || track->sampleMono == mono)
        return;

    if (onWillEdit)
        onWillEdit (true); // 次の発音から効く値なのでスナップショットの再pushは不要
    track->sampleMono = mono;
    refreshFromModel();
    if (onValueEdited)
        onValueEdited();
}

void InstrumentDetailView::applyRootNote (int note)
{
    if (track == nullptr || ! track->usesSampler())
        return;
    const int clamped = juce::jlimit (0, 127, note);
    if (track->sampleRootNote == clamped)
        return; // refreshFromModel による表示同期では発火させない

    if (onWillEdit)
        onWillEdit (true);
    track->sampleRootNote = clamped;
    if (onValueEdited) // 次の発音から効く値（鳴っている音は発音時のルート音のまま鳴り切る）
        onValueEdited();
}

void InstrumentDetailView::paint (juce::Graphics& g)
{
    // 設定行の見出し（値は各コントロールが描く。常設の数値は置かない方針）
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (Fonts::small());
    for (const auto& [text, area] : std::initializer_list<std::pair<const char*, juce::Rectangle<int>>> {
             { "SAMPLE", labelAreaFor (sampleNameLabel) },
             { "PITCH", labelAreaFor (fixedButton) },
             { "ROOT", labelAreaFor (rootBox) },
             { "VOICE", labelAreaFor (monoButton) },
             { "GAIN", labelAreaFor (gainSlider) } })
        g.drawText (text, area, juce::Justification::centredLeft);
}

juce::Rectangle<int> InstrumentDetailView::labelAreaFor (const juce::Component& c) const
{
    return { c.getX(), c.getY() - 14, juce::jmax (60, c.getWidth()), 12 };
}

void InstrumentDetailView::resized()
{
    auto area = getLocalBounds().reduced (10, 8);

    // 設定行を下に確保し、残り全部を波形に渡す（案A: 波形が主役）
    auto row = area.removeFromBottom (rowHeight);
    area.removeFromBottom (rowGap);
    wave->setBounds (area);

    // 行内は「見出し（上14px）＋コントロール（下）」の2段構成
    auto controls = row.removeFromBottom (rowHeight - 14);
    sampleNameLabel.setBounds (controls.removeFromLeft (150));
    controls.removeFromLeft (14);
    fixedButton.setBounds (controls.removeFromLeft (48).reduced (0, 1));
    controls.removeFromLeft (4);
    followButton.setBounds (controls.removeFromLeft (48).reduced (0, 1));
    controls.removeFromLeft (14);
    rootBox.setBounds (controls.removeFromLeft (76).reduced (0, 1));
    controls.removeFromLeft (14);
    monoButton.setBounds (controls.removeFromLeft (54).reduced (0, 1));
    controls.removeFromLeft (14);
    trimLabel.setBounds (controls.removeFromRight (140));
    controls.removeFromRight (14);
    gainSlider.setBounds (controls.removeFromLeft (juce::jmin (240, controls.getWidth())));
}
