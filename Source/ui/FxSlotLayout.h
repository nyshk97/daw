#pragma once

#include <atomic>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/GmInstruments.h"
#include "../shared/Project.h"

// FXスロット構成の単一の真実の源（FxEditorView と MixerStrip が共有する）。
// 以前は両者が同じ構成を別々にハードコードしており、スロット番号 0=EQ / 1=Comp / 2=Ext が
// 暗黙の契約になっていた。番号は意味ID（Id）としてここで固定し、surface別の表示
// （FXパネル=全スロット・ミキサー=Instrumentを除いた3枠）も投影規則としてここで定義する
namespace FxSlots
{
// 意味ID。値はスロット番号そのもの（外部契約: onOpenSlot / fxDetailSlot /
// BottomPanelHistory が同じ番号を指す）。Instrument は末尾＝既存の番号を動かさない
enum Id : int { eq = 0, comp = 1, ext = 2, instrument = 3 };

inline constexpr int maxSlots = 4;   // FXパネルの最大（MIDIトラック: Instrument込み）
inline constexpr int mixerSlots = 3; // ミキサーの表示枠（Instrumentを除外した投影。
                                     // ミキサーに出すかは未決の機能判断＝現状は出さない）

// GR（ゲインリダクション）ミニバーを載せるスロット
inline constexpr int grSlot = comp;

// バス/MasterのFX名（FXパネルとミキサーのスロット表記を揃える）
inline juce::String busFxName (int busIndex) { return busIndex == 2 ? "Delay" : "Reverb"; }
inline juce::String masterFxName() { return "Limiter"; }

struct Slot
{
    juce::String name;
    std::atomic<bool>* enabled = nullptr; // 電源トグルの実体（nullptr = トグルなし）
    bool placeholder = false;             // 空き/操作不可のグレー表示
};

struct Layout
{
    Slot slots[maxSlots]; // 配列index = スロット番号（= Id）
    int count = 0;        // 有効スロット数（トラック=3 or 4・バス/Master=1）
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

// トラックの基本3枠（EQ/Comp/Ext）。ON/OFFを持つのはEQ・Compのみ（enabled の実体は
// TrackParams）。Ext は外部AU（スライス6）まで空き表示。ミキサーはこの3枠だけを表示する
inline Layout trackBaseLayout (TrackParams* params)
{
    Layout layout;
    layout.slots[eq] = { "EQ", params != nullptr ? &params->eqEnabled : nullptr, false };
    layout.slots[comp] = { "Comp", params != nullptr ? &params->compEnabled : nullptr, false };
    layout.slots[ext] = { "Ext", nullptr, true };
    layout.count = mixerSlots;
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
        layout.slots[instrument] = { name, nullptr, ! sampler };
        layout.count = maxSlots;
    }
    return layout;
}

// バス/Master（単一スロット番号0・常在＝バイパスなし）
inline Layout busLayout (int busIndex)
{
    Layout layout;
    layout.slots[0] = { busFxName (busIndex), nullptr, false };
    layout.count = 1;
    return layout;
}

inline Layout masterLayout()
{
    Layout layout;
    layout.slots[0] = { masterFxName(), nullptr, false };
    layout.count = 1;
    return layout;
}

// FXパネルの表示順（音源＝Instrumentが一番上・以降は番号順）。order へスロット番号を
// 詰めて個数を返す。番号（配列index）と並びを分離しているのは、ミキサー側の
// スロット番号（0=EQ固定）と互換を保ちながら Instrument を追加するため
inline int panelOrder (const Layout& layout, int (&order)[maxSlots])
{
    int n = 0;
    if (layout.count > instrument)
        order[n++] = instrument;
    for (int i = 0; i < juce::jmin (layout.count, mixerSlots); ++i)
        order[n++] = i;
    return n;
}
} // namespace FxSlots
