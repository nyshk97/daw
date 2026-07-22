#pragma once

#include <functional>

// Sparkle (Objective-C framework) を C++ から使うための薄いブリッジ。実装は SparkleBridge.mm。
// すべてメッセージスレッドから呼ぶこと。
namespace SparkleBridge
{
    // SPUStandardUpdaterController を生成する。起動時に1回だけ呼ぶ。
    // SUEnableAutomaticChecks=false (Info.plist) のため、ネットワークアクセスは
    // checkForUpdates() を呼んだときだけ走る。
    // onCanCheckChanged は canCheckForUpdates の変化時にメッセージスレッドで呼ばれる
    // （メニュー項目の enable/disable 更新用。init 直後に初期値でも1回呼ばれる）
    void init (std::function<void (bool canCheck)> onCanCheckChanged);

    // "Check for Updates…" メニューの実体。更新チェックの UI は Sparkle が出す
    void checkForUpdates();

    // KVO オブザーバを解除しコールバックを無効化する。アプリの shutdown() から呼ぶ。
    // 呼ばないと main queue に積まれたコールバックが JUCE の破棄後に走りうる
    void shutdown();
}
