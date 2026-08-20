#include "EqEditorView.h"

#include <cmath>
#include <juce_dsp/juce_dsp.h>

#include "EqCurve.h"
#include "Fonts.h"
#include "HardwarePanelStyle.h"
#include "Theme.h"
#include "../shared/GainScale.h"

namespace
{
constexpr float minFreqShown = EqCurve::minFreqShown;
constexpr float maxFreqShown = EqCurve::maxFreqShown;
constexpr float dbRange = Eq::maxGainDb; // 縦軸 ±24dB（カーブ=加工量。アナライザの実量とは目盛りが別）
constexpr float pointRadius = 5.0f;
constexpr float hitRadius = 12.0f;
constexpr int readoutHeight = 40;
constexpr int curveResolution = 200;

juce::String freqText (float freqHz)
{
    if (freqHz >= 1000.0f)
        return juce::String (freqHz / 1000.0f, 1) + " kHz";
    return juce::String ((int) std::lround (freqHz)) + " Hz";
}

const char* bandName (int band)
{
    switch (band)
    {
        case Eq::highpass: return "HIGHPASS";
        case Eq::bell1: return "BELL 1";
        case Eq::bell2: return "BELL 2";
        default: return "SHELF";
    }
}
} // namespace

EqEditorView::EqEditorView()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

void EqEditorView::setTrack (Track* trackToShow)
{
    track = trackToShow;
    dragBand = -1;
    hoverBand = -1;

    // アナライザ: 表示中だけタップを有効化し、切替時は旧トラックの蓄積・表示を捨てる
    spectrumDb.fill (SpectrumAnalyzer::floorDb);
    analyzer.reset();
    if (tap != nullptr)
        tap->setTarget (track != nullptr ? track->id : 0, track != nullptr);
    if (track != nullptr)
        startTimerHz (30);
    else
        stopTimer();

    repaint();
}

void EqEditorView::timerCallback()
{
    if (tap == nullptr || track == nullptr)
        return;
    analyzer.update (*tap, sampleRate());

    // ピークホールド＋減衰（約30dB/s）。新しいフレームが低くても急落させず読み取れる速度で落とす
    bool anyVisible = false;
    const auto& frame = analyzer.magnitudesDb();
    for (int i = 0; i < SpectrumAnalyzer::numBins; ++i)
    {
        spectrumDb[(size_t) i] = juce::jmax (frame[(size_t) i],
                                             spectrumDb[(size_t) i] - 1.0f);
        anyVisible = anyVisible || spectrumDb[(size_t) i] > SpectrumAnalyzer::floorDb + 0.5f;
    }
    if (anyVisible || lastSpectrumVisible)
        repaint (graphArea);
    lastSpectrumVisible = anyVisible;
}

double EqEditorView::sampleRate() const
{
    const double sr = getSampleRate != nullptr ? getSampleRate() : 0.0;
    return sr > 0.0 ? sr : 48000.0;
}

// ---- 座標変換（横=対数周波数・縦=リニアdB）----

float EqEditorView::xForFreq (float freqHz) const
{
    const float t = std::log (freqHz / minFreqShown) / std::log (maxFreqShown / minFreqShown);
    return (float) graphArea.getX() + t * (float) graphArea.getWidth();
}

float EqEditorView::freqForX (float x) const
{
    const float t = juce::jlimit (0.0f, 1.0f,
                                  (x - (float) graphArea.getX()) / (float) graphArea.getWidth());
    return minFreqShown * std::pow (maxFreqShown / minFreqShown, t);
}

// 縦軸の両端に余白を取り、±24dB（上限）の点が窓の縁に乗らないようにする。縁に乗ると点の半分しか
// 見えず、クリックが上のタイトル行に逃げて「掴めない＝動かない」ように見える（実際に起きた）
float EqEditorView::halfPlotHeight() const
{
    return (float) graphArea.getHeight() * 0.5f - (pointRadius + 6.0f);
}

float EqEditorView::yForDb (float db) const
{
    const float t = juce::jlimit (-1.0f, 1.0f, db / dbRange);
    return (float) graphArea.getCentreY() - t * halfPlotHeight();
}

