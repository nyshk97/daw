#pragma once

#include <atomic>
#include <juce_core/juce_core.h>

// URL（YouTube等）から音声をダウンロードして、ローカルのWAVにするワーカー。
// yt-dlp を外部プロセスとして2回叩く: ①メタ取得で長さを見て弾く → ②本ダウンロード。
// 落ちてきたWAVの取り込みは既存の MainComponent::startImport() に渡すので、ここは
// 「URL → ローカルの音声ファイル」までを担当する。
//
// ネットワークI/Oでオーディオスレッドには一切触れないが、BounceRenderer / AudioImporter と
// 同じ形（専用スレッド＋atomic進捗＋pull型の完了通知）に揃えてある。
//
// 一時ディレクトリの所有:
//   成功Resultが takeResult() されるまでは worker が持つ。take された時点で呼び出し側へ移る。
//   失敗・キャンセル時は worker が削除して Result.tempDirectory を空にして返す（二重削除を防ぐ）。
class UrlDownloader : private juce::Thread
{
public:
    // 長さ判定に使う想定SR。中間WAVの実SRは元ソース依存で事前に判らないが、YouTubeのopus由来は
    // 48kで実測されており、44.1kソースに対しては安全側（実フレーム数はこれより少ない）に働く
    static constexpr double assumedSampleRate = 48000.0;

    struct Request
    {
        juce::String url;
    };

    enum class Status { idle, running, success, cancelled, failed };

    struct Result
    {
        Status       status = Status::idle;
        juce::File   tempDirectory;        // 成功時のみ非空。takeされたら所有権は呼び出し側へ
        juce::File   audioFile;            // tempDirectory 内のWAV
        juce::String title;                // 新規トラック名・リージョン名に使う
        juce::String errorMessage;         // failed のとき（URLマスク済み）
        bool         ytDlpMissing = false; // 不在は専用の文言を出すため区別する
    };

    UrlDownloader() : juce::Thread ("URL Downloader") {}
    ~UrlDownloader() override;

    bool start (Request&& requestToRun);

    void cancel() { signalThreadShouldExit(); }

    void cancelAndWait()
    {
        signalThreadShouldExit();
        stopThread (-1);
    }

    Status status() const { return currentStatus.load(); }
    float  progress() const { return progressValue.load(); }

    // 最後に起動した yt-dlp のプロセスグループID（0 = 未起動）。
    // キャンセル後に「ツリーごと死んだか」を名前でなくPGIDで検証するために公開する
    //（名前で数えるとユーザーが別用途で動かしている ffmpeg 等を誤検知する）
    int pgid() const { return lastPgid.load(); }

    // 終了後（status != running）に結果を取り出して idle に戻す。
    // 成功Resultを take した側が tempDirectory を削除する責任を負う
    Result takeResult();

    // yt-dlp の在り処（不在なら空File）。UIの事前チェックにも使えるよう公開する
    static juce::File findYtDlp();

private:
    void run() override;
    Status download();
    Status fail (const juce::String& message, bool missing = false);

    Request request;
    Result  result;
    std::atomic<Status> currentStatus { Status::idle };
    std::atomic<float>  progressValue { 0.0f };
    std::atomic<int>    lastPgid { 0 };

    JUCE_DECLARE_NON_COPYABLE (UrlDownloader)
};
