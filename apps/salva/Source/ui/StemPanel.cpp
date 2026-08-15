#include "StemPanel.h"

#include "shared/StemMix.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

// アイコン基準パレット（アクセント=レーベルのオレンジ。M/Sの機能色はLogic準拠のまま）
const juce::Colour panelBg { 0xff1c1b20 };
const juce::Colour rowBg { 0xff201f24 };
const juce::Colour border { 0xff2a2930 };
const juce::Colour textColour { 0xffe8e0d2 };
const juce::Colour textSilent { 0xff5f5b55 };
const juce::Colour accent { 0xffff7a2e };
const juce::Colour muteOn { 0xff5b82c4 };
const juce::Colour soloOn { 0xffdfae4a };
const juce::Colour meterBg { 0xff111114 };
const juce::Colour meterGreen { 0xff7bc47b };
const juce::Colour meterYellow { 0xffdfae4a };
const juce::Colour meterRed { 0xffd94a43 };
const juce::Colour msOffBg { 0xff34323a };
const juce::Colour msOffText { 0xff97908a };

// ステム別スウォッチ（Logicのトラック色の感覚。固定順）
const juce::Colour swatches[] = {
    juce::Colour (0xffd94a43), // drums = 赤
    juce::Colour (0xff4a6ea9), // bass = 青
    juce::Colour (0xffdfae4a), // vocals = 黄
    juce::Colour (0xff7bc47b), // other = 緑
    juce::Colour (0xffb06ac9), // guitar = 紫
    juce::Colour (0xff5bb8c4), // piano = シアン
};

constexpr int pad = 10;      // カラム内余白
constexpr int tabHeight = 24;
constexpr int tabGap = 4;
constexpr int rowHeight = 30;
constexpr int rowGap = 6;

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
    if (index >= 0)
        lastActiveGroup = index;
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
    g.fillRect (getLocalBounds().removeFromLeft (1)); // 波形との境界（左辺）

    if (groups.empty())
        return;

    // グループタブ（縦積み。1群しかなくても「オリジナル⇄ステム」の切替は必要なので表示）
    g.setFont (juce::FontOptions (12.0f));
    for (size_t t = 0; t < tabRects.size(); ++t)
    {
        const bool on = (int) t - 1 == groupIndex;
        g.setColour (on ? accent : rowBg);
        g.fillRoundedRectangle (tabRects[t].toFloat(), 6.0f);
        g.setColour (on ? juce::Colour (0xff2b1507) : msOffText); // オレンジ地には暗色文字
        // ラベルはステム本数から導出（manifestのname="4ステム"等は使わない。
        // 旧キャッシュも再分離なしで新表記になる）
        const auto label = t == 0 ? juce::String ("ORIGINAL")
                                  : juce::String (groups[t - 1].stems.size()) + " STEMS";
        g.drawText (label, tabRects[t], juce::Justification::centred);
    }

    // ステム行。ORIGINAL選択中も直近グループの行をディム表示で残す
    //（何が分離済みか見え続け、タブ切替でレイアウトが動かない）
    const bool dimmed = groupIndex < 0;
    const int displayIdx = dimmed ? lastActiveGroup : groupIndex;
    if (displayIdx < 0 || displayIdx >= (int) groups.size())
        return;
    const auto& group = groups[(size_t) displayIdx];
    const auto& st = states[(size_t) displayIdx];
    const auto audible = audibleStems(); // ORIGINAL時は空
    const float alpha = dimmed ? 0.35f : 1.0f;

    for (size_t i = 0; i < group.stems.size() && i < rowRects.size(); ++i)
    {
        const bool silent = i < audible.size() && ! audible[i];
        g.setColour (rowBg.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (rowRects[i].toFloat(), 7.0f);

        // rectはrebuildButtonsが組んだものを使う（描画とヒットテストの座標を揃える）
        auto row = rowRects[i].reduced (8, 5);
        g.setColour (swatches[i % std::size (swatches)].withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (row.removeFromLeft (3).toFloat().withSizeKeepingCentre (3.0f, 14.0f), 2.0f);
        row.removeFromLeft (7);
        row.setRight (meterRects[i].getX() - 6); // 残りが名前欄

        g.setColour ((silent ? textSilent : textColour).withMultipliedAlpha (alpha));
        g.setFont (juce::FontOptions (11.5f));
        g.drawText (group.stems[i].name, row, juce::Justification::centredLeft, true);

        // ミニメーター（-60〜0dBFS固定スケール。ディム中は空）
        const auto meter = meterRects[i];
        g.setColour (meterBg.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (meter.toFloat(), 2.0f);
        const float level = (! dimmed && i < displayLevels.size()) ? displayLevels[i] : 0.0f;
        const float w = meterNorm (level) * (float) meter.getWidth();
        if (w > 0.0f)
        {
            juce::ColourGradient grad (meterGreen, meter.toFloat().getTopLeft(),
                                       meterRed, meter.toFloat().getTopRight(), false);
            grad.addColour (0.75, meterGreen);
            grad.addColour (0.85, meterYellow);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (meter.toFloat().withWidth (w), 2.0f);
        }

        // M/Sボタン
        auto drawMs = [&g, alpha] (juce::Rectangle<int> r, const char* label, bool on,
                                   juce::Colour onColour, juce::Colour onText)
        {
            g.setColour ((on ? onColour : msOffBg).withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (r.toFloat(), 4.0f);
            g.setColour ((on ? onText : msOffText).withMultipliedAlpha (alpha));
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText (label, r, juce::Justification::centred);
        };
        drawMs (muteRects[i], "M", st.mute[i], muteOn, juce::Colours::white);
        drawMs (soloRects[i], "S", st.solo[i], soloOn, juce::Colour (0xff222226));
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

    auto area = getLocalBounds().reduced (pad, pad);

    // タブ（縦積み・全幅）
    tabRects.push_back (area.removeFromTop (tabHeight));
    for (size_t i = 0; i < groups.size(); ++i)
    {
        area.removeFromTop (tabGap);
        tabRects.push_back (area.removeFromTop (tabHeight));
    }
    area.removeFromTop (pad);

    // 行はORIGINAL中もディム表示するため、表示グループ（直近選択）で組む
    const int displayIdx = groupIndex >= 0 ? groupIndex : lastActiveGroup;
    if (displayIdx < 0 || displayIdx >= (int) groups.size())
        return;
    for (size_t i = 0; i < groups[(size_t) displayIdx].stems.size(); ++i)
    {
        auto row = area.removeFromTop (rowHeight);
        area.removeFromTop (rowGap);
        rowRects.push_back (row);
        auto inner = row.reduced (8, 5);
        soloRects.push_back (inner.removeFromRight (22).withSizeKeepingCentre (22, 20));
        inner.removeFromRight (4);
        muteRects.push_back (inner.removeFromRight (22).withSizeKeepingCentre (22, 20));
        inner.removeFromRight (6);
        meterRects.push_back (inner.removeFromRight (34).withSizeKeepingCentre (34, 4));
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
            if (newIndex >= 0)
                lastActiveGroup = newIndex;
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
