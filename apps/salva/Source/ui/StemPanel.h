#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "shared/StemCache.h"

// ステムM/Sパネル（確定モック案B: 縦リスト＋ステム別レベルメーター）。
// グループ（4ステム/6ステム等）はマニフェストの内容から動的に表示し、
// 1群しかなければ切替タブを隠す。M/S状態はグループごとに保持する。
// 競合規則（Solo集合＋Mute優先）の計算は shared/StemMix.h、ここは表示と操作だけ
class StemPanel : public juce::Component
{
public:
    StemPanel();

    // マニフェスト反映（グループタブ＋行を再構築）。無効なら clear と同じ
    void setManifest (const StemCache::Manifest& manifest);
    void clear();

    bool hasStems() const { return ! groups.empty(); }
    int selectedGroup() const { return groupIndex; } // -1 = オリジナル
    void selectGroup (int index); // タブクリックと同じ経路（onConfigChangedを発火）
    // 選択中グループのM/S結果（StemMix::audible）。オリジナル時は空
    std::vector<bool> audibleStems() const;
    const StemCache::Group* currentGroup() const;

    // レイアウト計算用（0 = 非表示）
    int preferredHeight() const;

    // Timerから流し込むステム別ピーク（リニア振幅・ポストゲイン）
    void updatePeaks (const std::function<float (int)>& peakForIndex);

    std::function<void()> onConfigChanged; // グループ切替・M/S変更（親がengineへ反映する）

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    struct GroupState
    {
        std::vector<bool> mute, solo;
    };

    void rebuildButtons();

    std::vector<StemCache::Group> groups;
    std::vector<GroupState> states;
    int groupIndex = -1; // -1 = オリジナル

    std::vector<float> displayLevels; // 減衰込みの表示値
    std::vector<juce::Rectangle<int>> tabRects, muteRects, soloRects, meterRects, rowRects;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemPanel)
};
