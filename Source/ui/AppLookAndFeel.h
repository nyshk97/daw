#pragma once

#include <cmath>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"

// アプリ全体のLookAndFeel。デフォルトsans-serifをmacOSのシステムUIフォント
// （SF Pro、CoreText内部名 ".AppleSystemUIFont"）に差し替える。
// 日本語グリフはSF Proに無いため、CoreTextのフォールバックでヒラギノになる。
// タイプフェイス名を明示したフォント（Fonts::mono等）は基底実装がそのまま解決する。
// 所有はDawApplication側（shutdownでsetDefaultLookAndFeel(nullptr)してから破棄すること）
class AppLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AppLookAndFeel()
    {
        setDefaultSansSerifTypefaceName (".AppleSystemUIFont");
        // 水平スライダー（音量・ベロシティ）: 溝は背景より一段明るく、値部分はアクセント青で塗る
        setColour (juce::Slider::backgroundColourId, juce::Colour (0xff3f3f46));
        setColour (juce::Slider::trackColourId, juce::Colour (0xff4a6ea9));
    }

    // JUCEデフォルトのコントロールフォント（高さ連動で最大16px）はmacOS標準の13pxより
    // 大きく、全角を目一杯使う日本語では特に大きく見えるためHIGの13pxに固定する。
    // getLabelFontは「Label自身のフォントを返す」実装なのでoverrideしない
    // （無条件に13pxを返すとFonts::mono等のsetFont指定を壊す。Labelは生成側でsetFontする）
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return Fonts::body(); }
    juce::Font getComboBoxFont (juce::ComboBox&) override          { return Fonts::body(); }
    juce::Font getPopupMenuFont() override                         { return Fonts::body(); }

    // 水平スライダーはデフォルトだと溝が背景に溶けて値も読めないため、
    // 「丸端の溝＋左端からつまみまでの値塗り＋つまみ」で描き直す
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearHorizontal)
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                                    minSliderPos, maxSliderPos, style, slider);
            return;
        }

        const float trackH = 4.0f;
        const float cy = (float) y + (float) height * 0.5f;
        const auto track = juce::Rectangle<float> ((float) x, cy - trackH * 0.5f,
                                                   (float) width, trackH);

        g.setColour (slider.findColour (juce::Slider::backgroundColourId));
        g.fillRoundedRectangle (track, trackH * 0.5f);

        g.setColour (slider.findColour (juce::Slider::trackColourId));
        g.fillRoundedRectangle (track.withRight (sliderPos), trackH * 0.5f);

        // レベルメーター（音量スライダーのみ。TrackHeaderComponentがmeterLevelプロパティで渡す）。
        // 描画順は 溝 → 値塗り → メーター → つまみ で固定。
        // 値はリニア振幅のままだと実用レベル（-20dB前後）がほぼ見えないため、
        // -60dB..0dB を 0..1 に写すdBスケールで表示する（実DAWのメーターと同じ）
        const float meter = (float) (double) slider.getProperties()
                                                   .getWithDefault ("meterLevel", 0.0);
        if (meter > 0.001f) // -60dB未満は表示しない
        {
            const float db = 20.0f * std::log10 (meter);
            const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            const float meterH = 2.0f;
            const auto meterBar = juce::Rectangle<float> (
                (float) x, cy - meterH * 0.5f, norm * (float) width, meterH);
            g.setColour (meter > 0.9f ? juce::Colour (0xffd94a43) : juce::Colour (0xff7bc47b));
            g.fillRoundedRectangle (meterBar, meterH * 0.5f);
        }

        const float thumbD = 12.0f;
        g.setColour (slider.findColour (juce::Slider::thumbColourId));
        g.fillEllipse (juce::Rectangle<float> (thumbD, thumbD).withCentre ({ sliderPos, cy }));
    }

    // 基底実装（LookAndFeel_V4::drawComboBox）のコピー。デフォルトの矢印（幅14px・
    // 太さ2px）は主張が強いため、macOSのポップアップボタン風の小さく細いシェブロンにする
    void drawComboBox (juce::Graphics& g, int width, int height, bool,
                       int, int, int, int, juce::ComboBox& box) override
    {
        const float cornerSize = 3.0f;
        const auto boxBounds = juce::Rectangle<int> (0, 0, width, height).toFloat();

        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (boxBounds, cornerSize);

        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (boxBounds.reduced (0.5f, 0.5f), cornerSize, 1.0f);

        const float cx = (float) width - 13.0f;
        const float cy = (float) height * 0.5f;
        juce::Path path;
        path.startNewSubPath (cx - 3.5f, cy - 1.5f);
        path.lineTo (cx, cy + 2.0f);
        path.lineTo (cx + 3.5f, cy - 1.5f);

        g.setColour (box.findColour (juce::ComboBox::arrowColourId)
                         .withAlpha (box.isEnabled() ? 0.7f : 0.2f));
        g.strokePath (path, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }

    // ToggleButtonはフォント取得フックがなく描画内に15pxがハードコードされているため、
    // 基底実装（LookAndFeel_V4::drawToggleButton）をコピーしてフォントだけ差し替える
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        const auto font = Fonts::body();
        const float tickWidth = font.getHeight() * 1.1f;

        drawTickBox (g, button, 4.0f, ((float) button.getHeight() - tickWidth) * 0.5f,
                     tickWidth, tickWidth,
                     button.getToggleState(),
                     button.isEnabled(),
                     shouldDrawButtonAsHighlighted,
                     shouldDrawButtonAsDown);

        g.setColour (button.findColour (juce::ToggleButton::textColourId));
        g.setFont (font);

        if (! button.isEnabled())
            g.setOpacity (0.5f);

        g.drawFittedText (button.getButtonText(),
                          button.getLocalBounds().withTrimmedLeft (juce::roundToInt (tickWidth) + 10)
                                                 .withTrimmedRight (2),
                          juce::Justification::centredLeft, 10);
    }
};
