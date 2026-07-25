#pragma once

#include <algorithm>
#include <juce_core/juce_core.h>

// ファイル一覧の並び順。DirectoryContentsList の並びは「名前の自然順」固定なので、
// 表示順のインデックス列をこちらで作って差し替える（GUI非依存にして daw_tests から直接テストする）
namespace FileSortOrder
{
enum class Mode { dateAdded, name };

struct Entry
{
    juce::String filename;
    juce::Time creationTime;
};

// 追加日は新しい順、名前は自然順（大文字小文字を区別せず、数字は数値として比較）の昇順。
// 追加日が同じときはファイル名で決める（まとめて落ちてきたファイルの並びが揺れないように）
inline juce::Array<int> sortedIndices (const juce::Array<Entry>& entries, Mode mode)
{
    juce::Array<int> order;
    for (int i = 0; i < entries.size(); ++i)
        order.add (i);

    std::stable_sort (order.begin(), order.end(), [&entries, mode] (int a, int b)
    {
        const auto& left = entries.getReference (a);
        const auto& right = entries.getReference (b);
        if (mode == Mode::dateAdded && left.creationTime != right.creationTime)
            return left.creationTime > right.creationTime;
        return left.filename.compareNatural (right.filename) < 0;
    });
    return order;
}
}
