"""自作 YIN（C++ へ移植する原型）。

de Cheveigné & Kawahara (2002) の手順そのまま:
 1. 差分関数 d(τ) = Σ (x[n] - x[n+τ])²  （FFT で自己相関を取って計算）
 2. 累積平均正規化 d'(τ)（τ=0 で 1、周期で谷）
 3. 閾値以下に最初に落ちた谷（絶対閾値法）→ 放物線補間で小数 τ
 4. 谷の深さ（d' の最小値）を「非周期性」として有声度に使う: prob = 1 - d'min。
    有声閾値 0.6 は lab で掃引して決めた（0.8 だと実声のラップで recall 0.63 まで落ちる。合成素材は 0.5〜0.8 で差なし）
出力は common.Curve と同じフレーム約束（フレーム k の中心 = k*hop）。
"""
from __future__ import annotations

import numpy as np

from common import Curve, FMIN, FMAX, frame_rms, hop_samples


def _cmndf_frame(frame: np.ndarray, tau_max: int) -> np.ndarray:
    n = len(frame)
    w = n - tau_max
    # d(τ) = Σ_{n<w} (x[n]-x[n+τ])² = E0 + Eτ - 2 r(τ)
    size = 1
    while size < 2 * n:
        size <<= 1
    fx = np.fft.rfft(frame, size)
    # 相関 r(τ) = Σ_{n<w} x[n] x[n+τ]: 先頭 w サンプルの窓と全体の相互相関
    head = np.zeros(n); head[:w] = frame[:w]
    fh = np.fft.rfft(head, size)
    corr = np.fft.irfft(np.conj(fh) * fx, size)[: tau_max + 1]
    sq = np.concatenate(([0.0], np.cumsum(frame * frame)))
    e0 = sq[w]
    etau = sq[np.arange(tau_max + 1) + w] - sq[np.arange(tau_max + 1)]
    d = e0 + etau - 2.0 * corr
    d[0] = 0.0
    cm = np.ones(tau_max + 1)
    run = np.cumsum(d[1:])
    with np.errstate(divide="ignore", invalid="ignore"):
        cm[1:] = np.where(run > 0, d[1:] * np.arange(1, tau_max + 1) / run, 1.0)
    return cm


def yin(x: np.ndarray, sr: int, frame_length: int = 2048, threshold: float = 0.15,
        voicing_prob_min: float = 0.6, rms_gate: float = 1e-3) -> Curve:
    hop = hop_samples(sr)
    tau_min = int(sr / FMAX)
    tau_max = int(sr / FMIN)
    half = frame_length // 2
    pad = np.pad(x.astype(np.float64), (half, half + frame_length))
    n_frames = int(np.ceil(len(x) / hop))
    f0 = np.zeros(n_frames); prob = np.zeros(n_frames)
    rms = frame_rms(x, hop, n_frames)
    win = np.hanning(frame_length)
    for k in range(n_frames):
        fr = pad[k * hop : k * hop + frame_length] * win
        if rms[k] < rms_gate:
            continue
        cm = _cmndf_frame(fr, tau_max)
        seg = cm[tau_min:]
        # 絶対閾値法: 閾値を下回った最初の谷
        below = np.where(seg < threshold)[0]
        if len(below):
            t = below[0]
            while t + 1 < len(seg) and seg[t + 1] < seg[t]:
                t += 1
        else:
            t = int(np.argmin(seg))
        tau = t + tau_min
        # 放物線補間
        if 1 <= tau < tau_max:
            a, b, c = cm[tau - 1], cm[tau], cm[tau + 1]
            denom = a - 2 * b + c
            tau_f = tau + (0.5 * (a - c) / denom if denom != 0 else 0.0)
        else:
            tau_f = float(tau)
        dmin = float(cm[tau])
        p = 1.0 - min(1.0, max(0.0, dmin))
        prob[k] = p
        f0[k] = sr / tau_f if p >= voicing_prob_min else 0.0
    voiced = (f0 > 0).astype(int)
    # 孤立フレームの掃除（1フレームだけの有声/無声はノイズ扱い）
    for k in range(1, n_frames - 1):
        if voiced[k] and not voiced[k - 1] and not voiced[k + 1]:
            voiced[k] = 0; f0[k] = 0.0
    return Curve(sr=sr, hop=hop, f0=f0.tolist(), voiced=voiced.tolist(), prob=prob.tolist(),
                 rms=rms.tolist(), algo="yin",
                 meta={"frame_length": frame_length, "threshold": threshold,
                       "voicing_prob_min": voicing_prob_min})
