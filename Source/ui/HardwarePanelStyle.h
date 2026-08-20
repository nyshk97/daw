#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"

// FX周り（左ラック・下部詳細パネル）の「Illustrated hardware」質感を描く共通ヘルパー。
// トラック画面（机）とは別の文法（機材）で描くための置き場。各Viewはここを呼ぶだけにして、
// 地・メーター窓・ラベルの描き方を自前で持たない（散ると1つ直し漏れただけで見た目が揃わなくなる）。
// docs/plans/2026-08-20-1836-fx-panel-hardware-look.md
namespace HardwarePanelStyle
{
// パネルの地: 上からの照明（上端がわずかに明るい縦グラデ＋上中央の淡いハイライト）＋ヘアライン。
// ヘアラインは 1×3px のタイルをタイル塗りする（毎paintで数百本の線を引かない）
inline void paintPanelBackground (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const auto area = bounds.toFloat();
    g.setGradientFill (juce::ColourGradient (Theme::hwPanelTop, 0.0f, area.getY(),
                                             Theme::hwPanelBottom, 0.0f, area.getBottom(), false));
    g.fillRect (area);

    static const juce::Image hairline = []
    {
        juce::Image img (juce::Image::ARGB, 1, 3, true);
        img.setPixelAt (0, 0, juce::Colours::white.withAlpha (0.012f));
        return img;
    }();
    g.setTiledImageFill (hairline, bounds.getX(), bounds.getY(), 1.0f);
    g.fillRect (area);

    // 上中央の照明（楕円の放射グラデ。横に広く縦に浅い）
    juce::ColourGradient light (juce::Colours::white.withAlpha (0.05f), area.getCentreX(), area.getY(),
                                juce::Colours::transparentWhite, area.getCentreX(), area.getY() + area.getHeight() * 0.6f,
                                true);
    g.setGradientFill (light);
    g.fillRect (area);

    // 上端の細いハイライト・下端の細い影（パネルの縁）
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.fillRect (area.withHeight (1.0f));
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect (area.withTop (area.getBottom() - 1.0f));
}

// メーター窓の地と縁: 黒地＋外側の暗い縁＋内側の薄い明るい縁＋上端の落ち影（内側に沈んで見える）。
// 中身（グリッド・カーブ・メーター）は呼び出し側がこの後に描く。グリッドを固有色で薄く引きたいときは
// gridColour を使う
inline void paintMeterWindow (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour (Theme::hwMeterBg);
    g.fillRect (area);
    g.setColour (juce::Colours::black.withAlpha (0.9f));
    g.drawRect (area, 2.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRect (area.reduced (2.0f), 1.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.55f), 0.0f, area.getY() + 3.0f,
                                             juce::Colours::transparentBlack, 0.0f, area.getY() + 17.0f, false));
    g.fillRect (area.reduced (3.0f).withHeight (14.0f));
}

// メーター窓の内側（縁の内側）。グリッドやカーブはこの矩形に収める
inline juce::Rectangle<float> meterWindowInner (juce::Rectangle<float> area) { return area.reduced (3.0f); }

// グリッド線の色（固有色をごく薄く。無彩色の 6% より「その機材の窓」に見える）
inline juce::Colour gridColour (juce::Colour hue) { return hue.withAlpha (0.10f); }

// ノブのラベル（上・大文字トラッキング）と数値（下）。各Viewの KnobText ループから呼ぶ
inline juce::Font labelFont()
{
    auto f = Fonts::small();
    f.setExtraKerningFactor (0.12f);
    return f;
}

inline void drawKnobLabel (juce::Graphics& g, juce::Rectangle<int> knobBounds,
                           const juce::String& name, const juce::String& value)
{
    g.setColour (Theme::hwLabel);
    g.setFont (labelFont());
    g.drawText (name, knobBounds.getX() - 14, knobBounds.getY() - 13, knobBounds.getWidth() + 28, 12,
                juce::Justification::centred);
    g.setColour (Theme::hwValue);
    g.setFont (Fonts::small());
    g.drawText (value, knobBounds.getX() - 14, knobBounds.getBottom() + 2, knobBounds.getWidth() + 28, 13,
                juce::Justification::centred);
}

// 補助テキスト（軸ラベル「in dB」・固定値「Knee: 6dB (soft)」等）
inline juce::Colour captionColour() { return Theme::hwLabel.withAlpha (0.8f); }

// パネルタイトル: 固定のFX名は大文字・トラッキング広め、後続（— チャンネル名）はユーザー入力なので
// CJK補正付きの通常フォントで続けて描く。fixedName=false（Instrumentはサンプル名＝ユーザー由来）
// のときは先頭も通常フォントで描く
inline void drawTitle (juce::Graphics& g, juce::Rectangle<int> area,
                       const juce::String& fxName, bool fixedName, const juce::String& tail)
{
    int x = area.getX();
    g.setColour (Theme::hwValue);
    if (fixedName)
    {
        auto f = Fonts::bodyStrong().withHeight (12.0f);
        f.setExtraKerningFactor (0.08f);
        g.setFont (f);
        const auto text = fxName.toUpperCase();
        const int w = f.getStringWidth (text) + 2;
        g.drawText (text, x, area.getY(), w, area.getHeight(), juce::Justification::centredLeft);
        x += w;
    }
    else
    {
        const auto f = Fonts::forText (Fonts::bodyStrong(), fxName);
        g.setFont (f);
        const int w = f.getStringWidth (fxName) + 2;
        g.drawText (fxName, x, area.getY(), w, area.getHeight(), juce::Justification::centredLeft);
        x += w;
    }
    if (tail.isNotEmpty())
    {
        g.setColour (Theme::hwValue.withAlpha (0.75f));
        g.setFont (Fonts::forText (Fonts::body(), tail));
        g.drawText (tail, x + 8, area.getY(), area.getRight() - x - 8, area.getHeight(),
                    juce::Justification::centredLeft);
    }
}
} // namespace HardwarePanelStyle
