#pragma once

// macOS ネイティブタイトルバーの配色切り替え（実装は TitleBarStyle.mm）。
// silver=true: タイトルバーを上部バーと同じシルバーに塗り、ネイティブのタイトル文字を隠して
// コンテンツをタイトルバーの下まで広げる（fullSizeContentView）。タイトルは MainComponent が
// 帯の中央に自前で描く（macOS 26 はネイティブのタイトルが左寄せ固定で、中央に揃える公開APIが無い）。
// false: 既定の暗いタイトルバー＋ネイティブのタイトルに戻す（選択画面用）
namespace TitleBarStyle
{
void apply (void* nsView, bool silver, float r, float g, float b);

// コンテンツ上端のうちタイトルバーに隠れる高さ（fullSizeContentView 時のみ > 0。
// フルスクリーンでタイトルバーが消えている間は 0）
int titleBarInset (void* nsView);
}
