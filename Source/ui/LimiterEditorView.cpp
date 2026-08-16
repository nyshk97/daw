#include "LimiterEditorView.h"

#include <cmath>

#include "Fonts.h"
#include "Theme.h"
#include "../shared/LimiterParams.h"

namespace
{
constexpr int knobRowHeight = 74;

// GR表示の色（CompエディタのGRと同じ系統。Logicの読み方に合わせる）
const juce::Colour grColour { 0xffd9a13c };
// ターゲットラインの色: -14=配信境界（accent系の青）・-9=hiphop帯の入口（GR系の黄）
const juce::Colour streamLineColour { 0xff7a9ede };
const juce::Colour hiphopLineColour { 0xffd8b04a };

juce::String lufsText (bool has, float value)
{
    return has ? juce::String (value, 1) : juce::String ("-");
}
} // namespace

LimiterEditorView::LimiterEditorView()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);

    configureKnob (gainSlider, Limiter::minGainDb, Limiter::maxGainDb, 0.1,
                   Limiter::defaults.gainDb, 0.0); // 線形（dBはそれ自体が対数）
    configureKnob (ceilingSlider, Limiter::minCeilingDb, Limiter::maxCeilingDb, 0.1,
                   Limiter::defaults.ceilingDb, 0.0);
    configureKnob (releaseSlider, Limiter::minReleaseMs, Limiter::maxReleaseMs, 1.0,
                   Limiter::defaults.releaseMs, 60.0); // 時間は対数的に触るのが自然
}

void LimiterEditorView::configureKnob (juce::Slider& slider, double min, double max, double step,
                                       double defaultValue, double skewMidpoint)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    slider.setRange (min, max, step);
    if (skewMidpoint > 0.0)
        slider.setSkewFactorFromMidPoint (skewMidpoint);
    slider.setDoubleClickReturnValue (true, defaultValue); // 復元手段（undo対象外の代わり）
    slider.setScrollWheelEnabled (false); // 変更経路を増やさない（GainSlider・Compと同じ流儀）
    slider.setWantsKeyboardFocus (false);
    slider.setMouseClickGrabsKeyboardFocus (false);
    slider.setColour (juce::Slider::rotarySliderFillColourId, Theme::accent);
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, Theme::controlBg);
    slider.onValueChange = [this]
    {
        if (loadingFromModel)
            return;
        applyToModel();
        if (onEdited)
            onEdited();
    };
    addAndMakeVisible (slider);
}

void LimiterEditorView::setMaster (TrackParams* masterParamsToShow)
{
    master = masterParamsToShow;
    currentGrDb = 0.0f;
    if (master != nullptr)
        loadFromModel();
    repaint();
}

void LimiterEditorView::loadFromModel()
{
    const auto values = Limiter::load (master->limiter);
    loadingFromModel = true;
    gainSlider.setValue (values.gainDb, juce::dontSendNotification);
    ceilingSlider.setValue (values.ceilingDb, juce::dontSendNotification);
    releaseSlider.setValue (values.releaseMs, juce::dontSendNotification);
    loadingFromModel = false;
}

void LimiterEditorView::applyToModel()
{
    if (master == nullptr)
        return;
    Limiter::Values values;
    values.gainDb = (float) gainSlider.getValue();
    values.ceilingDb = (float) ceilingSlider.getValue();
    values.releaseMs = (float) releaseSlider.getValue();
    Limiter::store (master->limiter, Limiter::normalized (values));
    repaint(); // ceilingはTP数値の赤判定にも効く
}

void LimiterEditorView::pushMeters (const MasterMeterFeed& feed, float grDb)
{
    if (master == nullptr || ! isShowing())
        return;
    meterFeed = feed;
    currentGrDb = grDb;
    repaint();
}

