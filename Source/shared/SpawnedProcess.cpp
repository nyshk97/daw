#include "SpawnedProcess.h"

#include <crt_externs.h> // _NSGetEnviron
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace
{
constexpr int pollTimeoutMs = 200;   // この間隔でキャンセル要求を見に戻る
constexpr int termGraceMs   = 1000;  // SIGTERM のあと SIGKILL に切り替えるまで
constexpr int killGraceMs   = 500;   // SIGKILL のあとグループが消えるのを待つ上限
constexpr int reapStepMs    = 10;

void closeFd (int& fd)
{
    if (fd >= 0)
    {
        ::close (fd);
        fd = -1;
    }
}

// 読めるだけ読んで、改行で切った行をコールバックへ渡す。
// バイト列のまま溜めて行に切ってから UTF-8 変換するのは、チャンク境界が
// マルチバイト文字の途中に来ても壊れないようにするため（タイトルに日本語が入る）
// 戻り値: まだ開いているか（false = EOF・エラー）
bool drainFd (int fd, std::string& buffer,
              const std::function<void (const juce::String&)>& onLine)
{
    char chunk[4096];

    for (;;)
    {
        const auto bytesRead = ::read (fd, chunk, sizeof (chunk));

        if (bytesRead > 0)
        {
            buffer.append (chunk, (size_t) bytesRead);

            for (;;)
            {
                // yt-dlp は --newline を付けても進捗以外で \r を使うことがあるので両方を行区切りにする
                const auto nl = buffer.find_first_of ("\r\n");
                if (nl == std::string::npos)
                    break;

                const auto line = buffer.substr (0, nl);
                buffer.erase (0, nl + 1);
                if (onLine != nullptr && ! line.empty())
                    onLine (juce::String::fromUTF8 (line.c_str(), (int) line.size()));
            }
            continue;
        }

        if (bytesRead == 0)
            return false; // EOF

        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true; // 今は無いだけ

        return false;
    }
}

void flushRemainder (std::string& buffer,
                     const std::function<void (const juce::String&)>& onLine)
{
    if (buffer.empty())
        return;
    if (onLine != nullptr)
        onLine (juce::String::fromUTF8 (buffer.c_str(), (int) buffer.size()));
    buffer.clear();
}
}

SpawnedProcess::~SpawnedProcess()
{
    terminate();
    closeFds();
}

void SpawnedProcess::closeFds()
{
    closeFd (stdoutFd);
    closeFd (stderrFd);
}

