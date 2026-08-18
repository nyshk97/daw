#pragma once

#include <cmath>
#include <memory>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

// オーディオクリップの移調・タイムストレッチ（非破壊）の共有データ構造。
// 設計の真実の源: docs/plans/2026-08-18-1028-audio-transpose-stretch.md
//
// 語彙:
// - 原音座標: ソースWAVバッファ内のサンプル位置（Clip::offsetSamples 等はすべてこちら）
// - render座標: 加工後のドメイン内位置（ドメイン先頭からの距離）。描画・再生・タイムラインは
//   こちらの長さ（見かけ長）で動く
// - 要求値: Clip::transposeSemitones / stretchRatio / renderDomain*（まだ音になっていないかも
//   しれない値）。実効値 = activeDomain の中身（鳴っている・見えている音）

// ---- 安全限界（音楽的な制限とは別の、実装上の防御）----
// 要求受付層（UI・JSON読込）はこの範囲へクランプして受理する。
// ClipStretcher の直呼びはクランプせず、範囲外を失敗として弾く（受付層を通らない呼び出しを
// 黙って通さない）。原音を返してごまかすことは絶対にしない
namespace ClipStretchLimits
{
inline constexpr int maxSemitones = 12;         // ±1オクターブ（キー合わせ±6の往復余地）
inline constexpr double minRatio = 0.25;        // 実装上の下限（出力長 >= 1 の担保と品質の底）
inline constexpr double maxRatio = 4.0;         // 実装上の上限（メモリ上限から逆算）
// 1レンダーの出力バッファ上限（float・全ch合計）。3分ステレオ48kHz ≈ 69MB なので
// 4倍ストレッチでも余裕があり、手編集JSONの巨大値はここで確実に落ちる
inline constexpr juce::int64 maxRenderBytes = (juce::int64) 768 * 1024 * 1024;

inline int clampSemitones (int semitones)
{
    return juce::jlimit (-maxSemitones, maxSemitones, semitones);
}

// 受付層のクランプ。非有限・0以下は 1.0（無加工）へ落とす
inline double clampRatio (double ratio)
{
    if (! std::isfinite (ratio) || ratio <= 0.0)
        return 1.0;
    return juce::jlimit (minRatio, maxRatio, ratio);
}
} // namespace ClipStretchLimits

// レンダリング要求・結果の識別キー。「結果をクリップへ送る」のではなく「クリップが自分の
// 指紋で引く」ためのもの（Clip::id を新設しない設計。planの「非同期結果の返し先」参照）。
// source は原音バッファのアドレス。誤ヒット（解放後のアドレス再利用）は
// RenderedDomain / Request が原音の shared_ptr を持つことで防ぐ
struct RenderFingerprint
{
    const void* source = nullptr;
    juce::int64 domainOffset = 0;
    juce::int64 domainLength = 0;
    int semitones = 0;
    double ratio = 1.0;
    double sampleRate = 0.0;

    bool isNeutral() const { return semitones == 0 && juce::exactlyEqual (ratio, 1.0); }

    bool operator== (const RenderFingerprint& other) const
    {
        // 指紋は「同じ値か」の識別なので厳密比較が正しい（近似だと別要求が同一視される）
        return source == other.source && domainOffset == other.domainOffset
            && domainLength == other.domainLength && semitones == other.semitones
            && juce::exactlyEqual (ratio, other.ratio)
            && juce::exactlyEqual (sampleRate, other.sampleRate);
    }
    bool operator!= (const RenderFingerprint& other) const { return ! (*this == other); }

    // std::map のキー用
    bool operator< (const RenderFingerprint& other) const
    {
        if (source != other.source) return source < other.source;
        if (domainOffset != other.domainOffset) return domainOffset < other.domainOffset;
        if (domainLength != other.domainLength) return domainLength < other.domainLength;
        if (semitones != other.semitones) return semitones < other.semitones;
        if (! juce::exactlyEqual (ratio, other.ratio)) return ratio < other.ratio;
        return sampleRate < other.sampleRate;
    }
};

// 加工結果のキャッシュ単位。**生成後は書き換えない**（不変）。分割・複製で複数クリップが
// 同じ1本を共有し、クリップごとの view（部分範囲）は Clip 側が絶対境界の差から導く。
// view をここに持たせないのは、分割後の左右が同じドメインを共有しながら別々の範囲を
// 表現する必要があるため
struct RenderedDomain
{
    // ドメイン全体の加工済みバッファ。無加工なら原音そのもの
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    // 原音への強参照。キャッシュに残っている間はこのアドレスが再利用されず、
    // 指紋（アドレス識別）の誤ヒットが起きない
    std::shared_ptr<const juce::AudioBuffer<float>> sourceAudio;
    // audio 内でドメイン先頭が始まる位置（無加工 = domainOffset / 加工済み = 0）。
    // 落とすと無加工の分割済みクリップが原音の先頭から鳴る
    juce::int64 audioBaseOffset = 0;
    // 覆っている原音範囲（原音座標）
    juce::int64 domainOffset = 0;
    juce::int64 domainLength = 0;
    int semitones = 0;
    double ratio = 1.0;
    double sampleRate = 0.0;
    // ドメイン全体の描画用ピークキャッシュ（Clip::samplesPerPeak 単位・render座標に整列）。
    // クリップは部分範囲を参照して描く（チョップを何個作ってもピークの再計算が起きない）
    std::vector<float> peakCache;

