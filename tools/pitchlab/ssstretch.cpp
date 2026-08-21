// ssstretch — signalsmith-stretch を「時変ピッチシフト＋区分線形の時間写像」で駆動する lab 用 CLI。
//
// 使い方: ssstretch <in.wav> <out.wav> <map.txt> [--no-formant]
//
// map.txt（Python 側 resynth.py が生成。すべてサンプル単位・入力 SR 基準）:
//   nodes <N>                 以降 N 行: "<inSample> <outSample>"  単調増加の時間写像ノード
//                             （先頭は 0 0、末尾の outSample が出力長）
//   hop <H>                   ピッチ列のホップ（出力サンプル）
//   semis <M>                 以降 M 行: "<semitones> <f0Hz>"  出力位置 k*H でのシフト量と
//                             フォルマント推定用の元 f0（0=検出器任せ）
//
// 処理: 出力を H サンプルずつ進め、各ブロックで setTransposeSemitones を更新し、
// 時間写像の逆写像 + 内部レイテンシ分だけ入力を供給する（cmd/main.cpp の手順をブロック化）。
// フォルマントは setFormantFactor(1, compensatePitch=true)（=保持）を常時 ON、
// setFormantBase に元 f0 を渡す（README: 位相ボコーダーの補正は PSOLA ほど鋭くない、と明記あり）。
// WAV 入出力は signalsmith 付属 cmd/util/wav.h（16bit）。lab の聴き比べ用途なので十分。
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "signalsmith-stretch/signalsmith-stretch.h"
#include "util/wav.h"

using Stretch = signalsmith::stretch::SignalsmithStretch<float>;

namespace {
struct Map {
    std::vector<double> inNodes, outNodes; // 単調増加
    int hop = 256;
    std::vector<float> semis, f0;
};

bool readMap(const std::string& path, Map& m) {
    std::ifstream f(path);
    if (!f) return false;
    std::string key;
    while (f >> key) {
        if (key == "nodes") {
            int n; f >> n;
            m.inNodes.resize(n); m.outNodes.resize(n);
            for (int i = 0; i < n; ++i) f >> m.inNodes[i] >> m.outNodes[i];
        } else if (key == "hop") {
            f >> m.hop;
        } else if (key == "semis") {
            int n; f >> n;
            m.semis.resize(n); m.f0.resize(n);
            for (int i = 0; i < n; ++i) f >> m.semis[i] >> m.f0[i];
        } else {
            std::cerr << "unknown key: " << key << "\n";
            return false;
        }
    }
    return m.inNodes.size() >= 2 && !m.semis.empty() && m.hop > 0;
}

// 出力座標 → 入力座標（区分線形・端は外挿）
double inverseMap(const Map& m, double out) {
    const auto& xs = m.outNodes; const auto& ys = m.inNodes;
    size_t i = 1;
    while (i + 1 < xs.size() && out > xs[i]) ++i;
    const double dx = xs[i] - xs[i - 1];
    const double t = dx > 0 ? (out - xs[i - 1]) / dx : 0.0;
    return ys[i - 1] + t * (ys[i] - ys[i - 1]);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: ssstretch <in.wav> <out.wav> <map.txt> [--no-formant]\n";
        return 2;
    }
    bool formant = true;
    for (int i = 4; i < argc; ++i) if (std::string(argv[i]) == "--no-formant") formant = false;

    Map map;
    if (!readMap(argv[3], map)) { std::cerr << "bad map\n"; return 1; }

    Wav in;
    if (!in.read(argv[1]).warn()) { std::cerr << "read failed\n"; return 1; }
    const int channels = int(in.channels);
    const double sr = in.sampleRate;
    const size_t inLength = in.length();
    const size_t outLength = size_t(std::llround(map.outNodes.back()));

    Stretch stretch(0x5eed); // LaLa 本体と同じ固定シード
    stretch.presetDefault(channels, float(sr));
    if (formant) stretch.setFormantFactor(1, true);

    // 入力は末尾にレイテンシ分の無音を足して、逆写像＋inputLatency が範囲内に収まるようにする
    const int inLat = stretch.inputLatency();
    const int outLat = stretch.outputLatency();
    in.offset = 0;
    in.resize(inLength + size_t(inLat) + size_t(stretch.blockSamples()) * 2);
    const size_t inTotal = in.length(); // 注意: wav.h の length() は offset を引いた残量を返すので、総長はここで固定

    Wav out;
    out.channels = in.channels; out.sampleRate = in.sampleRate;
    out.resize(outLength + size_t(stretch.blockSamples()));

    // 先頭: outputSeek（入力先頭のプリロール。Python 側は助走を前置しているので通常は無音）
    const double startRate = (map.inNodes[1] - map.inNodes[0]) / std::max(1.0, map.outNodes[1] - map.outNodes[0]);
    const int seekLen = stretch.outputSeekLength(float(startRate));
    in.offset = 0;
    stretch.outputSeek(in, seekLen);
    size_t inputConsumed = size_t(seekLen);
    size_t outputDone = 0;

    const size_t flushStart = outLength > size_t(stretch.intervalSamples()) ? outLength - size_t(stretch.intervalSamples()) : 0;
    while (outputDone < flushStart) {
        const size_t blockEnd = std::min(flushStart, outputDone + size_t(map.hop));
        const size_t k = std::min(map.semis.size() - 1, (outputDone + (blockEnd - outputDone) / 2) / size_t(map.hop));
        stretch.setTransposeSemitones(map.semis[k], float(8000.0 / sr));
        if (formant) stretch.setFormantBase(map.f0[k] > 0 ? float(map.f0[k] / sr) : 0.f);

        const double inPos = inverseMap(map, double(blockEnd + outLat));
        size_t inputTarget = size_t(std::llround(inPos)) + size_t(inLat);
        if (inputTarget > inTotal) inputTarget = inTotal;
        if (inputTarget < inputConsumed) inputTarget = inputConsumed;

        in.offset = inputConsumed; out.offset = outputDone;
        stretch.process(in, int(inputTarget - inputConsumed), out, int(blockEnd - outputDone));
        inputConsumed = inputTarget; outputDone = blockEnd;
    }
    out.offset = outputDone;
    stretch.flush(out, int(outLength - outputDone));
    out.offset = 0;
    out.resize(outLength);

    if (!out.write(argv[2]).warn()) { std::cerr << "write failed\n"; return 1; }
    std::printf("ssstretch ok: in=%zu out=%zu hop=%d blocks=%zu formant=%d\n", inLength, outLength, map.hop, map.semis.size(), int(formant));
    return 0;
}
