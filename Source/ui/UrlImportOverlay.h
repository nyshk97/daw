#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"

// 「URLから読み込む…」のURL入力オーバーレイ。BounceOverlay と同じ
// 「親全面を覆いパネルを自前描画」方式で、寸法（幅380）も揃えてある
// （確定するとそのまま進捗オーバーレイに切り替わるので、枠が動かない方が落ち着く）。
//
// 開いてキャンセルするだけでは副作用を起こさない（プロジェクトSRの確定も一時ファイル作成もしない）。
// 確定できるのは http:// / https:// で始まる入力だけ — サイトは制限しないが、
// yt-dlp のオプションとして解釈される文字列（--exec 等）を入口で弾くため
class UrlImportOverlay : public juce::Component
{
public:
    std::function<void (juce::String)> onSubmit;
    std::function<void()> onCancel;

    UrlImportOverlay()
    {
        editor.setMultiLine (false);
        editor.setReturnKeyStartsNewLine (false);
        editor.setFont (Fonts::mono (13.0f)); // URLは等幅の方が読み取りやすい
        editor.setTextToShowWhenEmpty (juce::String::fromUTF8 (u8"https://…"),
                                       juce::Colours::white.withAlpha (0.35f));
        // 地と枠はメモ欄と同じテキスト入力欄のものを流用する（色の重複定義を作らない）
        editor.setColour (juce::TextEditor::backgroundColourId, Theme::memoFocusedBg);
        editor.setColour (juce::TextEditor::outlineColourId, Theme::memoBorder);
        editor.setColour (juce::TextEditor::focusedOutlineColourId, Theme::accent);
        editor.setColour (juce::TextEditor::textColourId, juce::Colours::white.withAlpha (0.9f));
        editor.setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.4f));

        editor.onTextChange = [this] { repaint (submitButtonBounds()); };
        editor.onReturnKey  = [this] { submit(); };
        editor.onEscapeKey  = [this] { dismissWithCancel(); };

        addAndMakeVisible (editor);
        setInterceptsMouseClicks (true, true); // 表示中はモーダル（背後のUIを塞ぐ）
    }

    // クリップボードにURLが入っていれば拾って全選択状態にする
    //（ブラウザでコピー → メニューを選ぶ → Return だけで済む）
    void show()
    {
        const auto clipboard = juce::SystemClipboard::getTextFromClipboard().trim();
        editor.setText (isAcceptableUrl (clipboard) ? clipboard : juce::String(), false);

        setVisible (true);
        toFront (true);
        editor.grabKeyboardFocus();
        editor.selectAll();
        repaint();
    }

    void dismiss() { setVisible (false); }

    // Esc（MainComponent::keyPressed 側からも呼ぶ）
    void dismissWithCancel()
    {
        setVisible (false);
        if (onCancel != nullptr)
            onCancel();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.45f));

        const auto panel = panelBounds();
        g.setColour (Theme::popupBg);
        g.fillRoundedRectangle (panel.toFloat(), 8.0f);
        g.setColour (Theme::popupBorder);
        g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), 8.0f, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.95f));
        g.setFont (Fonts::title());
        g.drawText (juce::String::fromUTF8 (u8"URLから読み込む"),
                    panel.withHeight (titleHeight).withTrimmedTop (padY),
                    juce::Justification::centred);

        drawButton (g, cancelButtonBounds(), juce::String::fromUTF8 (u8"キャンセル"),
                    false, true, cancelHovered);
        drawButton (g, submitButtonBounds(), juce::String::fromUTF8 (u8"取り込み"),
                    true, canSubmit(), submitHovered);
    }

    void resized() override
    {
        editor.setBounds (fieldBounds());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (cancelButtonBounds().contains (e.getPosition()))
            dismissWithCancel();
        else if (submitButtonBounds().contains (e.getPosition()))
            submit();
        // パネル外クリックでは閉じない（BounceOverlayと同じ。キャンセルは明示操作のみ）
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool overCancel = cancelButtonBounds().contains (e.getPosition());
        const bool overSubmit = submitButtonBounds().contains (e.getPosition());

        if (overCancel != cancelHovered)
        {
            cancelHovered = overCancel;
            repaint (cancelButtonBounds());
        }
        if (overSubmit != submitHovered)
        {
            submitHovered = overSubmit;
            repaint (submitButtonBounds());
        }
    }

    static bool isAcceptableUrl (const juce::String& text)
    {
        const auto trimmed = text.trim();
        return trimmed.startsWith ("http://") || trimmed.startsWith ("https://");
    }

private:
    static constexpr int panelWidth  = 380;
    static constexpr int titleHeight = 22;
    static constexpr int fieldHeight = 30;
    static constexpr int buttonHeight = 28;
    static constexpr int padX = 20;
    static constexpr int padY = 16;
    static constexpr int gap  = 12;
    static constexpr int cancelWidth = 96;
    static constexpr int submitWidth = 84;

    juce::Rectangle<int> panelBounds() const
    {
        const int h = padY + titleHeight + gap + fieldHeight + gap + buttonHeight + padY - 2;
        return juce::Rectangle<int> (panelWidth, h).withCentre (getLocalBounds().getCentre());
    }

    juce::Rectangle<int> fieldBounds() const
    {
        const auto panel = panelBounds();
        return { panel.getX() + padX, panel.getY() + padY + titleHeight + gap,
                 panel.getWidth() - padX * 2, fieldHeight };
    }

    juce::Rectangle<int> submitButtonBounds() const
    {
        const auto panel = panelBounds();
        return { panel.getRight() - padX - submitWidth,
                 fieldBounds().getBottom() + gap, submitWidth, buttonHeight };
    }

    juce::Rectangle<int> cancelButtonBounds() const
    {
        const auto submitArea = submitButtonBounds();
        return { submitArea.getX() - 8 - cancelWidth, submitArea.getY(), cancelWidth, buttonHeight };
    }

    bool canSubmit() const { return isAcceptableUrl (editor.getText()); }

    void submit()
    {
        const auto url = editor.getText().trim();
        if (! isAcceptableUrl (url))
            return;

        setVisible (false);
        if (onSubmit != nullptr)
            onSubmit (url);
    }

    void drawButton (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text,
                     bool primary, bool enabled, bool hovered) const
    {
        const auto bounds = area.toFloat();

        if (primary && enabled)
            g.setColour (hovered ? Theme::accent.brighter (0.15f) : Theme::accent);
        else
            g.setColour (juce::Colours::white.withAlpha (hovered && enabled ? 0.14f : 0.08f));
        g.fillRoundedRectangle (bounds, 5.0f);

        g.setColour (juce::Colours::white.withAlpha (enabled ? 0.9f : 0.35f));
        g.setFont (Fonts::body());
        g.drawText (text, area, juce::Justification::centred);
    }

    juce::TextEditor editor;
    bool cancelHovered = false;
    bool submitHovered = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UrlImportOverlay)
};