float EqEditorView::dbForY (float y) const
{
    const float t = ((float) graphArea.getCentreY() - y) / halfPlotHeight();
    return juce::jlimit (-dbRange, dbRange, t * dbRange);
}

juce::Point<float> EqEditorView::pointFor (int band, const Eq::BandValue& value) const
{
    // HPはゲイン概念がないので0dB線上に置く
    const float db = band == Eq::highpass ? 0.0f : value.gainDb;
    return { xForFreq (value.freqHz), yForDb (db) };
}

int EqEditorView::bandAt (juce::Point<float> position) const
{
    if (track == nullptr)
        return -1;
    int best = -1;
    float bestDistance = hitRadius;
    for (int band = 0; band < Eq::numBands; ++band)
    {
        const auto value = Eq::load (track->params->eqBands[band]);
        const float distance = pointFor (band, value).getDistanceFrom (position);
        if (distance <= bestDistance)
        {
            bestDistance = distance;
            best = band;
        }
    }
    return best;
}

juce::Rectangle<int> EqEditorView::readoutColumn (int band) const
{
    const int w = readoutArea.getWidth() / Eq::numBands;
    return readoutArea.withX (readoutArea.getX() + band * w).withWidth (w);
}

juce::Rectangle<int> EqEditorView::hpToggleArea() const
{
    // HP列の名前ピル＝ON/OFFトグル（enabled を操作できる唯一のバンド）。
    // 他バンドの見出しと並んだとき不揃いにならないよう、テキスト幅相当に絞って中央へ置く
    return readoutColumn (Eq::highpass).removeFromTop (18).withSizeKeepingCentre (96, 18);
}

// ---- モデルへの書き込み ----

void EqEditorView::applyBand (int band, Eq::BandValue value)
{
    if (track == nullptr)
        return;
    Eq::store (track->params->eqBands[band], Eq::normalized (band, value));
    repaint();
}

void EqEditorView::notifyEdited()
{
    if (onEdited != nullptr)
        onEdited();
}

// ---- 描画 ----

