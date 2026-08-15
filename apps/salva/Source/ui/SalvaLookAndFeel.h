#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Salva全体のVinyl Warmテーマ（2026-08-15確定モック案B）。
// JUCEのポップアップメニュー・ComboBoxは全部自前描画なので、ここで角丸＋暖色に差し替える。
// メニュー背景色をわずかに非不透明にするのが肝: MenuWindowは背景色の不透明度で
// ウィンドウの透過可否を決める（juce_PopupMenu.cpp の setOpaque）ため、
// 完全不透明だと角丸の外が白く塗られてしまう。
// パレットの値は SalvaMainComponent.cpp の定数と同値（各ファイル内定数の流儀に合わせる）
class SalvaLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SalvaLookAndFeel()
    {
        // ColourScheme順: windowBackground, widgetBackground, menuBackground,
        //                 outline, defaultText, defaultFill, highlightedText, highlightedFill, menuText
        setColourScheme ({ juce::Colour (0xff201f24), juce::Colour (0xff2e2d34), juce::Colour (0xff242328),
                           juce::Colour (0xff46444e), juce::Colour (0xfff2e8d5), juce::Colour (0xffff7a2e),
                           juce::Colour (0xfff2e8d5), juce::Colour (0xff3a3842), juce::Colour (0xfff2e8d5) });
        setColour (juce::PopupMenu::backgroundColourId, menuBg);
        setColour (juce::PopupMenu::textColourId, itemText);
        setColour (juce::PopupMenu::highlightedTextColourId, cream);
    }

    //==============================================================================
    // ポップアップメニュー（キャッシュ削除メニュー・ComboBoxの選択肢の両方に効く）

    void drawPopupMenuBackgroundWithOptions (juce::Graphics& g, int width, int height,
                                             const juce::PopupMenu::Options&) override
    {
        const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
        g.setColour (menuBg);
        g.fillRoundedRectangle (r, cornerRadius);
        g.setColour (menuBorder);
        g.drawRoundedRectangle (r, cornerRadius, 1.0f);
    }

    juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions (12.5f)); }

    int getPopupMenuBorderSize() override { return 6; } // 角丸ハイライトの上下逃げ

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int /*standardMenuItemHeight*/,
                                    int& idealWidth, int& idealHeight) override
    {
        if (isSeparator)
        {
            idealWidth = 50;
            idealHeight = 9;
            return;
        }
        idealHeight = 28;
        // 左右余白（ハイライト逃げ5＋文字パディング8）×2 ぶん
        idealWidth = juce::GlyphArrangement::getStringWidthInt (getPopupMenuFont(), text) + 30;
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                            const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* /*icon*/, const juce::Colour* textColourToUse) override
    {
        if (isSeparator)
        {
            auto line = area.reduced (12, 0);
            g.setColour (menuBorder);
            g.fillRect (line.withHeight (1).withY (area.getCentreY()));
            return;
        }

        auto r = area.reduced (5, 1);
        if (isHighlighted && isActive)
        {
            g.setColour (accent.withAlpha (0.16f));
            g.fillRoundedRectangle (r.toFloat(), 6.0f);
        }

        auto content = r.reduced (8, 0);

        // チェック状態はドット欄でなくアンバー文字で示す（アイコンの無いメニューの余白を太らせない）
        const auto mainColour = textColourToUse != nullptr ? *textColourToUse
                                : ! isActive               ? itemText.withAlpha (0.35f)
                                : isTicked                 ? accent
                                : isHighlighted            ? cream
                                                           : itemText;
        g.setFont (getPopupMenuFont());

        if (hasSubMenu)
        {
            const auto arrowArea = content.removeFromRight (12).toFloat();
            juce::Path p;
            const auto cx = arrowArea.getCentreX() - 2.0f, cy = arrowArea.getCentreY();
            p.startNewSubPath (cx, cy - 3.5f);
            p.lineTo (cx + 4.0f, cy);
            p.lineTo (cx, cy + 3.5f);
            g.setColour (mainColour);
            g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        }

        if (shortcutKeyText.isNotEmpty())
        {
            g.setColour (dimText);
            g.drawText (shortcutKeyText, content, juce::Justification::centredRight, true);
        }

        g.setColour (mainColour);
        g.drawText (text, content, juce::Justification::centredLeft, true);
    }

    //==============================================================================
    // TextButtonの文字サイズ上書き: button.getProperties()["fontSize"] があればそれを使う
    // （TextButtonにはフォント設定APIが無いため、個別ボタンの縮小はここで受ける）

    juce::Font getTextButtonFont (juce::TextButton& button, int buttonHeight) override
    {
        auto font = [&]
        {
            if (const auto v = button.getProperties()["fontSize"]; ! v.isVoid())
                return juce::Font (juce::FontOptions ((float) (double) v));
            return LookAndFeel_V4::getTextButtonFont (button, buttonHeight);
        }();
        // 主役ボタン（塗り）は太字で押しの強さを揃える
        if (button.getProperties()["style"].toString() == "primary")
            font = font.boldened();
        return font;
    }

    //==============================================================================
    // ボタンの3階層（2026-08-15確定ボタンモック）: getProperties()["style"] で役割を指定。
    //   "primary" = オレンジ塗り・枠なし（主役。1画面に1つまで）
    //   "action"  = 薄い地＋描画アイコン＋文字（新規録音など）
    //   "ghost"   = 普段は透明・ホバーで地が出る（戻るなどのナビ）
    // アイコンは ["icon"] = "recdot"（赤い正円）/ "back"（シェブロン）。
    // 「●」「←」を文字グリフで置くとベースラインずれ・字間の詰まりが出るため描画にする。
    // style未指定のボタンは従来のLnF_V4描画（枠付きチップ）のまま

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool highlighted, bool down) override
    {
        // "forceHover": 親（空状態のカード等）がホバー中もボタンをホバー表示にする
        // （カードとボタンはクリックの結果が同じなので、表示も揃える）
        highlighted = highlighted || (bool) button.getProperties().getWithDefault ("forceHover", false);
        const auto style = button.getProperties()["style"].toString();
        if (style.isEmpty())
        {
            LookAndFeel_V4::drawButtonBackground (g, button, backgroundColour, highlighted, down);
            return;
        }
        const auto r = button.getLocalBounds().toFloat().reduced (0.5f);
        if (style == "primary")
        {
            g.setColour (down ? accent.darker (0.15f) : highlighted ? accentHover : accent);
            g.fillRoundedRectangle (r, 8.0f);
        }
        else if (style == "action")
        {
            g.setColour (down ? buttonFill.darker (0.2f) : highlighted ? hoverFill : buttonFill);
            g.fillRoundedRectangle (r, 8.0f);
        }
        else if (style == "ghost")
        {
            if (highlighted || down)
            {
                g.setColour (buttonFill);
                g.fillRoundedRectangle (r, 7.0f);
            }
        }
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool highlighted, bool down) override
    {
        highlighted = highlighted || (bool) button.getProperties().getWithDefault ("forceHover", false);
        const auto style = button.getProperties()["style"].toString();
        if (style.isEmpty())
        {
            LookAndFeel_V4::drawButtonText (g, button, highlighted, down);
            return;
        }

        const auto font = getTextButtonFont (button, button.getHeight());
        g.setFont (font);
        auto textColour = style == "primary" ? accentTextDark
                        : style == "ghost"   ? (highlighted ? cream : dimText)
                                             : itemText;
        if (! button.isEnabled())
            textColour = textColour.withAlpha (0.5f);

        // アイコン＋間隔＋文字を1組として中央揃え（アイコンなしなら文字だけが中央に来る）
        const auto icon = button.getProperties()["icon"].toString();
        const float iconW = icon == "recdot" ? 9.0f : icon == "back" ? 7.0f : 0.0f;
        const float gap = iconW > 0.0f ? 9.0f : 0.0f;
        const auto bounds = button.getLocalBounds().toFloat();
        const float textW = juce::GlyphArrangement::getStringWidth (font, button.getButtonText());
        const float startX = bounds.getCentreX() - (iconW + gap + textW) * 0.5f;
        const float cy = bounds.getCentreY();

        if (icon == "recdot")
        {
            if (highlighted && button.isEnabled()) // ホバーでほんのり光る
            {
                g.setColour (recordRed.withAlpha (0.35f));
                g.fillEllipse (startX - 3.5f, cy - 8.0f, 16.0f, 16.0f);
            }
            g.setColour (recordRed);
            g.fillEllipse (startX, cy - 4.5f, 9.0f, 9.0f);
        }
        else if (icon == "back")
        {
            juce::Path p;
            p.startNewSubPath (startX + 5.5f, cy - 4.5f);
            p.lineTo (startX + 1.0f, cy);
            p.lineTo (startX + 5.5f, cy + 4.5f);
            g.setColour (textColour);
            g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        }

        g.setColour (textColour);
        g.drawText (button.getButtonText(),
                    juce::Rectangle<float> (startX + iconW + gap, 0.0f, textW + 4.0f, bounds.getHeight()),
                    juce::Justification::centredLeft, false);
    }

    //==============================================================================
    // ComboBox本体（閉じた状態の箱と矢印）

    void drawComboBox (juce::Graphics& g, int width, int height, bool /*isButtonDown*/,
                       int, int, int, int, juce::ComboBox& box) override
    {
        const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (r, 6.0f, 1.0f);

        // 右端のシェブロン
        juce::Path p;
        const auto cx = (float) width - 15.0f, cy = (float) height * 0.5f - 1.0f;
        p.startNewSubPath (cx - 4.0f, cy);
        p.lineTo (cx, cy + 4.0f);
        p.lineTo (cx + 4.0f, cy);
        g.setColour (dimText);
        g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (13.0f)); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 1, box.getWidth() - 32, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

private:
    static constexpr float cornerRadius = 9.0f;
    // 0xfa = 微透過（角丸の透明抜きのため非不透明にする。見た目はほぼ不透明）
    const juce::Colour menuBg { 0xfa1f1e23 };
    const juce::Colour menuBorder { 0xff46444e };
    const juce::Colour itemText { 0xffe8e0d2 };
    const juce::Colour cream { 0xfff2e8d5 };
    const juce::Colour dimText { 0xff97908a };
    const juce::Colour accent { 0xffff7a2e };
    const juce::Colour accentHover { 0xffff8a45 };
    const juce::Colour accentTextDark { 0xff2b1507 };
    const juce::Colour buttonFill { 0xff2e2d34 };
    const juce::Colour hoverFill { 0xff3a3842 };
    const juce::Colour recordRed { 0xffd94a43 };
};
