#pragma once

// FXの「見た目上の種類」。1FX 1色（エフェクト固有色）とラックLEDの有無を引くための表示専用ID。
// FxSlots::Id（スロット番号）とは別物: スロット番号はトラックFXだけを表し、バス/Masterは
// 全て番号0で構築されるため、色をスロット番号で引くと Reverb/Delay/Limiter が EQ と衝突する。
// FxSlots::Id が非スコープenumなので、同名列挙子の衝突を避けるため enum class にしている。
// neutral = 固有色なし（Instrument・Ext など）。LEDも描かない
enum class FxVisualKind
{
    neutral = 0,
    eq,
    comp,
    sat,
    lofi,
    reverbA,
    reverbB,
    delay,
    limiter,
};