void LimiterEditorView::resized()
{
    auto area = getLocalBounds().reduced (10, 8);

    // 左: ノブ3（固定幅）→ GR縦メーター → 仕切り → 右: 計測
    auto knobColumn = area.removeFromLeft (300);
    auto knobRow = knobColumn.withHeight (knobRowHeight)
                       .withY (knobColumn.getY() + (knobColumn.getHeight() - knobRowHeight) / 2 - 10);
    const int cell = knobRow.getWidth() / 3;
    juce::Slider* sliders[3] = { &gainSlider, &ceilingSlider, &releaseSlider };
    for (auto* slider : sliders)
    {
        auto cellArea = knobRow.removeFromLeft (cell);
        cellArea.removeFromTop (13);    // ノブ名
        cellArea.removeFromBottom (15); // 数値
        slider->setBounds (cellArea.withSizeKeepingCentre (
            juce::jmin (cellArea.getWidth(), cellArea.getHeight() + 8), cellArea.getHeight()));
    }

    area.removeFromLeft (14);
    grMeterArea = area.removeFromLeft (20).reduced (0, 14);
    area.removeFromLeft (18);

    // 右カラム: 数値LCD（右端）とバー2本
    readoutColumn = area.removeFromRight (juce::jmax (130, area.getWidth() / 5));
    area.removeFromRight (16);
    auto bars = area;
    bars.removeFromTop (22); // 「LOUDNESS (LUFS)」ラベル行
    lufsBarArea = bars.removeFromTop (26);
    bars.removeFromTop (34); // 目盛り数字＋間隔
    bars.removeFromTop (16); // 「CORRELATION」ラベル行
    correlationArea = bars.removeFromTop (14);
}

