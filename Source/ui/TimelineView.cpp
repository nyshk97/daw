#include "TimelineView.h"

#include <cmath>

#include "Fonts.h"
#include "GainControls.h"
#include "Shortcuts.h"
#include "Theme.h"
#include "../shared/AudioFileTypes.h"
#include "../shared/ClipDomains.h"
#include "../shared/MidiFileTypes.h"
#include "../shared/ReferenceTools.h"
#include "../shared/GainScale.h"
#include "../shared/Log.h"
#include "../shared/SongFade.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

// リージョンゲインの吹き出しの中身（CallOutBoxに載せる）。見出し＋現在値＋スライダーの1行構成。
// スライダーの作法（±12dB・0.1刻み・0dB吸着・ドラッグ中のdBポップアップ・0dB起点の帯）は
// サンプル音源のGAINと共通（GainControls.h / GainScale.h）。
//
// undoの粒度は「値が実際に変わる最初の瞬間に1件」。onDragStart で積んではいけない:
// JUCEのSliderは mouseDown ごとに ScopedDragNotification を作り（juce_Slider.cpp:900）、
// ダブルクリック確定時にも別の通知を出す（同:1121）ため、1回のダブルクリックで
// onDragStart が最大3回発火する（＝undoが3件積まれる）。値が動いていないクリックでも積まれてしまう
class RegionGainPanel : public juce::Component
{
public:
    RegionGainPanel (float initialGain, std::function<void()> willEditIn,
                     std::function<void (float)> applyIn)
        : willEdit (std::move (willEditIn)), apply (std::move (applyIn))
    {
        addAndMakeVisible (slider);
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setRange (-GainScale::rangeDb, GainScale::rangeDb, 0.1);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setDoubleClickReturnValue (true, 0.0);
        slider.textFromValueFunction = [] (double v) { return GainScale::text (v); };
        slider.setPopupDisplayEnabled (true, false, nullptr);
        slider.getProperties().set ("centerFill", true);
        slider.setValue (GainScale::toDb (initialGain), juce::dontSendNotification);
        slider.setWantsKeyboardFocus (false);
        slider.setMouseClickGrabsKeyboardFocus (false);
        // 1操作の区切りは「新しいクリック列の始まり」。ダブルクリックの2回目では区切らないので、
        // つまみ以外をダブルクリックしても（1クリック目のジャンプ＋0dBリセットで）undoは1件のまま
        slider.onNewClickSequence = [this] { editBegun = false; };
        slider.onValueChange = [this]
        {
            if (! editBegun) // 値が動いた最初の1回だけundoを積む（連続ドラッグは1件にまとまる）
            {
                editBegun = true;
                if (willEdit)
                    willEdit();
            }
            valueLabel.setDb (slider.getValue());
            if (apply)
                apply (GainScale::toLinear (slider.getValue())); // ドラッグ中も音と波形へ反映する
        };

        addAndMakeVisible (valueLabel);
        valueLabel.setDb (GainScale::toDb (initialGain));
        valueLabel.onReset = [this]
        {
            if (slider.getValue() == 0.0)
                return;
            // undoは onValueChange 側で1件だけ積まれる（ここで willEdit を呼ぶと二重になる）
            editBegun = false;
            slider.setValue (0.0, juce::sendNotificationSync);
            editBegun = false;
        };

        setSize (196, 52);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Theme::lcdLabel);
        g.setFont (Fonts::small());
        g.drawText ("GAIN", getLocalBounds().removeFromTop (16).reduced (2, 0),
                    juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto header = area.removeFromTop (16);
        valueLabel.setBounds (header.removeFromRight (72));
        slider.setBounds (area.reduced (2, 4));
    }

private:
    GainSlider slider;
    GainValueLabel valueLabel;
    std::function<void()> willEdit;
    std::function<void (float)> apply;
    bool editBegun = false; // この一連の操作でundoを積んだか（ドラッグ開始/終了でリセット）
};

// 移調・伸縮の吹き出しの中身（CallOutBoxに載せる）。見た目・操作はC案モック
// （scratchpad の stretch-callout-mock.html で確定・2026-08-18）:
// - PITCH: 半音ノッチ＋0デテント付きのカスタムスライダー（ダブルクリックで0）。値は右上に併記
// - LENGTH: LCD小窓（上下ドラッグ / ダブルクリックで入力）＋前後の整数小節へスナップする −/+
//   ステッパー。小節数で指定（元BPMのメタデータを要求せず、グリッドに噛むことが直接保証される）。
//   現在の見かけ長を**丸めずに**表示し、実際に編集されるまで stretchRatio に触れない
// - 倍率は「×N.NNN」の併記＋±10%ゾーン付き偏差ゲージ。圏外はソロ色（黄）で知らせる
//   （音楽的なハード制限はしない。安全限界は別）
// - undo の粒度は「吹き出しを開いてから閉じるまで1件」: willEdit は最初の変更で1回だけ呼ぶ

// 半音ノッチ・0デテント・センター起点フィルのピッチスライダー（±12・整数）
class PitchNotchSlider : public juce::Component
{
public:
    std::function<void (int)> onChange;

    void setValue (int newValue)
    {
        value = juce::jlimit (-ClipStretchLimits::maxSemitones, ClipStretchLimits::maxSemitones,
                              newValue);
        repaint();
    }
    int getValue() const { return value; }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (thumbRadius, 0.0f);
        const float railY = (float) getHeight() * 0.5f;

        // レール
        g.setColour (Theme::lcdBg);
        g.fillRoundedRectangle (area.getX(), railY - 2.0f, area.getWidth(), 4.0f, 2.0f);

        // 目盛り（半音ごと。オクターブと0は強調）
        for (int s = -ClipStretchLimits::maxSemitones; s <= ClipStretchLimits::maxSemitones; ++s)
        {
            const float x = xForValue (s);
            const bool major = s % 12 == 0;
            g.setColour (juce::Colours::white.withAlpha (major ? 0.30f : 0.14f));
            g.fillRect (x - 0.5f, railY + 5.0f, 1.0f, major ? 5.0f : 3.0f);
        }
        // 0デテント（レールを縦に貫く線）
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.fillRect (xForValue (0) - 0.5f, railY - 7.0f, 1.0f, 14.0f);

        // センター起点のフィル
        const float zeroX = xForValue (0);
        const float thumbX = xForValue (value);
        g.setColour (Theme::accent);
        g.fillRect (juce::jmin (zeroX, thumbX), railY - 2.0f, std::abs (thumbX - zeroX), 4.0f);

        // つまみ
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillEllipse (thumbX - thumbRadius, railY - thumbRadius + 1.0f,
                       thumbRadius * 2.0f, thumbRadius * 2.0f);
        g.setColour (juce::Colour (0xffd6d6da));
        g.fillEllipse (thumbX - thumbRadius, railY - thumbRadius,
                       thumbRadius * 2.0f, thumbRadius * 2.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override { applyFromX (e.position.x); }
    void mouseDrag (const juce::MouseEvent& e) override { applyFromX (e.position.x); }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (value != 0 && onChange)
        {
            value = 0;
            onChange (0);
        }
        repaint();
    }

private:
    static constexpr float thumbRadius = 8.0f;

    float xForValue (int v) const
    {
        const float usable = (float) getWidth() - thumbRadius * 2.0f;
        const float p = (float) (v + ClipStretchLimits::maxSemitones)
                        / (float) (ClipStretchLimits::maxSemitones * 2);
        return thumbRadius + p * usable;
    }

    void applyFromX (float x)
    {
        const float usable = juce::jmax (1.0f, (float) getWidth() - thumbRadius * 2.0f);
        const float p = juce::jlimit (0.0f, 1.0f, (x - thumbRadius) / usable);
        const int next = (int) std::llround (p * ClipStretchLimits::maxSemitones * 2)
                         - ClipStretchLimits::maxSemitones;
        if (next != value)
        {
            value = next;
            if (onChange)
                onChange (value);
        }
        repaint();
    }

    int value = 0;
};

// −/+ の小さな角丸ステッパー
class StepSquareButton : public juce::Component
{
public:
    explicit StepSquareButton (const juce::String& glyphIn) : glyph (glyphIn) {}
    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        g.setColour (hovered ? juce::Colour (0xff1c1c21) : Theme::lcdBg);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
        g.setColour (hovered ? juce::Colours::white : juce::Colours::white.withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawText (glyph, getLocalBounds(), juce::Justification::centred);
    }
    void mouseEnter (const juce::MouseEvent&) override { hovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovered = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()) && onClick)
            onClick();
    }

private:
    juce::String glyph;
    bool hovered = false;
};

// 小節数のLCD小窓。上下ドラッグで連続変更・ダブルクリックでインライン入力
class BarsLcdBox : public juce::Component
{
public:
    std::function<double()> getBars;          // 現在の見かけ小節数（丸めない）
    std::function<void (double)> setBars;     // 入力・ドラッグの適用先（受付はモデル側）

    BarsLcdBox()
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        addChildComponent (editor);
        editor.setInputRestrictions (8, "0123456789.");
        editor.setJustification (juce::Justification::centred);
        editor.setFont (Fonts::mono (18.0f));
        editor.setSelectAllWhenFocused (true);
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        editor.onReturnKey = [this] { commitPendingEdit(); };
        editor.onFocusLost = [this] { commitPendingEdit(); };
        editor.onEscapeKey = [this] { hideEditor(); };
    }

    static juce::String barsText (double bars)
    {
        auto text = juce::String (bars, 2);
        while (text.endsWith ("0"))
            text = text.dropLastCharacters (1);
        if (text.endsWith ("."))
            text = text.dropLastCharacters (1);
        return text;
    }

    // 破棄・確定時に未確定の入力を適用する（Return を押さず閉じても捨てられない —
    // 「閉じる＝キャンセルではない」の流儀）
    void commitPendingEdit()
    {
        if (! editor.isVisible())
            return;
        const double bars = editor.getText().getDoubleValue();
        hideEditor();
        if (bars > 0.0 && setBars)
            setBars (bars);
    }

    void paint (juce::Graphics& g) override
    {
        const bool editing = editor.isVisible();
        g.setColour (editing ? Theme::lcdEditBg
                             : hovered ? juce::Colour (0xff1c1c21) : Theme::lcdBg);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 7.0f);
        if (editing)
            return;

        const auto valueText = getBars != nullptr ? barsText (getBars()) : juce::String ("--");
        const auto valueFont = Fonts::mono (18.0f);
        const auto unitFont = Fonts::small();
        const int valueW = juce::GlyphArrangement::getStringWidthInt (valueFont, valueText);
        const int unitW = juce::GlyphArrangement::getStringWidthInt (unitFont, "bars");
        const int x0 = (getWidth() - (valueW + 5 + unitW)) / 2;
        g.setColour (juce::Colours::white);
        g.setFont (valueFont);
        g.drawText (valueText, x0, 0, valueW, getHeight(), juce::Justification::centredLeft);
        g.setColour (Theme::lcdLabel);
        g.setFont (unitFont);
        g.drawText ("bars", x0 + valueW + 5, 0, unitW + 4, getHeight() - 1,
                    juce::Justification::centredLeft);
    }

    void resized() override { editor.setBounds (getLocalBounds().reduced (6, 4)); }

    void mouseEnter (const juce::MouseEvent&) override { hovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovered = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override
    {
        dragStartBars = getBars != nullptr ? getBars() : 0.0;
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (editor.isVisible() || setBars == nullptr)
            return;
        // 上ドラッグで伸ばす（値の上下ドラッグ = LCDと同じ文法）。0.02小節/px
        const double bars = dragStartBars + (double) -e.getDistanceFromDragStartY() * 0.02;
        if (bars > 0.0)
            setBars (bars);
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        editor.setText (getBars != nullptr ? barsText (getBars()) : juce::String(), false);
        editor.setVisible (true);
        editor.grabKeyboardFocus();
        repaint();
    }

private:
    void hideEditor()
    {
        editor.setVisible (false);
        repaint();
    }

    juce::TextEditor editor;
    bool hovered = false;
    double dragStartBars = 0.0;
};

// フッターの控えめなテキストリンク（Reset）
class TextLinkLabel : public juce::Component
{
public:
    explicit TextLinkLabel (const juce::String& textIn) : text (textIn)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        g.setColour (hovered ? juce::Colours::white : juce::Colours::white.withAlpha (0.55f));
        g.setFont (Fonts::small());
        g.drawText (text, getLocalBounds(), juce::Justification::centredRight);
    }
    void mouseEnter (const juce::MouseEvent&) override { hovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovered = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()) && onClick)
            onClick();
    }

private:
    juce::String text;
    bool hovered = false;
};

class RegionTransposeStretchPanel : public juce::Component
{
public:
    RegionTransposeStretchPanel (const Clip& clip, double barLengthSamplesIn,
                                 std::function<void()> willEditIn,
                                 std::function<void (int, double)> applyIn)
        : sourceLengthSamples (juce::jmax ((juce::int64) 1, clip.lengthSamples)),
          barLengthSamples (juce::jmax (1.0, barLengthSamplesIn)),
          looped (clip.loopCount > 0),
          semitones (clip.transposeSemitones),
          ratio (clip.stretchRatio),
          willEdit (std::move (willEditIn)),
          apply (std::move (applyIn))
    {
        addAndMakeVisible (pitchSlider);
        pitchSlider.setValue (semitones);
        pitchSlider.onChange = [this] (int value)
        {
            if (value == semitones)
                return;
            semitones = value;
            pushValues();
        };

        addAndMakeVisible (barsBox);
        barsBox.getBars = [this] { return currentBars(); };
        barsBox.setBars = [this] (double bars) { setBarsValue (bars); };

        // 前後の整数小節へスナップ（グリッド合わせの最短経路）
        addAndMakeVisible (minusButton);
        minusButton.onClick = [this]
        {
            const auto target = std::ceil (currentBars() - 1e-6) - 1.0;
            if (target >= 1.0)
                setBarsValue (target);
        };
        addAndMakeVisible (plusButton);
        plusButton.onClick = [this] { setBarsValue (std::floor (currentBars() + 1e-6) + 1.0); };

        addAndMakeVisible (resetLink);
        resetLink.onClick = [this]
        {
            if (semitones == 0 && juce::exactlyEqual (ratio, 1.0))
                return;
            semitones = 0;
            ratio = 1.0;
            pitchSlider.setValue (0);
            pushValues();
        };

        setSize (264, 158);
    }

    // 「閉じる＝キャンセル」ではない: 外側クリックで CallOutBox ごと破棄されても、
    // 入力途中の小節数を適用してから死ぬ（focusLost が走らない経路の取りこぼし対策）
    ~RegionTransposeStretchPanel() override { barsBox.commitPendingEdit(); }

    void paint (juce::Graphics& g) override
    {
        const auto labelFont = Fonts::small().withExtraKerningFactor (0.08f);

        // PITCH 行（ラベル左・現在値右）
        g.setColour (Theme::lcdLabel);
        g.setFont (labelFont);
        g.drawText ("PITCH", 4, 0, 100, 16, juce::Justification::centredLeft);
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (Fonts::mono (13.0f));
        g.drawText ((semitones > 0 ? "+" : "") + juce::String (semitones) + " st",
                    getWidth() - 104, 0, 100, 16, juce::Justification::centredRight);

        // LENGTH 行（ラベル左・倍率右。±10%＝0.9〜1.1倍の外はソロ色で知らせる —
        // reference-beat の「近距離救済」の圏内表示。音楽的なハード制限はしない）
        const int lengthRowY = 52;
        g.setColour (Theme::lcdLabel);
        g.setFont (labelFont);
        g.drawText ("LENGTH", 4, lengthRowY, 100, 16, juce::Justification::centredLeft);
        const bool nearUnity = ratio >= 0.9 && ratio <= 1.1;
        g.setColour (nearUnity ? juce::Colours::white.withAlpha (0.55f) : Theme::soloOn);
        g.setFont (Fonts::mono (12.0f));
        g.drawText (juce::String::fromUTF8 (u8"×") + juce::String (ratio, 3),
                    getWidth() - 104, lengthRowY, 100, 16, juce::Justification::centredRight);

        // 偏差ゲージ（フルスケール±30%・中央±10%が推奨ゾーン）
        const auto gauge = gaugeBounds();
        g.setColour (Theme::lcdBg);
        g.fillRoundedRectangle (gauge, 2.0f);
        const float zoneHalf = gauge.getWidth() * (0.1f / 0.3f) * 0.5f;
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.fillRoundedRectangle (gauge.getCentreX() - zoneHalf, gauge.getY(),
                                zoneHalf * 2.0f, gauge.getHeight(), 2.0f);
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.fillRect (gauge.getCentreX() - 0.5f, gauge.getY() - 2.0f, 1.0f, gauge.getHeight() + 4.0f);
        const float deviation = juce::jlimit (-0.3f, 0.3f, (float) (ratio - 1.0));
        const float pinX = gauge.getCentreX() + deviation / 0.3f * gauge.getWidth() * 0.5f;
        g.setColour (nearUnity ? Theme::playGreen : Theme::soloOn);
        g.fillRoundedRectangle (pinX - 1.0f, gauge.getY() - 3.0f, 2.0f, gauge.getHeight() + 6.0f, 1.0f);

        // フッター注記（ループ中のみ。「小節数は本体1周分」の英語版）
        if (looped)
        {
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.setFont (Fonts::small());
            g.drawText ("bars = one cycle (repeats excluded)", 4, getHeight() - 16,
                        getWidth() - 60, 16, juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        pitchSlider.setBounds (2, 16, getWidth() - 4, 28);
        const int lengthControlsY = 70;
        minusButton.setBounds (2, lengthControlsY + 6, 22, 22);
        plusButton.setBounds (getWidth() - 24, lengthControlsY + 6, 22, 22);
        barsBox.setBounds (32, lengthControlsY, getWidth() - 64, 34);
        resetLink.setBounds (getWidth() - 64, getHeight() - 18, 60, 16);
    }

private:
    juce::Rectangle<float> gaugeBounds() const
    {
        return { 4.0f, 116.0f, (float) getWidth() - 8.0f, 4.0f };
    }

    double currentBars() const
    {
        // 現在の（要求）見かけ長を丸めずに扱う
        return (double) sourceLengthSamples * ratio / barLengthSamples;
    }

    void setBarsValue (double bars)
    {
        // 小節数はUIの入力手段であって保存値ではない（保存は ratio。BPM追従しないと決めた
        // 以上、小節数を保存すると BPM 変更時に意味が変わる）。換算・クランプはモデル側
        const auto next = ClipDomains::ratioForBars (bars, barLengthSamples, sourceLengthSamples);
        // view 長が 0 になる要求は受理しない（読込時の検証と同じ規則）
        if ((juce::int64) std::llround ((double) sourceLengthSamples * next) < 1)
        {
            repaint();
            return;
        }
        if (! juce::exactlyEqual (next, ratio))
        {
            ratio = next;
            pushValues();
        }
        barsBox.repaint();
        repaint();
    }

    void pushValues()
    {
        if (! editBegun)
        {
            editBegun = true; // 吹き出し1回の編集で undo 1件
            if (willEdit)
                willEdit();
        }
        if (apply)
            apply (semitones, ratio);
        barsBox.repaint();
        repaint();
    }

    juce::int64 sourceLengthSamples;
    double barLengthSamples;
    bool looped = false;
    int semitones = 0;
    double ratio = 1.0;
    PitchNotchSlider pitchSlider;
    BarsLcdBox barsBox;
    StepSquareButton minusButton { juce::String::fromUTF8 (u8"−") };
    StepSquareButton plusButton { "+" };
    TextLinkLabel resetLink { "Reset" };
    std::function<void()> willEdit;
    std::function<void (int, double)> apply;
    bool editBegun = false; // この吹き出しで undo を積んだか（開いてから閉じるまで1件）
};

// セクション種別ごとの固定色（ユーザー選択なし）。5種は有彩色、otherは「特定の種別ではない」ことが
// 一目でわかる無彩色。ダーク背景上で黒文字が読める明度に揃える
juce::Colour sectionColour (SectionType type)
{
    switch (type)
    {
        case SectionType::intro:  return Theme::sectionIntro;
        case SectionType::verse:  return Theme::sectionVerse;
        case SectionType::hook:   return Theme::sectionHook;
        case SectionType::bridge: return Theme::sectionBridge;
        case SectionType::outro:  return Theme::sectionOutro;
        case SectionType::other:  return Theme::sectionOther;
    }
    return Theme::sectionOther;
}
} // namespace

// ---- 内部コンポーネント -------------------------------------------------

class TimelineView::LaneViewport : public juce::Viewport
{
public:
    explicit LaneViewport (TimelineView& o) : owner (o) {}

