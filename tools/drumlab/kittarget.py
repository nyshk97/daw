#!/usr/bin/env python3
"""好きな曲のドラムから、ワンショットを選ぶための「目標値」を測る。

対象は round2 で score=+2 だった正例（＝ドラム判定が最高の曲）のドラムステム抜粋。
レーンごとに帯域を切って打点を検出し、1打点あたりの重心と減衰時間を出す。
同じ計算を手持ちのワンショットにも掛けて並べるので、「落としたパックが近いか」を
聴く前に見られる。

**減衰時間は実曲のステムからは測れない**（2026-08-22 実測で確認）。1打点の尾が次の打点と
他楽器に埋もれるため、定義を変えると桁が変わる — 同じ素材で「最後に閾値を上回った位置」
0.360s /「最初に下回った位置」0.003s /「5ms平滑して-20dB」0.027s。しかも前者はレーンが
違うのに値が一致し（窓の長さがそのまま出ていた）、素朴な実装は**もっともらしい嘘をつく**。
目標値に載せるのは重心・帯域RMS・打点/秒の3つだけにする。減衰時間は孤立したワンショット
側でのみ意味を持つので、パック同士の比較にだけ使う。

検出帯域は hatrate.py の HAT_BAND と揃えてある。delta も同じ 0.25（「はっきり聞こえる」の代理）。

  python3 tools/drumlab/kittarget.py                    # 目標値のみ
  python3 tools/drumlab/kittarget.py --oneshots DIR     # ワンショットも測って並べる
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf
from scipy import signal

REVIEW = Path.home() / "Music/daw/reference-beat-corpus/review"
ANSWERS = Path(__file__).resolve().parents[2] / "docs/labs/reference-beat-human-answers/2026-08-22-drum-pattern-answers.json"

# detect = 打点を見つける帯域（他レーンと重ならない狭さ）
# measure = 重心を測る帯域（ワンショットに同じ計算を掛けたとき意味が揃う広さ）
# 減衰時間は載せない（上記）。重心・帯域RMS・打点/秒の3つが目標値
LANES = {
    "kick":  {"detect": (30, 150),     "measure": (20, 400)},
    "snare": {"detect": (200, 2000),   "measure": (100, 8000)},
    "hat":   {"detect": (6500, 10500), "measure": (2000, 16000)},
}
DELTA = 0.25          # hatrate.py と同じ（「はっきり聞こえる」の代理）
DECAY_DB = 30.0       # ピークから何dB落ちるまでを減衰時間とするか
MAX_WINDOW_S = 0.6    # 1打点を見る最大長（次の打点が来たらそこで切る）


def _bandpass(y: np.ndarray, sr: int, lo: float, hi: float) -> np.ndarray:
    hi = min(hi, sr / 2 - 1)
    sos = signal.butter(4, [lo, hi], btype="band", fs=sr, output="sos")
    return signal.sosfilt(sos, y)


def _centroid(x: np.ndarray, sr: int, lo: float, hi: float) -> float:
    """指定帯域の中でのスペクトル重心。帯域外は無視する（帯域内のどこに寄っているか）。"""
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    freq = np.fft.rfftfreq(len(x), 1 / sr)
    m = (freq >= lo) & (freq <= hi)
    if not m.any() or spec[m].sum() <= 0:
        return float("nan")
    return float((freq[m] * spec[m]).sum() / spec[m].sum())


def _decay(x: np.ndarray, sr: int) -> float:
    """ピークから -DECAY_DB まで落ちるのにかかった秒数。窓の中で落ちきらなければ nan。

    実曲のステムでは打点の尾が次の打点と他楽器に埋もれ、-30dB まで落ちない。素朴に
    「最後に閾値を上回った位置」を返すと**窓の長さがそのまま減衰時間として出る**
    （8分刻みのハットとバックビートのスネアが同じ 0.279s になって発覚）。
    落ちきらなかったものは打切り扱いで nan にし、集計から外す。
    """
    env = np.abs(signal.hilbert(x)) if len(x) > 32 else np.abs(x)
    pk = float(env.max())
    if pk <= 0:
        return float("nan")
    thr = pk * 10 ** (-DECAY_DB / 20)
    peak_i = int(np.argmax(env))
    below = np.where(env[peak_i:] <= thr)[0]
    if len(below) == 0:
        return float("nan")   # 窓の中で落ちきらなかった＝打切り
    return float(below[0] / sr)


def measure_stem(path: Path) -> dict[str, dict]:
    y, sr = sf.read(str(path), dtype="float32")
    if y.ndim > 1:
        y = y.mean(axis=1)
    total_rms = float(np.sqrt(np.mean(y ** 2))) + 1e-12
    out: dict[str, dict] = {}
    for lane, band in LANES.items():
        det = _bandpass(y, sr, *band["detect"])
        env = librosa.onset.onset_strength(y=det, sr=sr, hop_length=256)
        frames = librosa.onset.onset_detect(onset_envelope=env, sr=sr, hop_length=256,
                                            backtrack=False, delta=DELTA, wait=2)
        times = librosa.frames_to_time(frames, sr=sr, hop_length=256)
        cents, decs = [], []
        for i, t in enumerate(times):
            a = int(t * sr)
            nxt = int(times[i + 1] * sr) if i + 1 < len(times) else len(y)
            b = min(a + int(MAX_WINDOW_S * sr), nxt, len(y))
            if b - a < 256:
                continue
            cents.append(_centroid(y[a:b], sr, *band["measure"]))   # 重心は素の信号で
            decs.append(_decay(det[a:b], sr))                       # 減衰は帯域を切って
        band_rms = float(np.sqrt(np.mean(_bandpass(y, sr, *band["detect"]) ** 2)))
        valid = int(np.sum(~np.isnan(decs))) if decs else 0
        out[lane] = {
            "hits": len(cents),
            "decay_valid": valid,
            "per_sec": len(times) / (len(y) / sr),
            "centroid": float(np.nanmedian(cents)) if cents else float("nan"),
            "decay": float(np.nanmedian(decs)) if decs else float("nan"),
            "band_db": 20 * np.log10(max(band_rms, 1e-12) / total_rms),
        }
    return out


def _decay_isolated(x: np.ndarray, sr: int) -> float:
    """孤立したワンショット用の減衰時間。ピークから最後に -DECAY_DB を上回るまで。

    ステム用の _decay と定義が違うのは意図的。**次の打点も他楽器も無い**ので尾を最後まで
    追えるし、追わないと過小評価する（帯域通過したノイズの包絡は一瞬 -30dB を下回るため、
    「最初の交差」だとハットが 4ms になる。実際は 50ms 前後）。
    """
    env = np.abs(signal.hilbert(x)) if len(x) > 32 else np.abs(x)
    pk = float(env.max())
    if pk <= 0:
        return float("nan")
    idx = np.where(env > pk * 10 ** (-DECAY_DB / 20))[0]
    return float((idx[-1] - idx[0]) / sr) if len(idx) else 0.0


def measure_oneshot(path: Path, lane: str) -> dict:
    y, sr = sf.read(str(path), dtype="float32")
    if y.ndim > 1:
        y = y.mean(axis=1)
    band = LANES[lane]
    return {
        "centroid": _centroid(y, sr, *band["measure"]),
        "decay": _decay_isolated(_bandpass(y, sr, *band["detect"]), sr),
        "rms_db": 20 * np.log10(max(float(np.sqrt(np.mean(y ** 2))), 1e-12)),
    }


def top_songs() -> list[dict]:
    """round2 で score=+2 の正例（混入した否定例3本を除く）→ 抜粋のパス。"""
    ans = json.loads(ANSWERS.read_text())["round2"]
    mixed = set(ans.get("mixed_in", []))
    want = {a["video_id"]: a["name"] for a in ans["answers"]
            if a["score"] == 2 and a["video_id"] not in mixed}
    key = json.loads((REVIEW / "drum-listen-2-key.json").read_text())
    return [{"name": want[i["video_id"]], "bpm": i["bpm"],
             "path": REVIEW / "drum-listen-2" / f'{i["label"]}.wav'}
            for i in key["items"] if i["video_id"] in want]


def summarize(rows: list[dict], lane: str, field: str) -> tuple[float, float, float]:
    v = np.array([r[lane][field] for r in rows], dtype=float)
    v = v[~np.isnan(v)]
    return float(np.median(v)), float(v.min()), float(v.max())


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--oneshots", type=str, default=None,
                    help="ワンショットのディレクトリ（再帰。ファイル名でレーンを推定）")
    args = ap.parse_args()

    songs = top_songs()
    print(f"対象: ドラム判定 +2 の正例 {len(songs)}曲（16小節抜粋・全曲同一RMS）\n")
    rows = []
    for s in songs:
        m = measure_stem(s["path"])
        rows.append(m)
        print(f'  {s["name"][:18]:18s} BPM {s["bpm"]:5.0f}  ' + "  ".join(
            f'{lane}: 重心{m[lane]["centroid"]:6.0f}Hz {m[lane]["band_db"]:+6.1f}dB '
            f'{m[lane]["per_sec"]:.1f}打/s' for lane in LANES))

    print(f"\n{'='*78}\n目標値（{len(songs)}曲の中央値と範囲）\n")
    print(f"{'レーン':<8}{'重心(Hz)':>20}{'帯域RMS(相対dB)':>22}{'打点/秒':>14}")
    target = {}
    for lane in LANES:
        c = summarize(rows, lane, "centroid")
        b = summarize(rows, lane, "band_db")
        s = summarize(rows, lane, "per_sec")
        target[lane] = {"centroid": c, "band_db": b, "per_sec": s}
        print(f"{lane:<8}{c[0]:8.0f} ({c[1]:.0f}–{c[2]:.0f}){'':>4}"
              f"{b[0]:10.1f} ({b[1]:.1f}〜{b[2]:.1f}){'':>3}"
              f"{s[0]:6.1f} ({s[1]:.1f}–{s[2]:.1f})")
    kb = target["kick"]["band_db"][0]
    print(f"\nレーン間のバランス（キック基準）: "
          + " / ".join(f"{l} {target[l]['band_db'][0] - kb:+.1f}dB" for l in LANES))
    print("減衰時間は測っていない（ステムからは測れない。docstring 参照）")

    if args.oneshots:
        root = Path(args.oneshots).expanduser()
        # 目標値と比べるのは3レーンだけ。オープンハットは「クローズハット」に混ざると
        # 減衰の分布を壊す（実測で上限が 0.80s まで伸びた）ので分けて、参考として出す
        pat = [("kick", "kick", r"kick"),
               ("snare", "snare", r"snare|rimshot"),
               ("hat", "hat", r"closed[ _-]*hi[ _-]*hat|closed[ _-]*hat"),
               ("hat(open) 参考", "hat", r"open[ _-]*hi[ _-]*hat|open[ _-]*hat")]
        print(f"\n{'='*78}\n手持ちのワンショット: {root}\n")
        print(f"{'レーン':<15}{'本数':>4}{'重心(Hz)':>22}{'減衰(s)':>20}{'目標との差':>14}")
        for label, lane, rx in pat:
            files = [f for f in root.rglob("*.wav") if re.search(rx, f.name, re.I)]
            if not files:
                continue
            ms = [measure_oneshot(f, lane) for f in files]
            c = np.array([m["centroid"] for m in ms]); c = c[~np.isnan(c)]
            d = np.array([m["decay"] for m in ms]); d = d[~np.isnan(d)]
            ratio = np.median(c) / target[lane]["centroid"][0]
            print(f"{label:<15}{len(files):4d}{np.median(c):8.0f} ({c.min():.0f}–{c.max():.0f}){'':>3}"
                  f"{np.median(d):8.3f} ({d.min():.3f}–{d.max():.3f}){'':>1}"
                  f"  重心 ×{ratio:.2f}")
        print("\n※ 減衰時間に実曲側の目標値は無い（ステムからは測れない）。パック同士の"
              "\n   比較にだけ使う。重心は実曲の目標値と直接比べてよい")


if __name__ == "__main__":
    main()
