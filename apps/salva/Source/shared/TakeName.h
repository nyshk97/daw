#pragma once

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <juce_core/juce_core.h>

// 録音テイクの自動命名（保存ダイアログなしの録音先行フロー用）。
// テイクは中間素材なので人間は命名しない。名前付きの成果物は区間書き出し（BpmMath::exportFileName）が担う
namespace TakeName
{

// 例: record-20260815-214733.wav（秒まで入れて連続テイクの衝突を避ける）
inline juce::String fileNameFor (const juce::Time& t)
{
    return "record-" + t.formatted ("%Y%m%d-%H%M%S") + ".wav";
}

// 保存先のファイル名を「作成することで」確保する（O_CREAT|O_EXCL の原子操作）。
// exists()確認→録音開始の2段だと、同じ保存先を使う別プロセス（Salva/Salva-dev並走）が
// 間に同名を作る窓が残るため、確保＝作成にして窓をなくす。呼び出し側（Recorder）は
// このファイルをunlinkせずtruncateで書き始めること（消した瞬間に確保が破れる）。
// 同一秒の連続録音は record-..._2.wav の連番。作成に失敗（権限等）したら無効なFileを返す
inline juce::File claimTargetFile (const juce::File& dir, const juce::Time& t)
{
    const auto base = dir.getChildFile (fileNameFor (t));
    for (int n = 1; n < 1000; ++n)
    {
        const auto f = n == 1 ? base
                              : base.getSiblingFile (base.getFileNameWithoutExtension()
                                                     + "_" + juce::String (n) + base.getFileExtension());
        const int fd = ::open (f.getFullPathName().toRawUTF8(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0)
        {
            ::close (fd);
            return f;
        }
        if (errno != EEXIST)
            return {}; // 権限等の失敗。連番を進めても直らないので諦める（呼び出し側のエラー経路へ）
    }
    return {}; // 1000連番まで埋まっているのは異常（同一秒に1000テイクは起きない）
}

} // namespace TakeName