    void visibleAreaChanged (const juce::Rectangle<int>&) override
    {
        owner.syncScroll();
    }

private:
    TimelineView& owner;
};

// 曲末フェードのカーブ帯。設定されているときだけ現れる（未設定なら1pxも占めない）。
// ルーラー・マーカーレーンと同じく viewport の外に置き、横スクロールに手動で追従させる。
//
// レーンの暗幕でカーブを描くと、高さがトラック数に比例するため下端（＝無音の位置）が
// 画面外へ出て「まだ落ちていない」と誤読する。固定高さの帯へ集約すると、トラック数にも
// 縦スクロールにも影響されずカーブ全体が読める
class TimelineView::SongFadeBandContent : public juce::Component
{
public:
    explicit SongFadeBandContent (TimelineView& o) : owner (o) { setInterceptsMouseClicks (false, false); }

    void paint (juce::Graphics& g) override
    {
        auto* proj = owner.project;
        if (proj == nullptr || ! proj->hasFadeOut())
            return;

        const int h = getHeight();
        g.fillAll (Theme::songFadeBandBg);
        g.setColour (Theme::gridLineSub);
        g.fillRect (0, h - 1, getWidth(), 1);

        const int x0 = owner.sixteenthToX (proj->fadeOutStartSixteenths);
        const int x1 = owner.sixteenthToX (proj->fadeOutEndSixteenths);
        const auto clip = g.getClipBounds();

        // カーブ。左端（フェード前＝ユニティ）から終端まで引き、終端以後は底に張り付く。
        // 描画にも SongFade::gainAt を使い、聴こえるカーブと一致させる
        const float top = 3.0f, bottom = (float) h - 4.0f;
        const auto yAt = [&] (int x)
        {
            return top + (1.0f - SongFade::gainAt (x, x0, x1)) * (bottom - top);
        };

        juce::Path curve;
        curve.startNewSubPath ((float) clip.getX(), yAt (clip.getX()));
        for (int x = juce::jmax (clip.getX(), x0); x < juce::jmin (clip.getRight(), x1); x += 2)
            curve.lineTo ((float) x, yAt (x));
        curve.lineTo ((float) clip.getRight(), yAt (clip.getRight()));

        auto fill = curve;
        fill.lineTo ((float) clip.getRight(), top);
        fill.lineTo ((float) clip.getX(), top);
        fill.closeSubPath();
        g.setColour (Theme::songFadeHandle.withAlpha (0.13f));
        g.fillPath (fill);

        g.setColour (Theme::songFadeHandle.withAlpha (0.95f));
        g.strokePath (curve, juce::PathStrokeType (1.5f));
    }

private:
    TimelineView& owner;
};

class TimelineView::RulerContent : public juce::Component
{
public:
    explicit RulerContent (TimelineView& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::rulerBg);
        const auto clip = g.getClipBounds();
        const double barWidth = owner.pxPerBar;

        const int firstBar = juce::jmax (0, (int) std::floor (clip.getX() / barWidth) - 1);
        const int lastBar = (int) std::floor (clip.getRight() / barWidth) + 1;
        const int div = owner.gridDivisionsPerBar();

        // ラベルが重ならないよう、ズームアウト時は2の冪の間隔で間引く
        int labelStep = 1;
        while (labelStep * barWidth < 48.0)
            labelStep *= 2;

        for (int bar = firstBar; bar <= lastBar; ++bar)
        {
            for (int i = 0; i < div; ++i)
            {
                const int x = (int) std::llround ((bar + i / (double) div) * barWidth);
                if (i == 0)
                {
                    g.setColour (Theme::rulerTickBar);
                    g.drawVerticalLine (x, 8.0f, (float) getHeight());
                }
                else if ((i * 4) % div == 0)   // 拍（1/2表示時はその線も同格に）
                {
                    g.setColour (Theme::rulerTickBeat);
                    g.drawVerticalLine (x, 14.0f, (float) getHeight());
                }
                else
                {
                    g.setColour (Theme::rulerTickSub);
                    g.drawVerticalLine (x, 19.0f, (float) getHeight());
                }
            }

            if (bar % labelStep == 0)
            {
                g.setColour (Theme::rulerLabel);
                g.setFont (Fonts::mono (11.0f));
                g.drawText (juce::String (bar + 1),
                            (int) std::llround (bar * barWidth) + 4, 0,
                            (int) (labelStep * barWidth) - 6, getHeight(),
                            juce::Justification::centredLeft);
            }
        }

        // サイクル帯（Logicのルック準拠: ON=黄・OFF=グレーで範囲を保持表示。
        // 小節番号の上・プレイヘッドの下に重ねる = プレイヘッド > サイクル帯 > 小節番号 の強弱）
        if (auto* proj = owner.project; proj != nullptr && proj->hasCycleRange())
        {
            const int x0 = owner.sixteenthToX (proj->cycleStartSixteenths);
            const int x1 = owner.sixteenthToX (proj->cycleEndSixteenths);
            if (x1 > clip.getX() && x0 < clip.getRight())
            {
                const bool on = proj->cycleEnabled;
                const auto colour = on ? Theme::cycleOn : Theme::cycleOff;
                g.setColour (colour.withAlpha (on ? 0.20f : 0.12f));
                g.fillRect (x0, 0, x1 - x0, getHeight());
                g.setColour (colour.withAlpha (on ? 0.95f : 0.55f));
                g.fillRect (x0, 0, x1 - x0, 4);       // 上端の帯（Logicのサイクルストリップ）
                g.fillRect (x0, 0, 2, getHeight());   // 両端の縦線（リサイズの掴み所の手掛かり）
                g.fillRect (x1 - 2, 0, 2, getHeight());
            }
        }

        // 曲末フェードの両端ハンドル。**サイクル帯と同じ文法**（範囲の薄塗り＋帯＋全高の
        // 両端縦線）にして、帯の位置（サイクル=上端／フェード=下端）と色（黄・グレー／寒色）
        // だけで区別する。掴み方が同じなので、サイクルで覚えた操作がそのまま通じる。
        // 縦線を全高にするのは必須: 下端だけの小さなハンドルにしていたとき「どこを掴めるのか
        // 分からない」で操作にたどり着けなかった（当たり判定はルーラー全高なのに見た目が
        // 下端8pxしかなく、掴める範囲が伝わっていなかった）
        if (auto* proj = owner.project; proj != nullptr && proj->hasFadeOut())
        {
            const int x0 = owner.sixteenthToX (proj->fadeOutStartSixteenths);
            const int x1 = owner.sixteenthToX (proj->fadeOutEndSixteenths);
            if (x1 > clip.getX() && x0 < clip.getRight())
            {
                const int h = getHeight();
                g.setColour (Theme::songFadeHandle.withAlpha (0.14f));
                g.fillRect (x0, 0, x1 - x0, h);
                g.setColour (Theme::songFadeHandle.withAlpha (0.95f));
                g.fillRect (x0, h - 4, x1 - x0, 4);  // 下端の帯（サイクルは上端）
                // 縦線は3px。⌃Fで作った直後は開始端が再生ヘッド（1pxの白線）と必ず重なるため、
                // 2pxだと隠れて見えなくなる（左右に1pxずつ残す）
                g.fillRect (x0, 0, 3, h);
                g.fillRect (x1 - 3, 0, 3, h);
            }
        }

        // 再生開始位置（＝次に鳴る場所）。ヘッドと同じ形・同じ大きさを「塗らずに輪郭だけ」で描く。
        // 塗り＝今いる場所 / 中抜き＝次に鳴る場所、という対比にすると2つの関係が読み取りやすく、
        // 重なっているとき（＝ほとんどの時間）はヘッドの塗りに隠れて1本に見える。
        // ヘッドより先に描くことで、重なったときはヘッドが上に来る
        {
            const float sx = (float) owner.sampleToX (owner.playStartSample) + 0.5f;
            juce::Path marker;
            marker.addTriangle (sx - 5.0f, 0.0f, sx + 5.0f, 0.0f, sx, 7.0f);
            g.setColour (Theme::playStartMarker);
            g.strokePath (marker.createPathWithRoundedCorners (1.5f), juce::PathStrokeType (1.0f));
        }

        const int playheadX = owner.sampleToX (owner.transport.uiPositionSample());
        g.setColour (Theme::playhead);
        g.drawVerticalLine (playheadX, 0.0f, (float) getHeight());

        // 三角の頭（下向き）。線1本だけでは広い画面で見失うため、
        // ルーラー上端に頭を付けて現在位置へ視線誘導する（Logic/Cubase等の定番）
        juce::Path head;
        const float cx = (float) playheadX + 0.5f;
        head.addTriangle (cx - 5.0f, 0.0f, cx + 5.0f, 0.0f, cx, 7.0f);
        g.fillPath (head.createPathWithRoundedCorners (1.5f));
    }

    void mouseDown (const juce::MouseEvent& e) override { owner.handleRulerMouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override { owner.handleRulerMouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override { owner.handleRulerMouseUp (e); }
    void mouseMove (const juce::MouseEvent& e) override { owner.handleRulerMouseMove (e); }

    void mouseMagnify (const juce::MouseEvent& e, float scaleFactor) override
    {
        owner.zoomAroundContentX ((double) scaleFactor, e.x);
    }

private:
    TimelineView& owner;
};

class TimelineView::MarkerLaneContent : public juce::Component
{
public:
    explicit MarkerLaneContent (TimelineView& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::markerLaneBg);
        const auto clip = g.getClipBounds();

        // ルーラーの小節目盛りをレーンまで通す（背景の同色化と合わせ、ルーラー＋レーンを
        // 一枚のナビゲーション帯に見せる。入力の扱いは従来どおり上下で別）
        {
            const double barWidth = owner.pxPerBar;
            const int firstBar = juce::jmax (0, (int) std::floor (clip.getX() / barWidth) - 1);
            const int lastBar = (int) std::floor (clip.getRight() / barWidth) + 1;
            g.setColour (Theme::rulerTickBar);
            for (int bar = firstBar; bar <= lastBar; ++bar)
                g.drawVerticalLine ((int) std::llround (bar * barWidth), 0.0f, (float) getHeight());
        }

        if (auto* proj = owner.project)
        {
            const auto& markers = proj->markers;
            for (int i = 0; i < (int) markers.size(); ++i)
            {
                const int x0 = owner.beatToX (markers[(size_t) i].startBeats);
                const int x1 = i + 1 < (int) markers.size()
                                   ? owner.beatToX (markers[(size_t) (i + 1)].startBeats)
                                   : getWidth(); // 最後のセクションはコンテンツ右端（曲末）まで
                if (x1 <= clip.getX() || x0 >= clip.getRight())
                    continue;

                // ベタ塗りにせず「薄い色帯＋開始位置の色線＋色文字」で描く。マーカーは常時見る
                // 情報ではないので、プレイヘッドやリージョンより視覚的に沈める（Logic/Ableton式）。
                // 開始位置の縦線を残すことで、テキストが出ない狭いセクションでも境界が分かる
                const auto colour = sectionColour (markers[(size_t) i].type);
                g.setColour (colour.withAlpha (0.12f));
                g.fillRect (x0, 1, x1 - x0, getHeight() - 2);
                g.setColour (colour);
                g.fillRect (x0, 1, 3, getHeight() - 2);

                const int textW = x1 - x0 - 10;
                if (textW > 16)
                {
                    g.setColour (colour.brighter (0.4f));
                    g.setFont (Fonts::mono (11.0f));
                    g.drawText (SectionMarkers::displayName (markers, i),
                                x0 + 7, 0, textW, getHeight(), juce::Justification::centredLeft);
                }
            }
        }

        // 再生ヘッド（ルーラー・レーンの縦線と繋がって見えるように同じ白）
        const int playheadX = owner.sampleToX (owner.transport.uiPositionSample());
        g.setColour (Theme::playhead);
        g.drawVerticalLine (playheadX, 0.0f, (float) getHeight());
    }

    void mouseDown (const juce::MouseEvent& e) override { owner.handleMarkerLaneMouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override { owner.handleMarkerLaneMouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override { owner.handleMarkerLaneMouseUp (e); }
    void mouseMove (const juce::MouseEvent& e) override { owner.handleMarkerLaneMouseMove (e); }

    void mouseMagnify (const juce::MouseEvent& e, float scaleFactor) override
    {
        owner.zoomAroundContentX ((double) scaleFactor, e.x);
    }

private:
    TimelineView& owner;
};

class TimelineView::LaneContent : public juce::Component,
                                  public juce::FileDragAndDropTarget,
                                  public juce::DragAndDropTarget
{
public:
    explicit LaneContent (TimelineView& o) : owner (o) {}

    // ---- オーディオファイルのD&D取り込み（コンテンツ空間の座標で受ける）----
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return isInterested (files);
    }

    void fileDragEnter (const juce::StringArray& files, int x, int y) override
    { owner.updateFileDrop (x, y, hasMidi (files)); }
    void fileDragMove (const juce::StringArray& files, int x, int y) override
    { owner.updateFileDrop (x, y, hasMidi (files)); }
    void fileDragExit (const juce::StringArray&) override { owner.clearFileDrop(); }

    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        completeDrop (files, x, y);
    }

    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        juce::StringArray files;
        files.add (details.description.toString());
        return isInterested (files);
    }

    void itemDragEnter (const SourceDetails& details) override
    {
        owner.updateFileDrop (details.localPosition.x, details.localPosition.y,
                              MidiFileTypes::isSupported (details.description.toString()));
    }

    void itemDragMove (const SourceDetails& details) override
    {
        owner.updateFileDrop (details.localPosition.x, details.localPosition.y,
                              MidiFileTypes::isSupported (details.description.toString()));
    }

    void itemDragExit (const SourceDetails&) override { owner.clearFileDrop(); }

    void itemDropped (const SourceDetails& details) override
    {
        juce::StringArray files;
        files.add (details.description.toString());
        completeDrop (files, details.localPosition.x, details.localPosition.y);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::timelineBg);
        const auto clip = g.getClipBounds();
        auto* proj = owner.project;
        if (proj == nullptr)
            return;

        const int numTracks = (int) proj->tracks.size();

        // 行背景・区切り線
        for (int t = 0; t < numTracks; ++t)
        {
            const int y = t * trackHeight;
            if (y > clip.getBottom() || y + trackHeight < clip.getY())
                continue;
            if (t == owner.selectedTrack)
            {
                g.setColour (Theme::laneSelectedRowBg);
                g.fillRect (clip.getX(), y, clip.getWidth(), trackHeight);
            }
            g.setColour (Theme::panelBorder);
            g.drawHorizontalLine (y + trackHeight - 1, (float) clip.getX(), (float) clip.getRight());
        }

        // 小節・拍グリッド（ズームに応じて最大1/16音符まで細分化）
        const double barWidth = owner.pxPerBar;
        const int firstBar = juce::jmax (0, (int) std::floor (clip.getX() / barWidth));
        const int lastBar = (int) std::floor (clip.getRight() / barWidth) + 1;
        const int div = owner.gridDivisionsPerBar();
        for (int bar = firstBar; bar <= lastBar; ++bar)
        {
            for (int i = 0; i < div; ++i)
            {
                const int x = (int) std::llround ((bar + i / (double) div) * barWidth);
                g.setColour (i == 0             ? Theme::gridLineBar
                             : (i * 4) % div == 0 ? Theme::gridLineBeat
                                                  : Theme::gridLineSub);
                g.drawVerticalLine (x, (float) clip.getY(), (float) clip.getBottom());
            }
        }

        // クリップ（オーディオ）とMIDIリージョン
        bool anySolo = false;
        for (auto& t : proj->tracks)
            anySolo = anySolo || t.params->solo.load();

