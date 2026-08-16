#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/MasterMeterStats.h"
#include "../shared/PlaybackSnapshot.h"

// 下部エディタ（FxDetailView）に載せるMaster Limiterの操作UI＋マスターメーター群。
// plan: docs/plans/2026-08-16-1523-fx-batch2-meters-limiter.md（レイアウトは2026-08-16の
// HTMLモック 案A「左=操作/右=計測」で確定）
//
// 構成:
//   左   = 3ノブ（Gain/Ceiling/Release）＋「Lookahead: 2ms（固定）」静的ラベル
//   中央 = GR縦メーター（0〜12dB。掛かり具合＝Gainの突っ込みの結果が隣に見える）
//   右   = LUFS横バー（-24〜0・short-term塗り＋integrated▲マーカー・-14/-9ターゲットライン）
//          ＋数値LCD（SHORT TERM / INTEGRATED / TRUE PEAK max）＋相関バー（-1〜+1）
//
// 音楽的な読み方（バーに焼き込む知識）:
//   -14 = 配信サービスの正規化境界（これより大きい曲は下げられる）
//   -9  = hiphopの音圧帯の入口（正規化で下げられても音の密度は残る）
//
// モデル（masterParams->limiter の atomic）への書き込みはこのクラスが直接行い
//（必ず Limiter::normalized を通す。ドラッグ中も即時反映＝音が追従する）、
// dirty化は onEdited で MainComponent に委ねる（Limiterはフェーダー・EQ/Compと同じくundo対象外）。
//
// メーター値は MainComponent のメータータイマーが30Hzで pushMeters() 配布する
//（リング・atomicはここでは読まない。リングの読み手はMainComponentの一箇所だけ）
class LimiterEditorView : public juce::Component
{
public:
    LimiterEditorView();

    // 表示対象（Masterの共有TrackParams）。nullptr = 空表示
    void setMaster (TrackParams* masterParamsToShow);
    TrackParams* shownMaster() const { return master; }

    // MainComponentのメータータイマーから30Hzで呼ばれる。
    // feed = 集約済みのLUFS/相関/TP・grDb = LimiterのブロックGR（正の減衰量dB）
    void pushMeters (const MasterMeterFeed& feed, float grDb);

    // 全変更経路（ノブ）で呼ぶ
    std::function<void()> onEdited;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void applyToModel();  // スライダー→normalized→store（dirty化は呼び出し側）
    void loadFromModel(); // atomic→スライダー（通知なし）
    void configureKnob (juce::Slider& slider, double min, double max, double step,
                        double defaultValue, double skewMidpoint);

    static constexpr float grRangeDb = 12.0f;    // GRメーターのスケール（0〜-12dB）
    static constexpr float lufsMin = -24.0f;     // LUFSバーの左端
    static constexpr float lufsMax = 0.0f;       // 右端

    TrackParams* master = nullptr;

    juce::Slider gainSlider, ceilingSlider, releaseSlider;
    bool loadingFromModel = false;

    MasterMeterFeed meterFeed;
    float currentGrDb = 0.0f;

    juce::Rectangle<int> grMeterArea, lufsBarArea, correlationArea, readoutColumn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterEditorView)
};
