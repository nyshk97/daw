#pragma once

#include <cmath>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"

// アプリ全体のLookAndFeel。デフォルトsans-serifをmacOSのシステムUIフォント
// （SF Pro、CoreText内部名 ".AppleSystemUIFont"）に差し替える。
// 日本語グリフはSF Proに無いため、CoreTextのフォールバックでヒラギノになる。
// タイプフェイス名を明示したフォント（Fonts::mono等）は基底実装がそのまま解決する。
// 所有はDawApplication側（shutdownでsetDefaultLookAndFeel(nullptr)してから破棄すること）
class AppLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static constexpr int menuItemGutter = 22; // メニュー項目の左ガター（チェックマーク用。macと同じく常に確保）

    AppLookAndFeel()
    {
        setDefaultSansSerifTypefaceName (".AppleSystemUIFont");
        // V4デフォルトの青みグレー(0xff323e44)は他パネルの無彩色系から浮くため差し替える
        setColour (juce::ResizableWindow::backgroundColourId, Theme::windowBg);
        // 水平スライダー（音量・ベロシティ）: 溝は背景より一段明るく、値部分はアクセント青で塗る
        setColour (juce::Slider::backgroundColourId, Theme::controlBg);
        setColour (juce::Slider::trackColourId, Theme::accent);
        // メニュー背景は透明にして自前の角丸パネルを描く（非opaqueなウィンドウになり角丸の外が抜ける。
        // juce_PopupMenu.cppのMenuWindowがこの色のisOpaque()でウィンドウの不透明化を決めている）
        setColour (juce::PopupMenu::backgroundColourId, juce::Colours::transparentBlack);
    }

    // JUCEデフォルトのコントロールフォント（高さ連動で最大16px）はmacOS標準の13pxより
    // 大きく、全角を目一杯使う日本語では特に大きく見えるためHIGの13pxに固定する。
    // getLabelFontは「Label自身のフォントを返す」実装なのでoverrideしない
    // （無条件に13pxを返すとFonts::mono等のsetFont指定を壊す。Labelは生成側でsetFontする）
    juce::Font getTextButtonFont (juce::TextButton& button, int) override
    {
        return button.getProperties().contains ("flatButton") ? Fonts::bodyStrong()
                                                              : Fonts::body();
    }
    juce::Font getComboBoxFont (juce::ComboBox&) override          { return Fonts::body(); }
    juce::Font getPopupMenuFont() override                         { return Fonts::body(); }

    // デフォルトのツールチップは13px boldで、日本語（ヒラギノ太字）だと主張が強すぎる。
    // 11px regular（Fonts::small）・PopupMenu系と同じ配色のパネルで控えめに描き直す。
    // bounds計算も同じレイアウトを使わないと太字前提の広い箱になるため両方overrideする
    static juce::TextLayout layoutTooltipText (const juce::String& text, juce::Colour colour)
    {
        juce::AttributedString s;
        s.setJustification (juce::Justification::centred);
        s.append (text, Fonts::small(), colour);
        juce::TextLayout tl;
        tl.createLayoutWithBalancedLineLengths (s, 400.0f);
        return tl;
    }

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override
    {
        const auto tl = layoutTooltipText (tipText, juce::Colours::black);
        const int w = (int) std::ceil (tl.getWidth()) + 12;
        const int h = (int) std::ceil (tl.getHeight()) + 6;
        return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12)
                                                                           : screenPos.x + 24,
                                     screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6)
                                                                           : screenPos.y + 6,
                                     w, h)
            .constrainedWithin (parentArea);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        const auto bounds = juce::Rectangle<float> ((float) width, (float) height);
        g.setColour (Theme::popupBg);
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (Theme::popupBorder);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.0f);
        layoutTooltipText (text, juce::Colours::white.withAlpha (0.9f))
            .draw (g, bounds);
    }

    // ボタンの状態表現は「hoverで少し明るく・押下で沈む（暗く）」に統一する。
    // 基底実装は押下で明るくなり（contrasting）物理感が逆になるため描き直す。
    // "flatButton"プロパティを立てたTextButton（M/S等の小型トグル）はアウトライン無しの
    // フラット角丸、それ以外は基底と同じ「塗り＋縁取り」の角丸で描く
    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override
    {
        if (! button.getProperties().contains ("flatButton"))
        {
            // 通常状態の見た目は基底実装（LookAndFeel_V4）と同一に保つ
            const float cornerSize = 6.0f;
            const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f, 0.5f);
            auto colour = backgroundColour
                              .withMultipliedSaturation (button.hasKeyboardFocus (true) ? 1.3f : 0.9f)
                              .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f);
            if (shouldDrawButtonAsDown)
                colour = colour.darker (0.25f);
            else if (shouldDrawButtonAsHighlighted)
                colour = colour.brighter (0.08f);

            g.setColour (colour);
            g.fillRoundedRectangle (bounds, cornerSize);
            g.setColour (button.findColour (juce::ComboBox::outlineColourId));
            g.drawRoundedRectangle (bounds, cornerSize, 1.0f);
            return;
        }

        auto colour = backgroundColour;
        if (shouldDrawButtonAsDown)
            colour = colour.darker (0.25f);
        else if (shouldDrawButtonAsHighlighted)
            colour = colour.brighter (0.1f);

        g.setColour (colour);
        g.fillRoundedRectangle (button.getLocalBounds().toFloat(), 4.0f);
    }

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
            g.setColour (meter > 0.9f ? Theme::recordRed : Theme::playGreen);
            g.fillRoundedRectangle (meterBar, meterH * 0.5f);
        }

        const float thumbD = 12.0f;
        g.setColour (slider.findColour (juce::Slider::thumbColourId));
        g.fillEllipse (juce::Rectangle<float> (thumbD, thumbD).withCentre ({ sliderPos, cy }));
    }

    // 小径ノブ（ミキサーのPan・send）用のロータリー描画。V4デフォルト（塗り円＋点サム）は
    // 小さいサイズだと状態が読めないため、「溝アーク＋値アーク＋ポインタ線」で描き直す。
    // 範囲が負〜正のスライダー（Pan）は値アークを中央起点の双方向にする
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (1.5f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const float thickness = juce::jmax (2.0f, radius * 0.22f);
        const float arcRadius = radius - thickness * 0.5f;

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                             rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (slider.findColour (juce::Slider::rotarySliderOutlineColourId));
        g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));

        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float valueFrom = bipolar ? (rotaryStartAngle + rotaryEndAngle) * 0.5f
                                        : rotaryStartAngle;
        if (std::abs (angle - valueFrom) > 0.02f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                 juce::jmin (valueFrom, angle), juce::jmax (valueFrom, angle), true);
            g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId));
            g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        }

        juce::Path pointer;
        pointer.startNewSubPath (centre.getPointOnCircumference (radius * 0.15f, angle));
        pointer.lineTo (centre.getPointOnCircumference (arcRadius - thickness * 0.6f, angle));
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.strokePath (pointer, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    // ---- PopupMenu（右クリックメニュー・ComboBoxのドロップダウン）----
    // V4デフォルトは直角パネル＋ハード枠＋全幅ハイライトで浮くため、
    // macOSのメニュー風（角丸パネル・角丸ハイライト・上下パディング・チェック用の左ガター）に描き直す

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        const auto bounds = juce::Rectangle<float> ((float) width, (float) height);
        g.setColour (Theme::popupBg);
        g.fillRoundedRectangle (bounds, 8.0f);
        g.setColour (Theme::popupBorder);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);
    }

    void drawPopupMenuBackgroundWithOptions (juce::Graphics& g, int width, int height,
                                             const juce::PopupMenu::Options&) override
    {
        drawPopupMenuBackground (g, width, height);
    }

    int getPopupMenuBorderSize() override { return 5; } // 項目とパネル縁の余白

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                    int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override
    {
        if (isSeparator)
        {
            idealWidth = 50;
            idealHeight = 9;
            return;
        }
        idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : 26;
        // 幅の計測にはショートカット表記が渡ってこないため、右側に多めの余白を足しておく
        idealWidth = menuItemGutter
                     + juce::GlyphArrangement::getStringWidthInt (getPopupMenuFont(), text) + 44;
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted, bool isTicked,
                            bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override
    {
        if (isSeparator)
        {
            g.setColour (Theme::popupBorder);
            g.fillRect (area.reduced (8, 0).withHeight (1).withY (area.getCentreY()));
            return;
        }

        const bool showHighlight = isHighlighted && isActive;
        if (showHighlight)
        {
            g.setColour (Theme::accent);
            g.fillRoundedRectangle (area.toFloat(), 5.0f);
        }

        const auto baseColour = textColour != nullptr ? *textColour : juce::Colours::white;
        const auto mainColour = baseColour.withAlpha (isActive ? (showHighlight ? 1.0f : 0.9f)
                                                               : 0.3f);
        const float cy = (float) area.getCentreY();

        if (isTicked)
        {
            juce::Path tick;
            tick.startNewSubPath (0.0f, 5.5f);
            tick.lineTo (3.5f, 9.0f);
            tick.lineTo (10.0f, 0.5f);
            g.setColour (mainColour);
            g.strokePath (tick, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded),
                          juce::AffineTransform::translation ((float) area.getX() + 7.0f,
                                                              cy - 5.0f));
        }
        else if (icon != nullptr)
        {
            icon->drawWithin (g,
                              area.toFloat().withWidth ((float) menuItemGutter).reduced (5.0f),
                              juce::RectanglePlacement::centred, isActive ? 1.0f : 0.3f);
        }

        g.setColour (mainColour);
        g.setFont (getPopupMenuFont());
        g.drawText (text, area.withTrimmedLeft (menuItemGutter).withTrimmedRight (10),
                    juce::Justification::centredLeft);

        if (hasSubMenu)
        {
            const float cx = (float) area.getRight() - 12.0f;
            juce::Path chevron;
            chevron.startNewSubPath (cx - 1.75f, cy - 3.5f);
            chevron.lineTo (cx + 1.75f, cy);
            chevron.lineTo (cx - 1.75f, cy + 3.5f);
            g.setColour (baseColour.withAlpha (showHighlight ? 0.9f : 0.6f));
            g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));
        }
        else if (shortcutKeyText.isNotEmpty())
        {
            g.setColour (baseColour.withAlpha (showHighlight ? 0.9f : 0.5f));
            g.drawText (shortcutKeyText, area.withTrimmedRight (10),
                        juce::Justification::centredRight);
        }
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
