#pragma once

#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

// 素材トリム（GAIN）のスケール定義。使うのは2箇所で、値域・刻み・見た目の作法を共通にしている:
//   - サンプル音源のGAIN（Track::sampleGain）
//   - オーディオリージョンのゲイン（Clip::gain）
// どちらも「素材の音量ばらつきを吸収する」トリムで、曲の中でのバランスを決めるフェーダーとは別物。
//
// 「スライダーの値 = dB」「モデル = 線形倍率」という対応をここに集約し、
// UI（InstrumentDetailView・リージョンゲインの吹き出し）と読み込みクランプ（Project.cpp）が
// 同じ値域を見るようにする。
//
// 素材ごとの音量ばらつきを吸収するトリムなので、無音（-∞）は持たせない（消音はミュート/フェーダーの仕事。
// トリムに無音域を割くと、実際に使う±数dB付近の解像度が無駄になる）。
// 耳は倍率でなく比率で音量を感じるため、スライダーの位置に対して等間隔にするのは倍率でなくdBの方。
namespace GainScale
{
inline constexpr double rangeDb = 12.0;  // ±12dB（中央がちょうど0dBになるので、つまみ位置で上げ/下げが読める）
inline constexpr double snapWidthDb = 0.3;  // 0dB付近の吸着幅（ドラッグ中のみ。広げると微調整を殺す）

// juce::Decibels::decibelsToGain は std::pow を使う非constexpr関数。inline変数として持つと
// 他のinline変数の初期化順序に巻き込まれてゼロを拾う事故があるため（Theme.h の色で実例あり）、関数で提供する
inline float minLinear() { return juce::Decibels::decibelsToGain ((float) -rangeDb); }  // ≈ 0.2512
inline float maxLinear() { return juce::Decibels::decibelsToGain ((float) rangeDb); }   // ≈ 3.9811

// 線形倍率 → dB。0.0（無音）は -∞ でなく下端（-12dB）に落ちる
inline double toDb (float linear)
{
    return juce::jlimit (-rangeDb, rangeDb,
                         (double) juce::Decibels::gainToDecibels (linear, (float) -rangeDb));
}

// dB → 線形倍率
inline float toLinear (double db)
{
    return juce::Decibels::decibelsToGain ((float) juce::jlimit (-rangeDb, rangeDb, db));
}

// 読み込み時のクランプ。UIが表現できない値をモデルに残すと「表示は-12dBなのに-20dBで鳴る」という
// 表示と実音の乖離になるため、範囲外は読み込んだ時点で寄せる。
// 保存側では使わない。sampleGain / Clip::gain のどちらも代入経路はスライダー・新規作成の1.0f・
// 読み込みの3つだけで、undo/redoはTrackのコピー、リージョンの分割・複製・ループもClipのコピーなので
// 新たな値を生まない＝この3経路が範囲を保証する
inline float clampLinear (float linear)
{
    return juce::jlimit (minLinear(), maxLinear(), linear);
}

// ドラッグ中だけ0dB付近へ吸着させる。プログラムからの同期（refreshFromModel）や
// 数値クリック／ダブルクリックでのリセットは吸着の対象にしない（それらは既にちょうど0dBを渡してくる）
inline double snapDb (double attemptedDb, bool dragging)
{
    if (dragging && std::abs (attemptedDb) <= snapWidthDb)
        return 0.0;
    return attemptedDb;
}

// 表示文字列。上げ側は符号を明示する（+6.0 dB / -3.5 dB / 0.0 dB）
inline juce::String text (double db)
{
    const double shown = std::abs (db) < 0.05 ? 0.0 : db;  // 「-0.0 dB」を出さない
    return (shown > 0.0 ? "+" : "") + juce::String (shown, 1) + " dB";
}
} // namespace GainScale
