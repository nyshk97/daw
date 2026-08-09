#pragma once

#include <atomic>
#include <juce_core/juce_core.h>

// リージョン右クリック「リファレンスとして分析」のワーカー。
// tools/reference/analyze.sh を外部プロセスとして走らせ（demucs 分離を含み並列で約2分）、
// 完了時に card.json から完了ダイアログ用の BPM / キーを拾う。
// UrlDownloader と同じ形（専用スレッド＋atomic状態＋pull型の完了通知）。
// 進捗は analyze.py が stdout に出す加重進捗行（==> P% | …）を AnalyzeProgress::parse で
// 割合と表示行に分けて公開する。
class ReferenceAnalyzer : private juce::Thread
{
public:
    struct Request
    {
        juce::File script;          // ~/daw/tools/reference/analyze.sh（呼び出し側が存在確認済み）
        juce::File referenceFolder; // <project>/references/<name>（track.wav 配置済み）
    };

    enum class Status { idle, running, success, cancelled, failed };

    struct Result
    {
        Status status = Status::idle;
        juce::String errorMessage; // failed のとき
        // 完了ダイアログ用（card.json から）。カードはゲート落ちで生成されないことがある
        bool hasCard = false;
        double bpm = 0.0;
        juce::String keyText;      // "D major" 等。キーのゲート落ちで省略されたら空
        juce::String noCardReason; // カード非生成の申告行（analyze.sh の stdout から）
    };

    ReferenceAnalyzer() : juce::Thread ("Reference Analyzer") {}

    ~ReferenceAnalyzer() override
    {
        cancelAndWait();
    }

    bool start (Request&& requestToRun)
    {
        if (isThreadRunning())
            return false;
        request = std::move (requestToRun);
        result = {};
        lastPgid.store (0);
        progressFraction.store (-1.0f);
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

    // 進捗表示用: analyze.sh の stdout の最新行（「==> 7/16 完了 ／ 実行中: …」等。進捗率は剥がし済み）
    juce::String currentLine() const
    {
        const juce::ScopedLock sl (lineLock);
        return lastLine;
    }

    // 加重進捗 0.0〜1.0。負なら未取得（オーバーレイは不定型バーのまま）
    float progress() const { return progressFraction.load(); }

    // 最後に起動したプロセスグループID（0 = 未起動）。キャンセル検証用（UrlDownloaderと同じ）
    int pgid() const { return lastPgid.load(); }

    Result takeResult()
    {
        waitForThreadToExit (-1);
        auto taken = std::move (result);
        result = {};
        currentStatus.store (Status::idle);
        return taken;
    }

private:
    void run() override;
    Status analyze();
    Status fail (const juce::String& message)
    {
        result.errorMessage = message;
        return Status::failed;
    }

    Request request;
    Result result;
    std::atomic<Status> currentStatus { Status::idle };
    std::atomic<int> lastPgid { 0 };
    std::atomic<float> progressFraction { -1.0f };
    mutable juce::CriticalSection lineLock;
    juce::String lastLine;

    JUCE_DECLARE_NON_COPYABLE (ReferenceAnalyzer)
};