    // 原音絶対位置 → render座標（ドメイン先頭からの距離）。
    // ⚠️ 原音座標と実効座標をまたぐ計算は必ずここを通す。相対値に × ratio / ÷ ratio を
    // 直接使うと、隣接 view の境界が一致せずバッファ終端を越えて読む（planの退化ケース参照）
    juce::int64 mapBoundary (juce::int64 srcPos) const
    {
        return (juce::int64) std::llround ((double) (srcPos - domainOffset) * ratio);
    }

    juce::int64 renderedDomainLength() const { return mapBoundary (domainOffset + domainLength); }

    // render座標 → 原音境界の逆変換。mapBoundary が丸めを含むため単純な ÷ ratio では戻らない。
    // 「mapBoundary(戻り値) が renderPos に最も近くなる原音位置」を返す（等距離なら小さい方）。
    // renderPos が到達可能な値なら mapBoundary(戻り値) == renderPos が保証される
    juce::int64 sourceForBoundary (juce::int64 renderPos) const
    {
        const auto safeRatio = ratio > 0.0 && std::isfinite (ratio) ? ratio : 1.0;
        auto candidate = domainOffset + (juce::int64) std::llround ((double) renderPos / safeRatio);
        candidate = juce::jlimit (domainOffset, domainOffset + domainLength, candidate);
        // 丸め誤差は±1に収まるので近傍を線形に詰める（mapBoundary は単調非減少）
        auto distance = [this, renderPos] (juce::int64 src)
        { return std::abs (mapBoundary (src) - renderPos); };
        while (candidate > domainOffset && distance (candidate - 1) <= distance (candidate))
            --candidate;
        while (candidate < domainOffset + domainLength && distance (candidate + 1) < distance (candidate))
            ++candidate;
        return candidate;
    }

    // [r0, r1)（render座標）の絶対値ピーク。ピークキャッシュの集約単位（512サンプル）に
    // 完全に含まれる区間はキャッシュを使い、端の部分区間だけ実バッファから集計し直す。
    // view の両端は512区切りに揃わないため、キャッシュだけで描くと**境界の外にあるピークが
    // 見えてしまう**（チョップした右側に、左側にしかないトランジェントが描かれる）
    float peakBetween (juce::int64 r0, juce::int64 r1, int samplesPerPeak) const
    {
        if (audio == nullptr || r1 <= r0)
            return 0.0f;
        const auto domainEnd = renderedDomainLength();
        r0 = juce::jlimit ((juce::int64) 0, domainEnd, r0);
        r1 = juce::jlimit ((juce::int64) 0, domainEnd, r1);
        if (r1 <= r0)
            return 0.0f;

        float peak = 0.0f;
        const int numChannels = juce::jmin (2, audio->getNumChannels());
        const auto bufferLength = (juce::int64) audio->getNumSamples();
        auto scanAudio = [&] (juce::int64 a0, juce::int64 a1)
        {
            a0 = juce::jlimit ((juce::int64) 0, bufferLength, audioBaseOffset + a0);
            a1 = juce::jlimit ((juce::int64) 0, bufferLength, audioBaseOffset + a1);
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* data = audio->getReadPointer (ch);
                for (juce::int64 i = a0; i < a1; ++i)
                    peak = juce::jmax (peak, std::abs (data[i]));
            }
        };

        const auto step = (juce::int64) samplesPerPeak;
        for (auto pos = r0; pos < r1;)
        {
            const auto binIndex = pos / step;
            const auto binStart = binIndex * step;
            const auto binEnd = juce::jmin (binStart + step, domainEnd);
            const auto segEnd = juce::jmin (binEnd, r1);
            const bool fullBin = pos == binStart && segEnd == binEnd
                              && binIndex < (juce::int64) peakCache.size();
            if (fullBin)
                peak = juce::jmax (peak, peakCache[(size_t) binIndex]);
            else
                scanAudio (pos, segEnd);
            pos = segEnd;
        }
        return peak;
    }
};

// バッファの [start, start + length) から絶対値ピーク列を作る（samplesPerPeak 単位・
// ステレオはL/Rのmax合成）。実装は Project.cpp（RenderedDomain の生成側から使う）
std::vector<float> buildDomainPeakCache (const juce::AudioBuffer<float>& audio,
                                         juce::int64 start, juce::int64 length,
                                         int samplesPerPeak);
