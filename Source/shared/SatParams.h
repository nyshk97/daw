#pragma once

#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

// トラックサチュレーションのパラメータ定義とDSP/UI共有の伝達カーブ計算。
// plan: docs/plans/2026-08-16-2058-fx-batch3-saturation-lofi.md
//
// サチュレーション＝音量の頭を軽く潰して倍音を足す道具（細い音を太く・埋もれる音を前に出す）。
// 「操作は削る、概念は隠さない」（ui-principles.md）:
//   - ノブは Drive / Mix の2個のみ。カーブは非対称ソフトクリップ1種固定（タイプ切替なし）
//   - 出力レベルは自動補償（下記 compensationGain）＝Driveを上げても音量がほぼ変わらず、
//     倍音の変化だけをA/Bできる（ラウドネスバイアス対策）
//
// 伝達カーブ（描画・倍音バー・DSPの全部がこの式を使う＝描画と音が同じ数式）:
//   f(x) = (tanh(g·x + b) − tanh(b)) / (g·sech²(b))、b = β·g（バイアスはドライブ比例）
//   - g→0 で f(x)→x に連続収束（Driveを0から動かした瞬間に不連続が出ない）
//   - 小信号傾きは全 g で厳密に f'(0)=1
//   - b≠0 の非対称が偶数倍音（暖かさ）の源。βはFFT実測で決定（下記コメント）
namespace Sat
{
// Driveノブ(0..1)→カーブ強度 g への写像の最大値。knob=1 で g=10（2乗カーブ経由）。
// β=0.15 との組で knob50% の倍音が H2≈-25dBc / H3≈-46dBc（偶数優勢の暖かい性格）、
// -6dBFS正弦の補償誤差が全Drive域で -2.5〜0dB に収まることを数値評価で確認済み
inline constexpr float maxDriveGain = 10.0f;
inline constexpr float biasBeta = 0.15f;

// これ未満の g は線形（恒等写像）として扱う数値安定化の閾値。
// f は g→0 で x に収束するため、この切替で不連続は生じない
inline constexpr float minDriveGain = 1.0e-4f;

// 自動補償の基準入力振幅（-18dBFS）。この振幅の正弦のAC RMSが不変になるよう出力を補償する。
// 基準以外の入力レベルでは誤差が残る（固定補償は熱い入力の圧縮量まで直せない）＝「ほぼ一定」
inline constexpr float compReferenceAmp = 0.12589254f;

// プレーン値（保存・バウンス・UI表示用）。既定は中立スタート（ONにしても音が変わらない）
struct Values
{
    float drive = 0.0f; // 0..1（表示は%）
    float mix = 1.0f;   // 0..1 dry/wet
};

inline constexpr Values defaults {};

// 音響的に素通しか（高速パス・バウンスactive判定・producesTailの材料）。
// drive==0 は恒等写像・mix==0 はdry 100%、どちらも中立
inline bool isNeutral (const Values& value)
{
    return value.drive <= 0.0f || value.mix <= 0.0f;
}

// 範囲クランプ＋非有限値（NaN/inf。手編集JSONで混入しうる）の既定値化。
// 読込と全代入経路で通すこと
inline Values normalized (Values value)
{
    if (! std::isfinite (value.drive))
        value.drive = defaults.drive;
    if (! std::isfinite (value.mix))
        value.mix = defaults.mix;
    value.drive = juce::jlimit (0.0f, 1.0f, value.drive);
    value.mix = juce::jlimit (0.0f, 1.0f, value.mix);
    return value;
}

// RT共有用（TrackParams に置く）。UI書込とオーディオ読取が並行するため個別atomic。
// 代入前に normalized() を通すのは書き込み側の責任
struct Params
{
    std::atomic<float> drive { defaults.drive };
    std::atomic<float> mix { defaults.mix };

