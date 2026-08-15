#include "StemCache.h"

#include <cerrno>
#include <csignal>
#include <sys/stat.h>
#include <sys/types.h>

namespace
{
juce::File lockDir (const juce::File& identityDirectory) { return identityDirectory.getChildFile ("lock"); }

// JSONを一時ファイル→renameで置く（書きかけを読まれない）
void writeJsonAtomic (const juce::File& target, const juce::var& json)
{
    const auto tmp = target.getSiblingFile (target.getFileName() + ".tmp-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    tmp.replaceWithText (juce::JSON::toString (json), false, false, "\n");
    tmp.moveFileTo (target);
}
} // namespace

namespace StemCache
{
juce::File stemsRoot()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support/salva/stems");
}

juce::File identityDir (const SourceIdentity& id)
{
    return stemsRoot().getChildFile (id.hash());
}

Manifest parseManifest (const juce::File& manifestFile)
{
    Manifest m;
    const auto parsed = juce::JSON::parse (manifestFile.loadFileAsString());
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr)
        return m;

    m.version = (int) obj->getProperty ("contractVersion");
    m.model = obj->getProperty ("model").toString();
    m.sampleRate = (double) obj->getProperty ("sampleRate");
    m.lengthSamples = (juce::int64) obj->getProperty ("lengthSamples");
    m.status = obj->getProperty ("status").toString();
    m.runRel = obj->getProperty ("run").toString();

    if (auto* src = obj->getProperty ("source").getDynamicObject())
    {
        m.source.path = src->getProperty ("path").toString();
        m.source.size = (juce::int64) src->getProperty ("size");
        m.source.mtimeMs = (juce::int64) src->getProperty ("mtimeMs");
    }

    if (auto* groups = obj->getProperty ("groups").getArray())
    {
        for (const auto& gv : *groups)
        {
            auto* g = gv.getDynamicObject();
            if (g == nullptr)
                continue;
            Group group;
            group.id = g->getProperty ("id").toString();
            group.displayName = g->getProperty ("name").toString();
            if (auto* stems = g->getProperty ("stems").getArray())
            {
                for (const auto& sv : *stems)
                {
                    if (auto* s = sv.getDynamicObject())
                        group.stems.push_back ({ s->getProperty ("name").toString(),
                                                 s->getProperty ("file").toString() });
                }
            }
            if (! group.stems.empty())
                m.groups.push_back (std::move (group));
        }
    }

    m.valid = m.version > 0 && ! m.groups.empty() && m.lengthSamples > 0 && m.sampleRate > 0.0;
    return m;
}

bool manifestUsable (const Manifest& m, const SourceIdentity& current, const juce::File& identityDirectory)
{
    if (! m.valid || m.version != contractVersion || m.status != "complete")
        return false;
    if (! (m.source == current))
        return false;
    for (const auto& g : m.groups)
        for (const auto& s : g.stems)
            if (! identityDirectory.getChildFile (s.relPath).existsAsFile())
                return false;
    return true;
}

bool acquireLock (const juce::File& identityDirectory, juce::int64 ownerPid)
{
    identityDirectory.createDirectory();
    const auto dir = lockDir (identityDirectory);
    // mkdirは原子操作。既存なら失敗（=別プロセスが分離中 or 死んだlock）
    if (mkdir (dir.getFullPathName().toRawUTF8(), 0755) != 0)
        return false;
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("pid", ownerPid);
    writeJsonAtomic (dir.getChildFile ("owner.json"), juce::var (obj));
    return true;
}

void releaseLock (const juce::File& identityDirectory)
{
    lockDir (identityDirectory).deleteRecursively();
}

LockInfo readLock (const juce::File& identityDirectory)
{
    LockInfo info;
    const auto dir = lockDir (identityDirectory);
    if (! dir.isDirectory())
        return info;
    info.exists = true;
    info.lastModified = dir.getLastModificationTime();

    // JSON::parseの戻りvarは名前を付けて生かしておく（temporaryのままgetDynamicObject()すると
    // if本体でdangling参照になる）
    const auto ownerVar = juce::JSON::parse (dir.getChildFile ("owner.json").loadFileAsString());
    if (auto* owner = ownerVar.getDynamicObject())
        info.ownerPid = (juce::int64) owner->getProperty ("pid");

    const auto workerFile = dir.getChildFile ("worker.json");
    const auto workerVar = juce::JSON::parse (workerFile.loadFileAsString());
    if (auto* worker = workerVar.getDynamicObject())
    {
        info.workerRecorded = true;
        info.workerPid = (juce::int64) worker->getProperty ("pid");
        info.workerPgid = (juce::int64) worker->getProperty ("pgid");
        info.lastModified = juce::jmax (info.lastModified, workerFile.getLastModificationTime());
    }
    return info;
}

bool defaultPidAlive (juce::int64 pid)
{
    if (pid <= 0)
        return false;
    return kill ((pid_t) pid, 0) == 0 || errno == EPERM;
}

