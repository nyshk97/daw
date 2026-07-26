#include "UrlDownloader.h"

#include <unistd.h> // getpid（一時ディレクトリ名に埋める）

#include "AudioImporter.h"
#include "shared/Log.h"
#include "shared/SpawnedProcess.h"
#include "shared/TempDirSweep.h"
#include "shared/YtDlpOutput.h"

namespace
{
// 進捗の内訳。メタ取得はすぐ終わるので頭に少しだけ取り、残りをダウンロードに割り当てる
constexpr float metadataShare = 0.05f;

const char* progressTemplate =
    "download:LALA_PROGRESS %(progress.downloaded_bytes)s "
    "%(progress.total_bytes)s %(progress.total_bytes_estimate)s";

// yt-dlp をどう起動しても外さない引数。
// --ignore-config: ユーザーの ~/.config/yt-dlp/config にある出力先・proxy・--exec 等が
//                  アプリの動作に混入しないようにする（再現性のため。副次的に安全側でもある）
// --no-exec:       多層防御。設定由来の --exec が仮に残っても実行させない
// --no-playlist / --playlist-items 1: 「動画＋playlist」のURLでも純playlistのURLでも1本に絞る
juce::StringArray baseArgs (const juce::File& ytDlp)
{
    return { ytDlp.getFullPathName(), "--ignore-config", "--no-exec",
             "--no-playlist", "--playlist-items", "1" };
}

juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

UrlDownloader::~UrlDownloader()
{
    cancelAndWait();

    // 成功したまま take されずにアプリが終わったケースの後始末。
    // 通常は pollUrlImport() が take して MainComponent 側に所有権が移る
    if (result.tempDirectory != juce::File())
        result.tempDirectory.deleteRecursively();
}

juce::File UrlDownloader::findYtDlp()
{
    // .app は launchd 起動でPATHが最小限（/usr/bin:/bin:/usr/sbin:/sbin）なので絶対パスで探す
    for (const auto* path : { "/opt/homebrew/bin/yt-dlp", "/usr/local/bin/yt-dlp" })
    {
        const juce::File candidate { path };
        if (candidate.existsAsFile())
            return candidate;
    }
    return {};
}

bool UrlDownloader::start (Request&& requestToRun)
{
    if (isThreadRunning())
        return false;

    request = std::move (requestToRun);
    result = {};
    progressValue.store (0.0f);
    lastPgid.store (0);
    currentStatus.store (Status::running);

    if (! startThread())
    {
        // 状態を戻さないと status が running のままになり、進捗オーバーレイが永久に閉じない
        currentStatus.store (Status::idle);
        request = {};
        return false;
    }
    return true;
}

UrlDownloader::Result UrlDownloader::takeResult()
{
    // run() の最終行で status を公開しても、JUCE がスレッドハンドルを閉じるのは run() から
    // 戻った後。その隙間では isThreadRunning() がまだ true なので、ここで確実に見送ってから
    // result を持ち出す（status が terminal になっていれば run() は終わっているので即座に返る）
    waitForThreadToExit (-1);

    auto taken = std::move (result);
    result = {};
    currentStatus.store (Status::idle);
    return taken;
}

UrlDownloader::Status UrlDownloader::fail (const juce::String& message, bool missing)
{
    result.errorMessage = message;
    result.ytDlpMissing = missing;
    return Status::failed;
}

void UrlDownloader::run()
{
    const auto startMs = juce::Time::getMillisecondCounter();

    result = {};
    const auto finalStatus = download();
    result.status = finalStatus;

    // 成功以外は自分で片付ける。呼び出し側が二重に消さないよう空にして返す
    if (finalStatus != Status::success && result.tempDirectory != juce::File())
    {
        result.tempDirectory.deleteRecursively();
        result.tempDirectory = juce::File();
        result.audioFile = juce::File();
    }

    Log::info ("url.download.end",
               "status=" + juce::String (static_cast<int> (finalStatus))
                   + " elapsedMs=" + juce::String (juce::Time::getMillisecondCounter() - startMs));

    // AudioImporter / BounceRenderer と同じ規約: status の更新は run() の最後の処理にする。
    // 先に公開すると、まだ run() の中に居るうちに pollUrlImport() が takeResult() を呼び、
    // takeResult() の jassert(! isThreadRunning()) に掛かる
    currentStatus.store (finalStatus);
}

UrlDownloader::Status UrlDownloader::download()
{
    const auto ytDlp = findYtDlp();
    if (ytDlp == juce::File())
        return fail (jp (u8"yt-dlp が見つかりません。"), true);

    // URLはサイト（host）を制限しないが、スキームは絞る。
    // yt-dlp のオプションとして解釈される文字列（--exec 等）を入口で弾くため
    if (! (request.url.startsWith ("http://") || request.url.startsWith ("https://")))
        return fail (jp (u8"http:// または https:// で始まるURLを入力してください。"));

    const auto ffmpegDir = ytDlp.getParentDirectory().getFullPathName();
    const auto maskedUrl = YtDlpOutput::redactUrls (request.url, request.url);

    // ---- ① メタ取得（長さで弾くのはダウンロード前）----
    juce::String metaOut, metaErr;
    {
        auto argv = baseArgs (ytDlp);
        argv.add ("--print");
        argv.add ("LALA_META:%(.{title,duration,is_live})j");
        argv.add ("--"); // これ以降はオプションとして解釈させない
        argv.add (request.url);

        SpawnedProcess proc;
        if (! proc.start (argv))
            return fail (jp (u8"yt-dlp を起動できませんでした。"));

        lastPgid.store (proc.pgid()); // キャンセル検証で外から見られるように残す
        Log::info ("url.meta.start", "pgid=" + juce::String (proc.pgid()) + " url=" + maskedUrl);

        const bool finished = proc.readUntilFinished (
            [this] { return threadShouldExit(); },
            [&metaOut] (const juce::String& line) { metaOut += line + "\n"; },
            [&metaErr] (const juce::String& line) { metaErr += line + "\n"; });

        if (! finished || threadShouldExit())
            return Status::cancelled;

        if (proc.exitCode() != 0)
        {
            const auto detail = YtDlpOutput::extractErrorLine (metaErr.isNotEmpty() ? metaErr : metaOut);
            Log::error ("url.meta.failed",
                        "exit=" + juce::String (proc.exitCode())
                            + " detail=" + YtDlpOutput::redactUrls (detail, request.url));
            return fail (YtDlpOutput::redactUrls (detail, request.url));
        }
    }

    const auto meta = YtDlpOutput::parseMetadata (metaOut);
    if (! meta.ok)
        return fail (jp (u8"動画の情報を取得できませんでした。"));

    if (meta.isLive)
        return fail (jp (u8"ライブ配信は取り込めません。"));
    if (! meta.hasDuration)
        return fail (jp (u8"長さが取得できない動画は取り込めません。"));

    if (meta.durationSeconds * assumedSampleRate > (double) AudioImporter::maxInputFrames)
        return fail (jp (u8"この動画は長すぎます（約34分まで）。"));

    result.title = meta.title.isNotEmpty() ? meta.title : jp (u8"ダウンロード");
    progressValue.store (metadataShare);

    // ---- ② 本ダウンロード ----
    const auto dirName = TempDirSweep::makeDirName ((int) ::getpid(), juce::Uuid().toString());
    const auto tempDir = TempDirSweep::rootDirectory().getChildFile (dirName);
    if (! tempDir.createDirectory())
        return fail (jp (u8"一時フォルダを作成できませんでした。"));

    // ここから先の失敗・キャンセルは run() が tempDirectory を消す
    result.tempDirectory = tempDir;

    juce::String dlOut, dlErr;
    {
        auto argv = baseArgs (ytDlp);
        argv.addArray (juce::StringArray {
            "--max-downloads", "1",
            "-x", "--audio-format", "wav", // ffmpegでPCM化。劣化がなく、opus/webmしか無い動画でも通る
            "--ffmpeg-location", ffmpegDir,
            "--newline",
            "--progress-template", progressTemplate,
            "-o", tempDir.getChildFile ("audio.%(ext)s").getFullPathName(),
            "--" });
        argv.add (request.url);

        SpawnedProcess proc;
        if (! proc.start (argv))
            return fail (jp (u8"yt-dlp を起動できませんでした。"));

        lastPgid.store (proc.pgid());
        Log::info ("url.download.start", "pgid=" + juce::String (proc.pgid()) + " url=" + maskedUrl);

        const bool finished = proc.readUntilFinished (
            [this] { return threadShouldExit(); },
            [this, &dlOut] (const juce::String& line)
            {
                if (const auto fraction = YtDlpOutput::parseProgress (line))
                    progressValue.store (metadataShare + (1.0f - metadataShare) * *fraction);
                else
                    dlOut += line + "\n";
            },
            [&dlErr] (const juce::String& line) { dlErr += line + "\n"; });

        if (! finished || threadShouldExit())
            return Status::cancelled;

        // --max-downloads に達したときの 101 は正常系（実測で確認済み）
        const int exitCode = proc.exitCode();
        if (exitCode != 0 && exitCode != 101)
        {
            const auto detail = YtDlpOutput::extractErrorLine (dlErr.isNotEmpty() ? dlErr : dlOut);
            Log::error ("url.download.failed",
                        "exit=" + juce::String (exitCode)
                            + " detail=" + YtDlpOutput::redactUrls (detail, request.url));
            return fail (YtDlpOutput::redactUrls (detail, request.url));
        }
    }

    // 出力ファイル名は固定と仮定せず、実際に出来たWAVを拾う
    auto wavs = tempDir.findChildFiles (juce::File::findFiles, false, "*.wav");
    if (wavs.isEmpty())
        return fail (jp (u8"音声ファイルを取り出せませんでした。"));

    std::sort (wavs.begin(), wavs.end());
    result.audioFile = wavs[0];
    for (int i = 1; i < wavs.size(); ++i)
        wavs[i].deleteFile();

    progressValue.store (1.0f);
    return Status::success;
}
