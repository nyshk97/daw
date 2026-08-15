#pragma once

#include <cmath>
#include <iterator>

#include <juce_core/juce_core.h>

// BPM逆算（planの仕様）: 拍子は4/4固定。区間の秒数と拍数候補 {4,8,16,32,64}
// （=1/2/4/8/16小節）から BPM = 拍数 ÷ 秒数 × 60 を逆算する。
// 70〜110 BPM は2倍未満の幅なので範囲内に落ちる候補は高々1つ — それを自動採用し、
// 無ければ範囲に最も近い候補を採用する。検出器は積まない（半分/倍テンポ問題の構造的回避）。
namespace BpmMath
{
inline constexpr int beatCandidates[] = { 4, 8, 16, 32, 64 };
inline constexpr double bpmRangeLow = 70.0;
inline constexpr double bpmRangeHigh = 110.0;

inline double bpmFor (int beats, double seconds)
{
    return seconds > 0.0 ? beats / seconds * 60.0 : 0.0;
}

// 区間秒数から拍数を自動選択する。範囲内の候補があればそれ（高々1つ）、
// 無ければ「範囲からの距離」最小の候補。距離が同点のとき（例: 範囲の上下に等距離）は
// 範囲中心に近い側へ倒す
inline int autoBeats (double seconds)
{
    int best = beatCandidates[0];
    double bestRangeDist = -1.0, bestCenterDist = 0.0;
    for (const int beats : beatCandidates)
    {
        const double bpm = bpmFor (beats, seconds);
        if (bpm >= bpmRangeLow && bpm <= bpmRangeHigh)
            return beats;
        const double rangeDist = bpm < bpmRangeLow ? bpmRangeLow - bpm : bpm - bpmRangeHigh;
        const double centerDist = std::abs (bpm - (bpmRangeLow + bpmRangeHigh) / 2.0);
        if (bestRangeDist < 0.0 || rangeDist < bestRangeDist
            || (juce::exactlyEqual (rangeDist, bestRangeDist) && centerDist < bestCenterDist))
        {
            best = beats;
            bestRangeDist = rangeDist;
            bestCenterDist = centerDist;
        }
    }
    return best;
}

// 拍数の手動修正用: 候補列の中で次の値へ循環する（クリックで一巡り）
inline int nextBeats (int beats)
{
    constexpr int n = (int) std::size (beatCandidates);
    for (int i = 0; i < n; ++i)
        if (beatCandidates[i] == beats)
            return beatCandidates[(i + 1) % n];
    return beatCandidates[0];
}

// 書き出しファイル名: <元ファイル名ベース>_<小節数>bars_<四捨五入BPM>bpm.wav（例 side-a_4bars_92bpm.wav）
inline juce::String exportFileName (const juce::String& sourceBaseName, int beats, double seconds)
{
    const int bars = beats / 4;
    const int bpm = (int) std::llround (bpmFor (beats, seconds));
    return sourceBaseName + "_" + juce::String (bars) + "bars_" + juce::String (bpm) + "bpm.wav";
}

// 表示用: 小数1桁のBPM文字列
inline juce::String bpmDisplayText (int beats, double seconds)
{
    return juce::String (bpmFor (beats, seconds), 1);
}
} // namespace BpmMath
