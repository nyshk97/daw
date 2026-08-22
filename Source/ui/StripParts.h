#pragma once

#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "EqCurve.h"
#include "Fonts.h"
#include "FxVisualKind.h"
#include "HardwarePanelStyle.h"
#include "Theme.h"
#include "../shared/EqParams.h"

// FXパネルとミキサーのストリップで共有する描画部品（Logicのチャンネルストリップ準拠）。
// hover時の分割表示・アクティブ枠などの対話装飾は使う側が上に重ねる
namespace StripParts
{
// EQサムネイル（実カーブの縮小描画。計算はEQエディタの大カーブと共有のEqCurve）。
// EQ OFF・中立時はフラット線の弱い表示に倒す — サムネイルは「設定の気配」が伝われば
// 十分で、詳細はエディタ側が担う（ui-principles の情報の強弱）
inline void drawEqThumbnail (juce::Graphics& g, juce::Rectangle<float> thumb,
                             const Eq::Values& bands, bool eqEnabled, double sampleRate)
{
    // メーター窓（FX詳細パネルのグラフと同じ縁取り）
    HardwarePanelStyle::paintMeterWindow (g, thumb);

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (HardwarePanelStyle::meterWindowInner (thumb).toNearestInt());

    const float centreY = thumb.getY() + thumb.getHeight() * 0.55f;
    if (! eqEnabled || Eq::isNeutral (bands) || sampleRate <= 0.0)
    {
        // フラット線（旧プレースホルダと同じ見た目）
        g.setColour (Theme::fxEq.withAlpha (0.14f)); // カーブ下の淡い塗り
        g.fillRect (thumb.withTop (centreY));
        g.setColour (Theme::fxEq);
        g.fillRect (juce::Rectangle<float> (thumb.getX(), centreY - 0.9f, thumb.getWidth(), 1.8f));
        return;
    }

    constexpr int numPoints = 48; // サムネイル幅（〜130px）に十分な解像度
    std::vector<double> freqs, mags;
    EqCurve::response (bands, sampleRate, numPoints, freqs, mags);

    juce::Path curve;
    for (int i = 0; i < numPoints; ++i)
    {
        const float x = thumb.getX() + thumb.getWidth() * (float) i / (float) (numPoints - 1);
        const float db = (float) juce::Decibels::gainToDecibels (mags[(size_t) i], -48.0);
        // 縦スケールはエディタと同じ ±maxGainDb のリニアdB（上下0.45hに収める）
        const float t = juce::jlimit (-1.0f, 1.0f, db / Eq::maxGainDb);
        const float y = centreY - t * thumb.getHeight() * 0.45f;
        if (i == 0)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);
    }

    // 0dB線との間を淡く塗る（エディタの面表示と同じ読み方）→ 線を上描き
    juce::Path fill (curve);
    fill.lineTo (thumb.getRight(), centreY);
    fill.lineTo (thumb.getX(), centreY);
    fill.closeSubPath();
    g.setColour (Theme::fxEq.withAlpha (0.14f));
    g.fillPath (fill);
    g.setColour (Theme::fxEq);
    g.strokePath (curve, juce::PathStrokeType (1.8f));
}

// ラックの押しボタンの地（FXスロットのピル・Sendsのピルで共有）。
// フラットな暗い面＋細い暗縁＋上端1pxのごく薄いハイライト（立体感は付けず、LEDを主役にする。モック案B）
inline void drawHardwareButton (juce::Graphics& g, juce::Rectangle<float> bounds)
{
    g.setColour (Theme::hwButtonFace);
    g.fillRoundedRectangle (bounds, 5.0f);
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 5.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.04f));
    g.fillRect (bounds.getX() + 5.0f, bounds.getY() + 1.0f, bounds.getWidth() - 10.0f, 1.0f);
}

// ラックLED（ピル左端の小さな丸。色＝FX固有色。lit=点灯（滲みあり）/ 消灯（暗い点））
inline void drawLed (juce::Graphics& g, juce::Rectangle<float> pill, juce::Colour hue, bool lit)
{
    const auto centre = juce::Point<float> (pill.getX() + 11.0f, pill.getCentreY());
    if (lit)
    {
        g.setColour (hue.withAlpha (0.25f));
        g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (centre));
        g.setColour (hue.withAlpha (0.5f));
        g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (centre));
    }
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (centre));
    g.setColour (lit ? hue : hue.withAlpha (0.3f));
    g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (centre));
}

// FXスロットのピル（Illustrated hardware: 共通のボタン質感＋左端にFX固有色のLED。
// ON=LED点灯＋白文字 / OFF=LED消灯＋暗い文字。kind=neutral（Instrument）はLEDを描かない。
// 空きスロット=沈んだ暗い枠）
inline void drawSlotPill (juce::Graphics& g, juce::Rectangle<int> bounds,
                          const juce::String& name, bool isOn, bool grayed,
                          FxVisualKind kind = FxVisualKind::neutral)
{
    if (grayed)
    {
        g.setColour (Theme::faderSlotBg);
        g.fillRoundedRectangle (bounds.toFloat(), 5.0f);
        g.setColour (Theme::hwLabel.withAlpha (0.35f));
        // 自由入力（サンプル名）が入り得るのでCJK補正を通す（ASCIIのみの固定文言は無補正のまま）
        g.setFont (Fonts::forText (Fonts::small(), name));
        g.drawText (name, bounds, juce::Justification::centred); // 収まらない名前は末尾省略
        return;
    }
    drawHardwareButton (g, bounds.toFloat());
    if (kind != FxVisualKind::neutral)
        drawLed (g, bounds.toFloat(), Theme::fxHue (kind), isOn);
    g.setColour (isOn ? Theme::hwValue : Theme::hwLabel.withAlpha (0.7f));
    g.setFont (Fonts::forText (Fonts::smallStrong(), name));
    g.drawText (name, bounds, juce::Justification::centred); // 収まらない名前は末尾省略
}
} // namespace StripParts
