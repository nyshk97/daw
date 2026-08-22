#!/usr/bin/env python3
"""ドラムの1性質だけを動かした検証素材を作る。

ドラムは生成できるので、上モノやベースで作れなかった「同一素材で1性質だけ動かす」
比較が直接作れる。曲全体の印象が軸へ投影される halo が原理的に入らない。

  --set hat   ハットの密度と音量（2026-08-22 実施。結論は docs/labs/drum-pattern.md）
  --set kick  キックの密度×等間隔性の2×2。ハット・スネアは固定

音量は正規化しない。打点を足せば実際に全体は大きくなるし、個別に正規化すると
そのぶん他のレーンが下がって「足したレーンが前に出る」という別の効果が混ざる。
4本へ同じ係数だけをかけて、実曲の抜粋(-20dBFS)と音量感を揃える。
"""
from __future__ import annotations

import argparse
import json
import string
from pathlib import Path

import numpy as np
import soundfile as sf
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "gacha"))
import drums as D  # noqa: E402

BPM = 92.0
BARS = 8
SIXTEENTH = D.TICKS_BAR // 16
VEL_KICK, VEL_SNARE, VEL_HAT, VEL_GHOST = 105, 100, 90, 25   # ghost は約 -12dB

EIGHTHS = [0, 2, 4, 6, 8, 10, 12, 14]
LAST_16THS = [3, 7, 11, 15]           # 各拍の "a"
SNARE_BACKBEAT = [4, 12]              # 2拍4拍。全ケースで固定

# hat セットのキックは「淡々」寄りの3打点で固定
KICK_BASE = [0, 8, 14]
# kick セットのハットは 8分・均一（毎秒3.07打点＝許容範囲と確定済み）で固定
HAT_BASE = [(p, VEL_HAT) for p in EIGHTHS]

CASE_SETS = {
    "hat": {
        "sparse_even": {"hat": [(p, VEL_HAT) for p in EIGHTHS],
                        "desc": "8分・全部同じ音量"},
        "dense_even": {"hat": [(p, VEL_HAT) for p in sorted(EIGHTHS + LAST_16THS)],
                       "desc": "8分＋各拍のa・全部同じ音量"},
        "dense_ghost": {"hat": [(p, VEL_HAT if p in EIGHTHS else VEL_GHOST)
                                for p in sorted(EIGHTHS + LAST_16THS)],
                        "desc": "打点数は dense_even と同じ。追加ぶんだけ小音(-12dB)"},
        "all_16th": {"hat": [(p, VEL_HAT) for p in range(16)],
                     "desc": "16分全部・全部同じ音量"},
    },
    # 密度（疎2打点 / 密6打点）× 等間隔性（キック+スネアの合計列が等間隔か）の2×2。
    # スネアは全ケースで2拍4拍に固定し、キックだけを動かす。
    "kick": {
        "sparse_even": {"kick": [0, 8],
                        "desc": "キック2打点。K+Sが完全に等間隔（0,4,8,12）"},
        "sparse_uneven": {"kick": [0, 5],
                          "desc": "キック2打点。K+Sの間隔がばらつく（0,4,5,12）"},
        "dense_even": {"kick": [0, 2, 6, 8, 10, 14],
                       "desc": "キック6打点。K+Sが8分で等間隔"},
        "dense_uneven": {"kick": [0, 3, 6, 10, 13, 15],
                         "desc": "キック6打点。K+Sの間隔がばらつく"},
    },
}
LABEL_START = {"hat": 15, "kick": 19}     # hat=P.., kick=T.. （前のセットと混ざらない）


def build(case: dict) -> dict[str, list[tuple[int, int]]]:
    kick = case.get("kick", KICK_BASE)
    hat = case.get("hat", HAT_BASE)
    notes = {"kick": [], "snare": [], "hat": []}
    for bar in range(BARS):
        base = bar * D.TICKS_BAR
        notes["kick"] += [(base + p * SIXTEENTH, VEL_KICK) for p in kick]
        notes["snare"] += [(base + p * SIXTEENTH, VEL_SNARE) for p in SNARE_BACKBEAT]
        notes["hat"] += [(base + p * SIXTEENTH, v) for p, v in hat]
    return notes


def describe(name: str, case: dict, bar_s: float) -> dict:
    """聴かせる前に「素材が意図どおりか」を確かめるための設計値。"""
    kick = case.get("kick", KICK_BASE)
    hat = case.get("hat", HAT_BASE)
    merged = sorted(set(kick) | set(SNARE_BACKBEAT))
    gaps = np.diff(merged + [merged[0] + 16])
    loud = sum(1 for _, v in hat if v >= VEL_HAT)
    return {
        "case": name, "desc": case["desc"],
        "kick_per_sec": round(len(kick) / bar_s, 2),
        "ks_per_sec": round(len(merged) / bar_s, 2),
        "ks_gap_cv": round(float(np.std(gaps) / np.mean(gaps)), 2),
        "hat_per_sec": round(len(hat) / bar_s, 2),
        "hat_loud_per_sec": round(loud / bar_s, 2),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", dest="case_set", choices=sorted(CASE_SETS), default="hat")
    ap.add_argument("--seed", type=int, default=20260822222)
    args = ap.parse_args()

    cases = CASE_SETS[args.case_set]
    out = Path.home() / f"Music/daw/reference-beat-corpus/review/drum-ab-{args.case_set}"
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.wav"):
        old.unlink()

    bar_s = 4 * 60.0 / BPM
    names = list(cases)
    order = np.random.default_rng(args.seed).permutation(len(names))
    labels = list(string.ascii_uppercase[LABEL_START[args.case_set]:])

    rendered = {n: D.synth_wav(build(cases[n]), BPM, BARS, {"kick": 1, "snare": 2, "hat": 3})
                for n in names}
    # 打点の多いケースほどピークが上がる。歪みの余地を残すため、
    # 全ケース中の最大ピークが -3dBFS になる係数を「全本へ共通で」かける。
    loudest = max(float(np.max(np.abs(y))) for y in rendered.values())
    gain = 10 ** (-3.0 / 20.0) / loudest

    key = []
    for label_idx, case_idx in enumerate(order):
        name = names[case_idx]
        y = rendered[name] * gain
        label = labels[label_idx]
        sf.write(str(out / f"{label}.wav"), y.astype(np.float32), D.SAMPLE_RATE)
        row = describe(name, cases[name], bar_s)
        row["label"] = label
        row["peak_dbfs"] = round(float(20 * np.log10(np.max(np.abs(y)) + 1e-12)), 2)
        key.append(row)
        print(f'{label}  {name:14s} キック{row["kick_per_sec"]:5.2f}/秒  '
              f'K+S{row["ks_per_sec"]:5.2f}/秒  間隔CV{row["ks_gap_cv"]:5.2f}  '
              f'ハット{row["hat_per_sec"]:5.2f}(大{row["hat_loud_per_sec"]:5.2f})/秒  '
              f'peak{row["peak_dbfs"]:6.2f}  {row["desc"]}')

    key_path = out.parent / f"drum-ab-{args.case_set}-key.json"
    key_path.write_text(json.dumps({"bpm": BPM, "bars": BARS, "set": args.case_set,
                                    "items": key}, ensure_ascii=False, indent=2))
    print(f"\n出力: {out}\n対応表: {key_path}")


if __name__ == "__main__":
    main()