bool defaultPgidAlive (juce::int64 pgid)
{
    if (pgid <= 0)
        return false;
    return kill ((pid_t) -pgid, 0) == 0 || errno == EPERM;
}

bool lockIsAlive (const LockInfo& info, const PidAlive& pidAlive, const PgidAlive& pgidAlive, juce::Time now)
{
    if (! info.exists)
        return false;
    if (pidAlive (info.ownerPid))
        return true; // アプリが生きている（spawn前後・失敗経路の解放待ち含む）
    if (info.workerRecorded)
        return pgidAlive (info.workerPgid); // アプリ死亡でもworkerツリーが残っていれば生
    // worker未記録の未完成lock: 安全側で保持。ただし1時間より古ければ解放（永久ブロック回避）
    return now - info.lastModified < juce::RelativeTime::hours (1);
}

namespace
{
// 現行manifestが参照しないrunを削除する（呼び出し側がlock保持中であることが前提）
void deleteUnreferencedRunsLocked (const juce::File& identityDirectory)
{
    const auto manifest = parseManifest (identityDirectory.getChildFile ("manifest.json"));
    for (const auto& run : identityDirectory.getChildFile ("runs").findChildFiles (juce::File::findDirectories, false))
        if (! manifest.valid || manifest.runRel != "runs/" + run.getFileName())
            run.deleteRecursively();
}
} // namespace

void sweep (const juce::File& root, juce::int64 myPid,
            const PidAlive& pidAlive, const PgidAlive& pgidAlive, juce::Time now)
{
    for (const auto& dir : root.findChildFiles (juce::File::findDirectories, false))
    {
        const auto info = readLock (dir);
        const bool alive = lockIsAlive (info, pidAlive, pgidAlive, now);
        if (alive)
            continue; // 生きているロックのidentityは触らない（dev/release並走の相手ジョブ）

        if (info.exists)
            releaseLock (dir); // ①記録された全プロセスが死んでいるlockのみ解放

        // ②削除は自分のlockを取ってから（解放〜削除の間に別プロセスが取得したらスキップ）
        if (! acquireLock (dir, myPid))
            continue;
        deleteUnreferencedRunsLocked (dir);
        releaseLock (dir);
    }
}

void cleanupAfterSuccess (const juce::File& root, const SourceIdentity& current, juce::int64 myPid,
                          const PidAlive& pidAlive, const PgidAlive& pgidAlive, juce::Time now)
{
    const auto currentHash = current.hash();
    for (const auto& dir : root.findChildFiles (juce::File::findDirectories, false))
    {
        if (dir.getFileName() == currentHash)
        {
            // 現行identity: 呼び出し側（分離ジョブ）がlock保持中なのでそのまま消せる
            deleteUnreferencedRunsLocked (dir);
            continue;
        }
        // 同じ元パスの旧identity（上書きで孤児化）を削除。deleteIdentityがlockを取ってから消す
        const auto manifest = parseManifest (dir.getChildFile ("manifest.json"));
        if (! manifest.valid || manifest.source.path != current.path)
            continue;
        deleteIdentity (dir, myPid, pidAlive, pgidAlive, now);
    }
}

DeleteResult deleteIdentity (const juce::File& identityDirectory, juce::int64 myPid,
                             const PidAlive& pidAlive, const PgidAlive& pgidAlive, juce::Time now)
{
    DeleteResult result;
    if (! identityDirectory.isDirectory())
        return result;

    // 同じmkdir lockを取得してから消す（実行中の分離ジョブを破壊しない）
    if (! acquireLock (identityDirectory, myPid))
    {
        if (lockIsAlive (readLock (identityDirectory), pidAlive, pgidAlive, now))
        {
            ++result.skippedInUse;
            return result;
        }
        // 死んだlock: 解放して取り直す
        releaseLock (identityDirectory);
        if (! acquireLock (identityDirectory, myPid))
        {
            ++result.skippedInUse;
            return result;
        }
    }

    if (identityDirectory.deleteRecursively())
        ++result.deleted;
    else
        releaseLock (identityDirectory); // 削除失敗の経路ではロックを残さない

    return result;
}

DeleteResult deleteAll (const juce::File& root, juce::int64 myPid,
                        const PidAlive& pidAlive, const PgidAlive& pgidAlive, juce::Time now)
{
    DeleteResult total;
    // 保存ルートの一括削除ではなくidentity単位で列挙して消す（使用中はスキップして件数表示）
    for (const auto& dir : root.findChildFiles (juce::File::findDirectories, false))
    {
        const auto r = deleteIdentity (dir, myPid, pidAlive, pgidAlive, now);
        total.deleted += r.deleted;
        total.skippedInUse += r.skippedInUse;
    }
    return total;
}

juce::int64 totalCacheBytes (const juce::File& root)
{
    juce::int64 total = 0;
    for (const auto& f : root.findChildFiles (juce::File::findFiles, true))
        total += f.getSize();
    return total;
}
} // namespace StemCache
