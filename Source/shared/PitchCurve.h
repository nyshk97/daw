#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

// ボーカルのピッチ補正（docs/plans/2026-08-20-2244-vocal-pitch-correction.md）の解析結果。
// 解析は**ソース WAV 単位**（共有原音ごとに1回）で、結果はサイドカー `clip-NNN.<curveDigest>.pitch`
// に世代不変で保存する（再解析で内容が変われば別ファイルが増える。同一内容なら同じ digest＝同じ
// ファイルを再利用。旧世代は GC が消すまで残る）。
//
// フレームの約束（tools/pitchlab の Python 原型と同じ）:
//   hop = 5ms。フレーム k の中心サンプル = k * hopSamples（WAV 先頭基準の絶対座標）。
//   f0[k] = Hz（無声 = 0）、voicing[k] = 有声度 0..1、rms[k] = フレーム中心 ±hop の RMS
//
// 128bit の内容ハッシュ（FNV-1a 64bit × 2 シード）。暗号強度は不要で、目的は
// 「同じ内容か」の識別と、指紋（RenderRecipe）へ入れて古いレンダーを再利用しないこと
struct ContentDigest
{
    std::uint64_t a = 0, b = 0;

    bool isNull() const { return a == 0 && b == 0; }
    juce::String toHex() const; // 32 文字（ファイル名・JSON 用）
    static std::optional<ContentDigest> fromHex (const juce::String& hex);

    bool operator== (const ContentDigest& o) const { return a == o.a && b == o.b; }
    bool operator!= (const ContentDigest& o) const { return ! (*this == o); }
    bool operator< (const ContentDigest& o) const { return a != o.a ? a < o.a : b < o.b; }
};

// 逐次ハッシュ（バイト列を食わせて ContentDigest を得る）
class DigestBuilder
{
public:
    DigestBuilder();
    void add (const void* data, size_t bytes);
    void add (std::uint64_t v) { add (&v, sizeof (v)); }
    void add (std::int64_t v) { add (&v, sizeof (v)); }
    void add (std::int32_t v) { add (&v, sizeof (v)); }
    void add (double v) { add (&v, sizeof (v)); }
    void add (float v) { add (&v, sizeof (v)); }
    void add (const juce::String& s);
    ContentDigest finish() const { return { a, b }; }

private:
    std::uint64_t a, b;
};

// 元 WAV の識別子。サイドカーの WAV と食い違えば未解析扱い（レビュー指摘5）
struct SourceIdentity
{
    juce::int64 frames = 0;
    int channels = 0;
    double sampleRate = 0.0;
    ContentDigest digest; // 全サンプルの内容ハッシュ

    static SourceIdentity of (const juce::AudioBuffer<float>& audio, double sampleRate);
    bool operator== (const SourceIdentity& o) const
    {
        return frames == o.frames && channels == o.channels
            && juce::exactlyEqual (sampleRate, o.sampleRate) && digest == o.digest;
    }
    bool operator!= (const SourceIdentity& o) const { return ! (*this == o); }
};

struct PitchCurve
{
    static constexpr double hopMs = 5.0;
    static int hopSamplesFor (double sampleRate) { return (int) std::llround (sampleRate * hopMs / 1000.0); }

    juce::String algoId;     // 検出アルゴリズムの識別（例: "yin-1"）。パラメータを変えたら変える
    double sampleRate = 0.0;
    int hopSamples = 0;
    SourceIdentity source;
    std::vector<float> f0;
    std::vector<float> voicing;
    std::vector<float> rms;

    int numFrames() const { return (int) f0.size(); }
    bool isVoiced (int k) const { return k >= 0 && k < numFrames() && f0[(size_t) k] > 0.0f; }
    // MIDI ノート番号（小数）。無声フレームは NaN
    double midiAt (int k) const;
    juce::int64 frameCenterSample (int k) const { return (juce::int64) k * hopSamples; }
    int frameForSample (juce::int64 sample) const
    {
        return hopSamples > 0 ? (int) juce::jlimit ((juce::int64) 0, (juce::int64) juce::jmax (0, numFrames() - 1),
                                                    sample / hopSamples)
                              : 0;
    }

    // 内容ハッシュ（algoId・SR・hop・全フレーム値。source は含めない — source はヘッダ側の整合用）
    ContentDigest digest() const;

    // 読込検証: サイズ一致・有限値・値域（f0 は 0 または 30〜2000Hz・有声度 0..1・rms >= 0）
    bool validate (juce::String* why = nullptr) const;
};

namespace PitchSidecar
{
// `clip-001.wav` + digest → `clip-001.<hex32>.pitch`（WAV と同じディレクトリ）
juce::File fileFor (const juce::File& wavFile, const ContentDigest& curveDigest);
// ファイル名だけ（ディレクトリを知らない UndoStack 等が「保持する世代」を列挙するため）
juce::String fileNameFor (const juce::String& wavFileName, const ContentDigest& curveDigest);
// サイドカー名 → 元 WAV 名（"clip-001.<hex32>.pitch" → "clip-001.wav"。形式外は空）
juce::String wavNameFor (const juce::File& sidecar);
// WAV に紐づく既存サイドカーをすべて列挙（GC・世代探索用）
std::vector<juce::File> listFor (const juce::File& wavFile);
// ファイル名から digest を取り出す（形式外なら nullopt）
std::optional<ContentDigest> digestFromFile (const juce::File& sidecar);

// 書き出し: 一時ファイル → atomic replace。同じ digest のファイルが既にあれば書き直さず true。
// 戻り値 false のとき error に理由
bool write (const PitchCurve& curve, const juce::File& wavFile, juce::String* error = nullptr);
// 読込: ヘッダ・サイズ・trailer の digest・validate() をすべて通ったものだけ返す
std::optional<PitchCurve> read (const juce::File& sidecar, juce::String* why = nullptr);
// 指定世代の読込（ファイルが無い／壊れている／source が食い違う → nullopt）
std::optional<PitchCurve> readFor (const juce::File& wavFile, const ContentDigest& curveDigest,
                                   const SourceIdentity& expectedSource, juce::String* why = nullptr);
} // namespace PitchSidecar
