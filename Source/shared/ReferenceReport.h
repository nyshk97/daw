#pragma once

#include <juce_core/juce_core.h>

// 分析レポート（references/<名前>/report.md）とその表示用HTMLキャッシュの状態判定。
// レポートウィンドウ・ガチャパネルのボタンはここを呼ぶだけにする
// （判断ロジックを ui/ に置かない — daw_tests で固定するため）。
namespace ReferenceReport
{
inline juce::File reportMd (const juce::File& referenceFolder)
{
    return referenceFolder.getChildFile ("report.md");
}

inline juce::File reportHtml (const juce::File& referenceFolder)
{
    return referenceFolder.getChildFile ("report.html");
}

// レポートが読める状態か（= report.md がある。HTML キャッシュの有無は問わない）
inline bool exists (const juce::File& referenceFolder)
{
    return reportMd (referenceFolder).existsAsFile();
}

// HTML キャッシュを作り直すべきか。キャッシュキーは report.md と変換器（render_report.py）の
// 両方の mtime — 変換器や埋め込みCSSを更新したのに古い HTML が永久に出続けるのを防ぐ。
// 画像の更新は再分析→レポート書き直しに随伴するので個別には見ない
inline bool needsRender (const juce::File& referenceFolder, const juce::File& renderer)
{
    const auto html = reportHtml (referenceFolder);
    if (! html.existsAsFile())
        return true;
    const auto htmlTime = html.getLastModificationTime();
    return htmlTime < reportMd (referenceFolder).getLastModificationTime()
        || htmlTime < renderer.getLastModificationTime();
}
}
