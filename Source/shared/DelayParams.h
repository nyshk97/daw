#pragma once

#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

// バスDelay（send用固定バス3 = "Delay"）のパラメータ定義とDSP/UI共有のマッピング計算。
// plan: docs/plans/2026-08-16-2305-fx-batch4-delay-reverb.md
//
// send バスの返しなので Mix ノブは無く full wet（dryはトラック本体から出ている）。
// ノブは Time（テンポ同期4値）/ Feedback / Tone / Ping-pong の4点:
// - Time: 1/16・1/8・1/4・1/2 の音価選択（初期値 1/4 = ボーカルエコーの定石）
// - Feedback: 0〜90%キャップ。自己発振なし＝テールが必ず有限時間で-60dBへ収束し、
//   バウンスの尻尾計算が閉じる
// - Tone: フィードバックループ**内**のローパス（繰り返すたびに暗くなる＝遠ざかる距離感。
//   ループ外だと本体と同じ明るさの繰り返しが並んで邪魔になる）
// - Ping-pong: 入力モノ化→L/R交互出し。減衰は1タップごとに feedback（ストレートと同じ耳あたり）
namespace Delay
{
inline constexpr int numTimeChoices = 4;
// 音価 → 拍（4分音符）換算の倍率。並びはUIのボタン列・保存値（timeIndex）と対応
inline constexpr double timeBeats[numTimeChoices] = { 0.25, 0.5, 1.0, 2.0 };
inline constexpr const char* timeLabels[numTimeChoices] = { "1/16", "1/8", "1/4", "1/2" };

inline constexpr float maxFeedback = 0.9f;

// ディレイ長計算のBPM下限（UIのBPM入力は30..300。手編集JSON・バウンスの安全クランプ20は
// ここで30へ底上げする＝DelayLineの容量契約「1/2音符×30BPM=4秒」を破らない）
inline constexpr double minBpmForTime = 30.0;
inline constexpr double maxDelaySeconds = 4.0; // 1/2音符 × BPM下限30（DelayLine::prepare に渡す）

// Tone: ループ内ローパスのカットオフ。ノブ0=開放20kHz → 1=800Hz の対数カーブ
//（800Hzは「こもったテープエコー」の帯域。500Hzまで落とすと繰り返しが台詞のように
// 不明瞭になるので、Lo-fi Toneより浅めに止める）
inline constexpr float toneOpenHz = 20000.0f;
inline constexpr float toneClosedHz = 800.0f;

inline float toneCutoffHz (float knob)
{
    return toneOpenHz * std::pow (toneClosedHz / toneOpenHz, knob);
}

// 音価×BPM → 秒（DSP・バウンステールの窓計算の両方がこれを使う。再実装禁止）
inline double timeSeconds (double bpm, int timeIndex)
{
    const auto index = juce::jlimit (0, numTimeChoices - 1, timeIndex);
    return timeBeats[index] * 60.0 / juce::jmax (minBpmForTime, bpm);
}

// プレーン値（保存・バウンス・UI表示用）
struct Values
{
    int timeIndex = 2;       // 0..3（timeBeats のindex）。既定 1/4
    float feedback = 0.35f;  // 0..maxFeedback。既定は3〜4回で消える控えめなエコー
    float tone = 0.5f;       // 0..1（既定は中庸 ≈4kHz。素のままより一歩暗い定石の初期値）
    bool pingPong = false;
};

inline constexpr Values defaults {};

// 範囲クランプ＋非有限値（NaN/inf。手編集JSONで混入しうる）の既定値化。
// 読込と全代入経路で通すこと
inline Values normalized (Values value)
{
    value.timeIndex = juce::jlimit (0, numTimeChoices - 1, value.timeIndex);
    if (! std::isfinite (value.feedback))
        value.feedback = defaults.feedback;
    if (! std::isfinite (value.tone))
        value.tone = defaults.tone;
    value.feedback = juce::jlimit (0.0f, maxFeedback, value.feedback);
    value.tone = juce::jlimit (0.0f, 1.0f, value.tone);
    return value;
}

// RT共有用（busParams[2] に置く）。UI書込とオーディオ読取が並行するため個別atomic。
// 代入前に normalized() を通すのは書き込み側の責任
struct Params
{
    std::atomic<int> timeIndex { defaults.timeIndex };
    std::atomic<float> feedback { defaults.feedback };
    std::atomic<float> tone { defaults.tone };
    std::atomic<bool> pingPong { defaults.pingPong };

    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<int>::is_always_lock_free);
};

inline void store (Params& params, const Values& value)
{
    params.timeIndex.store (value.timeIndex);
    params.feedback.store (value.feedback);
    params.tone.store (value.tone);
    params.pingPong.store (value.pingPong);
}

inline Values load (const Params& params)
{
    return { params.timeIndex.load(), params.feedback.load(), params.tone.load(),
             params.pingPong.load() };
}
} // namespace Delay
