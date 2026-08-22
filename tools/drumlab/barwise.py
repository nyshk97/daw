#!/usr/bin/env python3
"""抜粋wavを小節ごとに割って、帯域別の打点数と強さを出す。

「うるさい」は曲ではなく区間の性質だった（同じ曲の前半と後半で判定が割れた）ため、
曲ごとに代表区間の中央値を取る既存featureでは構造的に捉えられない。
小節単位まで降りて、区間内の変化そのものを見る。

  python3 tools/drumlab/barwise.py J W E L S
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf
from scipy import signal

LISTEN = Path.home() / "Music/daw/reference-beat-corpus/review/drum-listen-2"
KEY = LISTEN.parent / "drum-listen-2-key.json"
# groove.json の帯域定義に合わせる（low=キック/808, mid=スネア胴, high=ハット）
BANDS = {"low": (35, 120), "mid": (180, 900), "high": (6500, 10500)}


def band_onsets(y: np.ndarray, sr: int, lo: int, hi: int) -> tuple[np.ndarray, np.ndarray]:
    sos = signal.butter(4, [lo, min(hi, sr // 2 - 1)], btype="band", fs=sr, output="sos")
    yb = signal.sosfilt(sos, y)
    env = librosa.onset.onset_strength(y=yb, sr=sr, hop_length=256)
    frames = librosa.onset.onset_detect(onset_envelope=env, sr=sr, hop_length=256,
                                        backtrack=False, delta=0.25, wait=2)
    times = librosa.frames_to_time(frames, sr=sr, hop_length=256)
    return times, env[frames] if len(frames) else np.array([])


def main() -> None:
    key = {i["label"]: i for i in json.loads(KEY.read_text())["items"]}
    for label in sys.argv[1:]:
        item = key[label]
        y, sr = sf.read(str(LISTEN / f"{label}.wav"), dtype="float32")
        if y.ndim > 1:
            y = y.mean(axis=1)
        bar_s = 4 * 60.0 / float(item["bpm"])
        n_bars = int(round(len(y) / sr / bar_s))

        print(f"\n=== {label}  {item['display_name'][:52]}  {item['bpm']}BPM  {n_bars}小節 ===")
        counts = {}
        for name, (lo, hi) in BANDS.items():
            times, _ = band_onsets(y, sr, lo, hi)
            counts[name] = np.array([int(((times >= b * bar_s) & (times < (b + 1) * bar_s)).sum())
                                     for b in range(n_bars)])
        print("小節  " + " ".join(f"{b + 1:3d}" for b in range(n_bars)))
        for name in ("high", "mid", "low"):
            tag = {"high": "ハット", "mid": "スネア", "low": "キック"}[name]
            print(f"{tag}  " + " ".join(f"{c:3d}" for c in counts[name]))
        h = counts["high"]
        print(f"  ハット: 平均{h.mean():.1f}/小節  最小{h.min()}  最大{h.max()}  "
              f"ばらつき(標準偏差/平均){h.std() / max(h.mean(), 1e-9):.2f}  "
              f"実時間 {h.mean() / bar_s:.2f}打点/秒")


if __name__ == "__main__":
    main()
