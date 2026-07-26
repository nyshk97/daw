#include "InstrumentDetailView.h"

#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "../shared/GainScale.h"
#include "Fonts.h"
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
        if (model == nullptr || ! model->hasSample())
        {
            g.setColour (juce::Colours::white.withAlpha (0.3f));
            g.setFont (Fonts::small());
            g.drawText (jp (u8"サンプルがありません"), bounds, juce::Justification::centred);
            return;
        }

        ensurePixelPeaks();
        const float midY = (float) bounds.getCentreY();
        const float halfH = (float) (bounds.getHeight() / 2 - 6);
        const int w = juce::jmin ((int) pxMax.size(), juce::jmax (1, bounds.getWidth()));

        // 波形（1px＝そのピクセルが表す区間の実データの上下端を結ぶ縦線）。
        // クリップ描画の1.4倍強調は掛けない（小さなレーンで見せるための補正で、
        // この大きさでは頭打ちになって減衰の形が読めなくなる）
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        float prevTop = midY, prevBottom = midY;
        for (int px = 0; px < w; ++px)
        {
            const float top = midY - juce::jlimit (-halfH, halfH, pxMax[(size_t) px] * halfH);
            const float bottom = midY - juce::jlimit (-halfH, halfH, pxMin[(size_t) px] * halfH);
            // 隣のpxと範囲が重ならない区間（1px内に半周期未満しか入らない低音）でも
            // 線が途切れないよう、前のpxの範囲まで伸ばして繋ぐ
            const float y0 = px > 0 ? juce::jmin (top, prevBottom) : top;
            const float y1 = px > 0 ? juce::jmax (bottom, prevTop) : bottom;
            g.drawVerticalLine (px, y0, juce::jmax (y1, y0 + 1.0f));
            prevTop = top;
            prevBottom = bottom;
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
    // 表示幅に合わせた1pxごとの上下ピーク。samplePeakCache（512サンプル単位）を引き伸ばすと
    // この大きさでは1ビンが数px幅の階段になって波形が角張るため、元バッファから作り直す。
    // 上下を別々に取る（絶対値の対称ではない）のはLogicの波形と同じ読み方にするため。
    //
    // 作り直すのは「サンプルの実体が変わった or 幅が変わった」ときだけ。refreshFromModel()は
    // 音量・Mono・ルート音の変更でも走るので、そこから毎回無効化してはいけない。
    void ensurePixelPeaks()
    {
        auto* model = owner.track;
        const auto buffer = model != nullptr ? model->sampleAudio : nullptr;
        const int w = juce::jmax (1, getWidth());
        // shared_ptrの実体で比べる（生ポインタだと、解放後に同じアドレスへ載った別サンプルを
        // 取りこぼす）。バッファの中身が後から書き換わることはないのでこれで足りる
        if (cachedBuffer.lock() == buffer && w == cachedWidth)
            return;

        cachedBuffer = buffer;
        cachedWidth = w;
        pxMin.assign ((size_t) w, 0.0f);
        pxMax.assign ((size_t) w, 0.0f);
        const int numSamples = buffer != nullptr ? buffer->getNumSamples() : 0;
        if (numSamples <= 0)
            return;

        // 十分に長い素材では、元バッファの全走査をやめてsamplePeakCacheから引く
        // （走査量の上限を w * samplesPerPeak * minBinsPerPixel に抑える）。
        // 1pxが1〜2ビンしか表さない密度では使えない: ビン境界とpx境界のズレで最大1ビンぶん
        // 広い区間を見てしまい、片側にしか振れない区間まで中央線を跨いで太る。
        // 実測では8ビン/px あれば実データ直読みとの差は0.03以下（20Hz正弦・808とも）
        constexpr int minBinsPerPixel = 8;
        if ((juce::int64) numSamples >= (juce::int64) w * Clip::samplesPerPeak * minBinsPerPixel)
        {
            const auto& bins = model->samplePeakCache;
            for (int px = 0; px < w && ! bins.empty(); ++px)
            {
                // ビン数をpx数で按分すると、1pxが表す区間の端が最大1ビンぶん見落とされる
                // （波形が痩せる）。サンプル位置から引いて、区間に掛かるビンを全部見る
                const juce::int64 s0 = (juce::int64) px * numSamples / w;
                const juce::int64 s1 = juce::jmax (s0 + 1, (juce::int64) (px + 1) * numSamples / w);
                const int i0 = juce::jmin ((int) bins.size() - 1, (int) (s0 / Clip::samplesPerPeak));
                const int i1 = juce::jmin ((int) bins.size(),
                                           (int) ((s1 + Clip::samplesPerPeak - 1) / Clip::samplesPerPeak));
                float lo = bins[(size_t) i0].lo, hi = bins[(size_t) i0].hi;
                for (int i = i0 + 1; i < i1; ++i)
                {
                    lo = juce::jmin (lo, bins[(size_t) i].lo);
                    hi = juce::jmax (hi, bins[(size_t) i].hi);
                }
                pxMin[(size_t) px] = lo;
                pxMax[(size_t) px] = hi;
            }
            return;
        }

        const int numChannels = juce::jmin (2, buffer->getNumChannels());
        for (int px = 0; px < w; ++px)
        {
            const int s0 = (int) ((juce::int64) px * numSamples / w);
            const int s1 = juce::jmin (numSamples,
                                       juce::jmax (s0 + 1, (int) ((juce::int64) (px + 1) * numSamples / w)));
            // 0で初期化しない（区間が中央線を跨がないときまで跨がせると、正弦波が
            // 中央から生える櫛になってしまう。実際の上下端をそのまま採る）
            float lo = std::numeric_limits<float>::max();
            float hi = std::numeric_limits<float>::lowest();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* data = buffer->getReadPointer (ch);
                for (int i = s0; i < s1; ++i)
                {
                    lo = juce::jmin (lo, data[i]);
                    hi = juce::jmax (hi, data[i]);
                }
            }
            pxMin[(size_t) px] = lo;
            pxMax[(size_t) px] = hi;
        }
    }

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

    std::vector<float> pxMin, pxMax;                        // 1pxごとの上下ピーク（ensurePixelPeaksが作る）
    std::weak_ptr<juce::AudioBuffer<float>> cachedBuffer;   // 作った時点のサンプル（差し替え検出用）
    int cachedWidth = 0;
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
    // 値はdB（モデルの sampleGain は線形倍率）。倍率を等間隔に並べると耳に対して等間隔にならず、
    // 下げ側だけ詰まって微調整できなくなるため、スライダー位置に対して等間隔にするのはdBの方
    gainSlider.setRange (-GainScale::rangeDb, GainScale::rangeDb, 0.1);
    gainSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    gainSlider.setDoubleClickReturnValue (true, 0.0);
    gainSlider.textFromValueFunction = [] (double v) { return GainScale::text (v); };
    gainSlider.setPopupDisplayEnabled (true, false, nullptr); // ドラッグ中だけdB表示（ヘッダーと同じ流儀）
    gainSlider.getProperties().set ("centerFill", true);      // 0dB起点で左右に伸びる帯（Panノブと同じ読み方）
    gainSlider.setWantsKeyboardFocus (false);
    gainSlider.setMouseClickGrabsKeyboardFocus (false);
    // undoの粒度: ドラッグ開始で1回積み、値変更ごとには積まない（確定はドラッグ終了時）
    gainSlider.onDragStart = [this] { if (onWillEdit) onWillEdit (true); };
    gainSlider.onValueChange = [this]
    {
        if (track != nullptr)
            track->sampleGain = GainScale::toLinear (gainSlider.getValue());
        gainValueLabel.setDb (gainSlider.getValue()); // ドラッグ中も常設表示を追従させる
    };
    gainSlider.onDragEnd = [this] { if (onValueEdited) onValueEdited(); };

    // 現在値の常設表示。リセット専用ボタンを増やさず、この表示自体が「0 dBに戻す」を兼ねる
    // （ダブルクリックでも戻せるが、GAINは頻繁に触る場所ではないので操作を覚えていなくても戻せるように）
    addAndMakeVisible (gainValueLabel);
    gainValueLabel.onReset = [this]
    {
        if (track == nullptr || ! track->usesSampler() || gainSlider.getValue() == 0.0)
            return;
        // undo・確定通知の粒度はドラッグと揃える（ダブルクリックはJUCE側がドラッグ通知で包むので同じ経路になる）
        if (onWillEdit)
            onWillEdit (true);
        gainSlider.setValue (0.0, juce::sendNotificationSync); // onValueChange経由でモデルと表示が揃う
        if (onValueEdited)
            onValueEdited();
    };

    addAndMakeVisible (trimLabel);
    trimLabel.setFont (Fonts::small());
    trimLabel.setColour (juce::Label::textColourId, Theme::lcdLabel);
    trimLabel.setJustificationType (juce::Justification::centredRight);

    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

