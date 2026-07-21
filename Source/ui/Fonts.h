#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Apple HIG (macOS) のスケールに合わせたフォント定義。UIのフォントサイズは直書きせずここを経由する。
// タイプフェイス無指定のものはAppLookAndFeelがシステムUIフォント(SF Pro)に差し替える。
// semibold・等幅は「family名 + style名」の明示指定（macのJUCE実装はfamily+styleで解決するため、
// ".AppleSystemUIFontDemi" のようなfont名指定では引けない）
namespace Fonts
{
inline juce::Font small()      { return juce::Font (juce::FontOptions (11.0f)); }  // 小ラベル（鍵盤名など。11pxを下限とする）
inline juce::Font body()       { return juce::Font (juce::FontOptions (13.0f)); }  // 標準（トラック名・ボタン・キャプション）
inline juce::Font title()      { return juce::Font (juce::FontOptions (".AppleSystemUIFont", "Semibold", 15.0f)); }
inline juce::Font largeTitle() { return juce::Font (juce::FontOptions (20.0f, juce::Font::bold)); }

// 数字表示（小節ルーラー・BPM・再生位置）。可変幅だと再生中に数字がガタつくため等幅にする
inline juce::Font mono (float height)
{
    return juce::Font (juce::FontOptions (".AppleSystemUIFontMonospaced", height, juce::Font::plain));
}

// 自由入力テキスト（トラック名・プロジェクト名）用の光学補正。
// 同じポイント数でもCJKは仮想ボディを目一杯使うため欧文より大きく見える。
// CJKを含む文字列だけ8%縮めて、英語名のトラックと並んだときの大小差を抑える。
// 固定UI文言には使わない（OS標準ダイアログ等と並んだとき日本語だけ小さく見えるため）
inline bool containsCjk (const juce::String& text)
{
    for (auto t = text.getCharPointer(); ! t.isEmpty(); ++t)
    {
        const auto c = *t;
        if ((c >= 0x3000 && c <= 0x9fff)      // CJK記号・かな・カナ・漢字
            || (c >= 0xf900 && c <= 0xfaff)   // CJK互換漢字
            || (c >= 0xff00 && c <= 0xffef))  // 全角英数・半角カナ
            return true;
    }
    return false;
}

inline juce::Font forText (const juce::Font& base, const juce::String& text)
{
    return containsCjk (text) ? base.withHeight (base.getHeight() * 0.92f) : base;
}
} // namespace Fonts
