#include "RightPanel.h"

#include "Fonts.h"
#include "Theme.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }
}

MemoEditor::MemoEditor()
{
    // 枠はpaintOverChildrenで自前描画するため、JUCE側の枠は両状態とも消す
    setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
}

// setColourはrepaintを伴わない（Component::colourChangedは空実装）ため明示的に再描画する
void MemoEditor::focusGained (FocusChangeType cause)
{
    juce::TextEditor::focusGained (cause);
    setColour (juce::TextEditor::backgroundColourId, Theme::memoFocusedBg);
    repaint();
}

void MemoEditor::focusLost (FocusChangeType cause)
{
    juce::TextEditor::focusLost (cause);
    setColour (juce::TextEditor::backgroundColourId, Theme::timelineBg);
    repaint();
}

void MemoEditor::paintOverChildren (juce::Graphics& g)
{
    g.setColour (Theme::memoBorder);
    g.drawRect (getLocalBounds(), 1);
}

RightPanel::RightPanel()
{
    addAndMakeVisible (titleLabel);
    titleLabel.setFont (Fonts::smallStrong());
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.72f));
    titleLabel.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (scopeLabel);
    scopeLabel.setFont (Fonts::small());
    scopeLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.28f));
    scopeLabel.setJustificationType (juce::Justification::centredRight);

    addAndMakeVisible (memoEditor);
    memoEditor.setMultiLine (true, true);
    memoEditor.setReturnKeyStartsNewLine (true);
    memoEditor.setEscapeAndReturnKeysConsumed (true);
    memoEditor.setScrollbarsShown (true);
    memoEditor.setTabKeyUsedAsCharacter (false);
    memoEditor.setFont (Fonts::memo());
    memoEditor.setIndents (12, 10);
    memoEditor.setTextToShowWhenEmpty (jp (u8"このプロジェクトのメモ…"),
                                          juce::Colours::white.withAlpha (0.28f));
    memoEditor.setColour (juce::TextEditor::backgroundColourId, Theme::timelineBg);
    memoEditor.setColour (juce::TextEditor::textColourId,
                          juce::Colours::white.withAlpha (0.88f));
    memoEditor.setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.65f));
    memoEditor.onTextChange = [this]
    {
        if (bindingMemo || project == nullptr)
            return;
        project->memo = memoEditor.getText();
        if (onMemoChanged)
            onMemoChanged();
    };

    addChildComponent (browser);
    addChildComponent (gacha);
    refreshMode();
    setVisible (false);
}

void RightPanel::setProject (Project* projectToUse)
{
    project = projectToUse;
    bindingMemo = true;
    memoEditor.setText (project != nullptr ? project->memo : juce::String(),
                        juce::dontSendNotification);
    bindingMemo = false;
    gacha.setProject (projectToUse);
}

void RightPanel::open (Mode newMode)
{
    currentMode = newMode;
    openState = true;
    setVisible (true);
    refreshMode();
}

void RightPanel::close()
{
    openState = false;
    setVisible (false);
}

void RightPanel::focusNotesEditor()
{
    if (openState && currentMode == Mode::notes)
        memoEditor.grabKeyboardFocus();
}

void RightPanel::refreshMode()
{
    const bool notes = currentMode == Mode::notes;
    const bool files = currentMode == Mode::files;
    const bool isGacha = currentMode == Mode::gacha;
    titleLabel.setText (notes    ? jp (u8"プロジェクトメモ")
                        : files  ? jp (u8"ファイル")
                                 : jp (u8"ドラムガチャ"),
                        juce::dontSendNotification);
    scopeLabel.setText (notes ? "PROJECT" : files ? "AUDIO" : "GACHA", juce::dontSendNotification);
    memoEditor.setVisible (notes);
    browser.setVisible (files);
    gacha.setVisible (isGacha);
    if (isGacha)
    {
        // 開くたびに列挙し直す（分析の完了・ツールの導入を開き直しで拾う）
        gacha.refreshAvailability();
        gacha.refreshCards();
    }
    resized();
    repaint();
}

void RightPanel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::chooserPanelBg);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.drawVerticalLine (0, 0.0f, (float) getHeight());
    g.drawHorizontalLine (headerHeight - 1, 0.0f, (float) getWidth());
    g.setColour (Theme::windowBg.darker (0.08f));
    g.fillRect (1, 0, juce::jmax (0, getWidth() - 1), headerHeight - 1);
}

void RightPanel::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (headerHeight).reduced (14, 0);
    scopeLabel.setBounds (header.removeFromRight (72));
    titleLabel.setBounds (header);

    if (memoEditor.isVisible())
        memoEditor.setBounds (area.reduced (14));
    if (browser.isVisible())
        browser.setBounds (area);
    if (gacha.isVisible())
        gacha.setBounds (area);
}
