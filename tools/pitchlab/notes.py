#!/usr/bin/env python3
"""f0 カーブ → ノート列（C++ PitchNotes へ移植する仕様の原型）＋ブロブ PNG。

規則（plan「私の側で決めた細部」）:
- 無音・無声フレームはノートにしない（voiced==0 で区切る）
- 有声区間が続き、かつ「半音の半分以上のジャンプ」が無い限り同じノート。
  ジャンプ判定: 直近 SMOOTH_MS の中央値（平滑化した現在地）が、いま開いているノートの中央値から
  0.5 半音以上離れた状態が HOLD_MS 以上続いたら、その離れ始めでノートを切る
  （しゃくり・ビブラートの一瞬の逸脱では切らず、音の移り変わりだけを切る）
- ノートの代表音程は区間の中央値（ビブラート・語尾に引きずられない）
- MIN_NOTE_MS 未満のノートは隣（同じ有声区間内の直前）へ吸収。吸収先が無ければ捨てる
出力: DetectedPitchNote { startFrame, endFrame(排他), medianMidi }
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from common import WORK, Curve, hz_to_midi

SMOOTH_MS = 40.0
HOLD_MS = 90.0   # ビブラート半周期（5〜6Hz → 80〜100ms）より長く。音の移り変わりは恒久的に離れるので遅延するだけ
JUMP_SEMI = 0.5
MIN_NOTE_MS = 60.0


def detect_notes(curve: Curve) -> list[dict]:
    midi = hz_to_midi(curve.f0)
    voiced = np.array(curve.voiced) > 0
    hop_ms = curve.hop / curve.sr * 1000.0
    smooth_n = max(1, int(round(SMOOTH_MS / hop_ms)))
    hold_n = max(1, int(round(HOLD_MS / hop_ms)))
    min_n = max(1, int(round(MIN_NOTE_MS / hop_ms)))
    notes: list[dict] = []

    def close(start: int, end: int):
        seg = midi[start:end]
        notes.append({"startFrame": int(start), "endFrame": int(end), "medianMidi": float(np.median(seg))})

    n = len(midi)
    k = 0
    while k < n:
        if not voiced[k]:
            k += 1; continue
        # 有声区間 [k, e)
        e = k
        while e < n and voiced[e]:
            e += 1
        start = k
        away_since = -1
        for j in range(k, e):
            cur = np.median(midi[max(start, j - smooth_n + 1): j + 1])
            ref = np.median(midi[start: j + 1]) if j - start >= smooth_n else cur
            if abs(cur - ref) >= JUMP_SEMI:
                if away_since < 0:
                    away_since = j
                if j - away_since + 1 >= hold_n:
                    close(start, away_since)
                    start = away_since
                    away_since = -1
            else:
                away_since = -1
        close(start, e)
        k = e

    # 短いノートの吸収（同じ有声区間内の直前へ。先頭なら直後へ）
    merged: list[dict] = []
    for nt in notes:
        if nt["endFrame"] - nt["startFrame"] < min_n:
            if merged and merged[-1]["endFrame"] == nt["startFrame"]:
                prev = merged[-1]
                prev["endFrame"] = nt["endFrame"]
                prev["medianMidi"] = float(np.median(midi[prev["startFrame"]:prev["endFrame"]]))
                continue
            # 直後へ吸収するため保留: 次のノートが隣接していれば次に結合
            nt["_short"] = True
        if merged and merged[-1].get("_short") and merged[-1]["endFrame"] == nt["startFrame"]:
            prev = merged.pop()
            nt = {"startFrame": prev["startFrame"], "endFrame": nt["endFrame"],
                  "medianMidi": float(np.median(midi[prev["startFrame"]:nt["endFrame"]]))}
        merged.append(nt)
    return [ {k: v for k, v in m.items() if not k.startswith("_")} for m in merged if not m.get("_short")]


def blob_png(curve: Curve, notes: list[dict], out: Path, title: str, targets: dict | None = None) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle
    t = curve.times(); m = curve.midi(); v = np.array(curve.voiced) > 0
    fig, ax = plt.subplots(figsize=(16, 6))
    for nt in notes:
        s, e = nt["startFrame"], nt["endFrame"]
        ax.add_patch(Rectangle((t[s], nt["medianMidi"] - 0.5), t[min(e, len(t) - 1)] - t[s], 1.0,
                               color="tab:blue", alpha=0.25, lw=0))
        if targets:
            tg = targets.get(str(s))
            if tg is not None:
                ax.add_patch(Rectangle((t[s], tg - 0.5), t[min(e, len(t) - 1)] - t[s], 1.0,
                                       fill=False, ec="tab:red", lw=1.5))
    ax.plot(t, np.where(v, m, np.nan), color="k", lw=1)
    lo = np.nanmin(np.where(v, m, np.nan)) - 2; hi = np.nanmax(np.where(v, m, np.nan)) + 2
    for y in range(int(np.floor(lo)), int(np.ceil(hi)) + 1):
        ax.axhline(y - 0.5, color="gray", lw=0.3, alpha=0.5)
    ax.set_ylim(lo, hi); ax.set_xlim(t[0], t[-1]); ax.set_xlabel("time [s]"); ax.set_ylabel("MIDI")
    ax.set_title(f"{title} — {len(notes)} notes"); fig.tight_layout(); fig.savefig(out, dpi=110); plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("name"); ap.add_argument("--algo", default="yin")
    a = ap.parse_args()
    d = WORK / a.name
    curve = Curve.load(d / f"curve_{a.algo}.json")
    notes = detect_notes(curve)
    (d / f"notes_{a.algo}.json").write_text(json.dumps(notes, indent=1))
    blob_png(curve, notes, d / f"notes_{a.algo}.png", f"{a.name}/{a.algo}")
    durs = [(n["endFrame"] - n["startFrame"]) * curve.hop / curve.sr for n in notes]
    print(f"{a.name}/{a.algo}: {len(notes)} notes, 長さ中央値 {np.median(durs)*1000:.0f}ms, "
          f"最短 {min(durs)*1000:.0f}ms  png: {d/f'notes_{a.algo}.png'}")
    for n in notes[:12]:
        print(f"  {n['startFrame']*curve.hop/curve.sr:6.2f}s  {(n['endFrame']-n['startFrame'])*curve.hop/curve.sr*1000:5.0f}ms  midi {n['medianMidi']:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