    static_assert (std::atomic<float>::is_always_lock_free);
};

inline void store (Params& params, const Values& value)
{
    params.drive.store (value.drive);
    params.mix.store (value.mix);
}

inline Values load (const Params& params)
{
    return { params.drive.load(), params.mix.load() };
}

// Driveノブ(0..1) → g。2乗カーブで浅域の分解能を確保（実用域はノブ前半に集まる）
inline float driveGain (float drive)
{
    return drive * drive * maxDriveGain;
}

// カーブの前計算値（ブロック/サンプル単位で作り、transfer/transferAD が共有する）。
// g < minDriveGain は線形（isLinear）＝恒等写像
struct Curve
{
    float g = 0.0f, b = 0.0f, tanhB = 0.0f, invDenom = 1.0f; // invDenom = 1/(g·sech²(b))

    static Curve fromDriveGain (float gainValue)
    {
        Curve c;
        if (gainValue < minDriveGain)
            return c; // 線形
        c.g = gainValue;
        c.b = biasBeta * gainValue;
        c.tanhB = std::tanh (c.b);
        const float sech2 = 1.0f - c.tanhB * c.tanhB;
        c.invDenom = 1.0f / (gainValue * sech2);
        return c;
    }

    static Curve fromDrive (float drive) { return fromDriveGain (driveGain (drive)); }

    bool isLinear() const { return g <= 0.0f; }
};

// 伝達カーブ f(x)。小信号傾き1・g→0 で恒等写像。
// 内部は double: (tanh(g·x+b) − tanh(b)) は小さい g で桁落ちし、invDenom（最大1/minDriveGain）
// が誤差を増幅するため、float 内部だとDriveが0付近を平滑通過する間にノイズが出る
inline float transfer (const Curve& c, float x)
{
    if (c.isLinear())
        return x;
    const double t = std::tanh ((double) c.g * x + (double) c.b);
    return (float) ((t - (double) c.tanhB) * (double) c.invDenom);
}

// log(cosh(z)) の安定形（double）。素の実装は |z| が大きいと cosh がオーバーフローする
inline double logcosh (double z)
{
    const double az = std::abs (z);
    return az + std::log1p (std::exp (-2.0 * az)) - 0.6931471805599453; // − log 2
}

// f の不定積分 F(x)（1次ADAA用）。−tanh(b) に対応する線形項 −x·tanh(b) を含む
//（落とすとADAA出力がUIの伝達カーブと定数分ずれ、DC除去で隠す形になる）。線形時は x²/2。
// 戻り値も double: ADAAの差分商 (F(x)−F(x0))/(x−x0) は近接入力で桁落ちするため、
// float に落とすのは商を取った後（TrackSaturator側）に限る
inline double transferAD (const Curve& c, float x)
{
    if (c.isLinear())
        return 0.5 * (double) x * (double) x;
    const double lc = logcosh ((double) c.g * x + (double) c.b);
    return (lc / (double) c.g - (double) x * (double) c.tanhB) * (double) c.invDenom;
}

// 自動レベル補償ゲイン: 基準入力（-18dBFS正弦）をカーブに通したAC RMS（DC除去後を模す
// ため平均を引く）が入力RMSと一致する逆比。drive の決定的な関数（プログラム非依存）。
// 64点求積・超越関数64回なので毎サンプルは呼ばない（drive変化時のみブロック頭で再計算）
inline float compensationGain (float drive)
{
    const Curve c = Curve::fromDrive (drive);
    if (c.isLinear())
        return 1.0f;
    constexpr int numPoints = 64;
    double sum = 0.0, sumSq = 0.0;
    for (int k = 0; k < numPoints; ++k)
    {
        const double theta = juce::MathConstants<double>::twoPi * (k + 0.5) / numPoints;
        const double y = transfer (c, compReferenceAmp * (float) std::sin (theta));
        sum += y;
        sumSq += y * y;
    }
    const double mean = sum / numPoints;
    const double acRms = std::sqrt (juce::jmax (0.0, sumSq / numPoints - mean * mean));
    const double targetRms = compReferenceAmp / juce::MathConstants<double>::sqrt2;
    return acRms > 1.0e-12 ? (float) (targetRms / acRms) : 1.0f;
}
} // namespace Sat
