#pragma once

// macOS ネイティブタイトルバーの配色切り替え（Main.cpp から呼ぶ。実装は TitleBarStyle.mm）。
// silver=true: 上部バーと同じシルバーに塗り、ウィンドウの appearance を light にして
// タイトル文字を濃灰で出す（メイン画面用）。false: 既定の暗いタイトルバーに戻す（選択画面用）
namespace TitleBarStyle
{
void apply (void* nsView, bool silver, float r, float g, float b);
}
