#include "StemSeparator.h"

#include <unistd.h> // getpid

#include "shared/Log.h"

bool StemSeparator::startSeparation (const Request& r)
{
    if (isThreadRunning() || currentStatus.load() == Status::running)
        return false;

    const auto dir = StemCache::identityDir (r.identity);
    // プロセス間ロック（mkdir）。取得できなければ別プロセスが分離中
    if (! StemCache::acquireLock (dir, (juce::int64) getpid()))
        return false;

    request = r;
    currentStatus.store (Status::running);
    startThread();
    return true;
}

bool StemSeparator::consumeResult (StemCache::SourceIdentity& outIdentity, bool& outSuccess)
{
    const auto s = currentStatus.load();
    if (s != Status::success && s != Status::failed)
        return false;
    if (isThreadRunning())
        return false; // run()の後始末完了を待つ
    outIdentity = request.identity;
    outSuccess = (s == Status::success);
    currentStatus.store (Status::idle);
    return true;
}

void StemSeparator::run()
{
    const auto dir = StemCache::identityDir (request.identity);
    bool ok = false;

    {
        SpawnedProcess process;
        const juce::StringArray argv {
            "/bin/bash",
            request.script.getFullPathName(),
            request.input.getFullPathName(),
            dir.getFullPathName(),
            request.python.getFullPathName(),
            juce::String (juce::roundToInt (request.sampleRate)),
            juce::String (request.lengthSamples),
        };
        if (process.start (argv))
        {
            // 失敗診断用にstderr/stdoutの末尾だけ保持する（全量ログはdemucsの進捗で溢れるため。
            // モデル不足・ffmpeg不足・デコード失敗の原因行はほぼ末尾に出る）
            constexpr int tailLines = 40;
            juce::StringArray tail;
            const auto keep = [&tail] (const juce::String& line)
            {
                if (line.trim().isEmpty())
                    return;
                tail.add (line);
                if (tail.size() > tailLines)
                    tail.remove (0);
            };
            const bool finished = process.readUntilFinished (
                [this] { return threadShouldExit(); }, keep, keep);
            if (! finished && ! threadShouldExit())
                Log::warn ("separate.read_interrupted");
            if (threadShouldExit())
                process.terminate(); // アプリ終了: 子プロセスツリーを停止（planの契約）
            ok = finished && process.exitCode() == 0;
            if (! ok)
            {
                Log::warn ("separate.process_failed", "exit=" + juce::String (process.exitCode()));
                for (const auto& line : tail)
                    Log::warn ("separate.output", line);
            }
        }
        else
        {
            Log::error ("separate.spawn_failed", "script=" + request.script.getFullPathName());
        }
    }

    // 成果の検証（identity・complete・全WAV実在）まで通って成功
    if (ok)
    {
        const auto manifest = StemCache::parseManifest (dir.getChildFile ("manifest.json"));
        ok = StemCache::manifestUsable (manifest, request.identity, dir);
        if (! ok)
            Log::warn ("separate.manifest_invalid");
    }

    if (ok)
    {
        // 成功時の後始末: 非参照run＋孤児identity（元音源の上書きで置き換わった旧identity）の削除。
        // 現identityのrun削除は**ロックを保持したまま**行う（解放後だと、その間に別プロセスが
        // ロックを取って作り始めた新runを「manifest非参照」として消してしまう）
        StemCache::cleanupAfterSuccess (StemCache::stemsRoot(), request.identity,
                                        (juce::int64) getpid(),
                                        StemCache::defaultPidAlive, StemCache::defaultPgidAlive,
                                        juce::Time::getCurrentTime());
    }

    StemCache::releaseLock (dir); // 正常・失敗・キャンセルのどの経路でも解放

    currentStatus.store (ok ? Status::success : Status::failed);
}
