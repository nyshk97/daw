#!/usr/bin/env python3
"""検出器の比較: 自作 YIN / pYIN（librosa）/ CREPE（torchcrepe）。

使い方: analyze.py <wav> <name> [--truth synth.truth.json] [--crop start end]
出力: work/<name>/curve_{yin,pyin,crepe}.json, overlay.png, metrics.json（stdout にも要約）

指標の読み方:
- 不一致率（検出器同士）: 両方が有声と判定したフレームのうち、音程が半音以上違う割合。
  大半は「オクターブ飛び」（12半音±1）。どちらが正しいかはこの数値では分からない
- GPE（正解あり）: 正解が有声で検出器も有声のフレームのうち、半音以上外した割合。小さいほど良い。
  5% を超えるとブロブがゴミだらけに見える目安（plan）
- voicing precision = 検出器が有声と言ったうち本当に有声だった割合（低い＝息がノートになる）
  voicing recall   = 本当に有声のうち拾えた割合（低い＝ノートが欠ける）
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import soundfile as sf

from common import WORK, Curve, FMIN, FMAX, hz_to_midi, load_mono, hop_samples, frame_rms
from yin import yin


def run_pyin(x: np.ndarray, sr: int) -> Curve:
    import librosa
    hop = hop_samples(sr)
    f0, vflag, vprob = librosa.pyin(x, fmin=FMIN, fmax=FMAX, sr=sr, frame_length=2048,
                                     hop_length=hop, center=True, fill_na=0.0)
    n = len(f0)
    f0 = np.where(vflag, f0, 0.0)
    return Curve(sr=sr, hop=hop, f0=f0.tolist(), voiced=vflag.astype(int).tolist(),
                 prob=np.nan_to_num(vprob).tolist(), rms=frame_rms(x, hop, n).tolist(), algo="pyin")


def run_crepe(x: np.ndarray, sr: int) -> Curve:
    import torch, torchcrepe
    hop = hop_samples(sr)
    audio = torch.from_numpy(x.astype(np.float32))[None]
    with torch.no_grad():
        f0, per = torchcrepe.predict(audio, sr, hop_length=hop, fmin=FMIN, fmax=FMAX, model="full",
                                     return_periodicity=True, batch_size=512, device="cpu")
    per = torchcrepe.filter.median(per, 3)
    f0 = torchcrepe.filter.mean(f0, 3)
    f0 = f0[0].numpy().astype(float); per = per[0].numpy().astype(float)
    voiced = per > 0.21   # torchcrepe README の推奨閾値
    n = int(np.ceil(len(x) / hop))
    f0 = np.resize(np.where(voiced, f0, 0.0), n); per = np.resize(per, n)
    return Curve(sr=sr, hop=hop, f0=f0.tolist(), voiced=(f0 > 0).astype(int).tolist(),
                 prob=per.tolist(), rms=frame_rms(x, hop, n).tolist(), algo="crepe")


def disagreement(a: Curve, b: Curve) -> dict:
    n = min(len(a.f0), len(b.f0))
    ma, mb = hz_to_midi(a.f0[:n]), hz_to_midi(b.f0[:n])
    va, vb = np.array(a.voiced[:n]) > 0, np.array(b.voiced[:n]) > 0
    both = va & vb
    diff = np.abs(ma[both] - mb[both])
    return {
        "frames_both_voiced": int(both.sum()),
        "pitch_mismatch_rate": float(np.mean(diff >= 1.0)) if both.any() else 0.0,
        "octave_jump_rate": float(np.mean(np.abs(diff - 12) <= 1.0)) if both.any() else 0.0,
        "voicing_mismatch_rate": float(np.mean(va != vb)),
    }


def score_vs_truth(c: Curve, truth: dict) -> dict:
    n = min(len(c.f0), len(truth["f0"]))
    tv = np.array(truth["voiced"][:n]) > 0
    dv = np.array(c.voiced[:n]) > 0
    tm, dm = hz_to_midi(truth["f0"][:n]), hz_to_midi(c.f0[:n])
    both = tv & dv
    gpe = float(np.mean(np.abs(tm[both] - dm[both]) >= 1.0)) if both.any() else 1.0
    fine = np.abs(tm[both] - dm[both])
    return {
        "gpe": gpe,
        "fine_err_cents_median": float(np.median(fine[fine < 1.0]) * 100) if (fine < 1.0).any() else None,
        "voicing_precision": float((tv & dv).sum() / max(1, dv.sum())),
        "voicing_recall": float((tv & dv).sum() / max(1, tv.sum())),
    }


def overlay_png(curves: dict[str, Curve], out: Path, truth: dict | None, title: str) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(2, 1, figsize=(16, 7), sharex=True, height_ratios=[3, 1])
    ax = axes[0]
    if truth:
        t = np.arange(len(truth["f0"])) * truth["hop"] / truth["sr"]
        m = hz_to_midi(truth["f0"]); ax.plot(t, m, color="k", lw=3, alpha=0.25, label="truth")
    colors = {"yin": "tab:blue", "pyin": "tab:orange", "crepe": "tab:green"}
    for name, c in curves.items():
        m = c.midi(); m = np.where(np.array(c.voiced) > 0, m, np.nan)
        ax.plot(c.times(), m, ".", ms=3, color=colors.get(name), label=name, alpha=0.8)
    ax.set_ylabel("MIDI note"); ax.grid(True, alpha=0.3); ax.legend(loc="upper right"); ax.set_title(title)
    ax2 = axes[1]
    for name, c in curves.items():
        ax2.plot(c.times(), c.prob, color=colors.get(name), lw=1, label=f"{name} prob")
    ax2.set_ylim(0, 1.05); ax2.set_ylabel("voicing prob"); ax2.set_xlabel("time [s]"); ax2.grid(True, alpha=0.3)
    fig.tight_layout(); fig.savefig(out, dpi=110); plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("wav"); ap.add_argument("name")
    ap.add_argument("--truth"); ap.add_argument("--crop", nargs=2, type=float)
    ap.add_argument("--skip-crepe", action="store_true")
    a = ap.parse_args()
    x, sr = load_mono(Path(a.wav))
    if a.crop:
        x = x[int(a.crop[0] * sr): int(a.crop[1] * sr)]
    out = WORK / a.name; out.mkdir(parents=True, exist_ok=True)
    sf.write(str(out / "source.wav"), x, sr, subtype="PCM_24")  # 以降の notes/resynth が読む解析対象
    curves = {"yin": yin(x, sr), "pyin": run_pyin(x, sr)}
    if not a.skip_crepe:
        curves["crepe"] = run_crepe(x, sr)
    for k, c in curves.items():
        c.save(out / f"curve_{k}.json")
    truth = json.loads(Path(a.truth).read_text()) if a.truth else None
    metrics = {"disagreement": {}, "vs_truth": {}}
    names = list(curves)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            metrics["disagreement"][f"{names[i]}~{names[j]}"] = disagreement(curves[names[i]], curves[names[j]])
    if truth:
        for k, c in curves.items():
            metrics["vs_truth"][k] = score_vs_truth(c, truth)
    (out / "metrics.json").write_text(json.dumps(metrics, indent=1))
    overlay_png(curves, out / "overlay.png", truth, f"{a.name} ({Path(a.wav).name})")
    print(json.dumps(metrics, indent=1))
    print(f"png: {out/'overlay.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
