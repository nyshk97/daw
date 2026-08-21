"""再合成エンジン4種。共通インターフェース:
    render(x, sr, curve, shift_frames, tmap, out_dir, tag) -> np.ndarray（長さ = tmap 末尾の outSample）
- shift_frames[k]: 入力フレーム k の移動量[半音]
- tmap: (inSample, outSample) 単調ノード列
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import numpy as np
import soundfile as sf

from common import HERE, Curve
from correction import inverse_map, forward_map

SSSTRETCH = HERE / "work" / "ssstretch"


# ---------------------------------------------------------------- TD-PSOLA（有声）＋OLA 素通し（無声）
def pitch_marks(x: np.ndarray, sr: int, curve: Curve) -> tuple[np.ndarray, np.ndarray]:
    """有声区間に1周期ごとの基準点（ピッチマーク）を置く。
    マーク = 低域寄りにした波形の局所ピーク。次のマークは「現在位置 + 周期」の ±15% の窓内で最大の点。
    戻り値: marks（サンプル位置・昇順）, periods（各マークの周期サンプル数）"""
    from scipy.signal import butter, sosfiltfilt
    f0 = np.array(curve.f0); voiced = np.array(curve.voiced) > 0
    hop = curve.hop
    # ピーク探索用: 1kHz ローパス（フォルマントの細かい山で迷わないように。位相ゼロ）
    sos = butter(2, 1000.0 / (sr / 2), output="sos")
    xl = sosfiltfilt(sos, x.astype(np.float64))
    marks, periods = [], []
    n = len(x)
    k = 0
    while k < len(f0):
        if not voiced[k]:
            k += 1; continue
        e = k
        while e < len(f0) and voiced[e]:
            e += 1
        start, end = k * hop, min(n, e * hop)
        period = sr / f0[k]
        # 最初のマーク: 先頭1周期内の最大
        seg_end = min(end, int(start + period) + 1)
        t = start + int(np.argmax(xl[start:seg_end])) if seg_end > start else start
        while t < end:
            fk = f0[min(len(f0) - 1, int(t // hop))]
            if fk <= 0:
                fk = f0[k]
            period = sr / fk
            marks.append(t); periods.append(period)
            center = t + period
            lo = int(center - 0.15 * period); hi = int(center + 0.15 * period) + 1
            if lo >= end:
                break
            hi = min(hi, end)
            t = lo + int(np.argmax(xl[lo:hi])) if hi > lo else int(round(center))
        k = e
    return np.array(marks, dtype=int), np.array(periods, dtype=float)


def render_psola(x, sr, curve, shift, tmap, out_dir, tag, uv_grain_ms: float = 20.0):
    hop = curve.hop
    voiced = np.array(curve.voiced) > 0
    n_in = len(x); n_out = int(tmap[-1][1])
    marks, periods = pitch_marks(x, sr, curve)
    out = np.zeros(n_out + 4 * sr // 100); wsum = np.zeros_like(out)
    xpad = np.pad(x.astype(np.float64), (n_in, n_in))  # 端のグレインが外に出ても読めるように

    def add_grain(center_in: int, half: int, pos_out: float):
        g = xpad[n_in + center_in - half : n_in + center_in + half]
        w = np.hanning(2 * half)
        p = int(round(pos_out)) - half
        lo, hi = max(0, p), min(len(out), p + 2 * half)
        if hi <= lo:
            return
        out[lo:hi] += (g * w)[lo - p : hi - p]; wsum[lo:hi] += w[lo - p : hi - p]

    uv_half = int(uv_grain_ms / 1000 * sr / 2)
    uv_hop = uv_half  # 50% オーバーラップ（Hann は COLA → 等倍なら出力 = 入力）
    t_out = 0.0
    n_voiced_grains = n_uv_grains = 0
    while t_out < n_out:
        t_in = float(inverse_map(tmap, t_out))
        k = min(len(voiced) - 1, int(t_in // hop))
        if voiced[k] and len(marks):
            mi = int(np.searchsorted(marks, t_in))
            if mi >= len(marks) or (mi > 0 and abs(marks[mi - 1] - t_in) < abs(marks[mi] - t_in)):
                mi -= 1
            m, P = int(marks[mi]), periods[mi]
            if abs(m - t_in) > 1.5 * P:     # マークが無い（有声だが周期が取れない）→ 無声扱い
                add_grain(int(round(t_in)), uv_half, t_out); t_out += uv_hop; n_uv_grains += 1; continue
            ratio = 2.0 ** (shift[k] / 12.0)
            n_voiced_grains += 1
            if abs(ratio - 1.0) < 1e-9:
                # 無シフト: グレインを元マークの写像先に置き、次の入力マークへ正確に同期する
                # （等倍なら出力 == 入力のビット一致。シフト 0 の区間も同様に透明）
                pos = float(forward_map(tmap, m))
                add_grain(m, int(round(P)), pos)
                # 次のマークが同じ有声区間（1.5周期以内）にあるときだけ同期。別区間なら1周期進める
                has_next = mi + 1 < len(marks) and marks[mi + 1] - m <= 1.5 * P
                nxt = float(forward_map(tmap, marks[mi + 1])) if has_next else pos + P
                t_out = nxt if nxt > pos else pos + P
            else:
                add_grain(m, int(round(P)), t_out)
                t_out += P / ratio
        else:
            add_grain(int(round(t_in)), uv_half, t_out); t_out += uv_hop; n_uv_grains += 1
    y = out[:n_out]
    ws = wsum[:n_out]
    y = np.where(ws > 1e-3, y / np.maximum(ws, 1e-3), y)
    (out_dir / f"{tag}.psola.log").write_text(f"marks={len(marks)} voiced_grains={n_voiced_grains} uv_grains={n_uv_grains}\n")
    return y.astype(np.float32)


# ---------------------------------------------------------------- WORLD
def render_world(x, sr, curve, shift, tmap, out_dir, tag):
    import pyworld as pw
    fp = curve.hop / sr * 1000.0
    xd = x.astype(np.float64)
    f0, t = pw.harvest(xd, sr, f0_floor=60.0, f0_ceil=1000.0, frame_period=fp)
    f0 = pw.stonemask(xd, f0, t, sr)
    sp = pw.cheaptrick(xd, f0, t, sr)
    ap = pw.d4c(xd, f0, t, sr)
    n_out = int(tmap[-1][1])
    n_frames_out = int(np.ceil(n_out / curve.hop)) + 1
    out_pos = np.arange(n_frames_out) * curve.hop
    in_pos = inverse_map(tmap, out_pos)
    src = np.clip(np.round(in_pos / curve.hop).astype(int), 0, len(f0) - 1)
    ks = np.clip(src, 0, len(shift) - 1)
    f0o = f0[src] * 2.0 ** (shift[ks] / 12.0)
    y = pw.synthesize(np.ascontiguousarray(f0o), np.ascontiguousarray(sp[src]), np.ascontiguousarray(ap[src]), sr, fp)
    y = np.resize(y, n_out) if len(y) >= n_out else np.pad(y, (0, n_out - len(y)))
    return y.astype(np.float32)


# ---------------------------------------------------------------- signalsmith-stretch（自作 CLI）
def render_signalsmith(x, sr, curve, shift, tmap, out_dir, tag, formant=True):
    hop = 256
    n_out = int(tmap[-1][1])
    n_blocks = int(np.ceil(n_out / hop)) + 1
    out_pos = np.arange(n_blocks) * hop + hop / 2
    in_pos = inverse_map(tmap, out_pos)
    ks = np.clip((in_pos // curve.hop).astype(int), 0, len(shift) - 1)
    f0 = np.array(curve.f0)
    lines = [f"nodes {len(tmap)}"] + [f"{i} {o}" for i, o in tmap] + [f"hop {hop}", f"semis {n_blocks}"]
    lines += [f"{shift[k]:.5f} {f0[k]:.2f}" for k in ks]
    mp = out_dir / f"{tag}.ssmap.txt"; mp.write_text("\n".join(lines) + "\n")
    wi = out_dir / f"{tag}.ss_in.wav"; wo = out_dir / f"{tag}.ss_out.wav"
    sf.write(str(wi), x, sr, subtype="PCM_16")
    args = [str(SSSTRETCH), str(wi), str(wo), str(mp)] + ([] if formant else ["--no-formant"])
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"ssstretch failed: {r.stdout}\n{r.stderr}")
    y, _ = sf.read(str(wo), dtype="float32")
    return np.resize(y, n_out) if len(y) >= n_out else np.pad(y, (0, n_out - len(y)))


# ---------------------------------------------------------------- Rubber Band R3（参照のみ・GPL）
def render_rubberband(x, sr, curve, shift, tmap, out_dir, tag):
    n_in = len(x); n_out = int(tmap[-1][1])
    pm = out_dir / f"{tag}.rb_pitchmap.txt"
    rows = []
    for k in range(len(shift)):
        if k == 0 or abs(shift[k] - shift[k - 1]) > 1e-6:
            rows.append(f"{k * curve.hop} {shift[k]:.5f}")
    pm.write_text("\n".join(rows) + "\n")
    wi = out_dir / f"{tag}.rb_in.wav"; wo = out_dir / f"{tag}.rb_out.wav"
    sf.write(str(wi), x, sr, subtype="PCM_24")
    args = ["rubberband", "-3", "-F", "-q", "--pitchmap", str(pm), "-t", f"{n_out / n_in:.8f}"]
    if len(tmap) > 2:
        tm = out_dir / f"{tag}.rb_timemap.txt"
        tm.write_text("\n".join(f"{i} {o}" for i, o in tmap) + "\n")
        args += ["-M", str(tm)]
    r = subprocess.run(args + [str(wi), str(wo)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"rubberband failed: {r.stdout}\n{r.stderr}")
    y, _ = sf.read(str(wo), dtype="float32")
    (out_dir / f"{tag}.rb.log").write_text(f"out_len={len(y)} expected={n_out}\n")
    return np.resize(y, n_out) if len(y) >= n_out else np.pad(y, (0, n_out - len(y)))


ENGINES = {
    "psola": render_psola,
    "world": render_world,
    "signalsmith": render_signalsmith,
    "rubberband": render_rubberband,
}
