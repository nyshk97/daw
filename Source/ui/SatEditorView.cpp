#include "SatEditorView.h"

#include <cmath>

#include "Fonts.h"
#include "Theme.h"

namespace
{
constexpr int knobSize = 64;

// 倍音バーの計算に使う点数（1周期分。H8まで見るので十分な分解能）
constexpr int harmonicPoints = 512;
} // namespace

SatEditorView::SatEditorView()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    configureKnob (driveSlider, Sat::defaults.drive);
    configureKnob (mixSlider, Sat::defaults.mix);
    recomputeHarmonics();
}

void SatEditorView::configureKnob (juce::Slider& slider, double defaultValue)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    slider.setRange (0.0, 1.0, 0.001);
    slider.setDoubleClickReturnValue (true, defaultValue); // 復元手段（undo対象外の代わり）
    slider.setScrollWheelEnabled (false); // 変更経路を増やさない（GainSliderと同じ流儀）
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

void SatEditorView::setTrack (Track* trackToShow)
{
    track = trackToShow;
    if (track != nullptr)
        loadFromModel();
    recomputeHarmonics();
    repaint();
}

void SatEditorView::refreshFromModel()
{
    if (track != nullptr)
        loadFromModel();
    recomputeHarmonics();
    repaint();
}

void SatEditorView::loadFromModel()
{
    const auto values = Sat::load (track->params->sat);
    loadingFromModel = true;
    driveSlider.setValue (values.drive, juce::dontSendNotification);
    mixSlider.setValue (values.mix, juce::dontSendNotification);
    loadingFromModel = false;
}

void SatEditorView::applyToModel()
{
    if (track == nullptr)
        return;
    Sat::Values values;
    values.drive = (float) driveSlider.getValue();
    values.mix = (float) mixSlider.getValue();
    Sat::store (track->params->sat, Sat::normalized (values));
    recomputeHarmonics();
    repaint(); // 伝達カーブ・倍音バーがDriveに追従する
}

// -18dBFS正弦をDSPと同じ伝達カーブに通し、H1〜H8を相関で取り出して基本波比dBにする。
// メッセージスレッドの数百点ループ＝ドラッグ追従でも軽い（決定的なのでFFT表示より安定）
void SatEditorView::recomputeHarmonics()
{
    const float drive = track != nullptr ? Sat::load (track->params->sat).drive
                                         : (float) driveSlider.getValue();
    const auto curve = Sat::Curve::fromDrive (drive);
    float samples[harmonicPoints];
    for (int k = 0; k < harmonicPoints; ++k)
    {
        const double theta = juce::MathConstants<double>::twoPi * k / harmonicPoints;
        samples[k] = Sat::transfer (curve, Sat::compReferenceAmp * (float) std::sin (theta));
    }
    auto magnitude = [&] (int harmonic)
    {
        double re = 0.0, im = 0.0;
        for (int k = 0; k < harmonicPoints; ++k)
        {
            const double theta = juce::MathConstants<double>::twoPi * harmonic * k / harmonicPoints;
            re += samples[k] * std::cos (theta);
            im += samples[k] * std::sin (theta);
        }
        return std::hypot (re, im);
    };
    const double fundamental = juce::jmax (magnitude (1), 1.0e-12);
    for (int h = 0; h < numHarmonics; ++h)
        harmonicsDbc[h] = (float) (20.0 * std::log10 (
                              juce::jmax (magnitude (h + 2) / fundamental, 1.0e-6)));
}

void SatEditorView::resized()
{
    auto area = getLocalBounds().reduced (10, 8);

    knobColumn = area.removeFromRight (juce::jmax (120, area.getWidth() / 5));
    area.removeFromRight (10);
    curveArea = area.removeFromLeft (juce::jmin (area.getHeight(), area.getWidth() / 2));
    area.removeFromLeft (10);
    harmonicsArea = area;

    // ノブは縦に2個（上=Drive・下=Mix。名前と数値の描画は paint 側）
    auto knobs = knobColumn;
    const int cellH = knobs.getHeight() / 2;
    auto place = [&] (juce::Slider& slider)
    {
        auto cell = knobs.removeFromTop (cellH);
        cell.removeFromTop (13);
        cell.removeFromBottom (15);
        slider.setBounds (cell.withSizeKeepingCentre (juce::jmin (knobSize, cell.getWidth()),
                                                      juce::jmin (knobSize, cell.getHeight())));
    };
    place (driveSlider);
    place (mixSlider);
}

