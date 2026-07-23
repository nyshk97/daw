#pragma once

#include <array>
#include <cmath>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "Theme.h"

// UI配布用のL/Rピーク値ペア（[0]=L, [1]=R。リニア振幅）。
// オーディオスレッドのatomic（TrackParams::peakL/peakR）からMainComponentが
// 30Hzで exchange(0) した後の値をヘッダー・ミキサー・FXパネルへ配るときの形
using StereoPeak = std::array<float, 2>;

namespace Meters
{
inline constexpr int holdFrames = 45;    // ピークホールドの保持時間 ~1.5秒（30Hz）
inline constexpr float holdDecay = 0.94f;

// -60dB..0dBFS を 0..1 に写す（実DAWのメーターと同じdBスケール。GOTCHAS.md参照）
inline float norm (float level)
{
    if (level <= 0.001f) // -60dB未満は表示しない
        return 0.0f;
    return juce::jlimit (0.0f, 1.0f, (20.0f * std::log10 (level) + 60.0f) / 60.0f);
}

// レベル色のグラデーション（lowEnd = -60dB端, hotEnd = 0dBFS端）。
// スケール位置に固定して張り、レベルが上がると先端の色が緑→黄→赤に変わる
inline juce::ColourGradient gradient (juce::Point<float> lowEnd, juce::Point<float> hotEnd)
{
    juce::ColourGradient grad (Theme::meterGreenDeep, lowEnd, Theme::meterRed, hotEnd, false);
    grad.addColour (0.66, Theme::meterGreen);
    grad.addColour (0.78, Theme::meterYellow);
    grad.addColour (0.86, Theme::meterYellow);
    grad.addColour (0.93, Theme::meterOrange);
    return grad;
}

// 1chぶんのメーター表示状態（レベルの減衰＋ピークホールド。Logicと同じ「少し保持して落ちる」）。
// 30Hzで step() を呼び、trueが返ったら再描画する
struct ChannelDisplay
{
    float level = 0.0f;
    float hold = 0.0f;
    int holdAge = 0;

    bool step (float incoming)
    {
        bool changed = false;
        // 表示は「新しい値」か「前回の減衰」の大きい方。定常値（持続音）では変化なし扱い
        const float next = juce::jmax (incoming, level * 0.8f);
        const float shown = next < 0.005f ? 0.0f : next;
        if (! juce::approximatelyEqual (shown, level))
        {
            level = shown;
            changed = true;
        }
        if (level > hold)
        {
            hold = level;
            holdAge = 0;
            changed = true;
        }
        else if (hold > 0.0f && ++holdAge > holdFrames)
        {
            hold = hold < 0.005f ? 0.0f : hold * holdDecay;
            changed = true;
        }
        return changed;
    }
};
} // namespace Meters

// 縦型のL/R 2レーンメーター（Logicのチャンネルストリップのメーター相当）。
// スケールは下端=-60dB..上端=0dBFSの固定で、フェーダー位置とは独立。
// 幅に余裕があれば井戸の左にdB数字の列を出す（Logicと同じ）。
// 表示値の減衰・ピークホールドはここで持ち、update() を30Hz Timerから呼ぶ
class StereoMeter : public juce::Component
{
public:
    // 井戸の上下の詰め幅。隣に並ぶ縦フェーダーの溝（可動域=つまみ半径ぶん内側。
    // AppLookAndFeel::getSliderThumbRadius）と上下端が揃うように同じ値にする。
    // dB数字が上下にはみ出すための余白も兼ねる。レイアウト側はフェーダーと同じ高さで置くだけでよい
    static constexpr int wellInsetY = 14;

    StereoMeter()
    {
        setInterceptsMouseClicks (false, false);
    }

    void update (StereoPeak incoming)
    {
        bool changed = false;
        for (size_t i = 0; i < 2; ++i)
            changed = channels[i].step (incoming[i]) || changed;
        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();

        // dB数字の列（左側。狭い置き方をされたときは井戸だけにする）
        auto wellCol = area;
        juce::Rectangle<float> labelCol;
        if (area.getWidth() >= 22.0f)
        {
            labelCol = wellCol.removeFromLeft (area.getWidth() - 10.0f);
            labelCol.removeFromRight (3.0f);
        }
        const auto well = wellCol.reduced (0.0f, (float) wellInsetY);

        if (! labelCol.isEmpty())
            drawScaleLabels (g, labelCol, well);

        g.setColour (Theme::faderSlotBg);
        g.fillRoundedRectangle (well, 2.0f);

        // レーン塗りは矩形のまま、井戸の角丸パスでクリップ
        juce::Graphics::ScopedSaveState save (g);
        juce::Path wellPath;
        wellPath.addRoundedRectangle (well, 2.0f);
        g.reduceClipRegion (wellPath);

        g.setGradientFill (Meters::gradient (well.getBottomLeft(), well.getTopLeft()));
        const float innerH = well.getHeight() - 2.0f;
        const float laneW = (well.getWidth() - 3.0f) * 0.5f; // 縁1px＋レーン間1px
        for (size_t i = 0; i < 2; ++i)
        {
            const float laneX = well.getX() + 1.0f + (float) i * (laneW + 1.0f);
            const float h = Meters::norm (channels[i].level) * innerH;
            if (h > 0.0f)
                g.fillRect (juce::Rectangle<float> (laneX, well.getBottom() - 1.0f - h, laneW, h));

            const float holdNorm = Meters::norm (channels[i].hold);
            if (holdNorm > 0.0f) // ピークホールドの目印（色はグラデーションのその位置の色）
                g.fillRect (juce::Rectangle<float> (
                    laneX, well.getBottom() - 1.0f - holdNorm * innerH, laneW, 2.0f));
        }
    }

private:
    void drawScaleLabels (juce::Graphics& g, juce::Rectangle<float> labelCol,
                          juce::Rectangle<float> well) const
    {
        // Logicのストリップと同じ数字（位置は井戸のスケールに合わせる）。高さが足りないときは間引く
        static constexpr int dense[] = { 0, 3, 6, 9, 12, 15, 18, 21, 24, 30, 35, 40, 45, 50, 60 };
        static constexpr int sparse[] = { 0, 6, 12, 18, 24, 36, 48, 60 };
        const bool useDense = well.getHeight() >= 320.0f;
        const int* values = useDense ? dense : sparse;
        const int count = useDense ? (int) std::size (dense) : (int) std::size (sparse);

        g.setColour (Theme::meterScaleText);
        g.setFont (Fonts::small().withHeight (9.0f));
        const float innerTop = well.getY() + 1.0f;
        const float innerH = well.getHeight() - 2.0f;
        for (int i = 0; i < count; ++i)
        {
            const float y = innerTop + (float) values[i] / 60.0f * innerH;
            g.drawText (juce::String (values[i]),
                        juce::Rectangle<float> (labelCol.getX(), y - 5.0f, labelCol.getWidth(), 10.0f),
                        juce::Justification::centredRight);
        }
    }

    Meters::ChannelDisplay channels[2];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoMeter)
};