        for (int t = 0; t < numTracks; ++t)
        {
            const int y = t * trackHeight;
            if (y > clip.getBottom() || y + trackHeight < clip.getY())
                continue;

            auto& track = proj->tracks[(size_t) t];
            // 聞こえないトラック（ミュート中・他トラックのソロで実質ミュート）は
            // リージョン単位のミュートと同じグレー減光で描く（可聴判定は再生エンジンと同じ式）
            const auto& params = *track.params;
            const bool trackDimmed = params.mute.load() || (anySolo && ! params.solo.load());
            if (track.type == TrackType::audio)
            {
                for (int ci = 0; ci < (int) track.clips.size(); ++ci)
                {
                    const bool isSelected = (owner.selection.track == t && owner.selection.clip == ci);
                    drawClip (g, track.clips[(size_t) ci], y, isSelected, clip, trackDimmed);
                }
            }
            else
            {
                for (int ri = 0; ri < (int) track.midiRegions.size(); ++ri)
                {
                    const bool isSelected = (owner.regionSelection.track == t
                                             && owner.regionSelection.region == ri);
                    drawMidiRegion (g, track.midiRegions[(size_t) ri], y, isSelected, clip, trackDimmed);
                }
            }
        }

        // 録音中の仮クリップ（赤）
        if (owner.transport.recordArmed.load()
            && owner.selectedTrack >= 0 && owner.selectedTrack < numTracks)
        {
            const auto recordedLen = owner.transport.recordedSamples.load();
            if (recordedLen > 0)
            {
                const int x = owner.sampleToX (owner.transport.punchInSample.load());
                const int w = juce::jmax (2, (int) ((double) recordedLen / owner.samplesPerPixel()));
                const int y = owner.selectedTrack * trackHeight;
                g.setColour (juce::Colours::red.withAlpha (0.45f));
                g.fillRoundedRectangle ((float) x, (float) y + 4.0f, (float) w, (float) trackHeight - 8.0f, 4.0f);
            }
        }

        // フェードドラッグ中の数値ポップアップ（音量ドラッグと同じ流儀。常設の数値表示は置かない）
        drawFadeDragPopup (g);

        // 曲末フェードの暗幕（リージョンより上・再生ヘッドより下。落ちた音量ぶんだけ上から暗くする）
        owner.drawSongFadeShade (g, clip, numTracks * trackHeight);

        // 再生ヘッド
        const int playheadX = owner.sampleToX (owner.transport.uiPositionSample());
        if (playheadX >= clip.getX() - 1 && playheadX <= clip.getRight() + 1)
        {
            g.setColour (Theme::playhead.withAlpha (0.8f));
            g.drawVerticalLine (playheadX, (float) clip.getY(), (float) clip.getBottom());
        }

        // オーディオファイルD&Dのドロップインジケータ（対象レーンのハイライト＋挿入位置ライン）
        if (owner.fileDrop.active)
        {
            const int laneY = (owner.fileDrop.track >= 0 ? owner.fileDrop.track : numTracks) * trackHeight;
            const auto laneRect = juce::Rectangle<int> (clip.getX(), laneY, clip.getWidth(), trackHeight);

            if (owner.fileDrop.rejected)
            {
                // 不受理（減光で「置けない」を示す。挿入ラインは出さない）
                g.setColour (juce::Colours::black.withAlpha (0.3f));
                g.fillRect (laneRect);
            }
            else if (owner.fileDrop.instrument)
            {
                // MIDIトラック: サンプル音源の割り当て。行全体を枠で囲み、クリップ配置（挿入位置の
                // 縦線）と意味が違うことを見た目で区別する（位置は関係ない操作なので線を出さない）
                g.setColour (Theme::accent.withAlpha (0.16f));
                g.fillRect (laneRect);
                g.setColour (Theme::accent);
                g.drawRect (laneRect.reduced (1), 2);
                g.setColour (Theme::accent.brighter (0.4f));
                g.setFont (Fonts::small());
                g.drawText ("Instrument", laneRect.reduced (8, 6), juce::Justification::topLeft);
            }
            else
            {
                g.setColour (Theme::accent.withAlpha (0.08f));
                g.fillRect (laneRect);
                const int x = owner.sampleToX (owner.fileDrop.startSample);
                g.setColour (Theme::accent);
                g.fillRect (x - 1, laneY, 2, trackHeight);
                if (owner.fileDrop.track < 0) // 空白ゾーン = 新規トラック
                {
                    g.setColour (Theme::accent.withAlpha (0.8f));
                    g.setFont (Fonts::small());
                    g.drawText ("New Track", x + 8, laneY + 6, 80, 14, juce::Justification::centredLeft);
                }
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        owner.handleLaneMouseDown (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        owner.handleLaneMouseDrag (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        owner.handleLaneMouseUp (e);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        owner.handleLaneDoubleClick (e);
    }

    void mouseMagnify (const juce::MouseEvent& e, float scaleFactor) override
    {
        owner.zoomAroundContentX ((double) scaleFactor, e.x);
    }

private:
    static bool hasMidi (const juce::StringArray& files)
    {
        for (const auto& file : files)
            if (MidiFileTypes::isSupported (file))
                return true;
        return false;
    }

    bool isInterested (const juce::StringArray& files) const
    {
        if (owner.project == nullptr)
            return false;
        if (owner.onImportMidiDropped != nullptr && hasMidi (files))
            return true;
        if (owner.onImportFilesDropped == nullptr && owner.onAssignInstrumentDropped == nullptr)
            return false;
        for (const auto& file : files)
            if (TimelineView::isImportableAudioFile (file))
                return true;
        return false;
    }

    void completeDrop (const juce::StringArray& files, int x, int y)
    {
        owner.updateFileDrop (x, y, hasMidi (files));
        const auto drop = owner.fileDrop;
        owner.clearFileDrop();
        if (! drop.active || drop.rejected)
            return;

        // .mid はMIDI取り込みへ（オーディオの経路と混ぜない。startSample は小節頭スナップ済み）
        if (drop.midi)
        {
            juce::StringArray midiFiles;
            for (const auto& file : files)
                if (MidiFileTypes::isSupported (file))
                    midiFiles.add (file);
            if (! midiFiles.isEmpty() && owner.onImportMidiDropped != nullptr)
            {
                const auto bar = (juce::int64) std::llround ((double) drop.startSample
                                                             / owner.barLengthSamples());
                owner.onImportMidiDropped (midiFiles, bar * Ppq::ticksPerBar);
            }
            return;
        }

        juce::StringArray audioFiles;
        for (const auto& file : files)
            if (TimelineView::isImportableAudioFile (file))
                audioFiles.add (file);
        if (audioFiles.isEmpty())
            return;

        if (drop.instrument) // MIDIトラック行 = サンプル音源の割り当て（配置位置は使わない）
        {
            if (owner.onAssignInstrumentDropped != nullptr)
                owner.onAssignInstrumentDropped (audioFiles, drop.track);
        }
        else if (owner.onImportFilesDropped != nullptr)
        {
            owner.onImportFilesDropped (audioFiles, drop.track, drop.startSample);
        }
    }

    // ループ部分（本体の繰り返し）を1枚の帯として薄く敷く。タイルを並べず1枚にするのは、
    // 「複製が並んでいる」のではなく「1つの実体が繰り返されている」ことを見た目で分けるため。
    // 反復境界の縦線はこの後に本体・中身を描いてから drawLoopTicks で引く
    void drawLoopBand (juce::Graphics& g, juce::Colour base, int x, int loopRight, int y, int reps)
    {
        if (reps <= 1)
            return;
        g.setColour (base.withAlpha (0.42f));
        g.fillRoundedRectangle (juce::Rectangle<int> (x, y + 4, loopRight - x, trackHeight - 8).toFloat(), 4.0f);
    }

    // ループハンドル（選択中のみ）。右端リサイズ（本体右端8px）とは辺を分けているので、
    // 「長さを変えるつもりがループになった」が起きない
    void drawLoopHandle (juce::Graphics& g, int x, int loopRight, int y, bool isSelected)
    {
        if (! isSelected)
            return;
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.fillRoundedRectangle (owner.loopHandleRect (x, loopRight, y).toFloat(), 2.0f);
    }

    // ゲインバッジが占める幅（0 = 出さない）。表示名の描画幅を決めるのにも使う
    static int gainBadgeWidth (float gain, const juce::Rectangle<int>& rect)
    {
        // 狭いリージョンでは振幅の変化だけで示す（名前とバッジを詰め込まない）
        if (std::abs (GainScale::toDb (gain)) < 0.05 || rect.getWidth() < 56)
            return 0;
        // バッジ30px＋右マージン14px。マージンを4pxから広げているのはフェードアウトハンドル
        // （fadeOut=0 のとき右端10px）との重なりを避けるため。選択状態で動くとちらつくので
        // 条件分岐にせず常時10px左へ寄せる
        return 44;
    }

    // 移調バッジが占める幅（0 = 出さない）。移調は痕跡が見えない項目なのでバッジで示す
    // （region-settings の可視性の原則。ストレッチは長さが変わる＝痕跡が見えるのでバッジ不要）。
    // 表示するのは**実効値**（音・波形・長さと同時に切り替わる。要求値だとレンダー完了前に
    // バッジだけ先に変わってしまう）
    static int transposeBadgeWidth (const Clip& clip, const juce::Rectangle<int>& rect)
    {
        if (clip.effectiveTransposeSemitones() == 0 || rect.getWidth() < 56)
            return 0;
        return 30; // バッジ26px＋左マージン4px
    }

    // 移調バッジ（+2 等）。ゲインバッジの左に並べる（ゲインが無ければその位置に置く）
    void drawTransposeBadge (juce::Graphics& g, const Clip& clip, const juce::Rectangle<int>& rect,
                             bool dimmed)
    {
        if (transposeBadgeWidth (clip, rect) == 0)
            return;
        const int semitones = clip.effectiveTransposeSemitones();
        const auto label = (semitones > 0 ? "+" : "") + juce::String (semitones);
        // ゲインバッジ（right-44 起点・幅30）の左へ4px空けて並べる。ゲインが無ければその位置
        const int badgeX = gainBadgeWidth (clip.gain, rect) > 0 ? rect.getRight() - 74
                                                                : rect.getRight() - 40;
        const auto badge = juce::Rectangle<int> (badgeX, rect.getY() + 3, 26, 13);
        g.setColour (juce::Colours::black.withAlpha (0.34f));
        g.fillRoundedRectangle (badge.toFloat(), 3.0f);
        g.setColour (juce::Colours::white.withAlpha (dimmed ? 0.45f : 0.9f));
        g.setFont (Fonts::small());
        g.drawText (label, badge, juce::Justification::centred);
    }

    // ピッチ補正バッジ（♪）。補正は痕跡が見えない（長さも波形もほぼ同じ）項目なのでバッジで示す。
    // 表示は**実効**（鳴っている音が補正済みか。未確定プレビュー中も含む）。移調バッジの左に並べる
    static int pitchBadgeWidth (const Clip& clip, const juce::Rectangle<int>& rect)
    {
        return clip.effectivePitchCorrected() && rect.getWidth() >= 80 ? 22 : 0; // 18px＋左マージン4px
    }
    void drawPitchBadge (juce::Graphics& g, const Clip& clip, const juce::Rectangle<int>& rect, bool dimmed)
    {
        if (pitchBadgeWidth (clip, rect) == 0)
            return;
        int badgeX = rect.getRight() - 40;
        if (gainBadgeWidth (clip.gain, rect) > 0) badgeX -= 34;
        if (transposeBadgeWidth (clip, rect) > 0) badgeX -= 30;
        badgeX -= 0;
        const auto badge = juce::Rectangle<int> (badgeX - 0, rect.getY() + 3, 18, 13);
        g.setColour (juce::Colours::black.withAlpha (0.34f));
        g.fillRoundedRectangle (badge.toFloat(), 3.0f);
        g.setColour (juce::Colours::white.withAlpha (dimmed ? 0.45f : 0.9f));
        g.setFont (Fonts::small());
        g.drawText (juce::String::fromUTF8 (u8"♪"), badge, juce::Justification::centred);
    }

    // リージョンゲインのdB値。0dBのときは何も描かない（デフォルトは沈黙、逸脱だけ主張）。
    // 波形の振幅スケールだけでは -1dB のような微差が読めないので数値も添える。
    // 置き場所は本体の右上: 表示名（左上）とループハンドル（右下）を避けるため。
    // 将来トランスポーズのバッジが増えたら、ここから左へ並べる
    void drawGainBadge (juce::Graphics& g, float gain, const juce::Rectangle<int>& rect, bool dimmed)
    {
        if (gainBadgeWidth (gain, rect) == 0)
            return;

        const auto db = GainScale::toDb (gain);
        const auto label = (db > 0.0 ? "+" : "") + juce::String (db, 1);
        const auto badge = juce::Rectangle<int> (rect.getRight() - 44, rect.getY() + 3, 30, 13);
        g.setColour (juce::Colours::black.withAlpha (0.34f));
        g.fillRoundedRectangle (badge.toFloat(), 3.0f);
        g.setColour (juce::Colours::white.withAlpha (dimmed ? 0.45f : 0.9f));
        g.setFont (Fonts::small());
        g.drawText (label, badge, juce::Justification::centred);
    }

    // フェードの見え方: 斜線を引き、線より上を暗く落とす（Logic流）。波形は素のまま。
    // リージョンゲイン（波形の振幅をスケール）とは流儀が違うが、フェードは「区間の形」であって
    // 「一律の倍率」ではないので、区間を面で示す方が読める。
    // クリップ範囲は **chainRect**（連なり全体）。本体rectでクリップすると2反復目以降の
    // フェードアウトが音には掛かるのに画面に出ない
    void drawFades (juce::Graphics& g, const Clip& clip, const juce::Rectangle<int>& chainRect,
                    bool dimmed)
    {
        const double spp = owner.samplesPerPixel();
        // 斜線の長さは実効座標（波形に対する相対位置が伸縮しても動かない）
        const int inPx = (int) std::llround ((double) clip.renderedFadeIn() / spp);
        const int outPx = (int) std::llround ((double) clip.renderedFadeOut() / spp);
        if (inPx <= 0 && outPx <= 0)
            return;

        juce::Graphics::ScopedSaveState state (g);
        juce::Path chain;
        chain.addRoundedRectangle (chainRect.toFloat(), 4.0f);
        g.reduceClipRegion (chain);

        const auto left = (float) chainRect.getX();
        const auto right = (float) chainRect.getRight();
        const auto top = (float) chainRect.getY();
        const auto bottom = (float) chainRect.getBottom();
        const auto shade = juce::Colours::black.withAlpha (dimmed ? 0.24f : 0.42f);
        const auto lineColour = juce::Colours::white.withAlpha (dimmed ? 0.5f : 0.92f);

        if (inPx > 0)
        {
            juce::Path above; // 斜線より上（三角形）を暗く落とす
            above.startNewSubPath (left, top);
            above.lineTo (left + (float) inPx, top);
            above.lineTo (left, bottom);
            above.closeSubPath();
            g.setColour (shade);
            g.fillPath (above);
            g.setColour (lineColour);
            g.drawLine (left, bottom, left + (float) inPx, top, 1.4f);
        }
        if (outPx > 0)
        {
            juce::Path above;
            above.startNewSubPath (right, top);
            above.lineTo (right - (float) outPx, top);
            above.lineTo (right, bottom);
            above.closeSubPath();
            g.setColour (shade);
            g.fillPath (above);
            g.setColour (lineColour);
            g.drawLine (right - (float) outPx, top, right, bottom, 1.4f);
        }
    }

    // フェードハンドル（選択中のみ・連なり全体の上辺。位置はフェード終端に追従）。
    // 矩形はヒットテストと共有する（owner.fadeHandleRects）ので、見えている場所＝掴める場所になる
    void drawFadeHandles (juce::Graphics& g, const Clip& clip, int y, bool isSelected)
    {
        if (! isSelected)
            return;
        juce::Rectangle<int> fadeInHandle, fadeOutHandle;
        if (! owner.fadeHandleRects (clip, y, fadeInHandle, fadeOutHandle))
            return;

        g.setColour (juce::Colours::white.withAlpha (0.8f));
        for (const auto& handle : { fadeInHandle, fadeOutHandle })
            g.fillRoundedRectangle (handle.withHeight (fadeHandleVisibleHeight).toFloat(), 2.0f);
    }

    // ドラッグ中のフェード長（ms。1000ms以上は秒・小数2桁）。Logicのフェード表示と同じ単位で、
    // 数値の常設表示は置かない（設計ドキュメント「数値表示なし・ドラッグ中ポップアップのみ」）
    void drawFadeDragPopup (juce::Graphics& g)
    {
        const auto& drag = owner.regionDrag;
        if (! drag.isFadeDrag() || owner.project == nullptr)
            return;
        if (drag.track < 0 || drag.track >= (int) owner.project->tracks.size())
            return;
        const auto& track = owner.project->tracks[(size_t) drag.track];
        if (track.type != TrackType::audio || drag.item < 0 || drag.item >= (int) track.clips.size())
            return;

        const auto& clip = track.clips[(size_t) drag.item];
        juce::Rectangle<int> fadeInHandle, fadeOutHandle;
        if (! owner.fadeHandleRects (clip, drag.track * trackHeight, fadeInHandle, fadeOutHandle))
            return;

        const bool isFadeIn = drag.mode == RegionDrag::Mode::fadeIn;
        const auto handle = isFadeIn ? fadeInHandle : fadeOutHandle;
        // 表示は実効座標（聴こえる長さ）。保存値は原音座標なので chain 変換で写す
        const auto samples = isFadeIn ? clip.renderedFadeIn() : clip.renderedFadeOut();
        const double sr = juce::jmax (1.0, owner.effectiveSampleRate());
        const double ms = (double) samples * 1000.0 / sr;
        const auto label = ms >= 1000.0 ? juce::String (ms / 1000.0, 2) + " s"
                                        : juce::String ((int) std::llround (ms)) + " ms";

        const auto font = Fonts::small();
        const int w = juce::GlyphArrangement::getStringWidthInt (font, label) + 14;
        const int h = 18;
        // 見えている範囲（viewport の表示エリア）からはみ出さないよう寄せる
        const auto visible = owner.viewport->getViewArea();
        const int x = juce::jlimit (visible.getX() + 2,
                                    juce::jmax (visible.getX() + 2, visible.getRight() - w - 2),
                                    handle.getCentreX() - w / 2);
        const auto box = juce::Rectangle<int> (x, handle.getBottom() + 4, w, h);

        g.setColour (Theme::popupBg);
        g.fillRoundedRectangle (box.toFloat(), 3.0f);
        g.setColour (Theme::popupBorder);
        g.drawRoundedRectangle (box.toFloat().reduced (0.5f), 3.0f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.92f));
        g.setFont (font);
        g.drawText (label, box, juce::Justification::centred);
    }

    void drawClip (juce::Graphics& g, const Clip& clip, int y, bool isSelected,
                   const juce::Rectangle<int>& clipRegion, bool trackDimmed)
    {
        const double spp = owner.samplesPerPixel();
        const int x = owner.sampleToX (clip.startSample);
        // 幅・反復間隔・波形はすべて実効（見かけ）座標。要求値からは計算しない
        const auto renderedLength = clip.renderedLengthSamples();
        const int w = juce::jmax (2, (int) ((double) renderedLength / spp));
        const int reps = 1 + juce::jmax (0, clip.loopCount);
        const int loopRight = owner.sampleToX (clip.startSample + clip.renderedTotalLengthSamples());
        if (x > clipRegion.getRight() || loopRight < clipRegion.getX())
            return;

        // rect（本体1反復分）は波形・表示名・バッジ用。フェードの描画とハンドルは
        // 連なり全体（chainRect = drawLoopBand が敷く帯と同じ矩形）を基準にする
        const auto rect = juce::Rectangle<int> (x, y + 4, w, trackHeight - 8);
        const auto chainRect = juce::Rectangle<int> (x, y + 4, juce::jmax (w, loopRight - x),
                                                     trackHeight - 8);
        // ミュート中（リージョン単位 or トラック単位・ソロ含む）はグレー減光（Logic準拠）
        const bool dimmed = clip.muted || trackDimmed;
        const auto base = dimmed ? (isSelected ? Theme::clipMutedSelected : Theme::clipMuted)
                                 : (isSelected ? Theme::accent : Theme::clipAudio);
        drawLoopBand (g, base, x, loopRight, y, reps);
        g.setColour (base);
        g.fillRoundedRectangle (rect.toFloat(), 4.0f);
        // 上端1pxの微かなハイライトで面の上面を作る（Logicのリージョンと同じ。強くしない）
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (rect.getX() + 4, rect.getY() + 1, rect.getWidth() - 8, 1);
        if (isSelected)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (rect.toFloat().reduced (0.5f), 4.0f, 1.5f);
        }

        // 波形（activeDomain の共有ピークキャッシュから描く。view の端の部分区間だけ
        // peakBetween が実バッファから集計し直す — 境界の外にあるピークを見せないため）。
        // ループ部分は同じ形を薄く繰り返す
        const float midY = (float) rect.getCentreY();
        const float halfH = (float) (rect.getHeight() / 2 - 3);
        const auto* domain = clip.effectiveDomain(); // 未確定プレビューがあればその波形
        const auto viewStart = clip.viewStartRendered();
        const auto viewEnd = clip.viewEndRendered();
        for (int r = 0; r < reps; ++r)
        {
            const int repX = owner.sampleToX (clip.startSample + (juce::int64) r * renderedLength);
            if (repX > clipRegion.getRight())
                break;
            if (repX + w < clipRegion.getX())
                continue;
            if (r > 0) // 反復の切れ目
            {
                g.setColour (juce::Colours::white.withAlpha (0.25f));
                g.drawVerticalLine (repX, (float) rect.getY(), (float) rect.getBottom());
            }
            g.setColour (juce::Colours::white.withAlpha (dimmed ? 0.3f : (r > 0 ? 0.4f : 0.75f)));
            const int x0 = juce::jmax (repX, clipRegion.getX());
            const int x1 = juce::jmin (repX + w, clipRegion.getRight());
            for (int px = x0; domain != nullptr && px < x1; ++px)
            {
                // view 相対のピクセル範囲 → ドメイン render 座標。view の外は読まない
                const auto r0 = viewStart + (juce::int64) ((double) (px - repX) * spp);
                const auto r1 = juce::jmin (viewEnd,
                                            viewStart + (juce::int64) std::ceil ((double) (px - repX + 1) * spp));
                const float peak = domain->peakBetween (r0, r1, Clip::samplesPerPeak);

                // リージョンゲインを描画振幅に掛ける（「見た目＝出る音」。キャッシュは素のまま保つ）。
                // 上げ側は jlimit で頭打ちになり、リージョン内で潰れて見える
                const float h = juce::jlimit (1.0f, halfH, peak * clip.gain * halfH * 1.4f);
                g.drawVerticalLine (px, midY - h, midY + h);
            }
        }
        // フェードは波形の上に描く（斜線＝痕跡は未選択でも出す。ハンドルだけが選択中のみ）
        drawFades (g, clip, chainRect, dimmed);
        drawLoopHandle (g, x, loopRight, y, isSelected);
        drawFadeHandles (g, clip, y, isSelected);

        // 表示名（取り込みクリップのみ。録音クリップは空=無ラベル）。
        // 強弱方針: リージョン本体より控えめ（波形と同程度のアルファ）
        if (clip.name.isNotEmpty() && rect.getWidth() >= 40)
        {
            // 名前とバッジ（ゲイン・移調）は同じ高さに並ぶので、バッジがある分だけ名前の幅を削る
            // （長い取り込みファイル名でdB値が読めなくなるのを防ぐ）
            const int nameWidth = rect.getWidth() - 12 - gainBadgeWidth (clip.gain, rect)
                                  - transposeBadgeWidth (clip, rect);
            g.setColour (juce::Colours::white.withAlpha (dimmed ? 0.35f : 0.7f));
            g.setFont (Fonts::forText (Fonts::small(), clip.name));
            g.drawText (clip.name, rect.getX() + 6, rect.getY() + 3, juce::jmax (0, nameWidth), 12,
                        juce::Justification::centredLeft);
        }
        drawGainBadge (g, clip.gain, rect, dimmed); // 名前より後に描く（万一重なっても数値が読める）
        drawTransposeBadge (g, clip, rect, dimmed); // ゲインバッジの左に並ぶ
        drawPitchBadge (g, clip, rect, dimmed);     // さらにその左
    }

    void drawMidiRegion (juce::Graphics& g, const MidiRegion& region, int y, bool isSelected,
                         const juce::Rectangle<int>& clipRegion, bool trackDimmed)
    {
        const int x = owner.ppqToX (region.startPpq);
        const int w = juce::jmax (2, owner.ppqToX (region.startPpq + region.lengthPpq) - x);
        const int reps = 1 + juce::jmax (0, region.loopCount);
        const int loopRight = owner.ppqToX (region.startPpq + region.totalLengthPpq());
        if (x > clipRegion.getRight() || loopRight < clipRegion.getX())
            return;

        const auto rect = juce::Rectangle<int> (x, y + 4, w, trackHeight - 8);
        // ミュート中（リージョン単位 or トラック単位・ソロ含む）はグレー減光（Logic準拠）
        const bool dimmed = region.muted || trackDimmed;
        const auto base = dimmed ? (isSelected ? Theme::clipMutedSelected : Theme::clipMuted)
                                 : (isSelected ? Theme::regionMidiSelected : Theme::regionMidi);
        drawLoopBand (g, base, x, loopRight, y, reps);
        g.setColour (base);
        g.fillRoundedRectangle (rect.toFloat(), 4.0f);
        // 上端1pxの微かなハイライトで面の上面を作る（Logicのリージョンと同じ。強くしない）
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (rect.getX() + 4, rect.getY() + 1, rect.getWidth() - 8, 1);
        if (isSelected)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (rect.toFloat().reduced (0.5f), 4.0f, 1.5f);
        }

        // ノートのミニチュア（ピッチ範囲 C1..C7 に射影。範囲外はクランプ）。
        // ループ部分は同じノート列を薄く繰り返す（元を編集すると全部に反映されることを示す）
        constexpr int loPitch = 24, hiPitch = 96;
        const auto inner = rect.reduced (1, 3);
        const double tickW = (double) w / (double) juce::jmax ((juce::int64) 1, region.lengthPpq);

        for (int r = 0; r < reps; ++r)
        {
            const int repX = owner.ppqToX (region.startPpq + (juce::int64) r * region.lengthPpq);
            if (repX > clipRegion.getRight())
                break;
            if (repX + w < clipRegion.getX())
                continue;
            if (r > 0) // 反復の切れ目
            {
                g.setColour (juce::Colours::white.withAlpha (0.25f));
                g.drawVerticalLine (repX, (float) rect.getY(), (float) rect.getBottom());
            }
            g.setColour (juce::Colours::white.withAlpha (dimmed ? 0.3f : (r > 0 ? 0.45f : 0.8f)));
            for (const auto& note : region.notes)
            {
                const int nx = repX + (int) std::llround ((double) note.startPpq * tickW);
                const int nw = juce::jmax (2, (int) std::llround ((double) note.lengthPpq * tickW));
                const int clamped = juce::jlimit (loPitch, hiPitch, note.pitch);
                const float rel = (float) (hiPitch - clamped) / (float) (hiPitch - loPitch);
                const int ny = inner.getY() + (int) (rel * (float) (inner.getHeight() - 2));
                g.fillRect (juce::jmax (nx, repX), ny, juce::jmin (nw, repX + w - nx), 2);
            }
        }
        drawLoopHandle (g, x, loopRight, y, isSelected);
    }

