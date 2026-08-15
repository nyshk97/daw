#include "StemPanel.h"

#include "shared/StemMix.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

const juce::Colour panelBg { 0xff26262a };
const juce::Colour rowBg { 0xff222226 };
const juce::Colour border { 0xff2d2d32 };
const juce::Colour textColour { 0xffd3d3d3 };
const juce::Colour textSilent { 0xff5a5a60 };
const juce::Colour accent { 0xff4a6ea9 };
const juce::Colour muteOn { 0xff5b82c4 };
const juce::Colour soloOn { 0xffdfae4a };
const juce::Colour meterBg { 0xff111114 };
const juce::Colour meterGreen { 0xff7bc47b };
const juce::Colour meterYellow { 0xffdfae4a };
const juce::Colour meterRed { 0xffd94a43 };
const juce::Colour msOffBg { 0xff333338 };
const juce::Colour msOffText { 0xff8a8a90 };

// ステム別スウォッチ（Logicのトラック色の感覚。固定順）
const juce::Colour swatches[] = {
    juce::Colour (0xffd94a43), // drums = 赤
    juce::Colour (0xff4a6ea9), // bass = 青
    juce::Colour (0xffdfae4a), // vocals = 黄
    juce::Colour (0xff7bc47b), // other = 緑
    juce::Colour (0xffb06ac9), // guitar = 紫
    juce::Colour (0xff5bb8c4), // piano = シアン
};

constexpr int tabsHeight = 26;
constexpr int rowHeight = 24;
constexpr int rowGap = 3;

float meterNorm (float level)
{
    if (level <= 0.001f)
        return 0.0f;
    return juce::jlimit (0.0f, 1.0f, (20.0f * std::log10 (level) + 60.0f) / 60.0f);
}
} // namespace

StemPanel::StemPanel() = default;

void StemPanel::setManifest (const StemCache::Manifest& manifest)
{
    groups.clear();
    states.clear();
    groupIndex = -1;
    if (manifest.valid)
    {
        groups = manifest.groups;
        for (const auto& g : groups)
            states.push_back ({ std::vector<bool> (g.stems.size(), false),
                                std::vector<bool> (g.stems.size(), false) });
    }
    resized();
    repaint();
}

void StemPanel::clear()
{
    setManifest ({});
}

void StemPanel::selectGroup (int index)
{
    if (index < -1 || index >= (int) groups.size() || index == groupIndex)
        return;
    groupIndex = index;
    displayLevels.assign (displayLevels.size(), 0.0f);
    resized();
    repaint();
    if (onConfigChanged)
        onConfigChanged();
}

std::vector<bool> StemPanel::audibleStems() const
{
    if (groupIndex < 0 || groupIndex >= (int) states.size())
        return {};
    const auto& st = states[(size_t) groupIndex];
    return StemMix::audible (st.mute, st.solo);
}

const StemCache::Group* StemPanel::currentGroup() const
{
    if (groupIndex < 0 || groupIndex >= (int) groups.size())
        return nullptr;
    return &groups[(size_t) groupIndex];
}

int StemPanel::preferredHeight() const
{
    if (groups.empty())
        return 0;
    const int rows = groupIndex >= 0 ? (int) groups[(size_t) groupIndex].stems.size() : 0;
    return tabsHeight + 8 + (rows > 0 ? rows * (rowHeight + rowGap) + 5 : 0);
}

void StemPanel::updatePeaks (const std::function<float (int)>& peakForIndex)
{
    if (groupIndex < 0)
        return;
    const auto rows = groups[(size_t) groupIndex].stems.size();
    displayLevels.resize (rows, 0.0f);
    bool changed = false;
    for (size_t i = 0; i < rows; ++i)
    {
        const float target = juce::jmax (peakForIndex ((int) i), displayLevels[i] * 0.85f);
        changed = changed || std::abs (target - displayLevels[i]) > 0.002f;
        displayLevels[i] = target;
    }
    if (changed)
        repaint();
}

