#pragma once

#include <juce_core/juce_core.h>

// .mid 判定は AudioFileTypes とは別に持つ（AudioFileTypes::isSupported はファイルブラウザの
// 表示・試聴・D&D・Fileメニューで共用されており、そこへ .mid を足すと「MIDIを音声として試聴」
// 「MIDIトラックへのドロップがサンプル音源割り当てに化ける」「AudioImporter へ流れる」の
// 誤動作になる。全入口で MIDI を先に分岐し、専用の取り込み経路（MidiImport）へ渡すこと）
namespace MidiFileTypes
{
inline bool isSupported (const juce::File& file)
{
    return file.hasFileExtension ("mid;midi");
}

inline bool isSupported (const juce::String& path)
{
    return isSupported (juce::File (path));
}
}
