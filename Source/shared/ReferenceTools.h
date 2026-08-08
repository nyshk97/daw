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
inline juce::File bassScript()    { return repoRoot().getChildFile ("tools/gacha/bass.py"); }
inline juce::File renderScript()  { return repoRoot().getChildFile ("tools/reference/render_report.py"); }
inline juce::File reportScript()  { return repoRoot().getChildFile ("tools/reference/report.sh"); }

// 分析（右クリック「リファレンスとして分析」）に必要なツールが揃っているか
inline bool analyzeAvailable() { return analyzeScript().existsAsFile() && venvPython().existsAsFile(); }

// ガチャ（右パネル第3モード）に必要なツールが揃っているか
inline bool gachaAvailable() { return drumsScript().existsAsFile() && venvPython().existsAsFile(); }

// ベースガチャ（Bass パーツ）に必要なツールが揃っているか。drums と独立判定 —
// bass.py だけ欠けた checkout でもドラムガチャは使える
inline bool bassGachaAvailable() { return bassScript().existsAsFile() && venvPython().existsAsFile(); }

// ループ検索（Loops パーツ）。おすすめ5＝recommend.py・採用時の進行検出＝looproots.py
inline juce::File recommendScript() { return repoRoot().getChildFile ("tools/library/recommend.py"); }
inline juce::File looprootsScript() { return repoRoot().getChildFile ("tools/library/looproots.py"); }
// サンプルライブラリの実体（~/Music/daw/library。setup.sh が iCloud への symlink を張る）
inline juce::File libraryRoot()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
        .getChildFile ("Music/daw/library");
}
inline bool loopGachaAvailable()
{
    return recommendScript().existsAsFile() && looprootsScript().existsAsFile()
        && venvPython().existsAsFile() && libraryRoot().getChildFile ("index.json").existsAsFile();
}

// レポート閲覧（report.md→HTML変換）に必要なツールが揃っているか。
// ガチャや生成とは独立に判定する — ドラム生成器が欠けただけで閲覧まで無効にしない
inline bool renderAvailable() { return renderScript().existsAsFile() && venvPython().existsAsFile(); }

// レポート生成（report.sh = Claude Code ヘッドレス）に必要なツールが揃っているか。
// claude 本体の最終確認は report.sh 内のフォールバックに任せる
inline bool reportAvailable() { return reportScript().existsAsFile() && venvPython().existsAsFile(); }

// 無効化時にツールチップ等で出す理由（どちらの機能でも同じ文面でよい）
inline juce::String unavailableReason()
{
    return juce::String::fromUTF8 (u8"~/daw のツールが見つかりません（リポジトリを ~/daw に置き、"
                                   u8"mise run ref:setup で .venv を作成してください）");
}

// Loops タブ無効時の理由（ツール不在と index 未作成を区別する）
inline juce::String loopGachaUnavailableReason()
{
    if (! recommendScript().existsAsFile() || ! looprootsScript().existsAsFile()
        || ! venvPython().existsAsFile())
        return unavailableReason();
    return juce::String::fromUTF8 (u8"ライブラリの index.json がありません"
                                   u8"（パックを置いて mise run lib:index を実行してください）");
}
}
