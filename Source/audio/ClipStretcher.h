#pragma once

#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>

// オーディオクリップの移調（半音）・タイムストレッチ（長さのみ変更）のオフラインレンダラー。
// **オフライン専用**（BounceRenderer と同じ扱い）— オーディオスレッドからは絶対に呼ばない
// （内部でメモリ確保・FFT 初期化を行う）。呼び出し元は RenderCache のワーカースレッドとテスト。
//
// エンジンは signalsmith-stretch（MIT・commit SHA で pin。CMakeLists.txt 参照）。
// 乱数シードは固定する — 既定コンストラクタ（std::random_device）だと「読込時に再生成」した
// 音が起動ごとに変わり、回帰テストも不安定になる。
namespace ClipStretcher
{
// source の [domainOffset, domainOffset + domainLength) を semitones 半音移調し、
// 長さ round(domainLength × ratio) へ伸縮した新バッファを返す。モノ/ステレオ両対応。
//
// 参照範囲の前後の原音を助走として食わせる（範囲の先頭が分析窓の途中から始まると頭が滲む）。
// 範囲がバッファ端に接する場合は無音で埋める。
//
// 失敗（nullptr）: 引数不正・安全限界超え（ClipStretchLimits の範囲外・出力長の上限超え）・
// ライブラリの処理失敗。**原音を返してごまかすことはしない**（呼び出し側は旧 activeDomain を
// 維持する責務を持つ）。直呼びはクランプしない — 受付層（UI・JSON読込）を通らない値を
// 黙って通さないため
std::unique_ptr<juce::AudioBuffer<float>> render (const juce::AudioBuffer<float>& source,
                                                  juce::int64 domainOffset,
                                                  juce::int64 domainLength,
                                                  int semitones, double ratio,
                                                  double sampleRate);
} // namespace ClipStretcher
