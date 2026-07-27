#pragma once

// macOS 標準の About パネル（アプリアイコン＋名前＋バージョン）を出す薄いブリッジ。
// 実装は AboutPanel.mm。メッセージスレッドから呼ぶこと。
namespace AboutPanel
{
    // 表示内容は Info.plist（CFBundleName / CFBundleShortVersionString /
    // CFBundleVersion）から AppKit が組み立てるため、こちらで文字列を持たない。
    // = CMakeLists.txt の project(VERSION) が唯一の真実の源のまま
    void show();
}
