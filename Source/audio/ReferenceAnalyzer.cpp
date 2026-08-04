#include "ReferenceAnalyzer.h"

#include "shared/Log.h"
#include "shared/SpawnedProcess.h"

namespace
{
juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }

// card.json の global.key {root, mode} → "D major"。無ければ空
juce::String keyTextFromCard (const juce::var& card)
{
    const auto key = card.getProperty ("global", {}).getProperty ("key", {});
    if (! key.isObject())
        return {};
    const auto root = key.getProperty ("root", {}).toString();
    const auto mode = key.getProperty ("mode", {}).toString();
    return root.isNotEmpty() && mode.isNotEmpty() ? root + " " + mode : juce::String();
}
}

void ReferenceAnalyzer::run()
{
    const auto startMs = juce::Time::getMillisecondCounter();
    result = {};
    const auto finalStatus = analyze();
    result.status = finalStatus;

    Log::info ("reference.analyze.end",
               "status=" + juce::String (static_cast<int> (finalStatus))
                   + " elapsedMs=" + juce::String (juce::Time::getMillisecondCounter() - startMs));

    // status の更新は run() の最後の処理にする（UrlDownloader と同じ規約。
    // 先に公開すると poll 側が takeResult() を呼び、waitForThreadToExit 前提が崩れる）
    currentStatus.store (finalStatus);
}

ReferenceAnalyzer::Status ReferenceAnalyzer::analyze()
{
    // analyze.sh は自身の位置基準（cd "$(dirname "$0")"）なので cwd に依存しない。
    // 引数のリファレンスフォルダは絶対パスで渡す
    const juce::StringArray argv { "/bin/bash", request.script.getFullPathName(),
                                   request.referenceFolder.getFullPathName() };

    SpawnedProcess proc;
    if (! proc.start (argv))
        return fail (jp (u8"analyze.sh を起動できませんでした。"));

    lastPgid.store (proc.pgid());
    Log::info ("reference.analyze.spawn", "pgid=" + juce::String (proc.pgid())
                                              + " folder=" + request.referenceFolder.getFullPathName());

    juce::StringArray stderrTail; // 失敗時の理由抽出用（末尾だけ保持）
    const bool finished = proc.readUntilFinished (
        [this] { return threadShouldExit(); },
        [this] (const juce::String& line)
        {
            const auto trimmed = line.trim();
            if (trimmed.isEmpty())
                return;
            {
                const juce::ScopedLock sl (lineLock);
                lastLine = trimmed;
            }
            // card.py の「カードを生成しない（〜）」申告を完了ダイアログ用に拾う
            if (trimmed.contains (jp (u8"カードを生成しない")))
                result.noCardReason = trimmed;
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
        // demucs 等の進捗行でなくエラーらしい行を優先して1行選ぶ
        juce::String detail;
        for (int i = stderrTail.size(); --i >= 0;)
        {
            const auto line = stderrTail[i].trim();
            if (line.isEmpty())
                continue;
            if (detail.isEmpty())
                detail = line; // 最後の非空行（フォールバック）
            if (line.containsIgnoreCase ("error") || line.contains ("ERROR"))
            {
                detail = line;
                break;
            }
        }
        return fail (jp (u8"分析に失敗しました（exit ") + juce::String (proc.exitCode()) + ")\n" + detail);
    }

    // card.json（ゲート落ちで無いこともある。無いこと自体は失敗ではない）
    const auto cardFile = request.referenceFolder.getChildFile ("card.json");
    if (cardFile.existsAsFile())
    {
        const auto card = juce::JSON::parse (cardFile.loadFileAsString());
        const auto bpm = card.getProperty ("global", {}).getProperty ("bpm", {});
        if (bpm.isDouble() || bpm.isInt())
        {
            result.hasCard = true;
            result.bpm = (double) bpm;
            result.keyText = keyTextFromCard (card);
        }
    }
    return Status::success;
}