    TimelineView& owner;
};

// ---- TimelineView 本体 ---------------------------------------------------

TimelineView::TimelineView (TransportState& transportState)
    : transport (transportState)
{
    ruler = std::make_unique<RulerContent> (*this);
    markerLane = std::make_unique<MarkerLaneContent> (*this);
    songFadeBand = std::make_unique<SongFadeBandContent> (*this);
    lanes = std::make_unique<LaneContent> (*this);
    viewport = std::make_unique<LaneViewport> (*this);

    viewport->setViewedComponent (lanes.get(), false);
    viewport->setScrollBarsShown (true, true);

    addAndMakeVisible (*viewport);
    addAndMakeVisible (*ruler);
    addAndMakeVisible (*markerLane);
    addChildComponent (*songFadeBand); // 表示はフェード設定時のみ（laneTop/resizedが切り替える）

    startTimerHz (30); // GOTCHAS.md: 通知はpush型でなくpull型（Timerポーリング）
}

TimelineView::~TimelineView() = default;

void TimelineView::setProject (Project* p)
{
    project = p;
    selection.clear();
    refresh();
}

void TimelineView::setSelectedTrack (int index)
{
    if (selectedTrack == index)
        return;
    selectedTrack = index;
    lanes->repaint();
}

void TimelineView::refresh()
{
    // フェード帯の出入りでレーン上端が変わったらレイアウトを組み直す
    // （ヘッダ側の高さ合わせも必要なので onLaneTopChanged で外へ伝える）
    const bool bandVisible = project != nullptr && project->hasFadeOut();
    if (bandVisible != lastFadeBandVisible)
    {
        lastFadeBandVisible = bandVisible;
        resized();
        if (onLaneTopChanged)
            onLaneTopChanged();
    }

    updateContentSize();
    lanes->repaint();
    ruler->repaint();
    markerLane->repaint();
    songFadeBand->repaint();
}

void TimelineView::clearSelection()
{
    if (! selection.isValid() && ! regionSelection.isValid())
        return;
    selection.clear();
    regionSelection.clear();
    lanes->repaint();
}

void TimelineView::remapSelectionTracks (int newClipTrack, int newRegionTrack)
{
    if (selection.isValid())
    {
        if (newClipTrack >= 0)
            selection.track = newClipTrack;
        else
            selection.clear();
    }
    if (regionSelection.isValid())
    {
        if (newRegionTrack >= 0)
            regionSelection.track = newRegionTrack;
        else
            regionSelection.clear();
    }
    lanes->repaint();
}

double TimelineView::effectiveSampleRate() const
{
    if (project != nullptr && project->sampleRate > 0.0)
        return project->sampleRate;
    const auto deviceRate = transport.sampleRate.load();
    return deviceRate > 0.0 ? deviceRate : 48000.0;
}

double TimelineView::barLengthSamples() const
{
    const double bpm = juce::jlimit (20.0, 400.0, transport.bpm.load());
    return effectiveSampleRate() * 60.0 / bpm * 4.0; // 4/4固定
}

double TimelineView::samplesPerPixel() const
{
    return barLengthSamples() / pxPerBar;
}

int TimelineView::gridDivisionsPerBar() const
{
    // 線の間隔が12px以上確保できる最も細かい分割を選ぶ。上限は1/16音符
    // （Tier 1ではグリッドは表示とシークの目安のみなので、これ以上細かくしない）
    int div = 1;
    for (int candidate : { 2, 4, 8, 16 })
        if (pxPerBar / candidate >= 12.0)
            div = candidate;
    return div;
}

void TimelineView::zoomBy (double factor)
{
    const auto view = viewport->getViewArea();
    const int playheadX = sampleToX (transport.uiPositionSample());
    const int anchorX = (playheadX >= view.getX() && playheadX <= view.getRight())
                            ? playheadX
                            : view.getCentreX();
    zoomAroundContentX (factor, anchorX);
}

void TimelineView::zoomAroundContentX (double factor, int contentX)
{
    const double newPxPerBar = juce::jlimit (minPxPerBar, maxPxPerBar, pxPerBar * factor);
    if (juce::approximatelyEqual (newPxPerBar, pxPerBar))
        return;

    // アンカー位置のサンプルが画面上の同じ場所に留まるようスクロールを補正する
    const auto anchorSample = xToSample (contentX);
    const int anchorOffset = contentX - viewport->getViewPositionX();
    pxPerBar = newPxPerBar;
    updateContentSize();
    viewport->setViewPosition (juce::jmax (0, sampleToX (anchorSample) - anchorOffset),
                               viewport->getViewPositionY());
    lanes->repaint();
    ruler->repaint();
    markerLane->repaint();
}

int TimelineView::sampleToX (juce::int64 samplePos) const
{
    return (int) std::llround ((double) samplePos / samplesPerPixel());
}

juce::int64 TimelineView::xToSample (int x) const
{
    return (juce::int64) std::llround ((double) x * samplesPerPixel());
}

juce::int64 TimelineView::snapSampleToGrid (juce::int64 sample) const
{
    const double gridSamples = barLengthSamples() / gridDivisionsPerBar();
    const auto gridIndex = std::llround ((double) sample / gridSamples);
    return juce::jmax ((juce::int64) 0, (juce::int64) std::llround ((double) gridIndex * gridSamples));
}

juce::int64 TimelineView::snapSampleToVisibleGrid (juce::int64 sample) const
{
    return snapSampleToGrid (sample);
}

bool TimelineView::isImportableAudioFile (const juce::String& path)
{
    return AudioFileTypes::isSupported (path);
}

void TimelineView::updateFileDrop (int contentX, int contentY, bool isMidi)
{
    FileDropState next;
    next.active = true;
    next.startSample = snapSampleToGrid (juce::jmax ((juce::int64) 0, xToSample (contentX)));

    if (isMidi)
    {
        // .mid は常に新規トラック＋小節頭配置（行に依存しない）。挿入位置ラインも小節頭に出す
        next.midi = true;
        next.track = -1;
        const double barLen = barLengthSamples();
        const auto bar = (juce::int64) std::floor (
            (double) juce::jmax ((juce::int64) 0, xToSample (contentX)) / barLen);
        next.startSample = (juce::int64) std::llround ((double) bar * barLen);
        if (! (next == fileDrop))
        {
            fileDrop = next;
            lanes->repaint();
        }
        return;
    }

    const int numTracks = project != nullptr ? (int) project->tracks.size() : 0;
    const int row = contentY / trackHeight;
    if (row >= 0 && row < numTracks)
    {
        next.track = row;
        // MIDIトラック行はクリップを置けないが、サンプル音源の割り当てとして受理する
        next.instrument = project->tracks[(size_t) row].type == TrackType::midi;
    }
    else
    {
        next.track = -1; // 空白ゾーン = 新規トラックを作成して配置
    }

    if (! (next == fileDrop))
    {
        fileDrop = next;
        lanes->repaint();
    }
}

void TimelineView::clearFileDrop()
{
    if (fileDrop.active)
    {
        fileDrop = {};
        lanes->repaint();
    }
}

double TimelineView::samplesPerTick() const
{
    const double bpm = juce::jlimit (20.0, 400.0, transport.bpm.load());
    return 1.0 / Ppq::ticksPerSample (bpm, effectiveSampleRate());
}

int TimelineView::ppqToX (juce::int64 ppq) const
{
    return (int) std::llround ((double) ppq * samplesPerTick() / samplesPerPixel());
}

juce::int64 TimelineView::xToPpq (int x) const
{
    return (juce::int64) std::llround ((double) x * samplesPerPixel() / samplesPerTick());
}

juce::int64 TimelineView::gridPpq() const
{
    return Ppq::ticksPerBar / gridDivisionsPerBar();
}

juce::int64 TimelineView::beatStartSample (int beats) const
{
    return (juce::int64) std::llround ((double) juce::jmax (0, beats) * barLengthSamples() / 4.0);
}

int TimelineView::beatToX (int beats) const
{
    return (int) std::llround ((double) juce::jmax (0, beats) * pxPerBar / 4.0);
}

int TimelineView::snapUnitBeats() const
{
    // 表示中グリッドと同じ刻みで置けるが、上限は拍（セクションは曲構造のラベルなので
    // 1/8以下の細かさは誤操作リスクにしかならない）。ズームアウト中は小節頭のまま
    return 4 / juce::jmin (gridDivisionsPerBar(), 4);
}

int TimelineView::xToMarkerBeats (int x) const
{
    const int unit = snapUnitBeats();
    const double unitPx = pxPerBar / 4.0 * unit;
    return juce::jmax (0, (int) std::floor ((double) x / unitPx)) * unit;
}