void StemPanel::paint (juce::Graphics& g)
{
    g.fillAll (panelBg);
    g.setColour (border);
    g.fillRect (getLocalBounds().removeFromTop (1));

    if (groups.empty())
        return;

    // グループタブ（1群しかなくても「オリジナル⇄ステム」の切替は必要なので表示。
    // 群が1つならその群のタブだけ＝モデルラボの結果と矛盾しない）
    g.setFont (juce::FontOptions (12.0f));
    for (size_t t = 0; t < tabRects.size(); ++t)
    {
        const bool on = (int) t - 1 == groupIndex;
        g.setColour (on ? accent : rowBg);
        g.fillRoundedRectangle (tabRects[t].toFloat(), 5.0f);
        g.setColour (on ? juce::Colours::white : msOffText);
        const auto label = t == 0 ? jp (u8"オリジナル") : groups[t - 1].displayName;
        g.drawText (label, tabRects[t], juce::Justification::centred);
    }

    // ステム行（縦リスト: スウォッチ・名前・メーター・M/S）
    const auto audible = audibleStems();
    if (const auto* group = currentGroup())
    {
        for (size_t i = 0; i < group->stems.size(); ++i)
        {
            const bool silent = i < audible.size() && ! audible[i];
            g.setColour (rowBg);
            g.fillRoundedRectangle (rowRects[i].toFloat(), 5.0f);

            auto row = rowRects[i].reduced (8, 4);
            g.setColour (swatches[i % std::size (swatches)]);
            g.fillRoundedRectangle (row.removeFromLeft (4).toFloat(), 2.0f);
            row.removeFromLeft (8);

            g.setColour (silent ? textSilent : textColour);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (group->stems[i].name, row.removeFromLeft (64), juce::Justification::centredLeft);

            // メーター（-60〜0dBFS固定スケール）
            const auto meter = meterRects[i];
            g.setColour (meterBg);
            g.fillRoundedRectangle (meter.toFloat(), 2.5f);
            const float level = i < displayLevels.size() ? displayLevels[i] : 0.0f;
            const float w = meterNorm (level) * (float) meter.getWidth();
            if (w > 0.0f)
            {
                juce::ColourGradient grad (meterGreen, meter.toFloat().getTopLeft(),
                                           meterRed, meter.toFloat().getTopRight(), false);
                grad.addColour (0.75, meterGreen);
                grad.addColour (0.85, meterYellow);
                g.setGradientFill (grad);
                g.fillRoundedRectangle (meter.toFloat().withWidth (w), 2.5f);
            }

            // M/Sボタン
            const auto& st = states[(size_t) groupIndex];
            auto drawMs = [&g] (juce::Rectangle<int> r, const char* label, bool on, juce::Colour onColour, juce::Colour onText)
            {
                g.setColour (on ? onColour : msOffBg);
                g.fillRoundedRectangle (r.toFloat(), 4.0f);
                g.setColour (on ? onText : msOffText);
                g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
                g.drawText (label, r, juce::Justification::centred);
            };
            drawMs (muteRects[i], "M", st.mute[i], muteOn, juce::Colours::white);
            drawMs (soloRects[i], "S", st.solo[i], soloOn, juce::Colour (0xff222226));
        }
    }
}

void StemPanel::resized()
{
    rebuildButtons();
}

void StemPanel::rebuildButtons()
{
    tabRects.clear();
    rowRects.clear();
    muteRects.clear();
    soloRects.clear();
    meterRects.clear();
    if (groups.empty())
        return;

    auto area = getLocalBounds().reduced (10, 0);
    auto tabs = area.removeFromTop (tabsHeight + 6).withTrimmedTop (6);
    tabRects.push_back (tabs.removeFromLeft (86));
    tabs.removeFromLeft (4);
    for (size_t i = 0; i < groups.size(); ++i)
    {
        tabRects.push_back (tabs.removeFromLeft (76));
        tabs.removeFromLeft (4);
    }

    if (const auto* group = currentGroup())
    {
        area.removeFromTop (2);
        for (size_t i = 0; i < group->stems.size(); ++i)
        {
            auto row = area.removeFromTop (rowHeight);
            area.removeFromTop (rowGap);
            rowRects.push_back (row);
            auto inner = row.reduced (8, 3);
            auto s = inner.removeFromRight (26);
            inner.removeFromRight (4);
            auto m = inner.removeFromRight (26);
            inner.removeFromRight (10);
            soloRects.push_back (s);
            muteRects.push_back (m);
            inner.removeFromLeft (4 + 8 + 64 + 8);
            meterRects.push_back (inner.withSizeKeepingCentre (inner.getWidth(), 7));
        }
    }
}

void StemPanel::mouseUp (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    for (size_t t = 0; t < tabRects.size(); ++t)
    {
        if (tabRects[t].contains (pos))
        {
            const int newIndex = (int) t - 1;
            if (newIndex == groupIndex)
                return;
            groupIndex = newIndex;
            displayLevels.assign (displayLevels.size(), 0.0f);
            resized();
            repaint();
            if (onConfigChanged)
                onConfigChanged();
            return;
        }
    }

    if (groupIndex < 0)
        return;
    auto& st = states[(size_t) groupIndex];
    for (size_t i = 0; i < muteRects.size(); ++i)
    {
        if (muteRects[i].contains (pos))
        {
            st.mute[i] = ! st.mute[i];
            repaint();
            if (onConfigChanged)
                onConfigChanged();
            return;
        }
        if (soloRects[i].contains (pos))
        {
            st.solo[i] = ! st.solo[i];
            repaint();
            if (onConfigChanged)
                onConfigChanged();
            return;
        }
    }
}
