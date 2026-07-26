#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"
#include "../shared/GainScale.h"

// 素材トリム（GAIN）の共通コントロール。使い手は2つで、値域・刻み・吸着・リセットの作法を揃える:
//   - サンプル音源のGAIN（InstrumentDetailView）
//   - オーディオリージョンのゲイン（TimelineView の吹き出し）
// スケール定義（±12dB・0dB吸着・表示文字列）は GainScale が持つ。

// 値の意味はdB（モデル側は線形倍率）。ドラッグ中だけ0dB付近へ吸着させたいので snapValue を差し替える。
// プログラムからの同期や数値クリック／ダブルクリックでのリセットは既にちょうど0dBを渡してくるので
// 吸着させない
class GainSlider : public juce::Slider
{
public:
    GainSlider()
    {
        // ホイールでの値変更は無効にする。undoの区切り（onNewClickSequence）は mouseDown 基準で、
        // ホイールは mouseDown を伴わないため「ドラッグ後にホイールで微調整すると⌘Zがドラッグ前まで
        // 戻る」という不整合になる。GAINは頻繁に触る場所ではなく、ドラッグ＋0dBリセットで足りるので、
        // 区切りを増やすより経路を1本に絞る（パネル上でスクロールして意図せず音量が変わる事故も防げる）
        setScrollWheelEnabled (false);
    }

    double snapValue (double attemptedValue, DragMode dragMode) override
    {
        return GainScale::snapDb (attemptedValue, dragMode != notDragging);
    }

    // 新しいクリック列（＝ユーザーから見た1操作）の始まり。undoを「1操作=1件」にするために使う。
    // JUCEのSliderは mouseDown ごとに ScopedDragNotification を作り（juce_Slider.cpp:900）、
    // ダブルクリック確定時にも別の通知を出す（同:1121）ので、onDragStart では
    // 「ダブルクリックの2回目」を区別できない。つまみ以外をダブルクリックしたとき、
    // 1クリック目のジャンプと0dBリセットが別々のundoになると、最初の⌘Zが中間値へ戻ってしまう
    std::function<void()> onNewClickSequence;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.getNumberOfClicks() <= 1 && onNewClickSequence != nullptr)
            onNewClickSequence();
        juce::Slider::mouseDown (e);
    }
};

// GAINの現在値表示。リセット専用ボタンを増やさずに済むよう、この表示自体が
// 「0 dBに戻す」ボタンを兼ねる（ホバーで薄い枠を出して押せることを示す）。
// GAINは頻繁に触る場所ではないので、ダブルクリックだけだと操作を忘れて戻せなくなる
class GainValueLabel : public juce::Component,
                       public juce::SettableTooltipClient
{
public:
    GainValueLabel()
    {
        setTooltip (juce::String::fromUTF8 (u8"クリックで 0 dB に戻す"));
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus (false);
    }

    void setDb (double db)
    {
        if (std::abs (db - valueDb) < 1.0e-9)
            return;
        valueDb = db;
        repaint();
    }

    std::function<void()> onReset;

    void paint (juce::Graphics& g) override
    {
        const bool atDefault = std::abs (valueDb) < 0.05;
        const bool active = isEnabled();

        if (hovered && active) // 押せることはホバーで示す（常設の枠は置かない）
        {
            const auto area = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.fillRoundedRectangle (area, 4.0f);
            g.setColour (juce::Colours::white.withAlpha (0.13f));
            g.drawRoundedRectangle (area, 4.0f, 1.0f);
        }

        // 0dB（既定）のときは他の見出しと同じ静かさ、外れているときだけ明るくする
        auto colour = atDefault ? Theme::lcdLabel : juce::Colours::white.withAlpha (0.85f);
        if (! active)
            colour = colour.withMultipliedAlpha (0.4f);
        else if (hovered)
            colour = juce::Colours::white;

        g.setColour (colour);
        g.setFont (Fonts::small());
        g.drawText (GainScale::text (valueDb), getLocalBounds().reduced (5, 0),
                    juce::Justification::centredRight);
    }

    void mouseEnter (const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override
    {
        if (isEnabled() && onReset != nullptr)
            onReset();
    }
    void enablementChanged() override { repaint(); }

private:
    double valueDb = 0.0;
    bool hovered = false;
};
