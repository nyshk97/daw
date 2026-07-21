#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// トラック種別アイコン（Pathの自前描画・画像アセットなし）。
// トラック追加メニューと各トラックヘッダで共用する。色は呼び出し側がsetColourしてから呼ぶ
namespace TrackIcons
{
// オーディオトラック: 波形（中心線対称の縦バー）
inline void drawWaveform (juce::Graphics& g, juce::Rectangle<float> r)
{
    const float heights[] = { 0.35f, 0.75f, 1.0f, 0.55f, 0.85f, 0.3f };
    const int n = juce::numElementsInArray (heights);
    const float barW = 1.8f;
    const float step = (r.getWidth() - barW) / (float) (n - 1);
    for (int i = 0; i < n; ++i)
    {
        const float h = r.getHeight() * heights[i];
        g.fillRoundedRectangle (r.getX() + step * (float) i, r.getCentreY() - h * 0.5f,
                                barW, h, barW * 0.5f);
    }
}

// ソフトウェア音源トラック: 鍵盤（外枠＋黒鍵2つ＋黒鍵下の区切り線）
inline void drawKeyboard (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto kb = r.reduced (0.0f, r.getHeight() * 0.16f); // 横長にする
    const float stroke = 1.2f;
    g.drawRoundedRectangle (kb.reduced (stroke * 0.5f), 2.0f, stroke);

    const float keyW = kb.getWidth() / 3.0f;
    const float blackH = kb.getHeight() * 0.52f;
    for (int k = 1; k <= 2; ++k)
    {
        const float x = kb.getX() + keyW * (float) k;
        const float blackW = keyW * 0.55f;
        g.fillRect (x - blackW * 0.5f, kb.getY() + stroke, blackW, blackH);
        g.drawLine (x, kb.getY() + blackH, x, kb.getBottom() - stroke, stroke);
    }
}
} // namespace TrackIcons