bool SpawnedProcess::start (const juce::StringArray& argv)
{
    if (argv.isEmpty() || childPid > 0)
        return false;

    int outPipe[2] = { -1, -1 };
    int errPipe[2] = { -1, -1 };

    if (::pipe (outPipe) != 0)
        return false;
    if (::pipe (errPipe) != 0)
    {
        ::close (outPipe[0]);
        ::close (outPipe[1]);
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init (&actions);
    posix_spawn_file_actions_adddup2 (&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2 (&actions, errPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose (&actions, outPipe[0]);
    posix_spawn_file_actions_addclose (&actions, errPipe[0]);
    posix_spawn_file_actions_addclose (&actions, outPipe[1]);
    posix_spawn_file_actions_addclose (&actions, errPipe[1]);

    posix_spawnattr_t attr;
    posix_spawnattr_init (&attr);
    // 子を新しいプロセスグループのリーダーにする（pgroup=0 が「子自身のpidをpgidにする」の意味）。
    // これで killpg が孫（ffmpeg）まで届き、かつ自分のグループを巻き込まない
    posix_spawnattr_setflags (&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup (&attr, 0);

    // argv を char* const[] に。シェルを経由しないので引用符の心配がない
    std::vector<std::string> storage;
    storage.reserve ((size_t) argv.size());
    for (const auto& arg : argv)
        storage.push_back (arg.toStdString());

    std::vector<char*> args;
    args.reserve (storage.size() + 1);
    for (auto& s : storage)
        args.push_back (const_cast<char*> (s.c_str()));
    args.push_back (nullptr);

    pid_t spawnedPid = 0;
    const int spawnResult = ::posix_spawn (&spawnedPid, storage[0].c_str(),
                                           &actions, &attr, args.data(), *_NSGetEnviron());

    posix_spawn_file_actions_destroy (&actions);
    posix_spawnattr_destroy (&attr);

    // 書き込み端は子に渡ったので親側は閉じる（閉じないとEOFが来ない）
    ::close (outPipe[1]);
    ::close (errPipe[1]);

    if (spawnResult != 0)
    {
        ::close (outPipe[0]);
        ::close (errPipe[0]);
        return false;
    }

    childPid = (long) spawnedPid;
    groupId  = (long) spawnedPid; // 子はグループリーダー。子の回収後も killpg のために残す
    stdoutFd = outPipe[0];
    stderrFd = errPipe[0];
    exitCodeValue = -1;

    ::fcntl (stdoutFd, F_SETFL, O_NONBLOCK);
    ::fcntl (stderrFd, F_SETFL, O_NONBLOCK);
    return true;
}

bool SpawnedProcess::readUntilFinished (
    const std::function<bool()>& shouldCancel,
    const std::function<void (const juce::String&)>& onStdoutLine,
    const std::function<void (const juce::String&)>& onStderrLine)
{
    if (childPid <= 0)
        return false;

    std::string outBuffer, errBuffer;
    bool cancelled = false;

    while (stdoutFd >= 0 || stderrFd >= 0)
    {
        if (shouldCancel != nullptr && shouldCancel())
        {
            cancelled = true;
            break;
        }

        struct pollfd fds[2] {};
        int count = 0;
        int outIndex = -1;
        int errIndex = -1;

        if (stdoutFd >= 0)
        {
            fds[count] = { stdoutFd, POLLIN, 0 };
            outIndex = count++;
        }
        if (stderrFd >= 0)
        {
            fds[count] = { stderrFd, POLLIN, 0 };
            errIndex = count++;
        }

        const int ready = ::poll (fds, (nfds_t) count, pollTimeoutMs);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue; // タイムアウト。キャンセル確認へ戻る

        if (outIndex >= 0 && (fds[outIndex].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            if (! drainFd (stdoutFd, outBuffer, onStdoutLine))
                closeFd (stdoutFd);

        if (errIndex >= 0 && (fds[errIndex].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            if (! drainFd (stderrFd, errBuffer, onStderrLine))
                closeFd (stderrFd);
    }

    // 改行で終わっていない最後の行も拾う
    flushRemainder (outBuffer, onStdoutLine);
    flushRemainder (errBuffer, onStderrLine);

    if (cancelled)
    {
        terminate();
        closeFds();
        return false;
    }

    closeFds();
    waitForExit();
    return true;
}

void SpawnedProcess::reapExitStatus (int status)
{
    if (WIFEXITED (status))
        exitCodeValue = WEXITSTATUS (status);
    else if (WIFSIGNALED (status))
        exitCodeValue = -WTERMSIG (status);
    childPid = -1;
}

void SpawnedProcess::reapChildIfExited()
{
    if (childPid <= 0)
        return;

    int status = 0;
    const auto result = ::waitpid ((pid_t) childPid, &status, WNOHANG);
    if (result == (pid_t) childPid)
        reapExitStatus (status);
    else if (result < 0 && errno != EINTR)
        childPid = -1; // ECHILD 等。もう待てない
}

void SpawnedProcess::waitForExit()
{
    if (childPid <= 0)
        return;

    int status = 0;
    for (;;)
    {
        const auto result = ::waitpid ((pid_t) childPid, &status, 0);
        if (result == (pid_t) childPid)
        {
            reapExitStatus (status);
            return;
        }
        if (result < 0 && errno == EINTR)
            continue;
        break; // ECHILD 等。これ以上待てない
    }
    childPid = -1;
}

// 未回収の子（ゾンビ）もプロセスとして数えられてしまうので、必ず reapChildIfExited() の後に呼ぶ
bool SpawnedProcess::groupHasMembers() const
{
    if (groupId <= 0)
        return false;
    if (::killpg ((pid_t) groupId, 0) == 0)
        return true;
    return errno == EPERM; // 権限が無いだけで存在はしている
}

void SpawnedProcess::terminate()
{
    reapChildIfExited();

    if (groupId <= 0 || ! groupHasMembers())
    {
        if (childPid > 0)
            waitForExit();
        groupId = -1;
        return;
    }

    // 自分のプロセスグループを撃たないための保険。posix_spawn が成功していれば起きないが、
    // ここを取り違えるとアプリごと死ぬので明示的に弾く
    if ((pid_t) groupId == ::getpgrp())
    {
        childPid = -1;
        groupId = -1;
        return;
    }

    ::killpg ((pid_t) groupId, SIGTERM);

    // 「直接の子を回収できたら終わり」にはしない。子（yt-dlp）が先に終了しても、
    // SIGTERM を無視する孫（ffmpeg）が同じグループに残っていることがある
    for (int waited = 0; waited < termGraceMs; waited += reapStepMs)
    {
        reapChildIfExited();
        if (! groupHasMembers())
        {
            if (childPid > 0)
                waitForExit();
            groupId = -1;
            return;
        }
        ::usleep ((useconds_t) reapStepMs * 1000);
    }

    ::killpg ((pid_t) groupId, SIGKILL);

    for (int waited = 0; waited < killGraceMs; waited += reapStepMs)
    {
        reapChildIfExited();
        if (! groupHasMembers())
            break;
        ::usleep ((useconds_t) reapStepMs * 1000);
    }

    if (childPid > 0)
        waitForExit();
    groupId = -1;
}
