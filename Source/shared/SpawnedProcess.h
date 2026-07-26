#pragma once

#include <functional>
#include <juce_core/juce_core.h>

// 外部プロセスの起動と、行単位での出力読み取り。macOS前提でPOSIX APIを直接使う。
//
// juce::ChildProcess を使わない理由（JUCE 8.0.9 の juce_SharedCode_posix.h を読んで確認）:
//   ① read() が内部で fread() を呼ぶブロッキング実装（:1193）。出力が止まった
//      ネットワーク処理ではキャンセル要求を観測できず、終了が無期限に固まる
//   ② killProcess() が ::kill(childPID) だけで、子は setpgid されない（:1222）。
//      yt-dlp が spawn した ffmpeg が孤児として残る
//   ③ デストラクタが子を待たない／killしない（:1165。ヘッダにも明記されている）
//
// 代わりに posix_spawn で「子を新しいプロセスグループのリーダー」にして起動し、
// poll() のタイムアウトごとにキャンセルを確認し、killpg でツリーごと終了させる。
class SpawnedProcess
{
public:
    SpawnedProcess() = default;
    ~SpawnedProcess();

    SpawnedProcess (const SpawnedProcess&) = delete;
    SpawnedProcess& operator= (const SpawnedProcess&) = delete;

    // argv[0] は実行ファイルの絶対パス。posix_spawn は PATH を検索しないので相対名は使えない
    bool start (const juce::StringArray& argv);

    // 出力が尽きる（＝プロセス終了）まで読み続け、1行読めるたびにコールバックを呼ぶ。
    // shouldCancel が true を返した時点でプロセスグループごと終了させて false を返す。
    // 戻り値 = 最後まで読み切れたか（キャンセル・未起動は false）
    bool readUntilFinished (const std::function<bool()>& shouldCancel,
                            const std::function<void (const juce::String&)>& onStdoutLine,
                            const std::function<void (const juce::String&)>& onStderrLine);

    // SIGTERM → 猶予後に SIGKILL を「プロセスグループ全体」に送り、グループが空になるまで見届ける。
    // 直接の子を回収した時点では止めない — 子（yt-dlp）が先に終了しても、SIGTERMを無視する
    // 孫（ffmpeg）が残ることがあるため
    void terminate();

    int exitCode() const noexcept { return exitCodeValue; }

    // 子はグループリーダーなので pgid == 子のpid。子を回収した後も killpg のために保持し続ける
    //（キャンセルの検証で外から使うためログにも出す）
    int pgid() const noexcept { return (int) groupId; }

    bool isRunning() const noexcept { return childPid > 0; }

private:
    void closeFds();
    void reapExitStatus (int status);
    void reapChildIfExited(); // 子が終了していれば回収する（ブロックしない）
    void waitForExit();
    bool groupHasMembers() const; // プロセスグループにまだ誰か居るか（ゾンビ回収後に見ること）

    long childPid = -1; // pid_t（実体はint32）。ヘッダにsys型を持ち込まないためlongで保持する
    long groupId  = -1; // childPid と同値で始まるが、子の回収後も残す
    int  stdoutFd = -1;
    int  stderrFd = -1;
    int  exitCodeValue = -1;
};
