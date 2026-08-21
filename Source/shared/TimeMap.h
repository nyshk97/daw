#pragma once

#include <vector>
#include <juce_core/juce_core.h>

// 原音座標 → render 座標の単調な区分線形写像。一様 ratio はノード 2 点の特殊形
struct TimeMapNode
{
    juce::int64 source = 0; // 原音絶対サンプル
    juce::int64 output = 0; // render 座標（ドメイン先頭からの距離）
    bool operator== (const TimeMapNode& o) const { return source == o.source && output == o.output; }
};

struct TimeMap
{
    std::vector<TimeMapNode> nodes; // source 昇順・output 非減少（構築側が厳密昇順を保証する）

    static TimeMap uniform (juce::int64 domainOffset, juce::int64 domainLength, double ratio);
    bool isUniform() const { return nodes.size() <= 2; }
    juce::int64 outputLength() const { return nodes.empty() ? 0 : nodes.back().output; }
    // 原音絶対位置 → render 座標（ノード上は正確、間は線形補間＋丸め。単調非減少）
    juce::int64 map (juce::int64 source) const;
    // render 座標 → 原音位置（map(戻り値) が renderPos に最も近い原音位置。等距離なら小さい方。
    // 到達可能な値なら map(戻り値) == renderPos）
    juce::int64 inverse (juce::int64 renderPos) const;
    bool operator== (const TimeMap& o) const { return nodes == o.nodes; }
};
