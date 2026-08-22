#!/usr/bin/env python3
"""仮説「はっきり鳴るハットが毎秒4打点を超えるとうるさい」の検証素材を作る。

キック/スネアは4本とも完全に同一で、ハットだけを変える。
同一素材で1性質だけ動かす比較なので、曲全体の印象が軸へ投影される halo が入らない。

音量は正規化しない。ハットを足せば実際に全体は大きくなるし、正規化すると
そのぶんキック/スネアが下がって「ハットが前に出る」という別の効果が混ざる。
制作の実際（ハットを足してもキック/スネアの音量は変えない）に合わせる。
"""
from __future__ import annotations

import json
import string
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "gacha"))
import drums as D  # noqa: E402

BPM = 92.0
BARS = 8
OUT = Path.home() / "Music/daw/reference-beat-corpus/review/drum-ab"
SIXTEENTH = D.TICKS_BAR // 16

KICK_16TH = [0, 8, 14]
SNARE_16TH = [4, 12]
EIGHTHS = [0, 2, 4, 6, 8, 10, 12, 14]
LAST_16THS = [3, 7, 11, 15]          # 各拍の "a"
ALL_16THS = list(range(16))

VEL_KICK, VEL_SNARE, VEL_HAT, VEL_GHOST = 105, 100, 90, 25   # ghost は約 -12dB

CASES = {
    "sparse_even":  {"hat": [(p, VEL_HAT) for p in EIGHTHS],
                     "desc": "8分・全部同じ音量"},
    "dense_even":   {"hat": [(p, VEL_HAT) for p in sorted(EIGHTHS + LAST_16THS)],
                     "desc": "8分＋各拍のa・全部同じ音量"},
    "dense_ghost":  {"hat": [(p, VEL_HAT if p in EIGHTHS else VEL_GHOST)
                             for p in sorted(EIGHTHS + LAST_16THS)],
                     "desc": "打点数は dense_even と同じ。追加ぶんだけ小音(-12dB)"},
    "all_16th":     {"hat": [(p, VEL_HAT) for p in ALL_16THS],
                     "desc": "16分全部・全部同じ音量"},
}


def build(case: dict) -> dict[str, list[tuple[int, int]]]:
    notes = {"kick": [], "snare": [], "hat": []}
    for bar in range(BARS):
        base = bar * D.TICKS_BAR
        notes["kick"] += [(base + p * SIXTEENTH, VEL_KICK) for p in KICK_16TH]
        notes["snare"] += [(base + p * SIXTEENTH, VEL_SNARE) for p in SNARE_16TH]
        notes["hat"] += [(base + p * SIXTEENTH, v) for p, v in case["hat"]]
    return notes


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.wav"):
        old.unlink()

    bar_s = 4 * 60.0 / BPM
    rng = np.random.default_rng(20260822222)
    names = list(CASES)
    order = rng.permutation(len(names))
    labels = list(string.ascii_uppercase[15:])   # P, Q, R, S

    # 実曲の抜粋(-20dBFS)と音量感を揃える。ただし4本に「同じ」係数をかける:
    # 個別に正規化するとハットを足したぶんキック/スネアが下がり、
    # 「ハットが前に出る」という別の効果が混ざる。
    rendered = {names[i]: D.synth_wav(build(CASES[names[i]]), BPM, BARS,
                                      {"kick": 1, "snare": 2, "hat": 3}) for i in order}
    base = rendered["sparse_even"]
    gain = 10 ** (-20.0 / 20.0) / float(np.sqrt(np.mean(base ** 2)))

    key = []
    for label_idx, case_idx in enumerate(order):
        name = names[case_idx]
        case = CASES[name]
        y = rendered[name] * gain
        label = labels[label_idx]
        sf.write(str(OUT / f"{label}.wav"), y.astype(np.float32), D.SAMPLE_RATE)

        loud = sum(1 for p, v in case["hat"] if v >= VEL_HAT)
        key.append({
            "label": label, "case": name, "desc": case["desc"],
            "hat_hits_per_bar": len(case["hat"]),
            "loud_hits_per_bar": loud,
            "hits_per_sec": round(len(case["hat"]) / bar_s, 2),
            "loud_hits_per_sec": round(loud / bar_s, 2),
            "peak_dbfs": round(20 * np.log10(np.max(np.abs(y)) + 1e-12), 2),
        })
        print(f'{label}  {name:12s} 打点{len(case["hat"]):3d}/小節  うち大{loud:3d}  '
              f'全体{len(case["hat"]) / bar_s:5.2f}/秒  はっきり{loud / bar_s:5.2f}/秒  '
              f'peak {key[-1]["peak_dbfs"]:6.2f}dBFS  {case["desc"]}')

    key_path = OUT.parent / "drum-ab-key.json"
    key_path.write_text(json.dumps({"bpm": BPM, "bars": BARS, "items": key},
                                   ensure_ascii=False, indent=2))
    print(f"\n出力: {OUT}\n対応表: {key_path}")


if __name__ == "__main__":
    main()
