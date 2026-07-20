#pragma once

// GM音源（DLSMusicDevice）用の厳選楽器リスト。
// モデル側は gmProgram / drums の生値を持ち、このリストはUI表示と初期値の対応表として使う
// （リストの並び替え・追加が保存データに影響しないようにするため）。

struct GmInstrument
{
    const char* name;   // UI表示名（ASCIIのみ。日本語を入れるならfromUTF8を通すこと）
    int program;        // GMプログラム番号 0..127（drums==trueのときは未使用）
    bool drums;         // true = MIDIチャンネル10で発音（プログラムチェンジは送らない）
    int fixedPitch;     // >=0: ノートのピッチに関係なく常にこの打楽器で鳴る（1トラック=1打楽器）。-1 = 通常
};

inline constexpr GmInstrument gmInstruments[] = {
    { "Piano",          0,  false, -1 },
    { "Electric Piano", 4,  false, -1 },
    { "Finger Bass",    33, false, -1 },
    { "Synth Bass",     38, false, -1 },
    { "Strings",        48, false, -1 },
    { "Synth Lead",     80, false, -1 },
    { "Synth Pad",      88, false, -1 },
    { "Drum Kit",       0,  true,  -1 },
    { "Kick",           0,  true,  36 },
    { "Snare",          0,  true,  38 },
    { "Closed Hi-Hat",  0,  true,  42 },
    { "Open Hi-Hat",    0,  true,  46 },
    { "Crash",          0,  true,  49 },
};

inline constexpr int numGmInstruments = (int) (sizeof (gmInstruments) / sizeof (gmInstruments[0]));

// GMパーカッションマップ（ch10）。ピアノロールの鍵盤ラベルに使う。範囲外はnullptr
inline const char* gmDrumName (int pitch)
{
    switch (pitch)
    {
        case 35: return "Kick 2";
        case 36: return "Kick 1";
        case 37: return "Side Stick";
        case 38: return "Snare 1";
        case 39: return "Hand Clap";
        case 40: return "Snare 2";
        case 41: return "Low Tom 2";
        case 42: return "Closed HH";
        case 43: return "Low Tom 1";
        case 44: return "Pedal HH";
        case 45: return "Mid Tom 2";
        case 46: return "Open HH";
        case 47: return "Mid Tom 1";
        case 48: return "Hi Tom 2";
        case 49: return "Crash 1";
        case 50: return "Hi Tom 1";
        case 51: return "Ride 1";
        case 52: return "China";
        case 53: return "Ride Bell";
        case 54: return "Tambourine";
        case 55: return "Splash";
        case 56: return "Cowbell";
        case 57: return "Crash 2";
        case 58: return "Vibraslap";
        case 59: return "Ride 2";
        case 60: return "Hi Bongo";
        case 61: return "Low Bongo";
        case 62: return "Mute Conga";
        case 63: return "Open Conga";
        case 64: return "Low Conga";
        case 65: return "Hi Timbale";
        case 66: return "Low Timbale";
        case 67: return "Hi Agogo";
        case 68: return "Low Agogo";
        case 69: return "Cabasa";
        case 70: return "Maracas";
        case 71: return "Short Whistle";
        case 72: return "Long Whistle";
        case 73: return "Short Guiro";
        case 74: return "Long Guiro";
        case 75: return "Claves";
        case 76: return "Hi WoodBlock";
        case 77: return "Low WoodBlock";
        case 78: return "Mute Cuica";
        case 79: return "Open Cuica";
        case 80: return "Mute Triangle";
        case 81: return "Open Triangle";
        default: return nullptr;
    }
}