juce::int64 TimelineView::sixteenthStartSample (int sixteenths) const
{
    return (juce::int64) std::llround ((double) juce::jmax (0, sixteenths) * barLengthSamples() / 16.0);
}

int TimelineView::sixteenthToX (int sixteenths) const
{
    return (int) std::llround ((double) sixteenths * pxPerBar / 16.0);
}

int TimelineView::xToCycleSixteenths (int x) const
{
    // シークと同じ「表示中の最小グリッド」（1/16上限）へ、最近傍の線に吸着させる。
    // snapUnitBeats()（最小1拍・マーカー用）は使わない
    const int unit = 16 / gridDivisionsPerBar(); // 表示グリッド1マス分の16分音符数
    const double unitPx = pxPerBar / 16.0 * unit;
    return juce::jmax (0, (int) std::llround ((double) x / unitPx)) * unit;
}

int TimelineView::laneTop() const
{
    return topHeight + (project != nullptr && project->hasFadeOut() ? songFadeBandHeight : 0);
}

void TimelineView::resized()
{
    const int top = laneTop();
    const bool bandVisible = top > topHeight;
    songFadeBand->setVisible (bandVisible); // 幅は updateContentSize がコンテンツ幅へ合わせる

    viewport->setBounds (0, top, getWidth(), getHeight() - top);
    updateContentSize();
    syncScroll();
}

void TimelineView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::timelineBg);
}

void TimelineView::timerCallback()
{
    // 描画と同じ論理位置（保留中シーク込み）で判定する。生の再生位置を見ると、
    // クリック直後の1バッファ分だけ白線が旧位置に取り残される
    const auto playhead = transport.uiPositionSample();
    const bool active = transport.isPlaying.load() || transport.recordArmed.load();
    // トラックの減光表示（ミュート・ソロ）は、ヘッダ・ミキサー・m/sキーの
    // どの経路で変わってもここで拾う（pull型）
    const auto dimMask = trackDimMask();
    if (playhead == lastPaintedPlayhead && dimMask == lastPaintedTrackDimMask && ! active)
        return;

    lastPaintedPlayhead = playhead;
    lastPaintedTrackDimMask = dimMask;
    updateContentSize();

    // 再生ヘッドが見切れたら追従スクロール
    if (transport.isPlaying.load())
    {
        const int playheadX = sampleToX (playhead);
        const auto view = viewport->getViewArea();
        if (playheadX < view.getX() || playheadX > view.getRight() - 60)
            viewport->setViewPosition (juce::jmax (0, playheadX - 60), view.getY());
    }

    lanes->repaint();
    ruler->repaint();
    markerLane->repaint();
}

juce::uint64 TimelineView::trackDimMask() const
{
    // 描画に効く「行の減光状態」（ミュート＋他トラックのソロによる実質ミュート）を
    // ビット列化する。トラック数は〜50想定（CLAUDE.md）なのでuint64で足りる
    if (project == nullptr)
        return 0;

    bool anySolo = false;
    for (auto& t : project->tracks)
        anySolo = anySolo || t.params->solo.load();

    juce::uint64 mask = 0;
    for (size_t i = 0; i < project->tracks.size(); ++i)
    {
        const auto& params = *project->tracks[i].params;
        if (params.mute.load() || (anySolo && ! params.solo.load()))
            mask |= (juce::uint64) 1 << (i % 64);
    }
    return mask;
}

void TimelineView::updateContentSize()
{
    const double barLen = barLengthSamples();

    juce::int64 maxSample = editPositionSample();
    if (project != nullptr)
    {
        const double spt = samplesPerTick();
        for (auto& track : project->tracks)
        {
            // ループ終端まで含める（含めないと長いループがビューポート外に出てスクロールできない）
            for (auto& clip : track.clips)
                maxSample = juce::jmax (maxSample, clip.startSample + clip.renderedTotalLengthSamples());
            for (auto& region : track.midiRegions)
                maxSample = juce::jmax (maxSample,
                                        (juce::int64) std::llround ((double) (region.startPpq + region.totalLengthPpq()) * spt));
        }
        // 後方のマーカー（素材より先の小節）もコンテンツ幅に含める（見えない・操作できないを防ぐ）
        if (! project->markers.empty())
            maxSample = juce::jmax (maxSample,
                                    beatStartSample (project->markers.back().startBeats + 4));
        // サイクル範囲も同様（素材より先に範囲を描いたときに見切れない）
        if (project->hasCycleRange())
            maxSample = juce::jmax (maxSample, sixteenthStartSample (project->cycleEndSixteenths));
        // 曲末フェードの終端も同様。これが無いと、後方のマーカーやサイクルを頼りに終端を
        // 素材より先へ置いた後でそれらを消したとき、幅が縮んで終端ハンドルを掴めなくなる
        if (project->hasFadeOut())
            maxSample = juce::jmax (maxSample, sixteenthStartSample (project->fadeOutEndSixteenths));
    }
    if (transport.recordArmed.load())
        maxSample = juce::jmax (maxSample,
                                transport.punchInSample.load() + transport.recordedSamples.load());

    const int numBars = juce::jmax (64, (int) std::ceil ((double) maxSample / barLen) + 8);
    const int contentWidth = (int) std::ceil (numBars * pxPerBar);
    const int numTracks = project != nullptr ? (int) project->tracks.size() : 0;
    const int contentHeight = juce::jmax (numTracks * trackHeight,
                                          viewport->getMaximumVisibleHeight());

    if (contentWidth != lanes->getWidth() || contentHeight != lanes->getHeight())
        lanes->setSize (contentWidth, contentHeight);
    if (contentWidth != ruler->getWidth() || ruler->getHeight() != rulerHeight)
        ruler->setSize (contentWidth, rulerHeight);
    if (contentWidth != markerLane->getWidth() || markerLane->getHeight() != markerLaneHeight)
        markerLane->setSize (contentWidth, markerLaneHeight);
    if (contentWidth != songFadeBand->getWidth() || songFadeBand->getHeight() != songFadeBandHeight)
        songFadeBand->setSize (contentWidth, songFadeBandHeight);
}

void TimelineView::syncScroll()
{
    ruler->setTopLeftPosition (-viewport->getViewPositionX(), 0);
    markerLane->setTopLeftPosition (-viewport->getViewPositionX(), rulerHeight);
    songFadeBand->setTopLeftPosition (-viewport->getViewPositionX(), topHeight);
    if (onVerticalScroll)
        onVerticalScroll (viewport->getViewPositionY());
}

void TimelineView::scrollVertically (float wheelDeltaY)
{
    viewport->setViewPosition (viewport->getViewPositionX(),
                               juce::jmax (0, viewport->getViewPositionY()
                                                  - (int) (wheelDeltaY * 100.0f)));
}

void TimelineView::handleLaneMouseDown (const juce::MouseEvent& e)
{
    regionDrag = {};
    if (project == nullptr)
        return;

    const int numTracks = (int) project->tracks.size();
    const int row = e.y / trackHeight;
    if (row >= 0 && row < numTracks && onTrackSelected)
        onTrackSelected (row);

    // 右クリック: リージョン/クリップ上ならまず選択してからメニュー表示（ドラッグ・シークはしない）。
    // リージョン外の右クリックは何もしない
    if (e.mods.isPopupMenu())
    {
        if (row >= 0 && row < numTracks)
        {
            const auto& track = project->tracks[(size_t) row];
            const int item = track.type == TrackType::midi ? hitTestRegion (row, e.x)
                                                           : hitTestClip (row, e.x);
            if (item >= 0)
            {
                // 仮オブジェクトの右クリックは撤去して中止（メニューを出さない）
                if (track.type == TrackType::midi && onPreviewObjectGesture != nullptr
                    && onPreviewObjectGesture (track.id, track.midiRegions[(size_t) item].id))
                {
                    lanes->repaint();
                    return;
                }
                if (track.type != TrackType::midi && onPreviewClipGesture != nullptr
                    && onPreviewClipGesture (track.id, track.clips[(size_t) item].fileName))
                {
                    lanes->repaint();
                    return;
                }
                if (track.type == TrackType::midi)
                {
                    selection.clear();
                    regionSelection = { row, item };
                }
                else
                {
                    selection = { row, item };
                    regionSelection.clear();
                }
                if (onSelectionChanged)
                    onSelectionChanged();
                lanes->repaint();
                showItemMenu (row, item);
            }
        }
        return;
    }

    if (row >= 0 && row < numTracks)
    {
        auto& track = project->tracks[(size_t) row];

        if (track.type == TrackType::midi)
        {
            const int ri = hitTestRegion (row, e.x);
            // 仮オブジェクトを掴んだら撤去して中止（選択もドラッグも始めない。
            // ダブルクリックも1クリック目がここを通るため、ピアノロールが開くことはない）
            if (ri >= 0 && onPreviewObjectGesture != nullptr
                && onPreviewObjectGesture (track.id, track.midiRegions[(size_t) ri].id))
            {
                lanes->repaint();
                return;
            }
            if (ri >= 0)
            {
                const bool wasSelected = regionSelection.track == row && regionSelection.region == ri;
                selection.clear();
                regionSelection = { row, ri };
                auto& region = track.midiRegions[(size_t) ri];

                // 判定順: ループハンドル（選択中のみ・右下隅はこちら優先）→ 右端リサイズ → 移動。
                // ハンドルは選択中しか出ないので、未選択アイテムの1クリック目でループが伸びることはない
                const int itemX = ppqToX (region.startPpq);
                const int itemRight = ppqToX (region.startPpq + region.totalLengthPpq());
                const bool onHandle = wasSelected && ! e.mods.isAltDown()
                                      && loopHandleRect (itemX, itemRight, row * trackHeight)
                                             .contains (e.x, e.y);
                // 録音中はループ編集しない。ここで move/resize へ落とすと「ハンドルを掴んだつもりが
                // リージョンが動く」になるので、ドラッグを開始せず選択だけで終える
                if (onHandle && canEdit != nullptr && ! canEdit())
                {
                    if (onSelectionChanged)
                        onSelectionChanged();
                    lanes->repaint();
                    return;
                }
                // 右端リサイズはループ中は無効（本体の右端がループ部分に隠れて「真ん中でリサイズ」になるため）
                const int rightX = ppqToX (region.startPpq + region.lengthPpq);
                const bool onResize = ! onHandle && ! e.mods.isAltDown()
                                      && region.loopCount == 0 && e.x >= rightX - 8;
                regionDrag.mode = onHandle ? RegionDrag::Mode::loop
                                  : onResize ? RegionDrag::Mode::resize
                                             : RegionDrag::Mode::move;
                regionDrag.isMidi = true;
                regionDrag.track = row;
                regionDrag.item = ri;
                regionDrag.origStartPpq = region.startPpq;
                regionDrag.origLengthPpq = region.lengthPpq;
                regionDrag.origLoopCount = region.loopCount;
                regionDrag.startX = e.x;
                regionDrag.duplicateOnDrag = e.mods.isAltDown(); // 複製は実際に動いた時点で作る

                if (onSelectionChanged)
                    onSelectionChanged();
                lanes->repaint();
                return;
            }
        }
        else
        {
            const int ci = hitTestClip (row, e.x);
            if (ci >= 0)
            {
                // 仮クリップ（ループ採用のプレビュー）を掴んだら撤去して中止
                // （MIDI の仮リージョンと同じ規則。選択もドラッグも始めない）
                if (onPreviewClipGesture != nullptr
                    && onPreviewClipGesture (track.id, track.clips[(size_t) ci].fileName))
                {
                    lanes->repaint();
                    return;
                }
                const bool wasSelected = selection.track == row && selection.clip == ci;
                selection = { row, ci };
                regionSelection.clear();
                const auto& clip = track.clips[(size_t) ci];

                // オーディオは移動・ループ・フェードのみ（リサイズ＝トリムはTier 1スコープ外）。
                // ⌥ドラッグで複製。判定順は フェードハンドル（上辺）→ ループハンドル（下辺）→ 移動で、
                // どちらのハンドルも選択中しか出ない（MIDIリージョンと同じ規則）
                const int itemX = sampleToX (clip.startSample);
                const int itemRight = sampleToX (clip.startSample + clip.renderedTotalLengthSamples());
                const bool grabbable = wasSelected && ! e.mods.isAltDown();
                const int fadeHandle = grabbable ? hitTestFadeHandle (clip, row * trackHeight,
                                                                     { e.x, e.y })
                                                 : -1;
                const bool onHandle = fadeHandle < 0 && grabbable
                                      && loopHandleRect (itemX, itemRight, row * trackHeight)
                                             .contains (e.x, e.y);
                // 録音中はループ・フェード編集しない（移動へ落とさず選択だけで終える。MIDIと同じ規則）
                if ((onHandle || fadeHandle >= 0) && canEdit != nullptr && ! canEdit())
                {
                    if (onSelectionChanged)
                        onSelectionChanged();
                    lanes->repaint();
                    return;
                }
                regionDrag.mode = fadeHandle == 0   ? RegionDrag::Mode::fadeIn
                                  : fadeHandle == 1 ? RegionDrag::Mode::fadeOut
                                  : onHandle        ? RegionDrag::Mode::loop
                                                    : RegionDrag::Mode::move;
                regionDrag.isMidi = false;
                regionDrag.track = row;
                regionDrag.item = ci;
                regionDrag.origStartSample = clip.startSample;
                regionDrag.origLoopCount = clip.loopCount;
                regionDrag.origFadeInSamples = clip.fadeInSamples;
                regionDrag.origFadeOutSamples = clip.fadeOutSamples;
                regionDrag.origRenderedFadeIn = clip.renderedFadeIn();
                regionDrag.origRenderedFadeOut = clip.renderedFadeOut();
                regionDrag.startX = e.x;
                regionDrag.duplicateOnDrag = e.mods.isAltDown();

                if (onSelectionChanged)
                    onSelectionChanged();
                lanes->repaint();
                return;
            }
        }
    }

    // 空白クリック → 選択解除＋シーク（表示グリッドにスナップ）
    if (selection.isValid() || regionSelection.isValid())
    {
        selection.clear();
        regionSelection.clear();
        if (onSelectionChanged)
            onSelectionChanged();
    }
    seekFromX (e.x);
    lanes->repaint();
    ruler->repaint();
}

