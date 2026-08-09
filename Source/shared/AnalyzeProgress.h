#pragma once

#include <optional>
#include <juce_core/juce_core.h>

// analyze.py の進捗集約行「==> 42% | 7/16 完了 ／ 実行中: …」を読む純関数。
// YtDlpOutput と同じ「プロセスにもGUIにも依存しない」層で、daw_tests から直接テストする。
// 書式は tools/reference/analyze.py の announce() と対（片方を変えたら必ず両方直す）
namespace AnalyzeProgress
{
struct Parsed
{
    std::optional<float> progress; // 0.0〜1.0。進捗率トークンが無い行は nullopt
    juce::String display;          // 表示用の行。進捗率はバーが示すので剥がす（二重表示を避ける）
};

inline Parsed parse (const juce::String& line)
{
    const auto trimmed = line.trim();
    if (trimmed.startsWith ("==> "))
    {
        const auto rest = trimmed.substring (4);
        const auto sep = rest.indexOf (" | ");
        if (sep > 1) // 最短でも "0%"
        {
            const auto token = rest.substring (0, sep);
            const auto digits = token.dropLastCharacters (1);
            if (token.endsWithChar ('%') && digits.containsOnly ("0123456789"))
                return { juce::jlimit (0.0f, 1.0f, digits.getFloatValue() / 100.0f),
                         "==> " + rest.substring (sep + 3) };
        }
    }
    return { std::nullopt, trimmed };
}
}
