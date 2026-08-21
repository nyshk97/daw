#!/usr/bin/env python3
"""既知 f0 の合成素材（検出器の正解付き評価用）。

のこぎり波 → 簡易フォルマント（2共振）で声に寄せ、ビブラート・しゃくり・わずかなデチューン・無音・
子音代わりのノイズバーストを並べる。真値は hop ごとの f0 と有声フラグ（synth.truth.json）。
出力: work/synth/synth.wav, work/synth/synth.truth.json
"""
from __future__ import annotations

import json

import numpy as np
from scipy.signal import lfilter

from common import WORK, HOP_MS, midi_to_hz, write_wav, hop_samples

SR = 48000

# (midi, 長さ秒, デチューン[cent], ビブラート深さ[cent], しゃくり[半音: 開始時の下からの滑り])
MELODY = [
    (62, 0.55, +25, 0, 0.0),
    (64, 0.45, -30, 0, 1.0),
    (65, 0.70, +15, 40, 0.0),
    (67, 0.50, -20, 0, 0.0),
    (69, 0.90, +35, 50, 1.5),
    (67, 0.40, 0, 0, 0.0),
    (65, 0.60, -25, 30, 0.0),
    (62, 1.10, +10, 60, 0.5),
    (57, 0.50, -40, 0, 0.0),   # 低い語尾（ラップの音域）
    (60, 0.80, +20, 0, 0.0),
]
GAP_SIL = 0.12      # ノート間の無音
BURST = 0.06        # 子音代わりのノイズバースト（無声）
BREATH = 0.25       # 終端の息（無声・低レベル）


def main() -> int:
    rng = np.random.default_rng(7)
    hop = hop_samples(SR)
    f0_samples = []   # サンプル単位の真値 f0（無声=0）
    audio = []

    def add_sil(sec):
        n = int(sec * SR)
        audio.append(np.zeros(n)); f0_samples.append(np.zeros(n))

    def add_noise(sec, level):
        n = int(sec * SR)
        nz = rng.standard_normal(n) * level
        # 高域寄り（s 音っぽく）
        nz = lfilter([1, -0.95], [1], nz)
        env = np.hanning(n)
        audio.append(nz * env); f0_samples.append(np.zeros(n))

    add_sil(0.3)
    for i, (midi, dur, cents, vib, scoop) in enumerate(MELODY):
        if i % 3 == 1:
            add_noise(BURST, 0.08)
        n = int(dur * SR)
        t = np.arange(n) / SR
        m = np.full(n, midi + cents / 100.0)
        if scoop > 0:
            # 先頭 120ms でしゃくり上げ
            k = int(0.12 * SR)
            m[:k] -= scoop * (1 - np.linspace(0, 1, k))
        if vib > 0:
            # ビブラート 5.5Hz、先頭 150ms で深さが立ち上がる
            depth = (vib / 100.0) * np.clip(t / 0.15, 0, 1)
            m += depth * np.sin(2 * np.pi * 5.5 * t)
        f0 = midi_to_hz(m)
        phase = np.cumsum(2 * np.pi * f0 / SR)
        # のこぎり波（帯域制限なしで十分。フォルマントフィルタで高域は落ちる）
        saw = 2.0 * ((phase / (2 * np.pi)) % 1.0) - 1.0
        # 簡易フォルマント: 2つの共振（/a/ っぽく F1=700, F2=1200）
        y = saw
        for fc, bw in ((700, 130), (1200, 160)):
            r = np.exp(-np.pi * bw / SR)
            a = [1, -2 * r * np.cos(2 * np.pi * fc / SR), r * r]
            y = lfilter([1 - r], a, y)
        env = np.ones(n)
        a_n = int(0.02 * SR); r_n = int(0.06 * SR)
        env[:a_n] = np.linspace(0, 1, a_n); env[-r_n:] = np.linspace(1, 0, r_n)
        audio.append(y * env * 0.25); f0_samples.append(f0)
        add_sil(GAP_SIL)
    add_noise(BREATH, 0.02)
    add_sil(0.3)

    x = np.concatenate(audio)
    x = x / np.max(np.abs(x)) * 0.5
    f0s = np.concatenate(f0_samples)
    n_frames = int(np.ceil(len(x) / hop))
    truth_f0 = np.zeros(n_frames); truth_voiced = np.zeros(n_frames, dtype=int)
    for k in range(n_frames):
        c = k * hop
        # フレーム中心の真値。フェードの端 10ms は「どちらでも可」にせず有声扱い（検出器にとって厳しめ）
        v = f0s[min(c, len(f0s) - 1)]
        truth_f0[k] = v; truth_voiced[k] = int(v > 0)

    out = WORK / "synth"
    write_wav(out / "synth.wav", x, SR)
    (out / "synth.truth.json").write_text(json.dumps({
        "sr": SR, "hop": hop, "hop_ms": HOP_MS,
        "f0": truth_f0.tolist(), "voiced": truth_voiced.tolist(),
        "melody": MELODY,
    }))
    print(f"synth: {out/'synth.wav'} ({len(x)/SR:.2f}s, {n_frames} frames)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