void TimelineView::handleLaneMouseDrag (const juce::MouseEvent& e)
{
    if (regionDrag.mode == RegionDrag::Mode::none || project == nullptr)
        return;
    if (regionDrag.track < 0 || regionDrag.track >= (int) project->tracks.size())
        return;
    auto& sourceTrack = project->tracks[(size_t) regionDrag.track];
    const int itemCount = (int) (regionDrag.isMidi ? sourceTrack.midiRegions.size()
                                                   : sourceTrack.clips.size());
    if (regionDrag.item < 0 || regionDrag.item >= itemCount)
        return;

    // ループ伸縮のスナップ: ハンドルはループ終端を掴んでいる。ドラッグ後の総再生長を本体長で
    // 割って回数にする（本体長の整数倍のみ。本体終端まで戻すと 0 = ループなしへ正規化される）
    const auto loopCountFromDrag = [&] (double bodyLength, double delta)
    {
        const double body = juce::jmax (1.0, bodyLength);
        const double newTotal = body * (1 + regionDrag.origLoopCount) + delta;
        return juce::jlimit (0, maxLoopCount, (int) std::llround (newTotal / body) - 1);
    };

    // スナップ後の時間位置・長さを先に計算する（編集開始の「実際に値が変わるか」の判定にも使う）
    juce::int64 snappedStart = 0, snappedLength = 0;
    int snappedLoopCount = regionDrag.origLoopCount;
    juce::int64 draggedFadeIn = regionDrag.origRenderedFadeIn;
    juce::int64 draggedFadeOut = regionDrag.origRenderedFadeOut;
    if (regionDrag.isFadeDrag())
    {
        // フェードはスナップしない（平滑化であって音楽的位置ではない）。ドラッグは
        // **実効（見かけ）座標**で行い、クランプは「実効全長 - 相手のフェード」で止める
        // （clampFades は fadeIn 優先なので、そのまま通すと相手を押しのけてしまう）
        const auto& clip = sourceTrack.clips[(size_t) regionDrag.item];
        const auto deltaSamples = xToSample (e.x) - xToSample (regionDrag.startX);
        if (regionDrag.mode == RegionDrag::Mode::fadeIn)
            draggedFadeIn = clip.clampedRenderedFadeIn (regionDrag.origRenderedFadeIn + deltaSamples);
        else // フェードアウトのハンドルは右へ動かすほど短くなる
            draggedFadeOut = clip.clampedRenderedFadeOut (regionDrag.origRenderedFadeOut - deltaSamples);
    }
    if (regionDrag.isMidi)
    {
        const auto grid = juce::jmax ((juce::int64) 1, gridPpq());
        const auto deltaPpq = xToPpq (e.x) - xToPpq (regionDrag.startX);
        snappedStart = juce::jmax ((juce::int64) 0,
                                   (juce::int64) std::llround ((double) (regionDrag.origStartPpq + deltaPpq)
                                                               / (double) grid) * grid);
        snappedLength = juce::jmax (grid,
                                    (juce::int64) std::llround ((double) (regionDrag.origLengthPpq + deltaPpq)
                                                                / (double) grid) * grid);
        if (regionDrag.mode == RegionDrag::Mode::loop)
            snappedLoopCount = loopCountFromDrag ((double) regionDrag.origLengthPpq, (double) deltaPpq);
    }
    else
    {
        const auto deltaSamples = xToSample (e.x) - xToSample (regionDrag.startX);
        snappedStart = snapSampleToGrid (regionDrag.origStartSample + deltaSamples);
        if (regionDrag.mode == RegionDrag::Mode::loop)
            snappedLoopCount = loopCountFromDrag ((double) sourceTrack.clips[(size_t) regionDrag.item].renderedLengthSamples(),
                                                  (double) deltaSamples);
    }
    const bool timeChanged = regionDrag.mode == RegionDrag::Mode::resize
                                 ? snappedLength != regionDrag.origLengthPpq
                             : regionDrag.mode == RegionDrag::Mode::loop
                                 ? snappedLoopCount != regionDrag.origLoopCount
                             : regionDrag.mode == RegionDrag::Mode::fadeIn
                                 ? draggedFadeIn != regionDrag.origRenderedFadeIn
                             : regionDrag.mode == RegionDrag::Mode::fadeOut
                                 ? draggedFadeOut != regionDrag.origRenderedFadeOut
                                 : snappedStart != (regionDrag.isMidi ? regionDrag.origStartPpq
                                                                      : regionDrag.origStartSample);

    // ドロップ先トラック: 同種トラックのレーンだけ有効。無効な位置（異種トラック・レーン外の余白・
    // 上端より上）では最後に有効だったトラック（= 現在の所属）に留める。リサイズはトラック跨ぎなし
    int targetRow = regionDrag.track;
    if (regionDrag.mode == RegionDrag::Mode::move)
    {
        const int row = e.y < 0 ? -1 : e.y / trackHeight;
        if (row >= 0 && row < (int) project->tracks.size()
            && project->tracks[(size_t) row].type == sourceTrack.type)
            targetRow = row;
    }
    const bool trackChanged = targetRow != regionDrag.track;

    if (! regionDrag.edited)
    {
        // 編集開始 = ドラッグ閾値超え かつ モデルが実際に変わる操作（スナップ後の時間 or 所属トラック
        // の変化）。同一レーン内の縦ぶれ・スナップに満たない横ぶれでは undo登録も⌥複製もしない
        if (e.getDistanceFromDragStart() < 4 || (! timeChanged && ! trackChanged))
            return;
        // フェードは「クリップのオーディオ値」なのでundo種別・push経路が構造編集と別
        // （通常の pushSnapshot だと鳴っているMIDIが消音＋再発音される）
        if (regionDrag.isFadeDrag())
        {
            if (onWillEditClipValue)
                onWillEditClipValue();
        }
        else if (onWillEditModel)
        {
            onWillEditModel();
        }
        regionDrag.edited = true;

        // ⌥ドラッグ: 動き始めた今、複製を作ってドラッグ対象を差し替える
        if (regionDrag.duplicateOnDrag)
        {
            if (regionDrag.isMidi)
            {
                MidiRegion copy = sourceTrack.midiRegions[(size_t) regionDrag.item];
                copy.id = project->allocateId();
                for (auto& note : copy.notes)
                    note.id = project->allocateId();
                sourceTrack.midiRegions.push_back (std::move (copy));
                regionDrag.item = (int) sourceTrack.midiRegions.size() - 1;
                regionSelection = { regionDrag.track, regionDrag.item };
            }
            else
            {
                Clip copy = sourceTrack.clips[(size_t) regionDrag.item]; // fileName/audioは共有参照
                copy.id = 0; // 複製は新しい id（reconcile の ensureUniqueIds が採番）
        copy.previewDomain = nullptr;
                copy.previewDomain = nullptr;
                sourceTrack.clips.push_back (std::move (copy));
                regionDrag.item = (int) sourceTrack.clips.size() - 1;
                selection = { regionDrag.track, regionDrag.item };
            }
            if (onSelectionChanged)
                onSelectionChanged();
        }
    }

    // トラック跨ぎ: 有効なレーンに入った時点でvector間を実移動する（描画・選択はモデルに追従。
    // 再生への反映はドロップ時のonModelEditedまで行われない）
    if (trackChanged)
    {
        auto& dest = project->tracks[(size_t) targetRow];
        if (regionDrag.isMidi)
        {
            MidiRegion moving = std::move (sourceTrack.midiRegions[(size_t) regionDrag.item]);
            sourceTrack.midiRegions.erase (sourceTrack.midiRegions.begin() + regionDrag.item);
            dest.midiRegions.push_back (std::move (moving));
            regionDrag.item = (int) dest.midiRegions.size() - 1;
            regionSelection = { targetRow, regionDrag.item };
        }
        else
        {
            Clip moving = std::move (sourceTrack.clips[(size_t) regionDrag.item]);
            sourceTrack.clips.erase (sourceTrack.clips.begin() + regionDrag.item);
            dest.clips.push_back (std::move (moving));
            regionDrag.item = (int) dest.clips.size() - 1;
            selection = { targetRow, regionDrag.item };
        }
        regionDrag.track = targetRow;
        if (onSelectionChanged)
            onSelectionChanged();
    }

    auto& track = project->tracks[(size_t) regionDrag.track];
    if (regionDrag.isMidi)
    {
        auto& region = track.midiRegions[(size_t) regionDrag.item];
        if (regionDrag.mode == RegionDrag::Mode::move)
            region.startPpq = snappedStart;
        else if (regionDrag.mode == RegionDrag::Mode::resize)
            region.lengthPpq = snappedLength;
        else
            region.loopCount = snappedLoopCount;
    }
    else
    {
        auto& clip = track.clips[(size_t) regionDrag.item];
        if (regionDrag.mode == RegionDrag::Mode::loop)
        {
            clip.loopCount = snappedLoopCount;
            // ループを縮めると連なり全長が縮むのでフェードの再クランプが要る。基準は
            // 「ドラッグ開始時に控えた元値」（現在値からだと縮めすぎて戻したときに復元されない）
            clip.fadeInSamples = regionDrag.origFadeInSamples;
            clip.fadeOutSamples = regionDrag.origFadeOutSamples;
            clip.clampFades();
        }
        else if (regionDrag.mode == RegionDrag::Mode::fadeIn)
        {
            // タイムライン差分（実効座標）は chain 変換の逆で原音座標へ戻してから保存する
            // （/ ratio では合わない。clampFades は原音座標のままなので不変条件も保たれる）
            clip.fadeInSamples = clip.sourceFadeFromRendered (draggedFadeIn, false);
            clip.clampFades(); // 保険（全長が別要因で変わっていた場合のみ効く）
        }
        else if (regionDrag.mode == RegionDrag::Mode::fadeOut)
        {
            clip.fadeOutSamples = clip.sourceFadeFromRendered (draggedFadeOut, true);
            clip.clampFades();
        }
        else
        {
            clip.startSample = snappedStart;
        }
    }
    // ループ伸長中もコンテンツ幅を広げる（確定時だけだとハンドルを右へ引き続けられない）
    if (regionDrag.mode == RegionDrag::Mode::loop)
        updateContentSize();
    // フェードはドラッグ中も音へ反映する（「見た目＝出る音」を崩さない）。
    // 通常の onModelEdited は使わない（MIDIが鳴り直すため）
    if (regionDrag.isFadeDrag() && onClipValueEdited)
        onClipValueEdited();
    lanes->repaint();
}

void TimelineView::handleLaneMouseUp (const juce::MouseEvent&)
{
    if (regionDrag.mode != RegionDrag::Mode::none && regionDrag.edited)
    {
        // フェードは値の反映（onClipValueEdited）をドラッグ中に済ませているので、確定では
        // ログだけ残す。onModelEdited を呼ぶと鳴っているMIDIが消音＋再発音される
        if (regionDrag.isFadeDrag())
        {
            juce::int64 samples = -1;
            if (project != nullptr && regionDrag.track >= 0
                && regionDrag.track < (int) project->tracks.size())
            {
                const auto& track = project->tracks[(size_t) regionDrag.track];
                if (regionDrag.item >= 0 && regionDrag.item < (int) track.clips.size())
                {
                    const auto& clip = track.clips[(size_t) regionDrag.item];
                    samples = regionDrag.mode == RegionDrag::Mode::fadeIn ? clip.fadeInSamples
                                                                         : clip.fadeOutSamples;
                }
            }
            Log::info ("region.fade",
                       juce::String (regionDrag.mode == RegionDrag::Mode::fadeIn ? "which=in"
                                                                                : "which=out")
                           + " track=" + juce::String (regionDrag.track)
                           + " item=" + juce::String (regionDrag.item)
                           + " samples=" + juce::String (samples));
            regionDrag = {};
            lanes->repaint(); // ドラッグ中のポップアップを消す
            return;
        }
        if (regionDrag.mode == RegionDrag::Mode::loop)
        {
            int count = -1;
            if (project != nullptr && regionDrag.track >= 0
                && regionDrag.track < (int) project->tracks.size())
            {
                const auto& track = project->tracks[(size_t) regionDrag.track];
                if (regionDrag.isMidi && regionDrag.item < (int) track.midiRegions.size())
                    count = track.midiRegions[(size_t) regionDrag.item].loopCount;
                else if (! regionDrag.isMidi && regionDrag.item < (int) track.clips.size())
                    count = track.clips[(size_t) regionDrag.item].loopCount;
            }
            Log::info ("region.loop", juce::String (regionDrag.isMidi ? "type=midi" : "type=audio")
                                          + " track=" + juce::String (regionDrag.track)
                                          + " item=" + juce::String (regionDrag.item)
                                          + " count=" + juce::String (count));
        }
        else
        {
            Log::info ("region.drag", juce::String (regionDrag.isMidi ? "type=midi" : "type=audio")
                                          + (regionDrag.mode == RegionDrag::Mode::resize ? " mode=resize"
                                                                                         : " mode=move")
                                          + " track=" + juce::String (regionDrag.track)
                                          + " item=" + juce::String (regionDrag.item)
                                          + (regionDrag.duplicateOnDrag ? " dup=1" : ""));
        }
        updateContentSize();
        if (onModelEdited)
            onModelEdited();
    }
    regionDrag = {};
}

void TimelineView::handleLaneDoubleClick (const juce::MouseEvent& e)
{
    if (project == nullptr)
        return;
    const int row = e.y / trackHeight;
    if (row < 0 || row >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) row];
    if (track.type != TrackType::midi)
        return;

    const int ri = hitTestRegion (row, e.x);
    if (ri >= 0)
    {
        if (onOpenRegion)
            onOpenRegion (row, ri); // ピアノロールを開く（Phase 4）
        return;
    }

    // 空エリアのダブルクリック → 1小節のリージョンを作成（小節頭にスナップ）。
    // ガチャの自動作成トラックは「撤去して中止」（onWillEditModel の begin フックが
    // 仮トラックを撤去すると、この関数が保持する track 参照が失効するため）
    if (onPreviewObjectGesture != nullptr && onPreviewObjectGesture (track.id, 0))
    {
        lanes->repaint();
        return;
    }
    if (onWillEditModel)
        onWillEditModel();

    MidiRegion region;
    region.id = project->allocateId();
    region.startPpq = juce::jmax ((juce::int64) 0,
                                  (xToPpq (e.x) / Ppq::ticksPerBar) * Ppq::ticksPerBar);
    region.lengthPpq = Ppq::ticksPerBar;
    track.midiRegions.push_back (std::move (region));

    selection.clear();
    regionSelection = { row, (int) track.midiRegions.size() - 1 };
    updateContentSize();
    if (onSelectionChanged)
        onSelectionChanged();
    if (onModelEdited)
        onModelEdited();
    lanes->repaint();
}

int TimelineView::hitTestRegion (int trackIndex, int x) const
{
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return -1;
    const auto& track = project->tracks[(size_t) trackIndex];
    if (track.type != TrackType::midi)
        return -1;

    for (int ri = (int) track.midiRegions.size() - 1; ri >= 0; --ri)
    {
        const auto& region = track.midiRegions[(size_t) ri];
        const int x0 = ppqToX (region.startPpq);
        const int x1 = ppqToX (region.startPpq + region.totalLengthPpq()); // ループ部分も本体と同じ扱い
        if (x >= x0 && x <= x1)
            return ri;
    }
    return -1;
}

int TimelineView::hitTestClip (int trackIndex, int x) const
{
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return -1;
    const auto& track = project->tracks[(size_t) trackIndex];
    if (track.type != TrackType::audio)
        return -1;

    // 重なりは後勝ち＝後から録ったものを優先
    const auto samplePos = xToSample (x);
    for (int ci = (int) track.clips.size() - 1; ci >= 0; --ci)
    {
        const auto& clip = track.clips[(size_t) ci];
        // ループ部分も本体と同じ扱い（クリックで選択・ドラッグで移動）
        if (samplePos >= clip.startSample && samplePos < clip.startSample + clip.renderedTotalLengthSamples())
            return ci;
    }
    return -1;
}

int TimelineView::hitTestFadeHandle (const Clip& clip, int laneY, juce::Point<int> pos) const
{
    juce::Rectangle<int> fadeInHandle, fadeOutHandle;
    if (! fadeHandleRects (clip, laneY, fadeInHandle, fadeOutHandle))
        return -1;
    return pickFadeHandle (fadeInHandle, fadeOutHandle, pos);
}

void TimelineView::showItemMenu (int trackIndex, int itemIndex)
{
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    const auto& track = project->tracks[(size_t) trackIndex];
    const bool isMidi = track.type == TrackType::midi;
    if (itemIndex < 0 || itemIndex >= (int) (isMidi ? track.midiRegions.size() : track.clips.size()))
        return;
    const bool muted = isMidi ? track.midiRegions[(size_t) itemIndex].muted
                              : track.clips[(size_t) itemIndex].muted;
    const bool looped = isMidi ? track.midiRegions[(size_t) itemIndex].loopCount > 0
                               : track.clips[(size_t) itemIndex].loopCount > 0;

    // 分割は再生ヘッドが対象の内側（境界を除く）にあるときだけ有効
    bool canSplit = false;
    if (isMidi)
    {
        const auto& region = track.midiRegions[(size_t) itemIndex];
        const auto splitPpq = playheadPpq();
        canSplit = splitPpq > region.startPpq && splitPpq < region.startPpq + region.lengthPpq;
    }
    else
    {
        // 判定は見かけ長（実効座標）。レンダリング待ちの間は無効 — 実効 1.0 / 要求 1.5 の
        // 処理中に切ると、landing 後に右の開始位置が旧長のままで重なる
        const auto& clip = track.clips[(size_t) itemIndex];
        const auto playhead = editPositionSample();
        canSplit = playhead > clip.startSample
                && playhead < clip.startSample + clip.renderedLengthSamples()
                && ! clipRenderPending (clip);
    }

    // ショートカット持ちの項目は表記を横に出す（shortcutKeyDescriptionはsetterのない公開フィールド）
    const auto itemWithKey = [] (int id, const juce::String& text, Shortcuts::ID shortcutId,
                                 bool enabled = true)
    {
        juce::PopupMenu::Item item (text);
        item.itemID = id;
        item.isEnabled = enabled;
        item.shortcutKeyDescription = Shortcuts::keyText (shortcutId);
        return item;
    };

    // 実効と要求の不一致（移調・伸縮のレンダリング待ち）の間は長さ依存操作を無効にする
    // （分割・終端直後へ複製。startSample は landing 後に追従しないため重なりが生まれる）
    const bool pendingClip = ! isMidi && clipRenderPending (track.clips[(size_t) itemIndex]);

    juce::PopupMenu menu;
    menu.addItem (itemWithKey (1, muted ? jp (u8"ミュート解除") : jp (u8"ミュート"),
                               Shortcuts::ID::muteRegion));
    // ゲインはオーディオリージョン専用（MIDIリージョンには持たせない）。0dB以外なら現在値を横に出す
    if (! isMidi)
    {
        const auto gainDb = GainScale::toDb (track.clips[(size_t) itemIndex].gain);
        juce::PopupMenu::Item gainItem (jp (u8"ゲイン…"));
        gainItem.itemID = 7;
        if (std::abs (gainDb) >= 0.05)
            gainItem.shortcutKeyDescription = GainScale::text (gainDb);
        menu.addItem (gainItem);

        // 移調・伸縮（オーディオ専用・v20）。既定値以外なら現在値を横に出す（ゲインと同じ流儀）
        const auto& clip = track.clips[(size_t) itemIndex];
        juce::PopupMenu::Item stretchItem (jp (u8"移調・伸縮…"));
        stretchItem.itemID = 9;
        {
            juce::String current;
            if (clip.transposeSemitones != 0)
                current << (clip.transposeSemitones > 0 ? "+" : "")
                        << juce::String (clip.transposeSemitones);
            if (! juce::exactlyEqual (clip.stretchRatio, 1.0))
            {
                if (current.isNotEmpty())
                    current << " / ";
                current << juce::String::fromUTF8 (u8"×") << juce::String (clip.stretchRatio, 2);
            }
            stretchItem.shortcutKeyDescription = current;
        }
        menu.addItem (stretchItem);

        // ピッチ補正（オーディオ専用・v21）。独立ウィンドウのエディタを開く。有効なら「（有効）」を併記
        juce::PopupMenu::Item pitchItem (jp (u8"ピッチ補正…"));
        pitchItem.itemID = 10;
        pitchItem.isEnabled = onPitchEditRequested != nullptr;
        if (clip.pitchCorrection.has_value())
            pitchItem.shortcutKeyDescription = jp (u8"有効");
        menu.addItem (pitchItem);
    }
    menu.addItem (itemWithKey (2, jp (u8"複製"), Shortcuts::ID::repeatItem, ! pendingClip));
    // ループはハンドルのドラッグで作るので「解除」だけメニューに置く（ループ中のみ有効）
    menu.addItem (6, jp (u8"ループ解除"), looped);
    menu.addItem (itemWithKey (4, jp (u8"再生ヘッド位置で分割"), Shortcuts::ID::split,
                               canSplit && ! pendingClip));
    menu.addItem (itemWithKey (5, jp (u8"書き出し…"), Shortcuts::ID::exportRegion));
    // リファレンス分析はオーディオリージョン専用（リージョン範囲がそのままトリムになる）。
    // ツール群（~/daw の Python パイプライン）不在時は無効化し、理由を文言で示す
    if (! isMidi && onAnalyzeItemRequested != nullptr)
    {
        const bool toolsOk = ReferenceTools::analyzeAvailable();
        menu.addItem (8, toolsOk ? jp (u8"リファレンスとして分析…")
                                 : jp (u8"リファレンスとして分析…（~/daw のツールが見つかりません）"),
                      toolsOk);
    }
    menu.addItem (itemWithKey (3, jp (u8"削除"), Shortcuts::ID::deleteItem));

    // コールバックは後から呼ばれるためSafePointerで寿命を確認し、右クリック時点の対象を捕捉して渡す。
    // メニュー表示中はモーダルで他の編集操作が発生せずインデックスは変化しない前提（各操作側でも範囲チェックする）
    juce::Component::SafePointer<TimelineView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options(),
                        [safe, trackIndex, itemIndex] (int result)
                        {
                            if (safe == nullptr)
                                return;
                            if (result == 1)
                                safe->toggleMuteAt (trackIndex, itemIndex);
                            else if (result == 2)
                                safe->duplicateAt (trackIndex, itemIndex);
                            else if (result == 3 && safe->onDeleteItemRequested)
                                safe->onDeleteItemRequested (trackIndex, itemIndex);
                            else if (result == 4)
                                safe->splitAtPlayhead (trackIndex, itemIndex);
                            else if (result == 5 && safe->onExportItemRequested)
                                safe->onExportItemRequested (trackIndex, itemIndex);
                            else if (result == 6)
                                safe->clearLoopAt (trackIndex, itemIndex);
                            else if (result == 7)
                                safe->showClipGainCallout (trackIndex, itemIndex);
                            else if (result == 8 && safe->onAnalyzeItemRequested)
                                safe->onAnalyzeItemRequested (trackIndex, itemIndex);
                            else if (result == 9)
                                safe->showClipStretchCallout (trackIndex, itemIndex);
                            else if (result == 10 && safe->onPitchEditRequested)
                                safe->onPitchEditRequested (trackIndex, itemIndex);
                        });
}