InstrumentDetailView::~InstrumentDetailView() = default;

double InstrumentDetailView::GainSlider::snapValue (double attemptedValue, DragMode dragMode)
{
    return GainScale::snapDb (attemptedValue, dragMode != notDragging);
}

// ---- GAINの現在値表示（クリックで0 dBに戻す）---------------------------------

InstrumentDetailView::GainValueLabel::GainValueLabel()
{
    setTooltip (jp (u8"クリックで 0 dB に戻す"));
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus (false);
}

void InstrumentDetailView::GainValueLabel::setDb (double db)
{
    if (std::abs (db - valueDb) < 1.0e-9)
        return;
    valueDb = db;
    repaint();
}

void InstrumentDetailView::GainValueLabel::mouseDown (const juce::MouseEvent&)
{
    if (isEnabled() && onReset != nullptr)
        onReset();
}

void InstrumentDetailView::GainValueLabel::paint (juce::Graphics& g)
{
    const bool atDefault = std::abs (valueDb) < 0.05;
    const bool active = isEnabled();

    if (hovered && active) // 押せることはホバーで示す（常設の枠は置かない）
    {
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.fillRoundedRectangle (area, 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.13f));
        g.drawRoundedRectangle (area, 4.0f, 1.0f);
    }

    // 0dB（既定）のときは他の見出しと同じ静かさ、外れているときだけ明るくする
    auto colour = atDefault ? Theme::lcdLabel : juce::Colours::white.withAlpha (0.85f);
    if (! active)
        colour = colour.withMultipliedAlpha (0.4f);
    else if (hovered)
        colour = juce::Colours::white;

    g.setColour (colour);
    g.setFont (Fonts::small());
    g.drawText (GainScale::text (valueDb), getLocalBounds().reduced (5, 0),
                juce::Justification::centredRight);
}

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

    // モデルは線形倍率、スライダーはdB。setValue は通知なしなので snapValue は通らない（吸着はドラッグ中だけ）
    const double gainDb = hasSample ? GainScale::toDb (track->sampleGain) : 0.0;
    gainSlider.setValue (gainDb, juce::dontSendNotification);
    gainValueLabel.setDb (gainDb);
    gainValueLabel.setEnabled (hasSample);
    gainSlider.setEnabled (hasSample);

    trimLabel.setText (trimText(), juce::dontSendNotification);
    wave->repaint(); // 波形キャッシュの作り直しはWaveDisplay側が実体の変化で判断する
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
    // GAINは「スライダー＋現在値」で1組。値はスライダーのすぐ右に置く
    // （頭カット側に寄せるとGAINの値に見えない）。字送りと色は頭カットに合わせる
    gainSlider.setBounds (controls.removeFromLeft (juce::jmin (240, controls.getWidth())));
    controls.removeFromLeft (6);
    gainValueLabel.setBounds (controls.removeFromLeft (juce::jmin (66, controls.getWidth())));
}
