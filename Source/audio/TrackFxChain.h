#pragma once

#include <atomic>

#include "../shared/AnalyzerTap.h"
#include "../shared/CompParams.h"
#include "../shared/EqParams.h"
#include "../shared/PlaybackSnapshot.h" // TrackParams（loadSettings が atomic から読む）
#include "../shared/LofiParams.h"
#include "../shared/SatParams.h"
#include "TrackComp.h"
#include "TrackEq.h"
#include "TrackLofi.h"
#include "TrackSaturator.h"

// トラックFXチェーン（EQ→アナライザタップ→Comp→Sat→Lo-fi）の共通適用。
// plan: docs/plans/2026-08-16-1254-fx-batch1-foundation.md Phase 4
//（Sat追加: docs/plans/2026-08-16-2058-fx-batch3-saturation-lofi.md Phase 3）
//
// 以前は6経路（モノ/ステレオ/MIDI × RT/バウンス）が同じ処理列を別々に手書きしており、
// 新FXを足すたび全経路へのコピーと事故リスク（GOTCHAS.md の掛け算位置ズレ）が増えていた。
// ここに集約したのは「新トラックFXを追加するとき必ず触る場所」の3点:
//   1. evaluateActivity — active経路判定（高速パスに入って新FXを丸ごと迂回する事故を防ぐ）
//   2. process — FXの適用順（EQ→タップ→Comp。挿し込み位置の契約）
//   3. producesTail — バウンスのリングアウト対象（stateful FXの申告先）
// チャンネル構成・pan法則・ミックスへの合流は経路ごとに本当に違うので呼び出し側に残る。
//
// RT安全: 全関数 確保なし・noexcept。std::function・キャプチャラムダは使わない
// （オーディオコールバック内での確保リスク。GOTCHAS.md の RT 規則）
namespace TrackFx
{
// 呼び出し側が読み込んだプレーン値（RTはatomicから、バウンスはTrackRenderの固定値から）
struct Settings
{
    bool eqOn = false;
    Eq::Values eqTargets;
    bool compOn = false;
    Comp::Values compTargets;
    bool satOn = false;
    Sat::Values satTargets;
    bool lofiOn = false;
    Lofi::Values lofiTargets;
};

inline Settings loadSettings (const TrackParams& params) noexcept
{
    return { params.eqEnabled.load(), Eq::loadAll (params.eqBands),
             params.compEnabled.load(), Comp::load (params.comp),
             params.satEnabled.load(), Sat::load (params.sat),
             params.lofiEnabled.load(), Lofi::load (params.lofi) };
}

struct Activity
{
    bool eq = false;
    bool comp = false;
    bool sat = false;
    bool lofi = false;
    bool any() const noexcept { return eq || comp || sat || lofi; }
};

// active経路判定のpolicy:
// - realtime: TrackEq/TrackComp の needsActivePath（OFF後のクロスフェード完了までactiveに
//   留まる・アナライザタップ表示中はEQ中立でもactive）
// - offline: 素の enabled && !isNeutral（バウンスは開始時にsnapToで確定済み＝状態遷移がない）
enum class Policy { realtime, offline };

inline Activity evaluateActivity (Policy policy, const TrackEq& eq, const TrackComp& comp,
                                  const TrackSaturator& sat, const TrackLofi& lofi,
                                  const Settings& settings, bool analyzerTapActive) noexcept
{
    Activity activity;
    if (policy == Policy::realtime)
    {
        activity.eq = eq.needsActivePath (settings.eqOn, settings.eqTargets, analyzerTapActive);
        activity.comp = comp.needsActivePath (settings.compOn);
        activity.sat = sat.needsActivePath (settings.satOn, settings.satTargets);
        activity.lofi = lofi.needsActivePath (settings.lofiOn, settings.lofiTargets);
    }
    else
    {
        activity.eq = settings.eqOn && ! Eq::isNeutral (settings.eqTargets);
        activity.comp = settings.compOn;
        activity.sat = settings.satOn && ! Sat::isNeutral (settings.satTargets);
        activity.lofi = settings.lofiOn && ! Lofi::isNeutral (settings.lofiTargets);
    }
    return activity;
}

// FXとpanの掛け順を決めるクリップ構成の判定（RT/バウンス共通。GOTCHAS.md の掛け算位置ズレ）:
// - モノのみ → pan前のモノ信号にFX → 等パワーpanで分配（monoFx）
// - ステレオのみ → 2chでFX → バランスpanを後段へ（prePanFx）
// - 混在 → pan法則が2種混ざり後段panにできない → pan焼き込み後にFX（対象外）
inline bool isMonoClipTrack (const std::vector<ClipPlayback>& clips) noexcept
{
    for (const auto& clip : clips)
        if (clip.audio != nullptr && clip.audio->getNumChannels() >= 2)
            return false;
    return true;
}

inline bool isStereoOnlyClipTrack (const std::vector<ClipPlayback>& clips) noexcept
{
    for (const auto& clip : clips)
        if (clip.audio != nullptr && clip.audio->getNumChannels() < 2)
            return false;
    return true;
}

// バウンスのテール（リングアウト）を生むか: 「無音入力から出力を生み続けるFX」だけが対象。
// EQ = IIRフィルタ履歴の減衰、Sat = DC除去HPFのIIR履歴（同族）。
// Lo-fi は wow=ディレイライン・noise=エンベロープ減衰に加え、Tone の biquad は IIR 履歴・
// Crush の S&H は最後の保持値を持つため、どの成分が単独で有効でもテール対象
//（＝ !isNeutral がそのまま条件。漏れると範囲終端で余韻が切れる）。
// Comp は無音入力から音を生成しないので対象外
inline bool producesTail (const Settings& settings) noexcept
{
    return (settings.eqOn && ! Eq::isNeutral (settings.eqTargets))
           || (settings.satOn && ! Sat::isNeutral (settings.satTargets))
           || (settings.lofiOn && ! Lofi::isNeutral (settings.lofiTargets));
}

// メーター用ピークのCAS max更新（UI側の exchange(0) と組。TrackParams::peakL/peakR のコメント参照）
inline void storePeakMax (std::atomic<float>& target, float value) noexcept
{
    float current = target.load();
    while (value > current && ! target.compare_exchange_weak (current, value))
    {
    }
}

// RT固有の付帯物（固定サイズのcontext。バウンスはデフォルト値のまま渡す）
struct Context
{
    AnalyzerTap* analyzerTap = nullptr; // EQ直後・Comp前・フェーダー前にタップ（nullptr = なし）
    juce::uint64 trackId = 0;           // タップの宛先トラック
    std::atomic<float>* compGrDb = nullptr;        // CompのGR書き戻し先（nullptr = 書かない）
    std::atomic<float>* compDetectorPeak = nullptr; // 検波ピークの書き戻し先（同上）
};

// FXチェーンをインプレース適用する（left 必須・right は nullptr でモノ）。
// 処理順は全経路共通の契約: EQ → アナライザタップ → Comp（＋GR書き戻し）→ Sat → Lo-fi。
// Comp後にSatを置くのは「レベルが揃った後に歪ませるとドライブ量が読める」ため、
// Lo-fi 最後尾は「仕上げの質感・ノイズをコンプに持ち上げさせない」ため（plan バッチ3）。
// gain/pan の掛け場所（FX→gain/pan の後段移動）は呼び出し側の責任
inline void process (TrackEq& eq, TrackComp& comp, TrackSaturator& sat, TrackLofi& lofi,
                     const Activity& activity,
                     const Settings& settings, float* left, float* right, int numSamples,
                     double sampleRate, juce::uint64 serial, bool timelineJumped,
                     const Context& context) noexcept
{
    if (activity.eq)
        eq.process (left, right, numSamples, sampleRate, serial, timelineJumped,
                    settings.eqOn, settings.eqTargets);
    if (context.analyzerTap != nullptr)
        context.analyzerTap->pushSamples (context.trackId, left, right, numSamples);
    if (activity.comp)
    {
        comp.process (left, right, numSamples, sampleRate, serial, timelineJumped,
                      settings.compOn, settings.compTargets);
        if (context.compGrDb != nullptr)
            storePeakMax (*context.compGrDb, comp.blockMaxGainReductionDb());
        if (context.compDetectorPeak != nullptr)
            storePeakMax (*context.compDetectorPeak, comp.blockMaxDetectorPeak());
    }
    if (activity.sat)
        sat.process (left, right, numSamples, sampleRate, serial, timelineJumped,
                     settings.satOn, settings.satTargets);
    if (activity.lofi)
        lofi.process (left, right, numSamples, sampleRate, serial, timelineJumped,
                      settings.lofiOn, settings.lofiTargets);
}
} // namespace TrackFx