// リージョンゲインの吹き出し。CallOutBox は非同期だがモーダル（enterModalState）なので、
// 表示中にユーザー操作でクリップが動くことはない。位置決めと矢印の向きはJUCEに任せ、
// こちらは「指し示す矩形」だけを渡す
void TimelineView::showClipGainCallout (int trackIndex, int itemIndex)
{
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    if (track.type != TrackType::audio
        || itemIndex < 0 || itemIndex >= (int) track.clips.size())
        return;
    const auto& clip = track.clips[(size_t) itemIndex];

    // クリック対象の範囲をコンテンツ座標で求め、this のローカル座標へ変換する。
    // ループの反復部分も右クリックできるので、範囲はループ終端まで含める
    // （本体だけにすると「本体は画面外・反復だけ表示中」で交差が空になり、吹き出しが的を外す）
    const int x = sampleToX (clip.startSample);
    const int right = sampleToX (clip.startSample + clip.renderedTotalLengthSamples());
    const auto areaInLanes = juce::Rectangle<int> (x, trackIndex * trackHeight + 4,
                                                  juce::jmax (2, right - x), trackHeight - 8);
    // JUCEは渡した矩形の中央と各辺を矢印の候補にするため、長いリージョンや横スクロールで
    // 大半が画面外だと吹き出しが画面外を指してしまう。見えている部分（viewportとの交差）を渡す
    auto area = getLocalArea (lanes.get(), areaInLanes).getIntersection (viewport->getBounds());
    if (area.isEmpty()) // 念のため（右クリックできた時点でどこかは見えている）
        area = viewport->getBounds();

    juce::Component::SafePointer<TimelineView> safe (this);
    auto panel = std::make_unique<RegionGainPanel> (
        clip.gain,
        [safe] { if (safe != nullptr && safe->onWillEditClipValue) safe->onWillEditClipValue(); },
        [safe, trackIndex, itemIndex] (float gain)
        { if (safe != nullptr) safe->applyClipGain (trackIndex, itemIndex, gain); });

    gainCallout = &juce::CallOutBox::launchAsynchronously (std::move (panel), area, this);
}

void TimelineView::applyClipGain (int trackIndex, int itemIndex, float gain)
{
    // 吹き出し表示中の非同期な構造変更（録音終了など）に備えて、適用のたびに範囲を再検証する
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    if (track.type != TrackType::audio
        || itemIndex < 0 || itemIndex >= (int) track.clips.size())
        return;

    track.clips[(size_t) itemIndex].gain = GainScale::clampLinear (gain);
    if (onClipValueEdited)
        onClipValueEdited();
    lanes->repaint(); // 波形の振幅とバッジを追従させる（ドラッグ中もここを通る）
}

// 移調・伸縮の吹き出し（ゲインと同じ流儀・CallOutBox は非同期だがモーダル）
void TimelineView::showClipStretchCallout (int trackIndex, int itemIndex)
{
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    if (track.type != TrackType::audio
        || itemIndex < 0 || itemIndex >= (int) track.clips.size())
        return;
    const auto& clip = track.clips[(size_t) itemIndex];

    const int x = sampleToX (clip.startSample);
    const int right = sampleToX (clip.startSample + clip.renderedTotalLengthSamples());
    const auto areaInLanes = juce::Rectangle<int> (x, trackIndex * trackHeight + 4,
                                                  juce::jmax (2, right - x), trackHeight - 8);
    auto area = getLocalArea (lanes.get(), areaInLanes).getIntersection (viewport->getBounds());
    if (area.isEmpty())
        area = viewport->getBounds();

    juce::Component::SafePointer<TimelineView> safe (this);
    auto panel = std::make_unique<RegionTransposeStretchPanel> (
        clip, barLengthSamples(),
        [safe] { if (safe != nullptr && safe->onWillEditClipValue) safe->onWillEditClipValue(); },
        [safe, trackIndex, itemIndex] (int semitones, double ratio)
        { if (safe != nullptr) safe->applyClipStretch (trackIndex, itemIndex, semitones, ratio); });

    stretchCallout = &juce::CallOutBox::launchAsynchronously (std::move (panel), area, this);
}

void TimelineView::applyClipStretch (int trackIndex, int itemIndex, int semitones, double ratio)
{
    // 吹き出し表示中の非同期な構造変更（録音終了など）に備えて、適用のたびに範囲を再検証する
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    if (track.type != TrackType::audio
        || itemIndex < 0 || itemIndex >= (int) track.clips.size())
        return;
    auto& clip = track.clips[(size_t) itemIndex];

    // 受付の判断（クランプ・view長ガード・ドメインのリセット）はモデル側（ClipDomains）
    if (! ClipDomains::applyStretchRequest (clip, semitones, ratio))
        return;
    Log::info ("clip_stretch.request", "track=" + juce::String (trackIndex)
                                           + " item=" + juce::String (itemIndex)
                                           + " semitones=" + juce::String (semitones)
                                           + " ratio=" + juce::String (ratio, 4));
    if (onClipStretchEdited)
        onClipStretchEdited(); // dirty 化＋RenderCache の debounce 同期（MainComponent）
    lanes->repaint(); // バッジ・メニュー併記の即時反映（音と長さはレンダー完了時に切り替わる）
}

bool TimelineView::clipRenderPending (const Clip& clip) const
{
    return clip.renderPending (effectiveSampleRate());
}

void TimelineView::dismissGainCallout()
{
    if (gainCallout != nullptr)
        gainCallout->dismiss();
    gainCallout = nullptr;
    if (stretchCallout != nullptr)
        stretchCallout->dismiss();
    stretchCallout = nullptr;
}

void TimelineView::toggleMuteAt (int trackIndex, int itemIndex)
{
    if (canEdit != nullptr && ! canEdit())
        return; // 録音中（キー経由と右クリック経由で挙動を揃える）
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    const bool isMidi = track.type == TrackType::midi;
    if (itemIndex < 0 || itemIndex >= (int) (isMidi ? track.midiRegions.size() : track.clips.size()))
        return;

    if (onWillEditModel)
        onWillEditModel();
    bool& muted = isMidi ? track.midiRegions[(size_t) itemIndex].muted
                         : track.clips[(size_t) itemIndex].muted;
    muted = ! muted;
    Log::info ("region.mute", "track=" + juce::String (trackIndex)
                                  + " item=" + juce::String (itemIndex)
                                  + " muted=" + (muted ? "1" : "0"));
    if (onModelEdited)
        onModelEdited();
    lanes->repaint();
}

void TimelineView::clearLoopAt (int trackIndex, int itemIndex)
{
    if (canEdit != nullptr && ! canEdit())
        return; // 録音中
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    const bool isMidi = track.type == TrackType::midi;
    if (itemIndex < 0 || itemIndex >= (int) (isMidi ? track.midiRegions.size() : track.clips.size()))
        return;

    int& loopCount = isMidi ? track.midiRegions[(size_t) itemIndex].loopCount
                            : track.clips[(size_t) itemIndex].loopCount;
    if (loopCount == 0)
        return;

    if (onWillEditModel)
        onWillEditModel();
    loopCount = 0;
    // 連なり全長が縮むのでフェードの再クランプが要る（ループのドラッグと同じ規則）
    if (! isMidi)
        track.clips[(size_t) itemIndex].clampFades();
    Log::info ("region.loop", juce::String (isMidi ? "type=midi" : "type=audio")
                                  + " track=" + juce::String (trackIndex)
                                  + " item=" + juce::String (itemIndex) + " count=0");
    updateContentSize();
    if (onModelEdited)
        onModelEdited();
    lanes->repaint();
}

juce::Rectangle<int> TimelineView::loopHandleRect (int itemX, int itemRight, int laneY) const
{
    // 短いアイテムでははみ出さないよう幅を詰める（最低8px。それ未満ならアイテム幅どおり）
    const int w = juce::jmin (loopHandleWidth, juce::jmax (8, itemRight - itemX));
    return { itemRight - w, laneY + 4 + (trackHeight - 8) - loopHandleHeight, w, loopHandleHeight };
}

bool TimelineView::fadeHandleRects (const Clip& clip, int laneY,
                                   juce::Rectangle<int>& fadeInHandle,
                                   juce::Rectangle<int>& fadeOutHandle) const
{
    const int x = sampleToX (clip.startSample);
    const int right = sampleToX (clip.startSample + clip.renderedTotalLengthSamples());
    if (right - x < fadeHandleMinWidth)
        return false; // ハンドル2つ＋移動用の余白が取れない幅では出さない

    // ハンドル位置は実効（見かけ）座標。フェード長は chain 変換で実効へ写す
    fadeInHandle = fadeHandleRectAt (x, right, laneY,
                                     sampleToX (clip.startSample + clip.renderedFadeIn()));
    fadeOutHandle = fadeHandleRectAt (x, right, laneY,
                                      sampleToX (clip.startSample + clip.renderedTotalLengthSamples()
                                                 - clip.renderedFadeOut()));
    return true;
}

void TimelineView::selectItem (int trackIndex, int itemIndex, bool isMidi)
{
    if (isMidi)
    {
        selection.clear();
        regionSelection = { trackIndex, itemIndex };
    }
    else
    {
        selection = { trackIndex, itemIndex };
        regionSelection.clear();
    }
    if (onSelectionChanged)
        onSelectionChanged();
    lanes->repaint();
}

void TimelineView::duplicateAt (int trackIndex, int itemIndex)
{
    if (canEdit != nullptr && ! canEdit())
        return; // 録音中（キー経由と右クリック経由で挙動を揃える）
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];

    if (track.type == TrackType::midi)
    {
        if (itemIndex < 0 || itemIndex >= (int) track.midiRegions.size())
            return;
        if (onWillEditModel)
            onWillEditModel();
        MidiRegion copy = track.midiRegions[(size_t) itemIndex];
        copy.id = project->allocateId();
        for (auto& note : copy.notes)
            note.id = project->allocateId();
        // 元の終端直後（Logicのリピート相当）。ループ中はループ終端の直後＝重ならない位置へ
        copy.startPpq += copy.totalLengthPpq();
        track.midiRegions.push_back (std::move (copy));
        selection.clear();
        regionSelection = { trackIndex, (int) track.midiRegions.size() - 1 };
    }
    else
    {
        if (itemIndex < 0 || itemIndex >= (int) track.clips.size())
            return;
        if (clipRenderPending (track.clips[(size_t) itemIndex]))
            return; // レンダリング待ち: 「終端直後」が旧長のままで重なりを作るため無効
        if (onWillEditModel)
            onWillEditModel();
        Clip copy = track.clips[(size_t) itemIndex]; // fileName/audio/activeDomainは共有参照
        copy.id = 0; // 複製は新しい id（reconcile の ensureUniqueIds が採番）
        copy.previewDomain = nullptr;
        // 元の終端直後（Logicのリピート相当）。ループ中はループ終端の直後＝重ならない位置へ
        copy.startSample += copy.renderedTotalLengthSamples();
        track.clips.push_back (std::move (copy));
        selection = { trackIndex, (int) track.clips.size() - 1 };
        regionSelection.clear();
    }

    Log::info ("region.duplicate", "track=" + juce::String (trackIndex)
                                       + " item=" + juce::String (itemIndex));
    updateContentSize();
    if (onSelectionChanged)
        onSelectionChanged();
    if (onModelEdited)
        onModelEdited();
    lanes->repaint();
}

void TimelineView::splitAtPlayhead (int trackIndex, int itemIndex)
{
    if (canEdit != nullptr && ! canEdit())
        return; // 録音中（キー経由と右クリック経由で挙動を揃える）
    if (project == nullptr || trackIndex < 0 || trackIndex >= (int) project->tracks.size())
        return;
    auto& track = project->tracks[(size_t) trackIndex];
    const auto playhead = editPositionSample();

    // 左は元のindexを上書き、右は末尾に追加する（既存indexが動かないので選択の維持が単純になる。
    // 左右は重ならないため描画順が変わっても見た目に影響しない。duplicateAtと同じ方針）
    if (track.type == TrackType::midi)
    {
        if (itemIndex < 0 || itemIndex >= (int) track.midiRegions.size())
            return;
        MidiRegion left, right;
        if (! splitMidiRegion (track.midiRegions[(size_t) itemIndex], playheadPpq(), left, right))
            return;
        if (onWillEditModel)
            onWillEditModel();
        right.id = project->allocateId();
        // 分割はループを解除する（左右どちらに繰り返しを引き継ぐか自明でないため）
        left.loopCount = right.loopCount = 0;
        track.midiRegions[(size_t) itemIndex] = std::move (left);
        track.midiRegions.push_back (std::move (right));
        selection.clear();
        regionSelection = { trackIndex, itemIndex }; // 左側を選択したまま
    }
    else
    {
        if (itemIndex < 0 || itemIndex >= (int) track.clips.size())
            return;
        if (clipRenderPending (track.clips[(size_t) itemIndex]))
            return; // レンダリング待ち: 実効 1.0 / 要求 1.5 の処理中に切ると landing 後に重なる
        Clip left, right;
        if (! splitClip (track.clips[(size_t) itemIndex], playhead, left, right))
            return;
        if (onWillEditModel)
            onWillEditModel();
        // ループ解除（MIDIと同じ規則）は splitClip 側で済んでいる。ここで解除すると
        // フェードのクランプが解除前の全長で行われ、不変条件が破れる
        track.clips[(size_t) itemIndex] = std::move (left);
        track.clips.push_back (std::move (right));
        selection = { trackIndex, itemIndex };
        regionSelection.clear();
    }

    Log::info ("region.split", "track=" + juce::String (trackIndex)
                                   + " item=" + juce::String (itemIndex)
                                   + " pos=" + juce::String (playhead));
    if (onSelectionChanged)
        onSelectionChanged();
    if (onModelEdited)
        onModelEdited();
    lanes->repaint();
}

juce::int64 TimelineView::playheadPpq() const
{
    return (juce::int64) std::llround ((double) editPositionSample() / samplesPerTick());
}

// 編集の基準位置。保留中のシークを含む論理位置を 0 以上に丸めたもの（分割・貼り付け用）
juce::int64 TimelineView::editPositionSample() const
{
    return juce::jmax ((juce::int64) 0, transport.uiPositionSample());
}

void TimelineView::setPlayStartSample (juce::int64 samplePos)
{
    if (playStartSample == samplePos)
        return;

    playStartSample = samplePos;
    ruler->repaint(); // マーカーはルーラーにしか描かない
}

void TimelineView::seekFromX (int x)
{
    // 表示中の最小グリッド単位にスナップ（ズームが深いほど細かく移動できる）
    const double gridLen = barLengthSamples() / gridDivisionsPerBar();
    const auto samplePos = juce::jmax ((juce::int64) 0, xToSample (x));
    const auto gridIndex = (juce::int64) std::floor ((double) samplePos / gridLen);
    if (onSeek)
        onSeek ((juce::int64) std::llround ((double) gridIndex * gridLen));
}

// ---- サイクル範囲（ルーラーの操作） -------------------------------------

int TimelineView::hitTestCycleEdge (int x) const
{
    if (project == nullptr || ! project->hasCycleRange())
        return -1;

    const int d0 = std::abs (x - sixteenthToX (project->cycleStartSixteenths));
    const int d1 = std::abs (x - sixteenthToX (project->cycleEndSixteenths));
    if (d0 <= 4 && d0 <= d1)
        return 0;
    if (d1 <= 4)
        return 1;
    return -1;
}

int TimelineView::sampleToSnappedSixteenths (juce::int64 samplePos) const
{
    // xToCycleSixteenths と同じ規則（表示中の最小グリッドへ最近傍・1/16上限）を、
    // ピクセルではなくサンプル位置に適用する
    const int unit = 16 / gridDivisionsPerBar();
    const double unitSamples = barLengthSamples() / 16.0 * unit;
    if (unitSamples <= 0.0)
        return 0;
    return juce::jmax (0, (int) std::llround ((double) samplePos / unitSamples)) * unit;
}

int TimelineView::hitTestFadeEdge (int x) const
{
    if (project == nullptr || ! project->hasFadeOut())
        return -1;

    const int d0 = std::abs (x - sixteenthToX (project->fadeOutStartSixteenths));
    const int d1 = std::abs (x - sixteenthToX (project->fadeOutEndSixteenths));
    if (d0 <= 4 && d0 <= d1)
        return 0;
    if (d1 <= 4)
        return 1;
    return -1;
}

