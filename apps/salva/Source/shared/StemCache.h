#pragma once

#include <functional>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

// ステムキャッシュの規約（planの「ステム分離の契約」を実装する側）。
//
// 保存ルート: ~/Library/Application Support/salva/stems/<identity-hash>/
//   identity-hash = 元ファイルの絶対パス＋サイズ＋mtime のMD5（上書き・同名別ファイルを検出）
//
// identityディレクトリの中身:
//   lock/               … mkdirで取得するプロセス間ロック（owner.json = アプリ側PID、
//                          worker.json = separate.sh 自身が起動直後にatomic記録するPID/PGID）
//   runs/<uuid>/        … 不変な成果物（ステムWAV群）
//   manifest.json       … 現行runを指すマニフェスト（一時ファイル→renameの1回で公開）
//
// ここにはファイルシステム操作と判定だけを置き、プロセス生存判定は注入可能にして
// salva_tests から固定できるようにする
namespace StemCache
{
inline constexpr int contractVersion = 1;

struct SourceIdentity
{
    juce::String path;
    juce::int64 size = 0;
    juce::int64 mtimeMs = 0;

    static SourceIdentity forFile (const juce::File& f)
    {
        return { f.getFullPathName(), f.getSize(), f.getLastModificationTime().toMilliseconds() };
    }

    juce::String hash() const
    {
        const auto key = path + "|" + juce::String (size) + "|" + juce::String (mtimeMs);
        return juce::MD5 (key.toUTF8()).toHexString();
    }

    bool operator== (const SourceIdentity& o) const
    {
        return path == o.path && size == o.size && mtimeMs == o.mtimeMs;
    }
};

juce::File stemsRoot();
juce::File identityDir (const SourceIdentity& id);

// ---- マニフェスト ----
struct StemInfo
{
    juce::String name;    // drums / bass / vocals / other / guitar / piano
    juce::String relPath; // identityディレクトリからの相対パス（runs/<uuid>/...）
};

struct Group
{
    juce::String id;          // htdemucs / htdemucs_6s
    juce::String displayName; // manifestのname（表示はStemPanelがステム本数から導出するため未使用）
    std::vector<StemInfo> stems;
};

struct Manifest
{
    bool valid = false;
    int version = 0;
    juce::String model;
    SourceIdentity source;
    double sampleRate = 0.0;
    juce::int64 lengthSamples = 0;
    juce::String status;
    juce::String runRel; // "runs/<uuid>"
    std::vector<Group> groups;
};

Manifest parseManifest (const juce::File& manifestFile);

// identity一致・status=complete・契約version・全ステムWAVの実在まで確認する
bool manifestUsable (const Manifest& m, const SourceIdentity& current, const juce::File& identityDirectory);

// ---- プロセス間ロック ----
// mkdirによる原子的取得。成功時 owner.json（アプリPID）を書く
bool acquireLock (const juce::File& identityDirectory, juce::int64 ownerPid);
void releaseLock (const juce::File& identityDirectory);

struct LockInfo
{
    bool exists = false;
    juce::int64 ownerPid = -1;
    bool workerRecorded = false;
    juce::int64 workerPid = -1;
    juce::int64 workerPgid = -1;
    juce::Time lastModified;
};

LockInfo readLock (const juce::File& identityDirectory);

// プロセス生存判定（テストから差し替える）
using PidAlive = std::function<bool (juce::int64 pid)>;
using PgidAlive = std::function<bool (juce::int64 pgid)>;
bool defaultPidAlive (juce::int64 pid);
bool defaultPgidAlive (juce::int64 pgid);

// lockがまだ生きているか:
//   - owner（アプリ）生存 → 生。アプリが死んでいても worker の**PGIDにメンバーが残っていれば生**
//     （demucsが孫プロセスを持っても取りこぼさない。アプリ異常終了でもworkerは完走し得る）
//   - worker未記録の「未完成lock」は安全側で生扱い。ただし最終更新が1時間より古ければ死
//     （scriptが記録する直前かもしれないが、永久ブロックは避ける）
bool lockIsAlive (const LockInfo& info, const PidAlive& pidAlive, const PgidAlive& pgidAlive, juce::Time now);

// ---- 掃除・削除 ----
// 起動時掃除: ①死んだlockの解放 ②manifestから参照されず、かつロックが生きていない
// 中途 runs/<uuid>/ の削除。削除は必ず**自分でlockを取得してから**行う（死んだlockの
// 解放と削除の間に別プロセスが取得して新runを作り始めるTOCTOU競合を塞ぐ。
// 取得できなかったidentityはスキップする）
// currentContractVersion はテスト注入用（本番は既定のまま呼ぶ）。
// manifestのversionがこれより古いidentityは丸ごと削除する（未来versionは保持 ——
// rootはdev/release共有のため、旧アプリが新アプリのキャッシュを消してはいけない）
void sweep (const juce::File& root, juce::int64 myPid,
            const PidAlive& pidAlive, const PgidAlive& pgidAlive, juce::Time now,
            int currentContractVersion = contractVersion);

// 分離成功後の後始末: ①現行manifestが参照しない旧runの削除 —— **呼び出し側が現identityの
// lockを保持したまま呼ぶ契約**（解放後に呼ぶと、別プロセスの生成中runを消し得る）
// ②同じ元パスの孤児identity（上書きで置き換わった旧identity）の削除。こちらは各identityの
// lockを取得してから消す（取得不可はスキップ）
void cleanupAfterSuccess (const juce::File& root, const SourceIdentity& current, juce::int64 myPid,
                          const PidAlive& pidAlive, const PgidAlive& pgidAlive, juce::Time now);

struct DeleteResult
{
    int deleted = 0;
    int skippedInUse = 0;
};

// identity単位の削除。同じmkdir lockを取得してから消す。取得できなければ「使用中」としてスキップ
DeleteResult deleteIdentity (const juce::File& identityDirectory, juce::int64 myPid,
                             const PidAlive& pidAlive, const PgidAlive& pgidAlive, juce::Time now);
DeleteResult deleteAll (const juce::File& root, juce::int64 myPid,
                        const PidAlive& pidAlive, const PgidAlive& pgidAlive, juce::Time now);

juce::int64 totalCacheBytes (const juce::File& root);
} // namespace StemCache
