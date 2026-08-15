#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "shared/StemCache.h"

// ステムM/Sパネル（2026-08-15確定モック案B: 波形右の固定幅カラム）。
// タブ（ORIGINAL/4 STEMS/6 STEMS）を縦に積み、その下にステム行（スウォッチ・名前・
// ミニメーター・M/S）。固定幅なのでタブ切替で波形のレイアウトが一切動かない。
// ORIGINAL選択中は直近グループの行をディム表示で残す（何が分離済みかは見え続ける）。
// 競合規則（Solo集合＋Mute優先）の計算は shared/StemMix.h、ここは表示と操作だけ
class StemPanel : public juce::Component
{
public:
    static constexpr int preferredWidth = 200; // 右カラムの固定幅

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
    int groupIndex = -1;      // -1 = オリジナル
    int lastActiveGroup = 0;  // ORIGINAL中にディム表示する直近グループ

    std::vector<float> displayLevels; // 減衰込みの表示値
    std::vector<juce::Rectangle<int>> tabRects, muteRects, soloRects, meterRects, rowRects;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemPanel)
};
