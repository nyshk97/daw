#pragma once

#include <optional>
#include <juce_core/juce_core.h>

// プロジェクトのキー（曲の調）。生成MIDI（ベースガチャ等）は常にこのキーで作る。
// キーは「曲がどの音を中心に回るか」の宣言で、BPMと同格の基本情報として扱う
// （BPMがグリッドの座標系なら、キーは音高の座標系）。拍子4/4固定と同じ引き算で
// モードは major / minor の2択（教会旋法は扱わない）
enum class KeyMode { major, minor };

struct ProjectKey
{
    int root = 0;                    // ピッチクラス 0..11（0=C）
    KeyMode mode = KeyMode::minor;

    bool operator== (const ProjectKey& other) const { return root == other.root && mode == other.mode; }
    bool operator!= (const ProjectKey& other) const { return ! (*this == other); }
};

namespace ProjectKeys
{
    juce::String modeName (KeyMode mode);                        // "major" / "minor"（JSON・CLI用）
    bool modeFromName (const juce::String& name, KeyMode& out);  // 未知名は false

    juce::String rootName (int root);                            // 表示用 "C" "C♯" …（♯表記固定）
    bool rootFromName (const juce::String& name, int& out);      // "C#"/"Db"/"F♯" 等（カードの値）を受ける

    // 表示名。Logic等の慣例に合わせ major は素の音名、minor は m を付ける（例: "F♯" / "F♯m"）
    juce::String displayName (const ProjectKey& key);

    // bass.py の --key 形式（ASCII。例: "F#:minor"）
    juce::String cliText (const ProjectKey& key);

    // カードの表示テキスト "F# major"（ReferenceAnalyzer::Result::keyText）→ ProjectKey。
    // 読めなければ nullopt（キーのゲート落ちで空のことがある）
    std::optional<ProjectKey> fromCardText (const juce::String& text);
}
