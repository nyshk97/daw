#include "Log.h"

#include <algorithm>
#include <memory>

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

// juce::FileLogger は行ごとのタイムスタンプ・セッション中のサイズ上限を持たないため自前実装。
// juce::Logger を継承しておくと、JUCE内部の writeToLog もこのファイルに落ちる
class SessionLogger : public juce::Logger
{
public:
    explicit SessionLogger (const juce::File& file) : stream (file) {}

    bool openedOk() const { return stream.openedOk(); }

    void logMessage (const juce::String& message) override
    {
        write (message, false);
    }

    // session.end 用: サイズ上限到達後でも書く（次回起動の異常終了判定を誤らせないため）
    void logAlways (const juce::String& message)
    {
        write (message, true);
    }

private:
    void write (const juce::String& message, bool ignoreLimit)
    {
        const juce::ScopedLock sl (lock);
        if (! stream.openedOk())
            return;

        if (! ignoreLimit)
        {
            if (limitReached)
                return;
            if (bytesWritten > maxSessionBytes)
            {
                limitReached = true;
                stream << jp (u8"log.limit_reached（以降このセッションのログは破棄）") << juce::newLine;
                stream.flush();
                return;
            }
        }

        stream << message << juce::newLine;
        stream.flush(); // クラッシュ直前の行を残すため毎回flushする
        bytesWritten += (juce::int64) message.getNumBytesAsUTF8() + 1;
    }

    static constexpr juce::int64 maxSessionBytes = 1024 * 1024; // 暴走的な繰り返しログへの保険
    juce::CriticalSection lock;
    juce::FileOutputStream stream;
    juce::int64 bytesWritten = 0;
    bool limitReached = false;
};

std::unique_ptr<SessionLogger> sessionLogger;

juce::File logsDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
        .getChildFile ("Library/Logs/daw");
}

juce::String makeLine (const char* level, const juce::String& event, const juce::String& detail)
{
    auto line = juce::Time::getCurrentTime().toISO8601 (true) + " " + level + " " + event;
    if (detail.isNotEmpty())
        line << " " << detail;
    return line;
}
} // namespace

void Log::init (const juce::String& appVersion)
{
    auto dir = logsDirectory();
    dir.createDirectory();

    // ファイル名のタイムスタンプ順 = 時系列。新しい順に20世代だけ残す
    auto oldLogs = dir.findChildFiles (juce::File::findFiles, false, "daw-*.log");
    std::sort (oldLogs.begin(), oldLogs.end(),
               [] (const juce::File& a, const juce::File& b)
               { return a.getFileName() > b.getFileName(); });
    for (int i = 19; i < oldLogs.size(); ++i)
        oldLogs.getReference (i).deleteFile();

    // 前回セッションの異常終了検知: 直近ログに session.end が無ければクラッシュか強制終了
    const bool previousAbnormal =
        ! oldLogs.isEmpty() && ! oldLogs.getFirst().loadFileAsString().contains ("session.end");
    const auto previousName = oldLogs.isEmpty() ? juce::String() : oldLogs.getFirst().getFileName();

    // ミリ秒まで入れて同一秒の再起動でも別ファイルにする（同名再利用は異常終了判定を壊す）
    const auto now = juce::Time::getCurrentTime();
    const auto file = dir.getChildFile (
        "daw-" + now.formatted ("%Y%m%d-%H%M%S")
        + "-" + juce::String (now.toMilliseconds() % 1000).paddedLeft ('0', 3) + ".log");
    sessionLogger = std::make_unique<SessionLogger> (file);
    if (! sessionLogger->openedOk())
    {
        sessionLogger.reset(); // ログが書けなくてもアプリは動かす
        return;
    }
    juce::Logger::setCurrentLogger (sessionLogger.get());

    info ("session.start", "version=" + appVersion
                               + " os=" + juce::SystemStats::getOperatingSystemName());
    if (previousAbnormal)
        warn ("session.previous_abnormal",
              "file=" + previousName + jp (u8"（前回は正常終了していない。クラッシュなら ~/Library/Logs/DiagnosticReports/ を確認）"));
}

void Log::shutdown()
{
    if (sessionLogger == nullptr)
        return;
    sessionLogger->logAlways (makeLine ("INFO", "session.end", {}));
    juce::Logger::setCurrentLogger (nullptr); // current logger は生ポインタ参照なので破棄より先に外す
    sessionLogger.reset();
}

void Log::info (const juce::String& event, const juce::String& detail)
{
    if (sessionLogger != nullptr)
        sessionLogger->logMessage (makeLine ("INFO", event, detail));
}

void Log::warn (const juce::String& event, const juce::String& detail)
{
    if (sessionLogger != nullptr)
        sessionLogger->logMessage (makeLine ("WARN", event, detail));
}

void Log::error (const juce::String& event, const juce::String& detail)
{
    if (sessionLogger != nullptr)
        sessionLogger->logMessage (makeLine ("ERROR", event, detail));
}
