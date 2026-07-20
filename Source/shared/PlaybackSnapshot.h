#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

// トラックの音量・ミュート・ソロ。Trackモデル（メッセージスレッド）と
// PlaybackSnapshot（オーディオスレッドが参照）が shared_ptr で共有する。
// 値の変更はスナップショット再構築なしに atomic 経由で即反映される。
struct TrackParams
{
    std::atomic<float> gain { 0.8f };
    std::atomic<bool> mute { false };
    std::atomic<bool> solo { false };

    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<bool>::is_always_lock_free);
};

// エンジン用の再生スナップショット。メッセージスレッドが構築し、オーディオスレッドは読むだけ。
// shared_ptr の参照カウント操作（コピー・破棄）はメッセージスレッドでのみ起きる。
struct ClipPlayback
{
    std::shared_ptr<const juce::AudioBuffer<float>> audio; // モノラル・メモリ常駐。生存保証のため所有を共有
    juce::int64 startSample = 0;
};

struct TrackPlayback
{
    std::shared_ptr<TrackParams> params;
    std::vector<ClipPlayback> clips;
};

struct PlaybackSnapshot
{
    std::vector<TrackPlayback> tracks;
};

// GOTCHAS.md パターン3: UIイベント→構築→atomicポインタ差し替え。解放は必ずメッセージスレッド側。
//
//   メッセージスレッド: push() で新スナップショットを渡し、Timer で deleteRetired() を回す
//   オーディオスレッド: 毎ブロック acquire() で最新を取得（delete は絶対にしない）
//
// retired スロットが空いているときだけ current を差し替えるので、オーディオスレッドが
// 手放したスナップショットは必ず retired 経由でメッセージスレッドに戻って解放される。
class SnapshotExchange
{
public:
    SnapshotExchange() = default;

    ~SnapshotExchange()
    {
        // オーディオコールバック停止後（shutdownAudio 後）に呼ばれる前提
        delete pending.exchange (nullptr);
        delete retired.exchange (nullptr);
        delete current.exchange (nullptr);
    }

    // ---- メッセージスレッド専用 ----
    void push (std::unique_ptr<PlaybackSnapshot> snapshot)
    {
        // オーディオスレッドが未取得の pending は誰にも見られていないので即 delete してよい
        delete pending.exchange (snapshot.release());
    }

    void deleteRetired()
    {
        delete retired.exchange (nullptr);
    }

    // ---- オーディオスレッド専用 ----
    PlaybackSnapshot* acquire()
    {
        if (pending.load() != nullptr && retired.load() == nullptr)
        {
            if (auto* next = pending.exchange (nullptr))
            {
                retired.store (current.load());
                current.store (next);
            }
        }
        return current.load();
    }

private:
    std::atomic<PlaybackSnapshot*> pending { nullptr }; // メッセージスレッドが書く
    std::atomic<PlaybackSnapshot*> retired { nullptr }; // オーディオスレッドが書き、メッセージスレッドが解放
    std::atomic<PlaybackSnapshot*> current { nullptr }; // オーディオスレッドのみ書く

    JUCE_DECLARE_NON_COPYABLE (SnapshotExchange)
};
