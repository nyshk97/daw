#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <juce_events/juce_events.h>

#include "../shared/ClipDomains.h"
#include "../shared/RenderedDomain.h"

// 移調・タイムストレッチのバックグラウンドレンダラー＋指紋キーのキャッシュ。
// **メッセージスレッドが操作の入口**（requestSync / lookup / コールバック）で、レンダリング
// 本体（ClipStretcher）だけをワーカースレッドで走らせる。結果はクリップへ「送る」のではなく、
// コールバック側（ClipDomains::attachRenderResult）がクリップの指紋で引いて装着する。
//
// 寿命: デストラクタで停止要求 → join。投げ済みの完了通知は AsyncUpdater ごと消えるので、
// 破棄後の this に触れるコールバックは残らない（MainComponent のメンバとして持ち、
// プロジェクト切替 = MainComponent ごと作り直す前提）。
// 設計の真実の源: docs/plans/2026-08-18-1028-audio-transpose-stretch.md
class RenderCache : private juce::Timer,
                    private juce::AsyncUpdater
{
public:
    using Request = ClipDomains::Request;

    // 値の変化が落ち着いてから要求を積むデバウンス。スライダーの連続変化は全部が別指紋に
    // なるため、同一指紋の畳み込みでは滞留を防げない（planの「要求の間引き」）
    static constexpr int debounceMs = 150;
    // キャッシュのバイト上限（レンダー結果＋原音の強参照ぶん）。上限が縛るのは
    // 「再利用のために取っておく分」だけで、使用中のクリップは自分の shared_ptr を持つので
    // 追い出しても音は消えない
    static constexpr juce::int64 maxCacheBytes = (juce::int64) 512 * 1024 * 1024;

    RenderCache() = default;
    ~RenderCache() override;

    // ---- メッセージスレッド専用 ----

    // 待機キューの真実の源。「全クリップの要求指紋の集合」を作り直して返す
    //（ClipDomains::collectRequests を束ねたもの。キャッシュ済みの装着もこの中で行われる）
    std::function<std::vector<Request>()> collectRequests;
    // レンダー完了（メッセージスレッドで呼ばれる）。装着は受け手の責務
    std::function<void (const std::shared_ptr<const RenderedDomain>&)> onRenderReady;
    // レンダー失敗（同上）。巻き戻し・dirty 化・トーストは受け手の責務
    std::function<void (const RenderFingerprint&)> onRenderFailed;

    void requestSync(); // debounceMs 後に syncNow()（値が落ち着いてから積む）
    void syncNow();     // 即時に要求集合を作り直してワーカーへ渡す（読込時の一括・undo直後）

    std::shared_ptr<const RenderedDomain> lookup (const RenderFingerprint& fingerprint);

    // ---- テスト用（アプリ本体では使わない）----
    bool isIdle() const;                  // 待機・実行中・未配達の完了なし
    bool waitForRenders (int timeoutMs);  // 待機＋実行中が空になるまでブロック
    void drainCompletedNow();             // handleAsyncUpdate と同じ配達処理を同期で行う

private:
    void timerCallback() override { stopTimer(); syncNow(); }
    void handleAsyncUpdate() override { drainCompletedNow(); }
    void workerLoop();
    void ensureWorkerStarted();
    void evictOverBudget();

    struct Completed
    {
        RenderFingerprint fingerprint;
        std::shared_ptr<const RenderedDomain> domain; // nullptr = 失敗
    };

    // ワーカーと共有する状態（mutex 保護）
    mutable std::mutex mutex;
    std::condition_variable wakeWorker;
    std::vector<Request> pendingJobs;
    bool jobRunning = false;
    RenderFingerprint runningFingerprint; // jobRunning のときだけ有効（sync の再キュー除外用）
    bool stopRequested = false;
    std::vector<Completed> completed;
    std::thread worker;

    // ---- 以下はメッセージスレッド専用（ワーカーは触らない）----
    struct Entry
    {
        std::shared_ptr<const RenderedDomain> domain;
        juce::int64 bytes = 0;
        juce::uint64 lastUse = 0;
    };
    std::map<RenderFingerprint, Entry> cache;
    juce::int64 cacheBytes = 0;
    juce::uint64 useCounter = 0;

    JUCE_DECLARE_NON_COPYABLE (RenderCache)
};