void EqEditorView::paint (juce::Graphics& g)
{
    const auto hue = Theme::fxHue (FxVisualKind::eq);
    HardwarePanelStyle::paintMeterWindow (g, graphArea.toFloat());

    if (track == nullptr)
        return;

    const bool eqOn = track->params->eqEnabled.load();
    const float dim = eqOn ? 1.0f : 0.35f; // OFF中はカーブを沈める（Logicのバイパス表示と同じ発想）

    // dBグリッド（6dB刻み・0dB線だけ強調）＋左端に数値
    g.setFont (Fonts::small());
    for (int db = -18; db <= 18; db += 6)
    {
        const float y = yForDb ((float) db);
        g.setColour (db == 0 ? hue.withAlpha (0.3f) : HardwarePanelStyle::gridColour (hue));
        g.drawHorizontalLine ((int) y, (float) graphArea.getX(), (float) graphArea.getRight());
        if (db % 12 == 0)
        {
            g.setColour (HardwarePanelStyle::captionColour());
            g.drawText ((db > 0 ? "+" : "") + juce::String (db),
                        graphArea.getX() + 4, (int) y - 13, 30, 12, juce::Justification::centredLeft);
        }
    }

    // 周波数グリッド（対数）。1-2-5系の主要線にだけラベル
    for (const float f : { 30.0f, 40.0f, 50.0f, 60.0f, 80.0f, 100.0f, 200.0f, 300.0f, 400.0f,
                           500.0f, 600.0f, 800.0f, 1000.0f, 2000.0f, 3000.0f, 4000.0f, 5000.0f,
                           6000.0f, 8000.0f, 10000.0f })
    {
        const bool major = f == 50.0f || f == 100.0f || f == 200.0f || f == 500.0f
                           || f == 1000.0f || f == 2000.0f || f == 5000.0f || f == 10000.0f;
        const float x = xForFreq (f);
        g.setColour (major ? HardwarePanelStyle::gridColour (hue) : hue.withAlpha (0.04f));
        g.drawVerticalLine ((int) x, (float) graphArea.getY(), (float) graphArea.getBottom());
        if (major)
        {
            g.setColour (HardwarePanelStyle::captionColour());
            const auto label = f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k"
                                            : juce::String ((int) f);
            g.drawText (label, (int) x - 15, graphArea.getBottom() - 15, 30, 12,
                        juce::Justification::centred);
        }
    }

    // スペクトラムアナライザ（背景・EQ直後の信号の実量 -60〜0dBFS。カーブとは目盛りが独立）。
    // 「今なにが鳴っているか」の参考表示なので、カーブより沈む無彩色で描く
    {
        juce::Path spectrum;
        bool started = false;
        for (int i = 0; i < SpectrumAnalyzer::numBins; ++i)
        {
            const float x = xForFreq (SpectrumAnalyzer::binFrequency (i));
            const float t = (spectrumDb[(size_t) i] - SpectrumAnalyzer::floorDb)
                            / (0.0f - SpectrumAnalyzer::floorDb);
            const float y = (float) graphArea.getBottom()
                            - juce::jlimit (0.0f, 1.0f, t) * (float) graphArea.getHeight();
            if (! started)
            {
                spectrum.startNewSubPath ((float) graphArea.getX(), (float) graphArea.getBottom());
                spectrum.lineTo (x, y);
                started = true;
            }
            else
            {
                spectrum.lineTo (x, y);
            }
        }
        if (started)
        {
            spectrum.lineTo ((float) graphArea.getRight(), (float) graphArea.getBottom());
            spectrum.closeSubPath();
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.fillPath (spectrum);
            g.setColour (juce::Colours::white.withAlpha (0.16f));
            g.strokePath (spectrum, juce::PathStrokeType (1.0f));
        }
    }

    // 合成カーブ。DSPと同じRBJ係数の周波数応答から描く（計算はサムネイルと共有のEqCurve）
    const auto bands = Eq::loadAll (track->params->eqBands);
    const double sr = sampleRate();
    std::vector<double> freqs, totalMags;
    EqCurve::response (bands, sr, curveResolution, freqs, totalMags);

    juce::Path curve;
    for (int i = 0; i < curveResolution; ++i)
    {
        const float x = xForFreq ((float) freqs[(size_t) i]);
        const float db = (float) juce::Decibels::gainToDecibels (totalMags[(size_t) i], -48.0);
        const float y = juce::jlimit ((float) graphArea.getY(), (float) graphArea.getBottom(),
                                      yForDb (db));
        if (i == 0)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);
    }

    // 0dB線との間を薄く塗る（どの帯域をどれだけ動かしたかの面表示）→ 線を上描き
    {
        juce::Path fill (curve);
        fill.lineTo ((float) graphArea.getRight(), yForDb (0.0f));
        fill.lineTo ((float) graphArea.getX(), yForDb (0.0f));
        fill.closeSubPath();
        g.setColour (hue.withAlpha (0.16f * dim));
        g.fillPath (fill);
    }
    g.setColour (hue.withAlpha (dim));
    g.strokePath (curve, juce::PathStrokeType (2.0f));

    // バンドのポイント（HPは無効時に沈める）
    for (int band = 0; band < Eq::numBands; ++band)
    {
        const auto& value = bands[(size_t) band];
        const auto p = pointFor (band, value);
        const bool active = band == dragBand || (dragBand < 0 && band == hoverBand);
        const bool bandDim = band == Eq::highpass && ! value.enabled;
        auto colour = active ? juce::Colours::white : hue.brighter (0.3f);
        g.setColour (colour.withAlpha ((bandDim ? 0.3f : 1.0f) * dim));
        g.fillEllipse (p.x - pointRadius, p.y - pointRadius, pointRadius * 2.0f, pointRadius * 2.0f);
        g.setColour (Theme::hwMeterBg);
        g.drawEllipse (p.x - pointRadius, p.y - pointRadius, pointRadius * 2.0f, pointRadius * 2.0f, 1.0f);
    }

    // ---- 数値表示行（実単位のまま。抽象ラベルに置き換えない）----
    for (int band = 0; band < Eq::numBands; ++band)
    {
        auto column = readoutColumn (band);
        auto nameArea = column.removeFromTop (18);

        if (band == Eq::highpass)
        {
            // 名前ピル＝ON/OFFトグル
            const auto pill = hpToggleArea().toFloat();
            g.setColour (bands[Eq::highpass].enabled ? Theme::hwButtonOn : Theme::hwButtonOff);
            g.fillRoundedRectangle (pill, 4.0f);
            g.setColour (bands[Eq::highpass].enabled ? Theme::hwValue : Theme::hwLabel.withAlpha (0.6f));
            g.setFont (Fonts::smallStrong());
            g.drawText (bandName (band), pill.toNearestInt(), juce::Justification::centred);
        }
        else
        {
            g.setColour (Theme::hwLabel);
            g.setFont (HardwarePanelStyle::labelFont());
            g.drawText (bandName (band), nameArea, juce::Justification::centred);
        }

        const auto& value = bands[(size_t) band];
        juce::String text;
        if (band == Eq::highpass)
            text = freqText (value.freqHz) + juce::String::fromUTF8 (u8" · 12dB/oct");
        else if (band == Eq::highShelf)
            text = freqText (value.freqHz) + "  " + GainScale::text (value.gainDb);
        else
            text = freqText (value.freqHz) + "  " + GainScale::text (value.gainDb)
                   + "  Q " + juce::String (value.q, 2);
        g.setColour (Theme::hwValue);
        g.setFont (Fonts::small());
        g.drawText (text, column, juce::Justification::centred);
    }
}

