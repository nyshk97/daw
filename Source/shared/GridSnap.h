#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

// 表示グリッドの刻みと、クリックシークのスナップ。タイムラインとピッチ補正エディタで同じ規則を使う
// （同じ場所をクリックしたら同じサンプルへ行く。ui/ に置くと daw_tests で固定できないのでここ）
namespace GridSnap
{
    // 線の間隔が minPx 以上確保できる最も細かい分割（1小節あたり）。上限は 1/16（grid-resolution.md）
    inline int divisionsPerBar (double pxPerBar, double minPx = 12.0)
    {
        int div = 1;
        for (int candidate : { 2, 4, 8, 16 })
            if (pxPerBar / candidate >= minPx)
                div = candidate;
        return div;
    }

    // 表示している線の刻み（1/16 単位）。divisionsPerBar の逆数
    inline int stepSixteenths (double pxPerBar, double minPx = 12.0) { return 16 / divisionsPerBar (pxPerBar, minPx); }

    // 0 起点の格子（長さ gridLen サンプル）へ切り下げ。負の位置は 0 に丸める
    inline juce::int64 floorToGrid (juce::int64 sample, double gridLen)
    {
        if (sample <= 0 || gridLen <= 0.0) return 0;
        return (juce::int64) std::llround (std::floor ((double) sample / gridLen) * gridLen);
    }

    // 描画の開始インデックスを step の倍数に揃える（揃えないと線が 7,11,15… に引かれ、スナップ先と線がずれる）
    inline juce::int64 firstIndexAligned (double startSample, double unitLen, int step)
    {
        const auto first = (juce::int64) std::floor (startSample / unitLen);
        const auto rem = ((first % step) + step) % step;
        return first - rem;
    }
}
