#pragma once

#include <juce_core/juce_core.h>

// リサンプラー状態のリセット判定（PlaybackCallbackの純ロジック部）。
//
// エンジンの状態遷移（シーク・ファイル変更・オリジナル⇄ステム切替）は複数ストリームへの
// requestSeek()の列で、コールバックと競合すると「一部のストリームだけ新位置」のブロックが
// できる（masterの世代だけ監視しても、非masterだけが変わる順序は検出できない）。
//
// そこでエンジン側は遷移全体を**epoch（begin/end）**で囲む:
//   begin: transitionsActive++（release）→ transitionVersion++（release）
//   ... requestSeek()の列 ...
//   end:   transitionsActive--（release）
//
// コールバック側は preBlock でversion→activeの順にacquireで読み（versionのacquireが
// active++の可視性を保証する）、postBlock で version を読み直す:
//   - version変化をpreBlockで見た＝前ブロック以降に**完了した**遷移がある → 処理前reset（通常経路）
//   - ブロック開始時点で active>0、またはversionが処理中に変わった＝**遷移が処理と重なった**
//     → そのブロックは一部ストリームだけ新位置の音を混ぜた可能性がある。
//       SRに関係なく無音化し、リサンプラー状態を破棄する（次ブロックからクリーン）
//
// シーク採用の可視化（seekGenerationのacquire）とversion++（begin時・release）の順序により、
// 「採用だけ見えてepochが見えない」ことはない（採用が見えた時点で、それ以前のbegin書き込みは
// 以後の読みに可視）
class DiscontinuityGuard
{
public:
    // ブロック冒頭で毎回呼ぶ（version→activeの順で読んだ値を渡す）。
    // trueなら処理前にリサンプラーをresetすること
    bool preBlock (juce::uint32 version, int activeTransitions)
    {
        versionAtStart = version;
        suspectAtStart = activeTransitions > 0;
        const bool changed = version != lastSeenVersion;
        lastSeenVersion = version;
        return changed || suspectAtStart;
    }

    // ブロック末尾で毎回呼ぶ。trueなら「遷移が処理と重なったブロック」＝
    // SRに関係なく出力を無音化し、リサンプラーをresetすること
    bool postBlock (juce::uint32 version)
    {
        return suspectAtStart || version != versionAtStart;
    }

private:
    juce::uint32 lastSeenVersion = 0;
    juce::uint32 versionAtStart = 0;
    bool suspectAtStart = false;
};
