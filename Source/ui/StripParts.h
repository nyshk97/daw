#pragma once

#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "EqCurve.h"
#include "Fonts.h"
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
    g.setColour (Theme::faderSlotBg);
    g.fillRoundedRectangle (thumb, 4.0f);

    juce::Graphics::ScopedSaveState save (g);
    juce::Path clipPath;
    clipPath.addRoundedRectangle (thumb, 4.0f);
    g.reduceClipRegion (clipPath);

    const float centreY = thumb.getY() + thumb.getHeight() * 0.55f;
    if (! eqEnabled || Eq::isNeutral (bands) || sampleRate <= 0.0)
    {
        // フラット線（旧プレースホルダと同じ見た目）
        g.setColour (Theme::eqThumbCurve.withAlpha (0.14f)); // カーブ下の淡い塗り
        g.fillRect (thumb.withTop (centreY));
        g.setColour (Theme::eqThumbCurve);
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
    g.setColour (Theme::eqThumbCurve.withAlpha (0.14f));
    g.fillPath (fill);
    g.setColour (Theme::eqThumbCurve);
    g.strokePath (curve, juce::PathStrokeType (1.8f));
}

// FXスロットのピル（ON=青・OFF=グレー・空きスロット=暗い枠）
inline void drawSlotPill (juce::Graphics& g, juce::Rectangle<int> bounds,
                          const juce::String& name, bool isOn, bool grayed)
{
    if (grayed)
    {
        g.setColour (Theme::faderSlotBg);
        g.fillRoundedRectangle (bounds.toFloat(), 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        // 自由入力（サンプル名）が入り得るのでCJK補正を通す（ASCIIのみの固定文言は無補正のまま）
        g.setFont (Fonts::forText (Fonts::small(), name));
        g.drawText (name, bounds, juce::Justification::centred); // 収まらない名前は末尾省略
        return;
    }
    g.setColour (isOn ? Theme::accent : Theme::controlBg);
    g.fillRoundedRectangle (bounds.toFloat(), 5.0f);
    g.setColour (juce::Colours::white.withAlpha (isOn ? 0.95f : 0.75f));
    g.setFont (Fonts::forText (Fonts::smallStrong(), name));
    g.drawText (name, bounds, juce::Justification::centred); // 収まらない名前は末尾省略
}
} // namespace StripParts
