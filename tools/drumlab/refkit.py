#!/usr/bin/env python3
"""リファレンスのドラムから単発を切り出し、自分のパターンで鳴らして3段で比べる。

「自分のドラムとリファレンスの差は、音色（サンプル）か処理（ミックス）か」を、
**分類させずに決める**ための実験。1回聴いて分類するのは聞き分けの訓練が要る作業なので、
変数を1つに絞って「似ているほう」を選ぶだけにする。

  段1: 自分のキットで基準パターン
  段2: リファレンス由来の単発で、まったく同じパターン
  段3: リファレンス本体

段2が段3に近ければ差は**サンプルが持っているもの**（＝調達で埋まる）。
段2が段1に近いままなら差は**ミックスの処理**（＝EQ・サチュ・リバーブの仕事で、買っても埋まらない）。

**切り出しにブリードは残る。** 実曲のドラムはキック・スネア・ハットが同時に鳴っていて、
1発だけを取り出すことはできない。ただし①Demucsのドラムステムを使うので音程のある成分は
既にほぼ無い ②残るブリードは他のドラムで、自分のパターンでも同じ組み合わせで鳴るので実害が薄い。
低域だけはハイパスで落とす（スネア・ハットにキックが乗ると二重に鳴る）。

**切り出した音は実際の曲に使わない**（正例は国内曲＝sampling-rights.md の全面回避帯）。実験専用。

  python3 tools/drumlab/refkit.py --kit "<キットのディレクトリ>" [--songs 3]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf
from scipy import signal

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kittarget import DELTA, LANES, _bandpass, top_songs  # noqa: E402
from kitcompare import (BARS, BASE, INVENTORY, SR, TEMPO_RANGE,  # noqa: E402
                        normalize, pick_kit, render, solve_gains)

# レーンごとの切り出し設定: (長さ秒, ハイパスHz)
CUT = {"kick": (0.35, 25.0), "snare": (0.28, 150.0), "hat": (0.16, 500.0)}
QUIET_WIN = 0.060   # 打点の直前どれだけを「静かさ」の評価に使うか
AVOID_TOL = 0.040   # 他レーンの打点がこの範囲にあったら選ばない
# 「その打点で避けるべき他レーン」。**自分より低い＝エネルギーが大きい**レーンだけを避ける。
# ハットの打点がキックと同時なら、切り出した「ハット」はほぼキックになる（実際に踏んだ）。
# キックは何も避けない — キック自身が一番大きいうえ、スネア帯(200-2000Hz)の検出器は
# キックの倍音でも発火するので、避けると「全打点がスネアと同時」になって1つも選べなくなる
AVOID = {"kick": [], "snare": ["kick"], "hat": ["kick", "snare"]}


def _onsets(y: np.ndarray, sr: int, lane: str) -> np.ndarray:
    det = _bandpass(y, sr, *LANES[lane]["detect"])
    env = librosa.onset.onset_strength(y=det, sr=sr, hop_length=256)
    fr = librosa.onset.onset_detect(onset_envelope=env, sr=sr, hop_length=256,
                                    backtrack=False, delta=DELTA, wait=2)
    return librosa.frames_to_time(fr, sr=sr, hop_length=256)


def _cut(y: np.ndarray, sr: int, a: int, lane: str) -> np.ndarray:
    dur, hp = CUT[lane]
    seg = y[a:a + int(dur * sr)].copy()
    sos = signal.butter(4, hp, btype="high", fs=sr, output="sos")
    seg = signal.sosfilt(sos, seg)
    n = int(0.004 * sr)
    seg[:n] *= np.linspace(0, 1, n)
    tail = int(0.02 * sr)
    seg[-tail:] *= np.linspace(1, 0, tail)
    return seg / (np.max(np.abs(seg)) + 1e-12)


def cleanest_hit(y: np.ndarray, sr: int, lane: str) -> tuple[np.ndarray, float]:
    """そのレーンらしい単発を1つ切り出す。

    **「直前が静か」だけで選ばない。** 検出は帯域を切ってやるが切り出しは全帯域なので、
    同時に鳴っている低い音がそのまま入る。しかも「直前が静か」は小節頭を選びやすく、
    小節頭にはキックが居るので、素朴にやるとスネアもハットもキックになる（実測で判明）。

    候補ごとに実際に切り出して**自分の帯域が最大かを検査**し、通ったものの中から
    直前が最も静かなものを採る。返り値は (波形, 直前の静かさdB)。
    """
    times = _onsets(y, sr, lane)
    others = np.concatenate([_onsets(y, sr, o) for o in AVOID[lane]]) if AVOID[lane] else np.array([])
    dur, _ = CUT[lane]
    best, checked = None, 0
    for t in times:
        a = int(t * sr)
        q0 = a - int(QUIET_WIN * sr)
        if q0 < 0 or a + int(dur * sr) > len(y):
            continue
        if len(others) and np.min(np.abs(others - t)) < AVOID_TOL:
            continue
        seg = _cut(y, sr, a, lane)
        e = {ln: float(np.sqrt(np.mean(_bandpass(seg, sr, *v["detect"]) ** 2)))
             for ln, v in LANES.items()}
        checked += 1
        if max(e, key=e.get) != lane:      # 他レーンを掴んでいる候補は捨てる
            continue
        pre = float(np.sqrt(np.mean(y[q0:a] ** 2))) + 1e-12
        pk = float(np.max(np.abs(y[a:a + int(0.03 * sr)]))) + 1e-12
        score = 20 * np.log10(pre / pk)    # 低いほど「直前が静か」
        if best is None or score < best[0]:
            best = (score, seg)
    if best is None:
        raise SystemExit(f"{lane}: 条件を満たす打点が無い"
                         f"（候補 {checked} 件を検査したが、どれも自分の帯域が最大にならない）")
    return best[1], best[0]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--kit", type=str, required=True)
    ap.add_argument("--songs", type=int, default=3)
    ap.add_argument("--out", type=str,
                    default=str(Path.home() / "Music/daw/reference-beat-corpus/review/kit-三段"))
    args = ap.parse_args()

    mine = pick_kit(Path(args.kit).expanduser())
    picked = []
    for n, s in enumerate(top_songs(), 1):
        if not (TEMPO_RANGE[0] <= s["bpm"] <= TEMPO_RANGE[1]):
            continue
        m = sorted(INVENTORY.glob(f"{n:02d}-*-A.wav"))
        if len(m) != 1:
            raise SystemExit(f"{INVENTORY}: {n:02d}-*-A.wav が1件でない。drum:inventory を先に走らせる")
        picked.append((s, m[0]))
    picked = picked[: args.songs]

    out = Path(args.out).expanduser()
    (out / "extracted").mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.wav"):
        old.unlink()

    rows = []
    print(f"{'曲':16s}{'BPM':>5}  切り出した打点の直前の静かさ（低いほどきれい）")
    for i, (s, f) in enumerate(picked, 1):
        ref, sr = sf.read(str(f), dtype="float32")
        if ref.ndim > 1:
            ref = ref.mean(axis=1)
        refkit, scores = {}, {}
        for lane in BASE:
            seg, sc = cleanest_hit(ref, sr, lane)
            refkit[lane] = (Path(f"{lane}.wav"), 0.0, seg)
            scores[lane] = sc
            sf.write(str(out / "extracted" / f"{i:02d}-{lane}.wav"), seg.astype(np.float32), sr)
        print(f'{s["name"][:16]:16s}{s["bpm"]:5.0f}  '
              + "  ".join(f"{ln} {scores[ln]:6.1f}dB" for ln in BASE))

        bar_s = 4 * 60.0 / s["bpm"]
        gap = np.zeros(int(0.25 * SR))
        # ゲインはキットごとに解き直す（切り出したキットは実効音量が全く違う）
        seg1 = normalize(render(mine, s["bpm"], solve_gains(mine, s["bpm"])))
        seg2 = normalize(render(refkit, s["bpm"], solve_gains(refkit, s["bpm"])))
        seg3 = normalize(ref[: int(BARS * bar_s * sr)].copy())
        name = f"{i:02d}-{f.stem[3:-2]}-三段.wav"
        sf.write(str(out / name), np.concatenate([seg1, gap, seg2, gap, seg3]).astype(np.float32), SR)
        rows.append({"file": name, "song": s["name"], "bpm": s["bpm"],
                     "switch_sec": [round(len(seg1) / SR, 1),
                                    round((len(seg1) + len(gap) + len(seg2)) / SR, 1)],
                     "quiet_db": {k: round(v, 1) for k, v in scores.items()}})

    md = ["# 音色か処理か — 3段の比較", "", f"素材: `{out}`", "", "## 聴き方", "",
          "各ファイルは3段。0.25秒の無音で区切ってある。", "",
          "1. **自分のキット**（Crate Digger）で基準パターン",
          "2. **リファレンスから切り出した単発**で、まったく同じパターン",
          "3. **リファレンス本体**", "",
          "答えるのは1つだけ: **2段目は、1段目と3段目のどちらに近いか。**", "",
          "- 2が3に近い → 差は**サンプルが持っているもの**。良い素材を調達すれば埋まる",
          "- 2が1に近いまま → 差は**ミックスの処理**。パックを買っても埋まらない"
          "（EQ・サチュ・リバーブの仕事）",
          "- どちらとも言えない → 両方が効いている", "",
          "**分類しなくていい。似ているほうを選ぶだけです。**", "",
          "## 回答", "", "| ファイル | 切替(秒) | 2段目は1と3のどちらに近いか |", "|---|---|---|"]
    for r in rows:
        md.append(f'| {r["file"]} | {r["switch_sec"][0]} / {r["switch_sec"][1]} |  |')
    md += ["", "## 切り出しの品質", "",
           "打点の直前60msの音量（打点のピーク比dB）。低いほど前の音の尾が乗っていない。",
           "実曲のドラムは同時に鳴っているので**完全な単独は取れない**。ブリードは残る。", "",
           "| 曲 | kick | snare | hat |", "|---|---|---|---|"]
    for r in rows:
        q = r["quiet_db"]
        md.append(f'| {r["song"]} | {q["kick"]} | {q["snare"]} | {q["hat"]} |')
    md += ["", "切り出した単発は `extracted/` にある。**実際の曲には使わない**"
           "（正例は国内曲＝ sampling-rights.md の全面回避帯）。実験専用。", "",
           "回答後、このファイルを `docs/labs/reference-beat-human-answers/` へコピーする。"]
    (out / "answers.md").write_text("\n".join(md) + "\n")
    (out / "meta.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2))
    print(f"\n完了: {len(rows)}本 → {out}\n  回答シート: {out / 'answers.md'}")


if __name__ == "__main__":
    main()
