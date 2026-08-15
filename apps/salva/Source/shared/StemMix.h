#pragma once

#include <vector>

// ステムのM/S競合規則（Logic/LaLaミキサーと同じ）:
//   - Soloが1つ以上あればSoloされたステムの集合のみが鳴り、複数Soloは加算
//   - MuteはSoloより優先（Solo中でもMuteされていれば鳴らない）
namespace StemMix
{
inline std::vector<bool> audible (const std::vector<bool>& mute, const std::vector<bool>& solo)
{
    const auto n = mute.size();
    bool anySolo = false;
    for (size_t i = 0; i < n && i < solo.size(); ++i)
        anySolo = anySolo || solo[i];

    std::vector<bool> result (n, false);
    for (size_t i = 0; i < n; ++i)
    {
        const bool soloed = i < solo.size() && solo[i];
        result[i] = ! mute[i] && (! anySolo || soloed);
    }
    return result;
}
} // namespace StemMix
