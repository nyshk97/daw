#pragma once

#include <atomic>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/GmInstruments.h"
#include "../shared/Project.h"

// FXスロット構成の単一の真実の源（FxEditorView と MixerStrip が共有する）。
// 以前は両者が同じ構成を別々にハードコードしており、スロット番号 0=EQ / 1=Comp / 2=Ext が
// 暗黙の契約になっていた。番号は意味ID（Id）としてここで固定し、surface別の表示順
//（FXパネル=panelOrder・ミキサー=mixerOrder）も投影規則としてここで定義する。
//
// 既存の番号（onOpenSlot / fxDetailSlot / BottomPanelHistory の外部契約）は動かさない:
// Sat / Lo-fi はバッチ3で末尾に追加した番号で、**表示位置とIDは別物**
//（表示は EQ, Comp, Sat, Lo-fi, Ext の順だがIDは 0,1,4,5,2）。
// ミキサー側が表示位置をそのままIDとして使うと ID3=Instrument を誤参照するため、
// 表示位置→IDの変換は必ず mixerOrder / panelOrder を経由する
namespace FxSlots
{
enum Id : int { eq = 0, comp = 1, ext = 2, instrument = 3, sat = 4, lofi = 5 };

inline constexpr int maxSlots = 6;   // FXパネルの最大（MIDIトラック: Instrument込み）
inline constexpr int mixerSlots = 5; // ミキサーの表示枠（Instrumentを除外した投影。
                                     // ミキサーに出すかは未決の機能判断＝現状は出さない）

// ミキサーの表示位置→意味ID投影（縦積みの上からこの順）
inline constexpr Id mixerOrder[mixerSlots] = { eq, comp, sat, lofi, ext };

// FXパネルの表示順（音源＝Instrumentが一番上・以降はミキサーと同順）
inline constexpr Id panelSequence[maxSlots] = { instrument, eq, comp, sat, lofi, ext };

// 表示位置の逆引き（GRミニバー等「特定IDのピルはどこか」用。無ければ-1）
inline constexpr int mixerPositionOf (Id id)
{
    for (int i = 0; i < mixerSlots; ++i)
        if (mixerOrder[i] == id)
            return i;
    return -1;
}

// GR（ゲインリダクション）ミニバーを載せるスロット
inline constexpr Id grSlot = comp;

// バス/MasterのFX名（FXパネルとミキサーのスロット表記を揃える）
inline juce::String busFxName (int busIndex) { return busIndex == 2 ? "Delay" : "Reverb"; }
inline juce::String masterFxName() { return "Limiter"; }

struct Slot
{
    juce::String name;
    std::atomic<bool>* enabled = nullptr; // 電源トグルの実体（nullptr = トグルなし）
    bool placeholder = false;             // 空き/操作不可のグレー表示
    bool used = false;                    // このチャンネルに存在するスロットか
};

struct Layout
{
    Slot slots[maxSlots]; // 配列index = スロット番号（= Id）。存在は used で判定する
};

// 内蔵GM音源の表示名（ヘッダーの楽器プルダウンと同じ対応表。一致が無ければ先頭＝Piano）
inline juce::String gmInstrumentName (const Track& track)
{
    for (int i = 0; i < numGmInstruments; ++i)
        if (gmInstruments[i].program == track.gmProgram && gmInstruments[i].drums == track.drums
            && gmInstruments[i].fixedPitch == track.drumPitch)
            return gmInstruments[i].name;
    return gmInstruments[0].name;
}

// トラックの基本5枠（EQ/Comp/Sat/Lo-fi/Ext）。ON/OFFを持つのはEQ・Comp・Sat・Lo-fi
//（enabled の実体は TrackParams）。Ext は外部AU（スライス6）まで空き表示。
// ミキサーはこの5枠を mixerOrder の順で表示する
inline Layout trackBaseLayout (TrackParams* params)
{
    Layout layout;
    layout.slots[eq] = { "EQ", params != nullptr ? &params->eqEnabled : nullptr, false, true };
    layout.slots[comp] = { "Comp", params != nullptr ? &params->compEnabled : nullptr, false, true };
    layout.slots[sat] = { "Sat", params != nullptr ? &params->satEnabled : nullptr, false, true };
    layout.slots[lofi] = { "Lo-fi", params != nullptr ? &params->lofiEnabled : nullptr, false, true };
    layout.slots[ext] = { "Ext", nullptr, true, true };
    return layout;
}

// FXパネル用: MIDIトラックは Instrument スロット（番号3）を追加。サンプル割当時だけ
// 点灯してクリック可、内蔵GM音源のときは Ext と同じグレー表示（音源の差し替えは
// ヘッダーのプルダウンで行う）。表示順は panelOrder が決める
inline Layout trackPanelLayout (const Track& track)
{
    auto layout = trackBaseLayout (track.params.get());
    if (track.type == TrackType::midi)
    {
        const bool sampler = track.usesSampler();
        const auto name = sampler ? (track.sampleName.isNotEmpty() ? track.sampleName
                                                                   : juce::String ("Sample"))
                                  : gmInstrumentName (track);
        layout.slots[instrument] = { name, nullptr, ! sampler, true };
    }
    return layout;
}

// バス/Master（単一スロット番号0・常在＝バイパスなし）
inline Layout busLayout (int busIndex)
{
    Layout layout;
    layout.slots[0] = { busFxName (busIndex), nullptr, false, true };
    return layout;
}

inline Layout masterLayout()
{
    Layout layout;
    layout.slots[0] = { masterFxName(), nullptr, false, true };
    return layout;
}

// FXパネルの表示順（panelSequence のうち used なスロットだけ）。order へスロット番号を
// 詰めて個数を返す。番号（配列index）と並びを分離しているのは、既存の外部契約
//（スロット番号）を動かさずに表示位置を自由にするため
inline int panelOrder (const Layout& layout, int (&order)[maxSlots])
{
    int n = 0;
    for (const Id id : panelSequence)
        if (layout.slots[id].used)
            order[n++] = id;
    return n;
}
} // namespace FxSlots
