#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"

// FXパネルとミキサーのストリップで共有する描画部品（Logicのチャンネルストリップ準拠）。
// hover時の分割表示・アクティブ枠などの対話装飾は使う側が上に重ねる
namespace StripParts
{
// EQサムネイル（EQ DSP実装まではフラットカーブのプレースホルダ）
inline void drawEqThumbnail (juce::Graphics& g, juce::Rectangle<float> thumb)
{
    g.setColour (Theme::faderSlotBg);
    g.fillRoundedRectangle (thumb, 4.0f);

    juce::Graphics::ScopedSaveState save (g);
    juce::Path clip;
    clip.addRoundedRectangle (thumb, 4.0f);
    g.reduceClipRegion (clip);

    const float curveY = thumb.getY() + thumb.getHeight() * 0.55f;
    g.setColour (Theme::eqThumbCurve.withAlpha (0.14f)); // カーブ下の淡い塗り
    g.fillRect (thumb.withTop (curveY));
    g.setColour (Theme::eqThumbCurve);
    g.fillRect (juce::Rectangle<float> (thumb.getX(), curveY - 0.9f, thumb.getWidth(), 1.8f));
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
        g.setFont (Fonts::small());
        g.drawText (name, bounds, juce::Justification::centred);
        return;
    }
    g.setColour (isOn ? Theme::accent : Theme::controlBg);
    g.fillRoundedRectangle (bounds.toFloat(), 5.0f);
    g.setColour (juce::Colours::white.withAlpha (isOn ? 0.95f : 0.75f));
    g.setFont (Fonts::smallStrong());
    g.drawText (name, bounds, juce::Justification::centred);
}
} // namespace StripParts
