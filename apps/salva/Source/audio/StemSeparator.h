#pragma once

#include <atomic>

#include <juce_core/juce_core.h>

#include "shared/SpawnedProcess.h"
#include "shared/StemCache.h"

// ステム分離ジョブの実行（GOTCHASのpull型ワーカー: juce::Thread + atomic status、
// UI側のTimerがポーリングする）。
//
// 流れ: startSeparation（メッセージスレッド）でidentityロックを取得してスレッド起動 →
// run()（ワーカースレッド）が separate.sh を SpawnedProcess で実行 → 終了後に
// manifestを検証してロック解放 → UIが consumeResult で結果を1回だけ受け取る。
// ジョブはsource identityを保持し、UIは「現在開いているファイルと一致する場合のみ」反映する
class StemSeparator : private juce::Thread
{
public:
    enum class Status { idle, running, success, failed };

    StemSeparator() : juce::Thread ("salva separate") {}
    ~StemSeparator() override
    {
        signalThreadShouldExit();
        stopThread (10000); // run()側がterminate（プロセスグループごとSIGTERM→SIGKILL）する
    }

    struct Request
    {
        juce::File input;
        StemCache::SourceIdentity identity;
        juce::File script;              // バンドル同梱の separate.sh
        juce::File python;              // <venv>/bin/python の絶対パス（venv解決はアプリ側の責務）
        double sampleRate = 0.0;        // 元音源のSR（JUCEが読んだ値。scriptは元音源をデコードしない）
        juce::int64 lengthSamples = 0;  // 元音源のサンプル長（同上）
    };

    // ロック取得→スレッド起動。falseなら開始できず（別プロセスが分離中 or 実行中）
    bool startSeparation (const Request& r);

    Status status() const { return currentStatus.load(); }

    // success/failedを1回だけ消費してidleへ戻す。trueを返したとき outIdentity/outSuccess が有効
    bool consumeResult (StemCache::SourceIdentity& outIdentity, bool& outSuccess);

private:
    void run() override;

    Request request; // run()開始前に設定、実行中は変更しない
    std::atomic<Status> currentStatus { Status::idle };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparator)
};