void LimiterEditorView::paint (juce::Graphics& g)
{
    const bool hasMaster = master != nullptr;
    const bool valid = meterFeed.measurementValid;

    // ---- GR縦メーター（0〜-12dB。上から下がる=下げている量）----
    {
        const auto area = grMeterArea.toFloat();
        g.setColour (Theme::timelineBg);
        g.fillRoundedRectangle (area, 3.0f);
        if (hasMaster && currentGrDb > 0.02f)
        {
            const float h = juce::jmin (currentGrDb, grRangeDb) / grRangeDb * area.getHeight();
            g.setColour (grColour);
            g.fillRoundedRectangle (area.withHeight (h), 3.0f);
        }
        // 目盛り（0/3/6/9/12）
        g.setColour (Theme::meterScaleText);
        g.setFont (Fonts::small().withHeight (9.0f));
        for (int db = 0; db <= (int) grRangeDb; db += 3)
        {
            const float y = area.getY() + (float) db / grRangeDb * area.getHeight();
            g.drawText (juce::String (db),
                        juce::Rectangle<float> (area.getX() - 16.0f, y - 5.0f, 13.0f, 10.0f),
                        juce::Justification::centredRight);
        }
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (Fonts::small());
        g.drawText ("GR", grMeterArea.withY (grMeterArea.getBottom() + 2).withHeight (13)
                              .expanded (10, 0),
                    juce::Justification::centred);
    }

    const auto lufsToX = [&] (float lufs)
    {
        const auto area = lufsBarArea.toFloat();
        return area.getX() + 1.0f
               + (juce::jlimit (lufsMin, lufsMax, lufs) - lufsMin) / (lufsMax - lufsMin)
                     * (area.getWidth() - 2.0f);
    };

    // ---- LUFSバー（-24〜0・short-term塗り＋integrated▲・ターゲットライン2本）----
    {
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (Fonts::small());
        g.drawText ("LOUDNESS (LUFS)", lufsBarArea.withY (lufsBarArea.getY() - 17).withHeight (14),
                    juce::Justification::centredLeft);

        const auto area = lufsBarArea.toFloat();
        g.setColour (Theme::faderSlotBg);
        g.fillRoundedRectangle (area, 3.0f);

        if (hasMaster && valid && meterFeed.hasShortTerm)
        {
            // スケール位置固定のグラデーション（メーターの読み方と同じ: 上がるほど黄→赤）
            juce::ColourGradient gradient (Theme::meterGreenDeep, area.getBottomLeft(),
                                           Theme::meterRed, area.getBottomRight(), false);
            gradient.addColour (0.55, Theme::meterGreen);
            gradient.addColour (0.75, Theme::meterYellow);
            gradient.addColour (0.90, Theme::meterOrange);
            g.setGradientFill (gradient);
            const float xe = lufsToX (meterFeed.shortTermLufs);
            g.fillRect (juce::Rectangle<float> (area.getX() + 1.0f, area.getY() + 1.0f,
                                                xe - area.getX() - 1.0f, area.getHeight() - 2.0f));
        }

        // 目盛り（4dB刻み）
        g.setColour (Theme::meterScaleText);
        g.setFont (Fonts::small().withHeight (9.0f));
        for (int v = (int) lufsMin; v <= 0; v += 4)
        {
            const float x = lufsToX ((float) v);
            g.setColour (juce::Colours::black.withAlpha (0.3f));
            g.fillRect (juce::Rectangle<float> (x, area.getY() + 1.0f, 1.0f, area.getHeight() - 2.0f));
            g.setColour (Theme::meterScaleText);
            g.drawText (juce::String (v),
                        juce::Rectangle<float> (x - 12.0f, area.getBottom() + 3.0f, 24.0f, 10.0f),
                        juce::Justification::centred);
        }

        // ターゲットライン: -14（配信正規化の境界）・-9（hiphop音圧帯の入口）
        struct Line { float lufs; const char* name; juce::Colour colour; };
        for (const auto& line : { Line { -14.0f, "-14", streamLineColour },
                                  Line { -9.0f, "-9", hiphopLineColour } })
        {
            const float x = lufsToX (line.lufs);
            g.setColour (line.colour);
            g.fillRect (juce::Rectangle<float> (x - 0.75f, area.getY() - 4.0f, 1.5f,
                                                area.getHeight() + 8.0f));
            g.setFont (Fonts::small().withHeight (9.0f));
            g.drawText (line.name,
                        juce::Rectangle<float> (x - 12.0f, area.getBottom() + 13.0f, 24.0f, 10.0f),
                        juce::Justification::centred);
        }

        // integratedマーカー（▲。バーの下辺）
        if (hasMaster && valid && meterFeed.hasIntegrated)
        {
            const float x = lufsToX (meterFeed.integratedLufs);
            juce::Path marker;
            marker.addTriangle (x, area.getBottom() + 1.0f,
                                x - 4.5f, area.getBottom() + 8.0f,
                                x + 4.5f, area.getBottom() + 8.0f);
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.fillPath (marker);
        }
    }

    // ---- 相関バー（-1〜+1。マイナス域=モノで消える成分あり=赤）----
    {
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (Fonts::small());
        g.drawText ("CORRELATION",
                    correlationArea.withY (correlationArea.getY() - 15).withHeight (13),
                    juce::Justification::centredLeft);

        const auto area = correlationArea.toFloat();
        g.setColour (Theme::faderSlotBg);
        g.fillRoundedRectangle (area, 3.0f);

        const float cx = area.getCentreX();
        if (hasMaster && valid && meterFeed.hasCorrelation)
        {
            const float xe = cx + (area.getWidth() * 0.5f - 2.0f) * meterFeed.correlation;
            g.setColour (meterFeed.correlation >= 0.0f ? Theme::meterGreen : Theme::recordRed);
            g.fillRect (juce::Rectangle<float> (juce::jmin (cx, xe), area.getY() + 1.0f,
                                                std::abs (xe - cx), area.getHeight() - 2.0f));
        }
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRect (juce::Rectangle<float> (cx - 0.5f, area.getY(), 1.0f, area.getHeight()));
        g.setColour (Theme::meterScaleText);
        g.setFont (Fonts::small().withHeight (9.0f));
        g.drawText ("-1", juce::Rectangle<float> (area.getX() - 18.0f, area.getY() + 2.0f, 14.0f, 10.0f),
                    juce::Justification::centredRight);
        g.drawText ("+1", juce::Rectangle<float> (area.getRight() + 4.0f, area.getY() + 2.0f, 16.0f, 10.0f),
                    juce::Justification::centredLeft);
        if (hasMaster && valid && meterFeed.hasCorrelation)
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (Fonts::small());
            g.drawText (juce::String (meterFeed.correlation, 2),
                        correlationArea.withX (correlationArea.getRight() + 24).withWidth (40),
                        juce::Justification::centredLeft);
        }
    }

    // ---- 数値LCD列（SHORT TERM / INTEGRATED / TRUE PEAK max）----
    {
        auto column = readoutColumn;
        struct Readout { const char* name; juce::String value; juce::Colour colour; };
        const float ceilingDb = hasMaster ? Limiter::load (master->limiter).ceilingDb
                                          : Limiter::defaults.ceilingDb;
        const bool tpHot = valid && meterFeed.hasTruePeak && meterFeed.maxTruePeakDb > ceilingDb + 0.05f;
        const Readout readouts[3] = {
            { "SHORT TERM", lufsText (valid && meterFeed.hasShortTerm, meterFeed.shortTermLufs),
              juce::Colours::white.withAlpha (0.9f) },
            { "INTEGRATED", lufsText (valid && meterFeed.hasIntegrated, meterFeed.integratedLufs),
              juce::Colours::white.withAlpha (0.9f) },
            { "TRUE PEAK",
              (valid && meterFeed.hasTruePeak ? juce::String (meterFeed.maxTruePeakDb, 1) + " dBTP"
                                              : juce::String ("-")),
              tpHot ? Theme::recordRed : Theme::playGreen },
        };
        const int rowHeight = juce::jmin (56, column.getHeight() / 3);
        for (const auto& readout : readouts)
        {
            auto row = column.removeFromTop (rowHeight);
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.setFont (Fonts::small().withHeight (10.0f));
            g.drawText (readout.name, row.removeFromTop (13), juce::Justification::centredLeft);
            auto box = row.withTrimmedBottom (juce::jmax (0, row.getHeight() - 28));
            g.setColour (Theme::lcdBg);
            g.fillRoundedRectangle (box.toFloat(), 4.0f);
            g.setColour (readout.colour);
            g.setFont (Fonts::mono (17.0f));
            g.drawText (readout.value, box.reduced (8, 0), juce::Justification::centredRight);
        }
        // リング溢れ（読み手の停止等。稀）: 数値は「-」になるので理由を一言添える
        if (hasMaster && ! valid)
        {
            g.setColour (juce::Colours::white.withAlpha (0.4f));
            g.setFont (Fonts::small().withHeight (10.0f));
            g.drawText (juce::String::fromUTF8 (u8"計測が中断されました（再生し直すと再開）"),
                        readoutColumn.withTrimmedTop (readoutColumn.getHeight() - 14),
                        juce::Justification::centredLeft);
        }
    }

    // ---- ノブ名・数値・固定値ラベル ----
    struct KnobText { const juce::Slider* slider; const char* name; juce::String value; };
    const KnobText texts[3] = {
        { &gainSlider, "GAIN", "+" + juce::String (gainSlider.getValue(), 1) + " dB" },
        { &ceilingSlider, "CEILING", juce::String (ceilingSlider.getValue(), 1) + " dB" },
        { &releaseSlider, "RELEASE",
          juce::String ((int) std::lround (releaseSlider.getValue())) + " ms" },
    };
    for (const auto& text : texts)
    {
        const auto bounds = text.slider->getBounds();
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (Fonts::small());
        g.drawText (text.name, bounds.getX() - 12, bounds.getY() - 13, bounds.getWidth() + 24, 12,
                    juce::Justification::centred);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawText (text.value, bounds.getX() - 12, bounds.getBottom() + 2, bounds.getWidth() + 24, 13,
                    juce::Justification::centred);
    }

    // Lookaheadは固定値＋静的ラベルで概念だけ見せる（Compの「Knee: 6dB (soft)」と同じパターン）
    g.setColour (juce::Colours::white.withAlpha (0.5f));
    g.setFont (Fonts::small());
    g.drawText (juce::String::fromUTF8 (u8"Lookahead: 2ms（固定）"),
                juce::Rectangle<int> (getLocalBounds().getX() + 10,
                                      gainSlider.getBottom() + 22, 300, 14),
                juce::Justification::centred);
}
