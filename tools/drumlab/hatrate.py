#!/usr/bin/env python3
"""ハットの打点/秒を実測し、耳の判定と並べる。

「うるさい」は曲でなく区間の性質だった（同じ曲の前半と後半で判定が割れた）ので、
曲全体の代表値だけでなく連続4小節の窓の最大値＝曲中で最も密な場所も出す。

判定はリポジトリの回答JSONから読む（ラベルは素材を作り直すと変わるのでvideo_idで照合）。
検出感度 delta は「はっきり聞こえるか」の代理。小さい打点まで拾う設定にすると
分離が壊れるが、それは肯定側にだけゴーストが存在するためで、詳細は
docs/labs/drum-pattern.md の「検出感度への依存」を読む。

  python3 tools/drumlab/hatrate.py [--delta 0.25]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf
from scipy import signal

REVIEW = Path.home() / "Music/daw/reference-beat-corpus/review"
ANSWERS = Path(__file__).resolve().parents[2] / "docs/labs/reference-beat-human-answers/2026-08-22-drum-pattern-answers.json"
HAT_BAND = (6500, 10500)
WINDOW_BARS = 4
LOUD_DELTA = 0.25   # 「はっきり聞こえる」の代理。round3のA/Bで妥当性を確認済み


def load_answers() -> dict[str, dict]:
    """video_id → 判定。両方にある曲は round1 を採る。

    round2 は正例23本の中へ否定例3本を混ぜたため、その3本の判定が中央へ圧縮された
    （-2→-1 / +2→+1 / -2→0）。方向は一致しているが強度が文脈に依存する。
    round1 は否定例10本だけを均質な文脈で判定しているので、重複する曲はそちらを使う。
    """
    data = json.loads(ANSWERS.read_text())
    out: dict[str, dict] = {}
    for a in data["round2"]["answers"]:
        out[a["video_id"]] = a
    for a in data["round1"]["answers"]:
        out[a["video_id"]] = a
    return out


def hat_counts(path: Path, bpm: float, delta: float) -> tuple[np.ndarray, float]:
    y, sr = sf.read(str(path), dtype="float32")
    if y.ndim > 1:
        y = y.mean(axis=1)
    sos = signal.butter(4, [HAT_BAND[0], min(HAT_BAND[1], sr // 2 - 1)],
                        btype="band", fs=sr, output="sos")
    env = librosa.onset.onset_strength(y=signal.sosfilt(sos, y), sr=sr, hop_length=256)
    frames = librosa.onset.onset_detect(onset_envelope=env, sr=sr, hop_length=256,
                                        backtrack=False, delta=delta, wait=2)
    times = librosa.frames_to_time(frames, sr=sr, hop_length=256)
    bar_s = 4 * 60.0 / bpm
    n_bars = int(round(len(y) / sr / bar_s))
    counts = np.array([int(((times >= b * bar_s) & (times < (b + 1) * bar_s)).sum())
                       for b in range(n_bars)], dtype=float)
    return counts, bar_s


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--delta", type=float, default=LOUD_DELTA)
    args = ap.parse_args()

    answers = load_answers()
    rows, seen = [], set()
    for name in ("drum-listen", "drum-listen-2"):
        key_path = REVIEW / f"{name}-key.json"
        if not key_path.exists():
            continue
        for item in json.loads(key_path.read_text())["items"]:
            vid = item["video_id"]
            ans = answers.get(vid)
            if vid in seen or ans is None or ans.get("score") is None:
                continue
            seen.add(vid)
            counts, bar_s = hat_counts(REVIEW / name / f'{item["label"]}.wav',
                                       float(item["bpm"]), args.delta)
            if len(counts) < WINDOW_BARS:
                continue
            win = np.convolve(counts, np.ones(WINDOW_BARS) / WINDOW_BARS, mode="valid")
            rows.append({"score": ans["score"], "name": ans["name"], "bpm": item["bpm"],
                         "median": float(np.median(counts)) / bar_s,
                         "peak": float(win.max()) / bar_s,
                         "floor": float(win.min()) / bar_s})

    print(f"検出感度 delta={args.delta}\n")
    print(f"{'点':>3s} {'曲':24s} {'BPM':>5s} {'中央値':>7s} {'最密4小節':>9s} {'最疎4小節':>9s}")
    for r in sorted(rows, key=lambda r: -r["peak"]):
        print(f'{r["score"]:+3d} {r["name"][:24]:24s} {r["bpm"]:5.0f} '
              f'{r["median"]:7.2f} {r["peak"]:9.2f} {r["floor"]:9.2f}')

    neg = [r["peak"] for r in rows if r["score"] < 0]
    pos = [r["peak"] for r in rows if r["score"] > 0]
    if neg and pos:
        sep = "完全分離" if min(neg) > max(pos) else "重なる"
        print(f'\n最密4小節  否定{len(neg)}本 {min(neg):.2f}–{max(neg):.2f} / '
              f'肯定{len(pos)}本 {min(pos):.2f}–{max(pos):.2f}  → {sep}')


if __name__ == "__main__":
    main()
