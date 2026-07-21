#pragma once

#include <juce_core/juce_core.h>

// セッション単位のアプリログ（~/Library/Logs/daw/daw-YYYYMMDD-HHMMSS.log）。
// 1行 = 「<ISO8601ミリ秒> LEVEL イベント名 key=value ...」。
//
// オーディオスレッドから呼んではならない（ロック・String確保・ファイルIOを含む）。
// オーディオ側の異常は TransportState の atomic に載せ、UIのTimerが集約してここに書く。
// init() 前・shutdown() 後の呼び出しは何もしない（テスト実行時はこの状態のまま）。
namespace Log
{
    void init (const juce::String& appVersion); // session.start を書く。前回ログに session.end が無ければ警告を残す
    void shutdown();                            // session.end を書いてから logger を外して破棄する

    void info (const juce::String& event, const juce::String& detail = {});
    void warn (const juce::String& event, const juce::String& detail = {});
    void error (const juce::String& event, const juce::String& detail = {});
}
