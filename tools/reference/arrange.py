#!/usr/bin/env python
"""構成（アレンジ）のマップを作る。

「どこで何を抜き差ししているか」＝エネルギー曲線を、小節×ステムの表で出す。
ミックス全体の音量だけ見ても「何を抜いたか」は分からないので、ステム単位で見る。

出力:
  analysis/arrangement.json  小節ごとの各ステムの在/不在とセクション境界
  analysis/arrangement.png   小節×ステムのヒートマップ
  標準出力                    そのまま report に貼れる文字表

使い方: arrange.py <stems.json> <outdir> [--on-db -34]
"""
import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ORDER = ["drums", "bass", "piano", "guitar", "other", "vocals"]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("stems")
    ap.add_argument("outdir")
    ap.add_argument("--on-db", type=float, default=-34.0, help="このdB以上で「鳴っている」とみなす")
    args = ap.parse_args()

    data = json.loads(Path(args.stems).read_text())
    # セクションの音量はミックスの実測を使う。
    # ステムのdBを平均すると、鳴っていないステムの底に引っ張られる
    # （アカペラ区間が -76dB と出て「一番静か」に見えた。実際は声だけで普通の音量）。
    # dB は対数なので、そもそも複数ステムを算術平均しても合成音の大きさにならない。
    bars_json = Path(args.outdir) / "basics.json"
    mix_by_bar = [b["rms_db"] for b in json.loads(bars_json.read_text())["bars"]] if bars_json.exists() else None
    outdir = Path(args.outdir)
    names = [n for n in ORDER if n in data["stems"]] + [n for n in data["stems"] if n not in ORDER]
    mat = np.array([data["stems"][n]["rms_db_by_bar"] for n in names])
    n_bars = mat.shape[1]

    # 各ステムの「鳴っている」判定は、そのステム自身のピークからの相対で決める。
    # 絶対dBだと、元から小さいステム（ギター等）が全区間で不在になってしまう。
    on = np.zeros_like(mat, dtype=bool)
    for i in range(len(names)):
        ref = np.percentile(mat[i], 90)
        on[i] = mat[i] > max(ref - 12, args.on_db)

    # セクションは 4小節ブロックの多数決で「編成の状態」を作り、同じ状態が続く限り繋ぐ。
    # 1小節ごとの在/不在をそのまま境界にすると、フィルや一瞬の抜きで刻まれてしまう。
    block = 4
    n_block = n_bars // block
    state = [tuple(on[:, k * block : (k + 1) * block].mean(axis=1) > 0.5) for k in range(n_block)]
    sections, start = [], 0
    for k in range(1, n_block + 1):
        if k == n_block or state[k] != state[start]:
            a, b = start * block, min(k * block, n_bars)
            sections.append(
                {
                    "bar_start": a + 1,
                    "bar_end": b,
                    "length_bars": b - a,
                    "active": [names[i] for i in range(len(names)) if state[start][i]],
                    "rms_db_mean": (
                        round(float(np.mean(mix_by_bar[a:b])), 2)
                        if mix_by_bar and len(mix_by_bar) > a
                        else None
                    ),
                }
            )
            start = k

    result = {"model": data["model"], "n_bars": n_bars, "sections": sections, "stems": names}
    (outdir / "arrangement.json").write_text(json.dumps(result, indent=2, ensure_ascii=False))

    fig, ax = plt.subplots(figsize=(18, 3 + 0.4 * len(names)))
    ax.imshow(mat, aspect="auto", cmap="magma", vmin=-55, vmax=-8, interpolation="nearest")
    ax.set_yticks(range(len(names)))
    ax.set_yticklabels(names)
    ax.set_xlabel("bar")
    for s in sections[1:]:
        ax.axvline(s["bar_start"] - 1.5, color="cyan", lw=1.2)
    ax.set_title(f"arrangement — {data['model']} (bar RMS)")
    fig.tight_layout()
    fig.savefig(outdir / "arrangement.png", dpi=110)

    width = 100
    scale = max(1, int(np.ceil(n_bars / width)))
    print(f"1文字 = {scale}小節 / ● = 鳴っている")
    print("        " + "".join(str((b * scale // 4) % 10) if (b * scale) % 16 == 0 else " " for b in range(n_bars // scale)))
    for i, n in enumerate(names):
        row = "".join("●" if on[i, b * scale : (b + 1) * scale].mean() > 0.5 else "·" for b in range(n_bars // scale))
        print(f"{n:>7s} {row}")
    print()
    for s in sections:
        print(f"  bar {s['bar_start']:>3d}-{s['bar_end']:<3d} ({s['length_bars']:2d}小節) {s['rms_db_mean']:>6.2f}dB  {' + '.join(s['active'])}")


if __name__ == "__main__":
    main()