void EqEditorView::resized()
{
    auto area = getLocalBounds();
    readoutArea = area.removeFromBottom (readoutHeight);
    graphArea = area.reduced (0, 2);
}

// ---- 操作 ----

void EqEditorView::mouseDown (const juce::MouseEvent& e)
{
    if (track == nullptr)
        return;

    if (hpToggleArea().contains (e.getPosition()))
    {
        auto value = Eq::load (track->params->eqBands[Eq::highpass]);
        value.enabled = ! value.enabled;
        applyBand (Eq::highpass, value);
        notifyEdited();
        return;
    }

    dragBand = bandAt (e.position);
    dragMoved = false;
    repaint();
}

void EqEditorView::mouseDrag (const juce::MouseEvent& e)
{
    if (track == nullptr || dragBand < 0)
        return;

    auto value = Eq::load (track->params->eqBands[dragBand]);
    value.freqHz = freqForX (e.position.x);
    if (dragBand != Eq::highpass)
        value.gainDb = dbForY (e.position.y); // HPは横のみ（ゲイン概念なし）
    applyBand (dragBand, value);
    dragMoved = true;
}

void EqEditorView::mouseUp (const juce::MouseEvent&)
{
    if (dragBand >= 0 && dragMoved)
        notifyEdited(); // dirty化はジェスチャ確定時に1回（値自体はドラッグ中に反映済み）
    dragBand = -1;
    repaint();
}

void EqEditorView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (track == nullptr)
        return;
    const int band = bandAt (e.position);
    if (band == Eq::bell1 || band == Eq::bell2 || band == Eq::highShelf)
    {
        auto value = Eq::load (track->params->eqBands[band]);
        value.gainDb = 0.0f; // 0dB＝実質OFF（enabledを操作できるのはHPだけ、の設計）
        applyBand (band, value);
        notifyEdited();
    }
}

void EqEditorView::mouseMove (const juce::MouseEvent& e)
{
    const int band = bandAt (e.position);
    if (band != hoverBand)
    {
        hoverBand = band;
        repaint();
    }
}

void EqEditorView::mouseExit (const juce::MouseEvent&)
{
    if (hoverBand != -1)
    {
        hoverBand = -1;
        repaint();
    }
}

void EqEditorView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (track == nullptr)
        return;
    // スクロール=Qはベルのみ（HP・シェルフはQ固定）。対象はドラッグ中＞ホバー中のポイント
    const int band = dragBand >= 0 ? dragBand : hoverBand;
    if (band != Eq::bell1 && band != Eq::bell2)
        return;
    if (wheel.deltaY == 0.0f)
        return;

    auto value = Eq::load (track->params->eqBands[band]);
    // 上スクロール=Qを上げる（狭める）。倍率で動かす＝対数スケール（Qの知覚は比率）
    value.q *= std::pow (2.0f, wheel.deltaY * 1.5f);
    applyBand (band, value);
    notifyEdited();
}