void TimelineView::drawSongFadeShade (juce::Graphics& g, const juce::Rectangle<int>& clipBounds,
                                      int laneHeight) const
{
    if (project == nullptr || ! project->hasFadeOut() || laneHeight <= 0)
        return;

    // レーン側はカーブを描かない（高さがトラック数に比例するため、下端＝無音の位置が画面外へ
    // 出て「まだ落ちていない」と誤読する）。ここは「この区間は音量が落ちている」ことを
    // 一様な濃さで示すだけにして、カーブは固定高さのフェード帯に読ませる
    const int x0 = sixteenthToX (project->fadeOutStartSixteenths);
    const int x1 = sixteenthToX (project->fadeOutEndSixteenths);
    const float h = (float) laneHeight;

    const int from = juce::jmax (x0, clipBounds.getX());
    if (from < clipBounds.getRight())
    {
        g.setColour (Theme::songFadeShade);
        g.fillRect ((float) from, 0.0f, (float) (clipBounds.getRight() - from), h);
    }

    // 終端の縦線。「ここから先は鳴らない」を縦スクロール位置によらず確定させる
    if (x1 >= clipBounds.getX() - 1 && x1 <= clipBounds.getRight() + 1)
    {
        g.setColour (Theme::songFadeHandle.withAlpha (0.9f));
        g.fillRect ((float) x1 - 1.0f, 0.0f, 2.0f, h);
    }
}

void TimelineView::handleRulerMouseDown (const juce::MouseEvent& e)
{
    cycleDrag = {};
    if (project == nullptr)
        return;
    if (e.mods.isPopupMenu())
    {
        showRulerMenu (e.x);
        return;
    }

    cycleDrag.startX = e.x;
    cycleDrag.origStart = project->cycleStartSixteenths;
    cycleDrag.origEnd = project->cycleEndSixteenths;

    // 判定順は「サイクル端 → フェード端 → サイクル範囲内移動 → サイクル新規作成」。
    // サイクルを先に見るのは使用頻度が高いため（重なったときはサイクルが勝つ）。
    // どれもドラッグが動くまで何もせず、動かさず離したらシーク（mouseUpで判定）
    const int edge = hitTestCycleEdge (e.x);
    const int fadeEdge = edge >= 0 ? -1 : hitTestFadeEdge (e.x);
    if (edge == 0)
    {
        cycleDrag.mode = CycleDrag::Mode::resizeStart;
    }
    else if (edge == 1)
    {
        cycleDrag.mode = CycleDrag::Mode::resizeEnd;
    }
    else if (fadeEdge >= 0)
    {
        cycleDrag.mode = fadeEdge == 0 ? CycleDrag::Mode::fadeStart : CycleDrag::Mode::fadeEnd;
        cycleDrag.origStart = project->fadeOutStartSixteenths;
        cycleDrag.origEnd = project->fadeOutEndSixteenths;
    }
    else if (project->hasCycleRange()
             && e.x > sixteenthToX (project->cycleStartSixteenths)
             && e.x < sixteenthToX (project->cycleEndSixteenths))
    {
        cycleDrag.mode = CycleDrag::Mode::moveRange;
    }
    else
    {
        cycleDrag.mode = CycleDrag::Mode::create;
        cycleDrag.anchorSixteenths = xToCycleSixteenths (e.x);
    }
}

void TimelineView::handleRulerMouseDrag (const juce::MouseEvent& e)
{
    if (project == nullptr || cycleDrag.mode == CycleDrag::Mode::none)
        return;
    if (! cycleDrag.edited && e.getDistanceFromDragStart() < 4)
        return;

    // 曲末フェードの端。undo対象なので、値を動かす前に一度だけスナップショットを積む
    if (cycleDrag.isFade())
    {
        const int at = xToCycleSixteenths (e.x);
        int newStart = cycleDrag.origStart;
        int newEnd = cycleDrag.origEnd;
        if (cycleDrag.mode == CycleDrag::Mode::fadeStart)
            newStart = juce::jmin (at, cycleDrag.origEnd);
        else
            newEnd = juce::jmax (at, cycleDrag.origStart);

        if (newStart == project->fadeOutStartSixteenths && newEnd == project->fadeOutEndSixteenths)
            return;

        if (! cycleDrag.edited && onWillEditClipValue)
            onWillEditClipValue(); // 1操作＝1件（ドラッグ開始で1回だけ）
        cycleDrag.edited = true;
        project->fadeOutStartSixteenths = newStart;
        project->fadeOutEndSixteenths = newEnd;
        // refresh() を通す: フェード帯は独立コンポーネントなので個別に repaint しないと
        // カーブが古いまま残る。潰し切って hasFadeOut() が false になったときの
        // 帯の消滅（＝レイアウト再構成）もここが検出する
        refresh();
        if (onClipValueEdited)
            onClipValueEdited(); // ドラッグ中も音へ反映（MIDI世代は据え置き）
        return;
    }

    int newStart = cycleDrag.origStart;
    int newEnd = cycleDrag.origEnd;
    switch (cycleDrag.mode)
    {
        case CycleDrag::Mode::create:
        {
            const int at = xToCycleSixteenths (e.x);
            newStart = juce::jmin (cycleDrag.anchorSixteenths, at);
            newEnd = juce::jmax (cycleDrag.anchorSixteenths, at);
            break;
        }
        case CycleDrag::Mode::moveRange:
        {
            const int unit = 16 / gridDivisionsPerBar();
            const double unitPx = pxPerBar / 16.0 * unit;
            const int delta = (int) std::llround ((double) (e.x - cycleDrag.startX) / unitPx) * unit;
            newStart = juce::jmax (0, cycleDrag.origStart + delta);
            newEnd = newStart + (cycleDrag.origEnd - cycleDrag.origStart); // 左端クランプ後も長さ維持
            break;
        }
        case CycleDrag::Mode::resizeStart:
            newStart = juce::jmin (xToCycleSixteenths (e.x), cycleDrag.origEnd);
            break;
        case CycleDrag::Mode::resizeEnd:
            newEnd = juce::jmax (xToCycleSixteenths (e.x), cycleDrag.origStart);
            break;
        case CycleDrag::Mode::none:
            return;
    }

    if (newStart == project->cycleStartSixteenths && newEnd == project->cycleEndSixteenths)
        return;

    cycleDrag.edited = true;
    project->cycleStartSixteenths = newStart;
    project->cycleEndSixteenths = newEnd;
    if (cycleDrag.mode == CycleDrag::Mode::create && project->hasCycleRange())
        project->cycleEnabled = true; // 範囲を描いたら自動でON（Logicと同じ）

    ruler->repaint();
    if (onCycleChanged)
        onCycleChanged(); // ドラッグ中も即時反映（再生中ならループ範囲がライブで変わる）
}

void TimelineView::handleRulerMouseUp (const juce::MouseEvent& e)
{
    if (project == nullptr)
    {
        cycleDrag = {};
        return;
    }

    if (cycleDrag.isFade())
    {
        if (cycleDrag.edited)
        {
            // 幅ゼロまで潰した＝フェード削除
            if (! project->hasFadeOut())
            {
                project->fadeOutStartSixteenths = 0;
                project->fadeOutEndSixteenths = 0;
                Log::info ("song_fade.clear", "reason=drag_collapsed");
            }
            else
            {
                Log::info ("song_fade.range", "start=" + juce::String (project->fadeOutStartSixteenths)
                                                  + " end=" + juce::String (project->fadeOutEndSixteenths));
            }
            refresh(); // updateContentSize＋全再描画＋帯の出入り検出（潰し切りで解除したときに要る）
            if (onClipValueEdited)
                onClipValueEdited();
        }
        else
        {
            seekFromX (e.x); // 動かさず離した＝クリック → シーク（サイクルと同じ流儀）
        }
        cycleDrag = {};
        return;
    }

    if (cycleDrag.mode != CycleDrag::Mode::none && cycleDrag.edited)
    {
        if (! project->hasCycleRange())
        {
            // 幅ゼロまで潰した＝範囲削除（OFFへ戻す）
            project->cycleStartSixteenths = 0;
            project->cycleEndSixteenths = 0;
            project->cycleEnabled = false;
            Log::info ("cycle.clear");
        }
        else
        {
            Log::info ("cycle.range", "start=" + juce::String (project->cycleStartSixteenths)
                                          + " end=" + juce::String (project->cycleEndSixteenths)
                                          + " enabled=" + juce::String ((int) project->cycleEnabled));
        }
        updateContentSize();
        ruler->repaint();
        if (onCycleChanged)
            onCycleChanged();
    }
    else if (cycleDrag.mode != CycleDrag::Mode::none)
    {
        seekFromX (e.x); // 動かさず離した＝クリック → 従来どおりシーク
    }
    cycleDrag = {};
}

void TimelineView::handleRulerMouseMove (const juce::MouseEvent& e)
{
    ruler->setMouseCursor (hitTestCycleEdge (e.x) >= 0 || hitTestFadeEdge (e.x) >= 0
                               ? juce::MouseCursor::LeftRightResizeCursor
                               : juce::MouseCursor::NormalCursor);
}

void TimelineView::showRulerMenu (int x)
{
    if (project == nullptr)
        return;

    // 開始点は再生ヘッドではなく右クリックした位置。⌃Fと同じ規則でグリッドへ丸める
    const int atSixteenths = xToCycleSixteenths (x);

    juce::PopupMenu menu;
    menu.addItem (1, jp (u8"ここから曲末までフェードアウト")
                         + " (" + Shortcuts::keyText (Shortcuts::ID::songFadeOut) + ")");
    menu.addItem (2, jp (u8"フェードアウトを解除"), project->hasFadeOut());

    // ルーラーは横スクロールするコンテンツ（原点がスクロール量ぶん画面外の左にあり、幅は全小節ぶん）
    // なので withTargetComponent を使ってはいけない — メニューがウィンドウの遥か左に出る。
    // 素の Options() ＝マウス位置（他のタイムライン内メニューと同じ流儀）
    menu.showMenuAsync (juce::PopupMenu::Options(),
                        [this, atSixteenths] (int result)
                        {
                            if (result == 1 && onSongFadeRequested)
                                onSongFadeRequested (atSixteenths);
                            else if (result == 2 && onSongFadeClearRequested)
                                onSongFadeClearRequested();
                        });
}

// ---- セクションマーカー -------------------------------------------------

int TimelineView::hitTestMarker (int x) const
{
    if (project == nullptr)
        return -1;

    // 区間 = 自分の開始x〜次のマーカーの開始x。最初のマーカーより前は無ラベル（-1）
    int found = -1;
    for (int i = 0; i < (int) project->markers.size(); ++i)
    {
        if (beatToX (project->markers[(size_t) i].startBeats) > x)
            break;
        found = i;
    }
    return found;
}

int TimelineView::hitTestMarkerEdge (int x) const
{
    if (project == nullptr)
        return -1;

    for (int i = 0; i < (int) project->markers.size(); ++i)
        if (std::abs (x - beatToX (project->markers[(size_t) i].startBeats)) <= 4)
            return i;
    return -1;
}

void TimelineView::handleMarkerLaneMouseDown (const juce::MouseEvent& e)
{
    markerDrag = {};
    if (project == nullptr)
        return;

    const int hit = hitTestMarker (e.x);
    if (e.mods.isPopupMenu())
    {
        if (hit >= 0)
            showMarkerMenu (hit, xToMarkerBeats (e.x));
        else
            showAddMarkerMenu (xToMarkerBeats (e.x));
        return;
    }

    // 境界・本体どちらを掴んでもドラッグで開始位置を移動できる。
    // 動かさず離したときだけシーク（mouseUpで判定）。空白は追加メニュー
    const int edge = hitTestMarkerEdge (e.x);
    const int target = edge >= 0 ? edge : hit;
    if (target >= 0)
    {
        markerDrag.index = target;
        markerDrag.origStartBeats = project->markers[(size_t) target].startBeats;
        markerDrag.startX = e.x;
        markerDrag.fromEdge = edge >= 0;
        return;
    }
    showAddMarkerMenu (xToMarkerBeats (e.x));
}

void TimelineView::handleMarkerLaneMouseDrag (const juce::MouseEvent& e)
{
    if (project == nullptr || markerDrag.index < 0
        || markerDrag.index >= (int) project->markers.size())
        return;

    // 境界掴みは最近傍のスナップ位置へ吸着、本体掴みは掴んだ位置からの相対移動。
    // 刻みは表示グリッド準拠（上限=拍）。どちらも隣のマーカーを越えない範囲にクランプ
    const int unit = snapUnitBeats();
    const double unitPx = pxPerBar / 4.0 * unit;
    const int wantedBeats = markerDrag.fromEdge
                                ? juce::jmax (0, (int) std::llround ((double) e.x / unitPx)) * unit
                                : markerDrag.origStartBeats
                                      + (int) std::llround ((double) (e.x - markerDrag.startX) / unitPx) * unit;
    const int targetBeats = SectionMarkers::clampStartBeats (project->markers, markerDrag.index, wantedBeats);

    auto& marker = project->markers[(size_t) markerDrag.index];
    if (targetBeats == marker.startBeats)
        return;

    if (! markerDrag.edited)
    {
        if (onWillEditModel)
            onWillEditModel();
        markerDrag.edited = true;
    }
    marker.startBeats = targetBeats;
    markerLane->repaint();
}

void TimelineView::handleMarkerLaneMouseUp (const juce::MouseEvent&)
{
    if (project != nullptr && markerDrag.index >= 0
        && markerDrag.index < (int) project->markers.size())
    {
        if (markerDrag.edited)
        {
            const auto& moved = project->markers[(size_t) markerDrag.index];
            Log::info ("marker.move", "index=" + juce::String (markerDrag.index)
                                          + " bar=" + juce::String (moved.bar())
                                          + " beat=" + juce::String (moved.beat()));
            updateContentSize();
            if (onModelEdited)
                onModelEdited();
        }
        else
        {
            // 動かさず離した＝クリック → セクション頭へシーク（「hookの頭からもう一回」の動線）。
            // 境界クリックも「境界から始まるセクション」の頭へのシークとして扱う。
            // 録音中のガードはonSeek側（MainComponent）が行う
            if (onSeek)
                onSeek (beatStartSample (project->markers[(size_t) markerDrag.index].startBeats));
        }
    }
    markerDrag = {};
}

void TimelineView::handleMarkerLaneMouseMove (const juce::MouseEvent& e)
{
    markerLane->setMouseCursor (hitTestMarkerEdge (e.x) >= 0
                                    ? juce::MouseCursor::LeftRightResizeCursor
                                    : juce::MouseCursor::NormalCursor);
}

void TimelineView::showAddMarkerMenu (int beats)
{
    if (project == nullptr || beats < 0)
        return;

    juce::PopupMenu menu;
    for (int i = 0; i < (int) std::size (SectionMarkers::allTypes); ++i)
        menu.addItem (i + 1, SectionMarkers::typeName (SectionMarkers::allTypes[i]));

    // コールバックは後から呼ばれるためSafePointerで寿命を確認する（showItemMenuと同じ方針）
    juce::Component::SafePointer<TimelineView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options(),
                        [safe, beats] (int result)
                        {
                            if (safe != nullptr && result > 0)
                                safe->addMarkerAt (beats, SectionMarkers::allTypes[result - 1]);
                        });
}

void TimelineView::showMarkerMenu (int markerIndex, int clickedBeats)
{
    if (project == nullptr || markerIndex < 0 || markerIndex >= (int) project->markers.size())
        return;

    const auto currentType = project->markers[(size_t) markerIndex].type;
    juce::PopupMenu addSub, typeSub;
    for (int i = 0; i < (int) std::size (SectionMarkers::allTypes); ++i)
    {
        const auto name = SectionMarkers::typeName (SectionMarkers::allTypes[i]);
        addSub.addItem (100 + i, name);
        typeSub.addItem (200 + i, name, true, SectionMarkers::allTypes[i] == currentType);
    }

    // 「ここに追加」= クリック位置の小節頭に境界を1本立てる（既存セクションの分割になる）
    juce::PopupMenu menu;
    menu.addSubMenu (jp (u8"ここにセクションを追加"), addSub);
    menu.addSubMenu (jp (u8"種別を変更"), typeSub);
    menu.addItem (1, jp (u8"削除"));

    juce::Component::SafePointer<TimelineView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options(),
                        [safe, markerIndex, clickedBeats] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            if (result == 1)
                                safe->removeMarker (markerIndex);
                            else if (result >= 200)
                                safe->changeMarkerType (markerIndex,
                                                        SectionMarkers::allTypes[result - 200]);
                            else if (result >= 100)
                                safe->addMarkerAt (clickedBeats,
                                                   SectionMarkers::allTypes[result - 100]);
                        });
}

void TimelineView::addMarkerAt (int beats, SectionType type)
{
    if (project == nullptr || beats < 0)
        return;
    // 同一位置への同種別は完全な変化なし → undo履歴を積まない
    for (const auto& marker : project->markers)
        if (marker.startBeats == beats && marker.type == type)
            return;

    if (onWillEditModel)
        onWillEditModel();
    SectionMarkers::set (project->markers, beats, type);
    Log::info ("marker.add", "bar=" + juce::String (beats / 4 + 1)
                                 + " beat=" + juce::String (beats % 4)
                                 + " type=" + SectionMarkers::typeName (type));
    updateContentSize();
    if (onModelEdited)
        onModelEdited();
    markerLane->repaint();
}

void TimelineView::changeMarkerType (int index, SectionType type)
{
    if (project == nullptr || index < 0 || index >= (int) project->markers.size())
        return;
    if (project->markers[(size_t) index].type == type)
        return;

    if (onWillEditModel)
        onWillEditModel();
    project->markers[(size_t) index].type = type;
    Log::info ("marker.type", "index=" + juce::String (index)
                                  + " type=" + SectionMarkers::typeName (type));
    if (onModelEdited)
        onModelEdited();
    markerLane->repaint();
}

void TimelineView::removeMarker (int index)
{
    if (project == nullptr || index < 0 || index >= (int) project->markers.size())
        return;

    if (onWillEditModel)
        onWillEditModel();
    Log::info ("marker.remove", "index=" + juce::String (index)
                                    + " bar=" + juce::String (project->markers[(size_t) index].bar())
                                    + " beat=" + juce::String (project->markers[(size_t) index].beat()));
    SectionMarkers::removeAt (project->markers, index);
    updateContentSize();
    if (onModelEdited)
        onModelEdited();
    markerLane->repaint();
}
