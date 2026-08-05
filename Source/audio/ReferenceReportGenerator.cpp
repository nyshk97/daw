#include "ReferenceReportGenerator.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "shared/Log.h"
#include "shared/SpawnedProcess.h"

namespace
{
juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

void ReferenceReportGenerator::run()
{
    const auto startMs = juce::Time::getMillisecondCounter();
    result = {};
    const auto finalStatus = generate();
    result.status = finalStatus;

    // キャンセル（SIGTERM→SIGKILL）や異常終了で report.sh の trap が走らなかった場合の
    // 残骸 .next を回収する。ロック（report.sh と同じ .report.lock への flock）が取れた
    // ときだけ触る — ターミナルからの手実行が並走していたらロックが取れず、何もしない
    if (finalStatus != Status::success)
        sweepStaleNext();

    Log::info ("report.generate.end",
               "status=" + juce::String (static_cast<int> (finalStatus))
                   + " elapsedMs=" + juce::String (juce::Time::getMillisecondCounter() - startMs));

    // status の更新は run() の最後の処理にする（ReferenceAnalyzer と同じ規約。
    // 先に公開すると poll 側が takeResult() を呼び、waitForThreadToExit 前提が崩れる）
    currentStatus.store (finalStatus);
}

ReferenceReportGenerator::Status ReferenceReportGenerator::generate()
{
    // report.sh は自身の位置基準（TOOLS）＋引数の絶対パスで動くので cwd に依存しない
    const juce::StringArray argv { "/bin/bash", request.script.getFullPathName(),
                                   request.referenceFolder.getFullPathName() };

    SpawnedProcess proc;
    if (! proc.start (argv))
        return fail (jp (u8"report.sh を起動できませんでした。"));

    lastPgid.store (proc.pgid());
    Log::info ("report.generate.spawn", "pgid=" + juce::String (proc.pgid())
                                            + " folder=" + request.referenceFolder.getFullPathName());

    juce::StringArray stderrTail; // 失敗時の理由抽出用（末尾だけ保持）
    const bool finished = proc.readUntilFinished (
        [this] { return threadShouldExit(); },
        [this] (const juce::String& line)
        {
            const auto trimmed = line.trim();
            if (trimmed.isEmpty())
                return;
            const juce::ScopedLock sl (lineLock);
            lastLine = trimmed;
        },
        [&stderrTail] (const juce::String& line)
        {
            stderrTail.add (line);
            while (stderrTail.size() > 20)
                stderrTail.remove (0);
        });

    if (! finished || threadShouldExit())
        return Status::cancelled;

    if (proc.exitCode() != 0)
    {
        // report.sh 自身の理由行（ロック競合・妥当性検査落ち等）は stderr の末尾に出る
        juce::String detail;
        for (int i = stderrTail.size(); --i >= 0;)
        {
            const auto line = stderrTail[i].trim();
            if (line.isNotEmpty())
            {
                detail = line;
                break;
            }
        }
        return fail (jp (u8"レポート生成に失敗しました（exit ")
                     + juce::String (proc.exitCode()) + ")\n" + detail);
    }
    return Status::success;
}

void ReferenceReportGenerator::sweepStaleNext()
{
    const auto lockFile = request.referenceFolder.getChildFile (".report.lock");
    const auto next = request.referenceFolder.getChildFile ("report.md.next");
    const int fd = ::open (lockFile.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
        return;
    if (::flock (fd, LOCK_EX | LOCK_NB) == 0)
    {
        if (next.existsAsFile())
        {
            next.deleteFile();
            Log::info ("report.generate.sweep", "removed=" + next.getFullPathName());
        }
        ::flock (fd, LOCK_UN);
    }
    ::close (fd);
}
