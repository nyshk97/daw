#pragma once

#include <functional>
#include <juce_core/juce_core.h>

// URL取り込みの一時ディレクトリ（$TMPDIR/lala-url-<pid>-<uuid>/）の残骸掃除。
// dev版とRelease版は並走できるので、起動時に lala-url-* を無条件で消すと
// 別インスタンスの作業ディレクトリを壊す。名前に埋めたPIDの生存で持ち主を判定する。
// 実削除とプロセス生存確認は呼び出し側に置き、判定だけを純関数にして daw_tests から直接テストする。
namespace TempDirSweep
{
inline const char* namePrefix = "lala-url-";

// 一時ディレクトリを置く親。ワーカー（作る側）と起動時の掃除（消す側）で同じ場所を見るために集約する
inline juce::File rootDirectory()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory);
}

inline juce::String makeDirName (int pid, const juce::String& uniqueId)
{
    return namePrefix + juce::String (pid) + "-" + uniqueId;
}

// names のうち「持ち主のプロセスが居なくなったので消してよい」ものだけを返す。
// PIDが再利用されていた場合は消し損ねるだけで、他インスタンスの作業は壊さない（安全側に倒す）
inline juce::StringArray selectStaleTempDirs (const juce::StringArray& names,
                                              const std::function<bool (int)>& isPidAlive)
{
    juce::StringArray stale;
    const int prefixLength = (int) juce::String (namePrefix).length();

    for (const auto& name : names)
    {
        if (! name.startsWith (namePrefix))
            continue;

        const auto rest = name.substring (prefixLength);
        const auto dash = rest.indexOfChar ('-');
        if (dash <= 0)
            continue; // PID部が空、または uuid との区切りが無い

        const auto pidText = rest.substring (0, dash);
        if (! pidText.containsOnly ("0123456789"))
            continue;

        if (rest.substring (dash + 1).isEmpty())
            continue; // uuid部が空

        const int pid = pidText.getIntValue();
        if (pid <= 0)
            continue;

        if (! isPidAlive (pid))
            stale.add (name);
    }
    return stale;
}
}
