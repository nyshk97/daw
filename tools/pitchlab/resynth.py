#!/usr/bin/env python3
"""ケース A〜D＋Z を4エンジンで合成し、数値検証と波形 PNG を出す。

使い方: resynth.py <name> [--algo yin] [--scale chromatic|<root>:<major|minor>|auto]
                   [--engines psola,world,signalsmith,rubberband] [--move-note IDX --move-ms MS]
前提: analyze.py と notes.py が work/<name>/ に curve_<algo>.json / notes_<algo>.json を出している
出力: work/<name>/resynth/<case>_<engine>.wav, verify.json, <case>_verify.png

ケース: Z=無加工（強さ0・移調0。透明性） A=スナップ・強さ100%・速さ120ms
        B=A＋全体+3半音  C=ケロケロ（速さ0）  D=A＋1音を横移動（隣接吸収）
検証:
- 長さ一致: 出力サンプル数 == 期待（時間写像の末尾）
- 達成誤差: 出力を YIN で再検出し、有声フレームで |実測 - 目標| の中央値/95%点[cent]
  （目標 = 元 midi + shift を出力座標へ写したもの。20cent 以内が plan の合格線）
- Z の透明性: max|wet - dry|（0 ならビット一致）
- クリック: 出力の隣接サンプル差分の最大値を入力の最大値で割った比（1 前後なら入力並み、
  3 を超えると境界のクリック疑い）
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np

from common import WORK, Curve, hz_to_midi, load_mono, write_wav
from correction import (estimate_key, identity_map, inverse_map, move_note, scale_pcs,
                        target_shift)
from engines import ENGINES
from yin import yin


def parse_scale(s: str, notes):
    if s == "chromatic":
        return None, "chromatic"
    if s == "auto":
        root, minor, r = estimate_key(notes)
        return scale_pcs(root, minor), f"auto={root}:{'minor' if minor else 'major'} (r={r:.2f})"
    root, mode = s.split(":")
    return scale_pcs(int(root), mode == "minor"), s


def verify(x, y, sr, curve, shift, tmap, case, engine, out_png):
    n_out = int(tmap[-1][1])
    res = {"len_ok": len(y) == n_out, "len": len(y), "expected": n_out}
    dx = np.max(np.abs(np.diff(x))) if len(x) > 1 else 1.0
    res["click_ratio"] = float(np.max(np.abs(np.diff(y))) / max(dx, 1e-9))
    if case == "Z":
        res["max_abs_diff"] = float(np.max(np.abs(y[: len(x)] - x[: len(y)])))
    cy = yin(y.astype(np.float32), sr)
    # 目標カーブを出力座標へ
    in_midi = hz_to_midi(curve.f0)
    voiced_in = np.array(curve.voiced) > 0
    tgt_in = np.where(voiced_in, in_midi + shift, np.nan)
    out_t = np.arange(len(cy.f0)) * cy.hop
    in_pos = inverse_map(tmap, out_t)
    ks = np.clip((in_pos // curve.hop).astype(int), 0, len(tgt_in) - 1)
    tgt_out = tgt_in[ks]
    got = np.where(np.array(cy.voiced) > 0, cy.midi(), np.nan)
    ok = np.isfinite(tgt_out) & np.isfinite(got)
    # 境界 ±15ms は検出窓のにじみがあるので除外
    edge = np.zeros_like(ok)
    vo = voiced_in[ks]
    for k in range(1, len(vo)):
        if vo[k] != vo[k - 1]:
            edge[max(0, k - 3): k + 3] = True
    ok &= ~edge
    err = np.abs(got[ok] - tgt_out[ok]) * 100
    res["err_cents_median"] = float(np.median(err)) if len(err) else None
    res["err_cents_p95"] = float(np.percentile(err, 95)) if len(err) else None
    res["frames_compared"] = int(ok.sum())
    res["voiced_recall_out"] = float(np.sum(np.isfinite(got) & np.isfinite(tgt_out)) / max(1, np.sum(np.isfinite(tgt_out))))
    # PNG
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(16, 5))
    t = out_t / sr
    ax.plot(t, np.where(vo, in_midi[ks], np.nan), color="gray", lw=1, label="original (mapped)")
    ax.plot(t, tgt_out, color="tab:red", lw=1.5, alpha=0.7, label="target")
    ax.plot(t, got, ".", ms=2.5, color="tab:blue", label=f"{engine} re-detected")
    ax.legend(loc="upper right"); ax.grid(True, alpha=0.3); ax.set_xlabel("time [s]"); ax.set_ylabel("MIDI")
    ax.set_title(f"{case}/{engine}: median {res['err_cents_median']} c, p95 {res['err_cents_p95']} c, len_ok={res['len_ok']}")
    fig.tight_layout(); fig.savefig(out_png, dpi=100); plt.close(fig)
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("name"); ap.add_argument("--algo", default="yin"); ap.add_argument("--scale", default="auto")
    ap.add_argument("--engines", default="psola,world,signalsmith,rubberband")
    ap.add_argument("--wav", help="解析に使った WAV（省略時は work/<name>/source.wav）")
    ap.add_argument("--move-note", type=int, default=-1); ap.add_argument("--move-ms", type=float, default=40.0)
    ap.add_argument("--cases", default="Z,A,B,C,D")
    a = ap.parse_args()
    d = WORK / a.name; out = d / "resynth"; out.mkdir(parents=True, exist_ok=True)
    x, sr = load_mono(Path(a.wav) if a.wav else d / "source.wav")
    curve = Curve.load(d / f"curve_{a.algo}.json")
    notes = json.loads((d / f"notes_{a.algo}.json").read_text())
    pcs, scale_desc = parse_scale(a.scale, notes)
    n = len(x)
    # 横移動の対象: 指定が無ければ「両隣に隙間がある最初のノート（2番目以降）」
    mv = a.move_note
    if mv < 0:
        for i in range(1, len(notes) - 1):
            if notes[i]["startFrame"] > notes[i - 1]["endFrame"] and notes[i + 1]["startFrame"] > notes[i]["endFrame"]:
                mv = i; break
        if mv < 0:
            mv = min(1, len(notes) - 1)
    cases = {
        "Z": dict(strength=0.0, speed_ms=120.0, transpose=0.0, tmap=identity_map(n)),
        "A": dict(strength=1.0, speed_ms=120.0, transpose=0.0, tmap=identity_map(n)),
        "B": dict(strength=1.0, speed_ms=120.0, transpose=3.0, tmap=identity_map(n)),
        "C": dict(strength=1.0, speed_ms=0.0, transpose=0.0, tmap=identity_map(n)),
        "D": dict(strength=1.0, speed_ms=120.0, transpose=0.0,
                  tmap=move_note(curve, notes, n, mv, int(a.move_ms / 1000 * sr))),
    }
    report = {"scale": scale_desc, "move_note": mv, "move_ms": a.move_ms, "cases": {}}
    for case in a.cases.split(","):
        p = cases[case]
        shift, resolved = target_shift(curve, notes, strength=p["strength"], speed_ms=p["speed_ms"], pcs=pcs,
                                       transpose=p["transpose"])
        tmap = p["tmap"]
        (out / f"{case}.targets.json").write_text(json.dumps({"notes": resolved, "tmap": tmap, "params": {k: v for k, v in p.items() if k != 'tmap'}}))
        report["cases"][case] = {}
        for eng in a.engines.split(","):
            t0 = time.time()
            try:
                y = ENGINES[eng](x, sr, curve, shift, tmap, out, f"{case}_{eng}")
            except Exception as ex:  # noqa: BLE001 — lab では失敗を記録して続行
                report["cases"][case][eng] = {"error": str(ex)[:300]}
                print(f"{case}/{eng}: ERROR {ex}")
                continue
            write_wav(out / f"{case}_{eng}.wav", y, sr)
            r = verify(x, y, sr, curve, shift, tmap, case, eng, out / f"{case}_{eng}_verify.png")
            r["seconds"] = round(time.time() - t0, 1)
            report["cases"][case][eng] = r
            print(f"{case}/{eng}: len_ok={r['len_ok']} err med={r['err_cents_median']} p95={r['err_cents_p95']} "
                  f"click={r['click_ratio']:.2f} " + (f"maxdiff={r['max_abs_diff']:.2e} " if 'max_abs_diff' in r else "") + f"({r['seconds']}s)")
    (out / "verify.json").write_text(json.dumps(report, indent=1))
    print(f"scale: {scale_desc} / moved note {mv} by {a.move_ms}ms / report: {out/'verify.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