void SatEditorView::paint (juce::Graphics& g)
{
    const bool hasTrack = track != nullptr;
    const bool enabled = hasTrack && track->params->satEnabled.load();
    const auto values = hasTrack ? Sat::load (track->params->sat) : Sat::Values {};
    const float dim = enabled ? 1.0f : 0.4f; // バイパス中は沈める（EQ/Compと同じ扱い）

    // ---- 伝達カーブ（線形軸±1.0）----
    {
        const auto area = curveArea.toFloat();
        g.setColour (Theme::timelineBg);
        g.fillRoundedRectangle (area, 4.0f);

        auto xFor = [&] (float v) { return area.getX() + (v + 1.0f) * 0.5f * area.getWidth(); };
        auto yFor = [&] (float v) { return area.getBottom() - (v + 1.0f) * 0.5f * area.getHeight(); };

        // 対角線＝素通し（Drive 0 の基準）
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        const float dash[2] = { 3.0f, 3.0f };
        g.drawDashedLine ({ xFor (-1.0f), yFor (-1.0f), xFor (1.0f), yFor (1.0f) }, dash, 2, 1.0f);
        // 0軸
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawVerticalLine ((int) xFor (0.0f), area.getY(), area.getBottom());
        g.drawHorizontalLine ((int) yFor (0.0f), area.getX(), area.getRight());

        if (hasTrack)
        {
            const auto curve = Sat::Curve::fromDrive (values.drive);
            juce::Path path;
            for (int i = 0; i <= 120; ++i)
            {
                const float x = -1.0f + 2.0f * (float) i / 120.0f;
                const float y = juce::jlimit (-1.0f, 1.0f, Sat::transfer (curve, x));
                const juce::Point<float> p (xFor (x), yFor (y));
                if (i == 0)
                    path.startNewSubPath (p);
                else
                    path.lineTo (p);
            }
            g.setColour (Theme::eqThumbCurve.withAlpha (dim));
            g.strokePath (path, juce::PathStrokeType (2.0f));
        }

        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.setFont (Fonts::small());
        g.drawText ("in", curveArea.withTrimmedTop (curveArea.getHeight() - 14).reduced (4, 0),
                    juce::Justification::centredRight);
        g.drawText ("out", curveArea.reduced (4, 2), juce::Justification::topLeft);
    }

    // ---- 倍音バー（H2〜H8・基本波比dBc・-60dB床。偶数=accent / 奇数=グレー）----
    {
        const auto area = harmonicsArea.toFloat();
        g.setColour (Theme::timelineBg);
        g.fillRoundedRectangle (area, 4.0f);

        const float padL = 26.0f, padB = 16.0f, padT = 8.0f;
        const auto plot = area.withTrimmedLeft (padL).withTrimmedBottom (padB).withTrimmedTop (padT);

        g.setFont (Fonts::small());
        for (float db = 0.0f; db >= barFloorDb; db -= 20.0f)
        {
            const float y = plot.getY() + (-db / -barFloorDb) * plot.getHeight();
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawText (juce::String ((int) db), (int) area.getX() + 2, (int) y - 6, 22, 12,
                        juce::Justification::centredRight);
        }

        const float barW = plot.getWidth() / (float) numHarmonics;
        for (int h = 0; h < numHarmonics; ++h)
        {
            const int harmonic = h + 2;
            const float t = juce::jmax (0.0f, (harmonicsDbc[h] - barFloorDb) / -barFloorDb);
            const auto bar = juce::Rectangle<float> (plot.getX() + barW * (float) h + 4.0f,
                                                     plot.getBottom() - t * plot.getHeight(),
                                                     barW - 8.0f, t * plot.getHeight());
            // 偶数倍音（暖かさの成分）をaccentで際立たせる＝非対称カーブの性格が読める
            g.setColour ((harmonic % 2 == 0 ? Theme::accent : juce::Colours::white.withAlpha (0.35f))
                             .withMultipliedAlpha (hasTrack ? dim : 0.4f));
            g.fillRect (bar);
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.drawText ("H" + juce::String (harmonic),
                        (int) (plot.getX() + barW * (float) h), (int) plot.getBottom() + 2,
                        (int) barW, 12, juce::Justification::centred);
        }

        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.drawText (juce::String::fromUTF8 (u8"倍音（基本波比 dB）"), harmonicsArea.reduced (6, 2),
                    juce::Justification::topRight);
    }

    // ---- ノブ名・数値 ----
    struct KnobText { const juce::Slider* slider; const char* name; juce::String value; };
    const KnobText texts[2] = {
        { &driveSlider, "DRIVE", juce::String ((int) std::lround (driveSlider.getValue() * 100.0)) + " %" },
        { &mixSlider, "MIX", juce::String ((int) std::lround (mixSlider.getValue() * 100.0)) + " %" },
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
}
