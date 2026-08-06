#pragma once

#include <atomic>
#include <juce_core/juce_core.h>

// ガチャパネル「レポートを書く」のワーカー。tools/reference/report.sh（Claude Code の
// ヘッドレス実行・約8分）を外部プロセスとして走らせる。ReferenceAnalyzer と同じ形
// （専用スレッド＋atomic状態＋pull型の完了通知）。readUntilFinished() は終了まで
// ブロックする API なので、UI スレッドは status()/currentLine() だけをポーリングする。
// 進捗は stream_progress.py が流す stdout の最新行をそのまま出す。
class ReferenceReportGenerator : private juce::Thread
{
public:
    struct Request
    {
        juce::File script;          // ~/daw/tools/reference/report.sh（呼び出し側が存在確認済み）
        juce::File referenceFolder; // <project>/references/<name>（analysis/gates.json あり）
    };

    enum class Status { idle, running, success, cancelled, failed };

    struct Result
    {
        Status status = Status::idle;
        juce::String errorMessage; // failed のとき
    };

    ReferenceReportGenerator() : juce::Thread ("Reference Report Generator") {}

    ~ReferenceReportGenerator() override { cancelAndWait(); }

    bool start (Request&& requestToRun)
    {
        // idle 限定（スレッド生死でなく状態で見る）: 完了直後〜takeResult() までの間に
        // 開始を許すと、前回の result と対象フォルダを黙って上書きし、poll 側が拾う前の
        // 完了通知（トースト・再読込）が消える
        if (currentStatus.load() != Status::idle || isThreadRunning())
            return false;
        request = std::move (requestToRun);
        result = {};
        startedAt = juce::Time::getCurrentTime();
        {
            const juce::ScopedLock sl (lineLock);
            lastLine.clear();
        }
        currentStatus.store (Status::running);
        if (! startThread())
        {
            currentStatus.store (Status::idle);
            request = {};
            return false;
        }
        return true;
    }

    void cancel() { signalThreadShouldExit(); }

    void cancelAndWait()
    {
        signalThreadShouldExit();
        stopThread (-1);
    }

    Status status() const { return currentStatus.load(); }

    // 生成対象のフォルダ（running 中の表示・完了時の再読込対象。idle では空File）
    juce::File targetFolder() const { return currentStatus.load() == Status::idle ? juce::File() : request.referenceFolder; }

    // 経過表示用: 開始時刻（経過分の算出）と stream_progress の最新行
    juce::Time startTime() const { return startedAt; }
    juce::String currentLine() const
    {
        const juce::ScopedLock sl (lineLock);
        return lastLine;
    }

    // 最後に起動したプロセスグループID（0 = 未起動）。キャンセル検証用（ReferenceAnalyzerと同じ）
    int pgid() const { return lastPgid.load(); }

    Result takeResult()
    {
        waitForThreadToExit (-1);
        auto taken = std::move (result);
        result = {};
        currentStatus.store (Status::idle);
        request = {};
        return taken;
    }

private:
    void run() override;
    Status generate();
    void sweepStaleNext(); // SIGKILL 経路の残骸 .next を、ロックが取れたときだけ削除する
    Status fail (const juce::String& message)
    {
        result.errorMessage = message;
        return Status::failed;
    }

    Request request;
    Result result;
    juce::Time startedAt;
    std::atomic<Status> currentStatus { Status::idle };
    std::atomic<int> lastPgid { 0 };
    mutable juce::CriticalSection lineLock;
    juce::String lastLine;

    JUCE_DECLARE_NON_COPYABLE (ReferenceReportGenerator)
};
