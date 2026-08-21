#include "TimeMap.h"

#include <algorithm>
#include <cmath>

namespace
{
juce::int64 roundMul (juce::int64 delta, double ratio)
{
    return (juce::int64) std::llround ((double) delta * ratio);
}
} // namespace

TimeMap TimeMap::uniform (juce::int64 domainOffset, juce::int64 domainLength, double ratio)
{
    TimeMap m;
    m.nodes.push_back ({ domainOffset, 0 });
    m.nodes.push_back ({ domainOffset + domainLength, roundMul (domainLength, ratio) });
    return m;
}

juce::int64 TimeMap::map (juce::int64 source) const
{
    if (nodes.empty())
        return 0;
    if (source <= nodes.front().source)
        return nodes.front().output;
    if (source >= nodes.back().source)
    {
        // 末尾より先は最後の区間の傾きで外挿（ループ展開などが端を越えて問い合わせても単調を保つ）
        const auto& a = nodes[nodes.size() - 2];
        const auto& b = nodes.back();
        const double slope = b.source > a.source ? (double) (b.output - a.output) / (double) (b.source - a.source) : 0.0;
        return b.output + (juce::int64) std::llround ((double) (source - b.source) * slope);
    }
    // 二分探索で区間を見つける
    auto it = std::upper_bound (nodes.begin(), nodes.end(), source,
                                [] (juce::int64 s, const TimeMapNode& n) { return s < n.source; });
    const auto& b = *it;
    const auto& a = *(it - 1);
    if (source == a.source)
        return a.output;
    const double t = (double) (source - a.source) / (double) (b.source - a.source);
    return a.output + (juce::int64) std::llround (t * (double) (b.output - a.output));
}

juce::int64 TimeMap::inverse (juce::int64 renderPos) const
{
    if (nodes.empty())
        return 0;
    renderPos = juce::jlimit (nodes.front().output, nodes.back().output, renderPos);
    auto it = std::upper_bound (nodes.begin(), nodes.end(), renderPos,
                                [] (juce::int64 r, const TimeMapNode& n) { return r < n.output; });
    if (it == nodes.begin()) return nodes.front().source;
    if (it == nodes.end()) it = nodes.end() - 1;
    const auto& b = *it;
    const auto& a = *(it - 1);
    juce::int64 candidate = a.source;
    if (b.output > a.output)
    {
        const double t = (double) (renderPos - a.output) / (double) (b.output - a.output);
        candidate = a.source + (juce::int64) std::llround (t * (double) (b.source - a.source));
    }
    candidate = juce::jlimit (a.source, b.source, candidate);
    auto distance = [this, renderPos] (juce::int64 src) { return std::llabs (map (src) - renderPos); };
    while (candidate > nodes.front().source && distance (candidate - 1) <= distance (candidate))
        --candidate;
    while (candidate < nodes.back().source && distance (candidate + 1) < distance (candidate))
        ++candidate;
    return candidate;
}

