#pragma once

#include <juce_core/juce_core.h>

// リファレンス分析・ドラムガチャの外部ツール群（Python パイプライン）のパス解決。
// リポジトリの場所は ~/daw 固定（yt-dlp の既知絶対パス方式と同じ発想。別マシンでは
// clone 先を ~/daw に揃えれば動く）。不在ならメニュー/タブを無効化して理由を表示する。
namespace ReferenceTools
{
inline juce::File repoRoot()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("daw");
}

inline juce::File analyzeScript() { return repoRoot().getChildFile ("tools/reference/analyze.sh"); }
inline juce::File venvPython()    { return repoRoot().getChildFile ("tools/reference/.venv/bin/python"); }
inline juce::File drumsScript()   { return repoRoot().getChildFile ("tools/gacha/drums.py"); }

// 分析（右クリック「リファレンスとして分析」）に必要なツールが揃っているか
inline bool analyzeAvailable() { return analyzeScript().existsAsFile() && venvPython().existsAsFile(); }

// ガチャ（右パネル第3モード）に必要なツールが揃っているか
inline bool gachaAvailable() { return drumsScript().existsAsFile() && venvPython().existsAsFile(); }

// 無効化時にツールチップ等で出す理由（どちらの機能でも同じ文面でよい）
inline juce::String unavailableReason()
{
    return juce::String::fromUTF8 (u8"~/daw のツールが見つかりません（リポジトリを ~/daw に置き、"
                                   u8"mise run ref:setup で .venv を作成してください）");
}
}
