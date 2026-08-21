"""補正カーブと時間写像の生成（C++ PitchCorrection::targetCurve() へ移植する仕様の原型）。

入力: Curve（元 f0・有声度）・ノート列・つまみ（strength 0..1, speed_ms, scale, transpose）・
     ノート単位の上書き（targetMidi / bypass）・タイミング編集（ノート index → Δサンプル）
出力:
- shift[k] : フレーム k（入力座標）の移動量[半音] = 有声マスク × 補正 + transpose
- time map : (inSample, outSample) の単調ノード列。編集が無ければ [(0,0),(N,N)]

「強さ」= ノートの中心（中央値）を目標へ寄せる割合。
「速さ」= フレームごとの誤差 e_k = target - midi_k を一次ローパスで追いかける時定数。
  speed 0 → ローパス無し → 各フレームが完全に目標 = 音程が平ら（ケロケロ）
  speed 大 → ほぼ定数（初期値 = target - median）= 中心だけ寄せてビブラート・しゃくりは残る
  Auto-Tune の Retune Speed と同じ考え方（速いほど機械的）。
"""
from __future__ import annotations

import numpy as np

from common import Curve, hz_to_midi

MAJOR = [0, 2, 4, 5, 7, 9, 11]
MINOR = [0, 2, 3, 5, 7, 8, 10]


def scale_pcs(root: int, minor: bool) -> list[int]:
    return [(root + d) % 12 for d in (MINOR if minor else MAJOR)]


def snap(midi: float, pcs: list[int] | None) -> int:
    """最寄りのスケール音（chromatic=None なら四捨五入）。同距離なら上を取る"""
    if pcs is None:
        return int(np.floor(midi + 0.5))
    best, bestd = None, 1e9
    for cand in range(int(np.floor(midi)) - 6, int(np.ceil(midi)) + 7):
        if cand % 12 in pcs:
            d = abs(cand - midi)
            if d < bestd - 1e-9 or (abs(d - bestd) < 1e-9 and cand > best):
                best, bestd = cand, d
    return int(best)


def estimate_key(notes: list[dict]) -> tuple[int, bool, float]:
    """Krumhansl-Schmuckler: ノートの長さで重み付けした音高クラス分布と、調ごとのプロファイルの
    相関係数を取り、最大の調を返す。相関係数は -1..1 で、1 に近いほど「その調の音の使われ方に似ている」。
    0.8 超なら明確、0.5 前後は曖昧（短いフレーズ・ラップの話し声的な音高では低くなる）。"""
    major_p = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
    minor_p = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])
    hist = np.zeros(12)
    for n in notes:
        hist[int(np.floor(n["medianMidi"] + 0.5)) % 12] += n["endFrame"] - n["startFrame"]
    if hist.sum() == 0:
        return 0, False, 0.0
    best = (0, False, -2.0)
    for root in range(12):
        for minor, prof in ((False, major_p), (True, minor_p)):
            r = float(np.corrcoef(hist, np.roll(prof, root))[0, 1])
            if r > best[2]:
                best = (root, minor, r)
    return best


def target_shift(curve: Curve, notes: list[dict], *, strength: float = 1.0, speed_ms: float = 120.0,
                 pcs: list[int] | None = None, transpose: float = 0.0,
                 overrides: dict[int, dict] | None = None) -> tuple[np.ndarray, list[dict]]:
    """戻り値: shift[k]（半音）, 解決済みノート（targetMidi / bypass 付き）"""
    midi = hz_to_midi(curve.f0)
    voiced = np.array(curve.voiced) > 0
    hop_ms = curve.hop / curve.sr * 1000.0
    shift = np.zeros(len(midi))
    alpha = 1.0 if speed_ms <= 0 else 1.0 - np.exp(-hop_ms / speed_ms)
    resolved = []
    for i, nt in enumerate(notes):
        ov = (overrides or {}).get(i, {})
        target = ov.get("targetMidi", snap(nt["medianMidi"], pcs))
        bypass = ov.get("bypass", False)
        resolved.append({**nt, "targetMidi": target, "bypass": bypass})
        if bypass:
            continue
        s = target - nt["medianMidi"]
        for k in range(nt["startFrame"], nt["endFrame"]):
            if not voiced[k] or not np.isfinite(midi[k]):
                continue
            e = target - midi[k]
            s = s + alpha * (e - s)
            shift[k] = strength * s
    shift = np.where(voiced, shift, 0.0) + transpose
    return shift, resolved


def identity_map(n_samples: int) -> list[tuple[int, int]]:
    return [(0, 0), (n_samples, n_samples)]


def move_note(curve: Curve, notes: list[dict], n_samples: int, index: int, delta: int,
              min_ms: float = 10.0) -> list[tuple[int, int]]:
    """ノート index を Δ サンプル横移動した時間写像（plan の不変条件）:
    - 動くのはそのノートの開始/終了ノードだけ（長さ不変）。両隣の区間（隙間。隙間0なら隣接ノート）が伸縮
    - 先頭・末尾ノードは固定。各区間の出力長の下限 = min(10ms, 初期長)。下回るならクランプ"""
    hop = curve.hop
    bounds = sorted({0, n_samples} | {n["startFrame"] * hop for n in notes} | {min(n_samples, n["endFrame"] * hop) for n in notes})
    out = {b: b for b in bounds}
    s, e = notes[index]["startFrame"] * hop, min(n_samples, notes[index]["endFrame"] * hop)
    i_s, i_e = bounds.index(s), bounds.index(e)
    min_len = min_ms / 1000.0 * curve.sr
    # 手前の区間 [bounds[i_s-1], s] と後ろの区間 [e, bounds[i_e+1]] の下限でクランプ
    prev_len = s - bounds[i_s - 1]
    next_len = bounds[i_e + 1] - e
    lo = -(prev_len - min(min_len, prev_len))
    hi = next_len - min(min_len, next_len)
    d = int(max(lo, min(hi, delta)))
    out[s] += d; out[e] += d
    return [(b, out[b]) for b in bounds]


def inverse_map(tmap: list[tuple[int, int]], out_pos) -> np.ndarray:
    xs = np.array([o for _, o in tmap], dtype=float); ys = np.array([i for i, _ in tmap], dtype=float)
    return np.interp(np.asarray(out_pos, dtype=float), xs, ys)


def forward_map(tmap: list[tuple[int, int]], in_pos) -> np.ndarray:
    xs = np.array([i for i, _ in tmap], dtype=float); ys = np.array([o for _, o in tmap], dtype=float)
    return np.interp(np.asarray(in_pos, dtype=float), xs, ys)
