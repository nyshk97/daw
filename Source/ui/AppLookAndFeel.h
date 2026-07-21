#pragma once

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
    }

    // JUCEデフォルトのコントロールフォント（高さ連動で最大16px）はmacOS標準の13pxより
    // 大きく、全角を目一杯使う日本語では特に大きく見えるためHIGの13pxに固定する。
    // getLabelFontは「Label自身のフォントを返す」実装なのでoverrideしない
    // （無条件に13pxを返すとFonts::mono等のsetFont指定を壊す。Labelは生成側でsetFontする）
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return Fonts::body(); }
    juce::Font getComboBoxFont (juce::ComboBox&) override          { return Fonts::body(); }
    juce::Font getPopupMenuFont() override                         { return Fonts::body(); }

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
