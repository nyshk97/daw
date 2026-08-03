#!/usr/bin/env python
"""BPM・拍グリッド・キー・構成の基礎分析。

出力:
  analysis/basics.json   機械可読の結果（後段のガチャの制約カードの原型）
  analysis/click.wav     元音源に拍クリックを重ねたもの（BPM/拍位置を耳で裏取りする用）
  analysis/sections.png  小節ごとのエネルギー曲線＋推定セクション境界

使い方: basics.py <audio> <outdir> [--bpm-min 70] [--bpm-max 150]
"""
import argparse
import json
import warnings
from pathlib import Path

warnings.filterwarnings("ignore")

import librosa
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import soundfile as sf

SR = 22050
HOP = 512

# Krumhansl-Kessler のキープロファイル。各音度が「そのキーらしさ」にどれだけ寄与するかの
# 経験的な重み。平均クロマとの相関が最大のキーを採用する定番手法。
KK_MAJOR = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
KK_MINOR = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])
PITCHES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


ONSET_HOP = 128  # 拍位置を詰めるので細かく取る（22050Hz で 5.8ms/フレーム）


def band_onset(y: np.ndarray, sr: int, fmin: float, fmax: float, hop: int = HOP) -> np.ndarray:
    """指定帯域だけのオンセット強度。キック(低域)とスネア(中域)を分けて見るために使う。"""
    S = np.abs(librosa.stft(y, n_fft=2048, hop_length=hop))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=2048)
    sel = (freqs >= fmin) & (freqs <= fmax)
    return librosa.onset.onset_strength(S=librosa.amplitude_to_db(S[sel], ref=np.max), sr=sr, hop_length=hop)


def fit_rigid_grid(env: np.ndarray, t: np.ndarray, bpm_min: float, bpm_max: float):
    """「テンポ一定」を仮定して BPM とグリッド原点を直接当てる。

    ビートトラッカーは1拍ずつ追うので打ち込みでも数十msの揺れが残り、そのまま
    小節グリッドにすると後半でズレる。打ち込みのHIPHOPはテンポが完全固定なので、
    等間隔グリッド上のオンセット強度の平均を最大化する (BPM, 原点) を探す方が正確。

    注意: この素点は「テンポの倍・半分」を区別できない。HIPHOPは8分ハットが
    4分のキック/スネアと同じくらいオンセットが立つため、探索レンジを広く取ると
    倍テンポ（8分グリッド）が僅差で勝つ。レンジで実用帯に絞った上で、
    オクターブ関係の素点も併記して人間が検算できるようにする。

    返り値: (bpm, 原点秒, 素点, オクターブ検算)
    """
    env = env / (env.max() + 1e-9)
    span = t[-1] - t[0]

    def score_at(bpm: float, offsets: np.ndarray) -> np.ndarray:
        period = 60.0 / bpm
        k = np.arange(int(span / period))
        grid = offsets[:, None] + k[None, :] * period  # (n_offset, n_beat)
        vals = np.interp(grid.ravel(), t, env).reshape(grid.shape)
        return vals.mean(axis=1)

    # 1段目: 曲の中ほど 45 秒だけで候補を絞る。
    # 全長でいきなり粗探索すると刻みが足りない — BPM が Δ ずれたグリッドは曲の終端で
    # span*Δ/bpm 秒ぶん歩くので、230秒・97BPM なら 0.25BPM 刻みでも 1/3 拍ずれて
    # 正解の近傍が谷になってしまう（実測でこれを踏んだ）。短い窓なら刻みが粗くても歩かない。
    mid = (t[0] + t[-1]) / 2
    wsel = (t >= mid - 22.5) & (t <= mid + 22.5)
    tw, ew = t[wsel], env[wsel]
    span_w = tw[-1] - tw[0]

    def score_window(bpm: float, offsets: np.ndarray) -> np.ndarray:
        period = 60.0 / bpm
        k = np.arange(int(span_w / period))
        grid = tw[0] + offsets[:, None] + k[None, :] * period
        return np.interp(grid.ravel(), tw, ew).reshape(grid.shape).mean(axis=1)

    coarse = []
    for bpm in np.arange(bpm_min, bpm_max, 0.1):
        offs = np.arange(0.0, 60.0 / bpm, 0.010)
        coarse.append((float(score_window(bpm, offs).max()), float(bpm)))
    coarse.sort(reverse=True)
    # 近接する候補は同じピークなので間引き、離れたピーク上位5本を全長で検証する
    peaks: list[float] = []
    for _, bpm in coarse:
        if all(abs(bpm - p) > 1.0 for p in peaks):
            peaks.append(bpm)
        if len(peaks) == 5:
            break

    # 2段目: 各候補の近傍を全長で精密化する（窓推定の誤差ぶん ±0.3 BPM 見る）
    best = (0.0, 0.0, -1.0)
    for p in peaks:
        for bpm in np.arange(p - 0.3, p + 0.3, 0.002):
            offs = np.arange(0.0, 60.0 / bpm, 0.001)
            s = score_at(bpm, offs)
            i = int(np.argmax(s))
            if s[i] > best[2]:
                best = (float(bpm), float(offs[i]), float(s[i]))
    bpm, origin, score = best
    octaves = {}
    for label, mul in (("half", 0.5), ("double", 2.0), ("x1.5", 1.5)):
        cand = bpm * mul
        offs = np.arange(0.0, 60.0 / cand, 0.001)
        octaves[label] = {"bpm": round(cand, 3), "score": round(float(score_at(cand, offs).max()), 4)}
    return bpm, origin, score, octaves


def estimate_key(y_harm: np.ndarray, sr: int) -> list[dict]:
    """平均クロマ vs K-K プロファイルの相関で 24 キーを順位づけする。"""
    chroma = librosa.feature.chroma_cqt(y=y_harm, sr=sr, hop_length=HOP)
    mean = chroma.mean(axis=1)
    mean = (mean - mean.mean()) / (mean.std() + 1e-9)
    out = []
    for i in range(12):
        for name, prof in (("major", KK_MAJOR), ("minor", KK_MINOR)):
            p = np.roll(prof, i)
            p = (p - p.mean()) / (p.std() + 1e-9)
            out.append({"key": f"{PITCHES[i]} {name}", "corr": float(np.dot(mean, p) / 12)})
    out.sort(key=lambda d: -d["corr"])
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("outdir")
    ap.add_argument("--bpm-min", type=float, default=70)
    ap.add_argument("--bpm-max", type=float, default=150)
    args = ap.parse_args()

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    y, sr = librosa.load(args.audio, sr=SR, mono=True)
    dur = len(y) / sr

    # --- テンポと拍 ---
    onset_env = librosa.onset.onset_strength(y=y, sr=sr, hop_length=ONSET_HOP)
    onset_t = librosa.frames_to_time(np.arange(len(onset_env)), sr=sr, hop_length=ONSET_HOP)
    tempo_candidates = librosa.feature.tempo(
        onset_envelope=librosa.onset.onset_strength(y=y, sr=sr, hop_length=HOP), sr=sr, hop_length=HOP, aggregate=None
    )
    tempo_bt, beats = librosa.beat.beat_track(y=y, sr=sr, hop_length=HOP, trim=False)
    beat_track_times = librosa.frames_to_time(beats, sr=sr, hop_length=HOP)

    bpm, origin, grid_score, octaves = fit_rigid_grid(onset_env, onset_t, args.bpm_min, args.bpm_max)
    period = 60.0 / bpm
    beat_times = origin + np.arange(int((dur - origin) / period) + 1) * period
    beat_times = beat_times[beat_times < dur]

    # 剛体グリッドが曲全体で成立しているかの確認。区間ごとに「グリッド上のオンセット」と
    # 「グリッドから半拍ずらした点のオンセット」の比を見る。テンポがずれていくなら後半で落ちる。
    env_n = onset_env / (onset_env.max() + 1e-9)
    on_grid = np.interp(beat_times, onset_t, env_n)
    off_grid = np.interp(beat_times + period / 2, onset_t, env_n)
    seg_n = max(1, len(beat_times) // 8)
    grid_contrast = [
        round(float(on_grid[i : i + seg_n].mean() / (off_grid[i : i + seg_n].mean() + 1e-9)), 3)
        for i in range(0, len(beat_times) - seg_n + 1, seg_n)
    ]

    # ビートトラッカーの拍を剛体グリッドに写したときのズレ（トラッカー側の揺れの大きさ）
    dev = np.abs(((beat_track_times - origin + period / 2) % period) - period / 2)
    residual_ms = float(dev.mean() * 1000)

    # --- 小節頭（ダウンビート）の位相 ---
    kick = band_onset(y, sr, 20, 120, hop=ONSET_HOP)
    snare = band_onset(y, sr, 150, 500, hop=ONSET_HOP)
    kt = librosa.frames_to_time(np.arange(len(kick)), sr=sr, hop_length=ONSET_HOP)
    kb = np.interp(beat_times, kt, kick)
    sb = np.interp(beat_times, librosa.frames_to_time(np.arange(len(snare)), sr=sr, hop_length=ONSET_HOP), snare)
    kb = (kb - kb.mean()) / (kb.std() + 1e-9)
    sb = (sb - sb.mean()) / (sb.std() + 1e-9)

    phase_scores = []
    for p in range(4):
        cls = (np.arange(len(beat_times)) - p) % 4
        # 4/4 の定石: キックは 1（と 3）、スネアは 2 と 4。両方の合致度を足して位相を決める
        kick_score = kb[cls == 0].mean() - kb[cls != 0].mean()
        snare_score = (sb[cls == 1].mean() + sb[cls == 3].mean()) / 2 - (sb[cls == 0].mean() + sb[cls == 2].mean()) / 2
        phase_scores.append(float(kick_score + snare_score))
    phase = int(np.argmax(phase_scores))
    downbeats = beat_times[phase::4]
    slope = period

    # --- テンポの安定性（トラッカー側の局所BPM。剛体グリッドの妥当性チェック用） ---
    win = 16
    local = []
    for i in range(0, len(beat_track_times) - win, win):
        seg = beat_track_times[i : i + win + 1]
        local.append(60.0 / np.diff(seg).mean())
    local = np.array(local) if local else np.array([bpm])

    # --- キー ---
    y_harm = librosa.effects.harmonic(y, margin=3.0)
    keys = estimate_key(y_harm, sr)

    # --- 小節ごとのエネルギー ---
    S = np.abs(librosa.stft(y, n_fft=2048, hop_length=HOP))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=2048)
    times = librosa.frames_to_time(np.arange(S.shape[1]), sr=sr, hop_length=HOP)
    bands = {"low": (20, 200), "mid": (200, 2000), "high": (2000, 11000)}
    bar_rows = []
    for b, t0 in enumerate(downbeats):
        t1 = downbeats[b + 1] if b + 1 < len(downbeats) else min(t0 + 4 * slope, dur)
        sel = (times >= t0) & (times < t1)
        if sel.sum() == 0:
            continue
        row = {"bar": b + 1, "t": float(t0)}
        seg = y[int(t0 * sr) : int(t1 * sr)]
        row["rms_db"] = float(librosa.amplitude_to_db(np.array([np.sqrt((seg**2).mean()) + 1e-9]), ref=1.0)[0])
        for name, (lo, hi) in bands.items():
            fsel = (freqs >= lo) & (freqs < hi)
            row[name] = float(librosa.amplitude_to_db(np.array([S[fsel][:, sel].mean() + 1e-9]), ref=1.0)[0])
        bar_rows.append(row)

    # --- セクション境界（小節特徴の変化点。4小節グリッドに寄せる） ---
    feat = np.array([[r["rms_db"], r["low"], r["mid"], r["high"]] for r in bar_rows])
    feat = (feat - feat.mean(axis=0)) / (feat.std(axis=0) + 1e-9)
    novelty = np.zeros(len(feat))
    w = 4
    for i in range(w, len(feat) - w):
        novelty[i] = np.linalg.norm(feat[i : i + w].mean(axis=0) - feat[i - w : i].mean(axis=0))
    cand = []
    for i in range(w, len(novelty) - w):
        if novelty[i] == novelty[max(0, i - 3) : i + 4].max() and novelty[i] > novelty.mean() + novelty.std():
            cand.append(i)
    # 4小節グリッドに丸め、近すぎるものは強い方を残す
    snapped = sorted({int(round(c / 4) * 4) for c in cand})
    boundaries = [s for s in snapped if 0 < s < len(bar_rows)]

    result = {
        "audio": str(Path(args.audio).name),
        "duration_sec": round(dur, 3),
        "tempo": {
            "bpm": round(float(bpm), 3),
            "grid_origin_sec": round(float(origin), 4),
            "grid_score": round(grid_score, 4),
            "octave_check": octaves,
            "grid_contrast_by_eighth": grid_contrast,
            "bpm_beat_track": round(float(np.atleast_1d(tempo_bt)[0]), 3),
            "beat_track_dev_ms": round(residual_ms, 2),
            "local_bpm_min": round(float(local.min()), 2),
            "local_bpm_max": round(float(local.max()), 2),
            "local_bpm_std": round(float(local.std()), 3),
            "candidates_top": [round(float(c), 2) for c in np.unique(np.round(tempo_candidates, 1))[:12]],
        },
        "grid": {
            "first_beat_sec": round(float(beat_times[0]), 4),
            "first_downbeat_sec": round(float(downbeats[0]), 4),
            "downbeat_phase": phase,
            "phase_scores": [round(s, 3) for s in phase_scores],
            "n_beats": int(len(beat_times)),
            "n_bars": int(len(downbeats)),
        },
        "key": {"top5": [{"key": k["key"], "corr": round(k["corr"], 4)} for k in keys[:5]]},
        "bars": bar_rows,
        "section_boundary_bars": [bar_rows[b]["bar"] for b in boundaries],
    }
    (outdir / "basics.json").write_text(json.dumps(result, indent=2, ensure_ascii=False))

    # --- クリック重ねwav（耳で裏取りする用） ---
    y_full, sr_full = librosa.load(args.audio, sr=44100, mono=True)
    clicks_beat = librosa.clicks(times=beat_times, sr=sr_full, click_freq=1200, length=len(y_full))
    clicks_down = librosa.clicks(times=downbeats, sr=sr_full, click_freq=2400, length=len(y_full))
    mixed = 0.7 * y_full + 0.35 * clicks_beat + 0.6 * clicks_down
    sf.write(outdir / "click.wav", np.clip(mixed, -1, 1), sr_full)

    # --- 構成図 ---
    fig, ax = plt.subplots(2, 1, figsize=(18, 7), sharex=True)
    bt = [r["t"] for r in bar_rows]
    ax[0].plot(bt, [r["rms_db"] for r in bar_rows], marker="o", ms=2.5, color="#d8663b")
    ax[0].set_ylabel("bar RMS (dBFS)")
    ax[0].grid(alpha=0.3)
    for name, color in (("low", "#c0392b"), ("mid", "#27ae60"), ("high", "#2980b9")):
        ax[1].plot(bt, [r[name] for r in bar_rows], label=name, lw=1.2, color=color)
    ax[1].set_ylabel("band level (dB)")
    ax[1].legend(loc="lower right")
    ax[1].grid(alpha=0.3)
    ax[1].set_xlabel("time (s)")
    for b in boundaries:
        for a in ax:
            a.axvline(bar_rows[b]["t"], color="black", ls="--", alpha=0.6)
        ax[0].text(bar_rows[b]["t"], ax[0].get_ylim()[1], f" bar {bar_rows[b]['bar']}", va="top", fontsize=8)
    ax[0].set_title(f"{Path(args.audio).name}  —  {bpm:.2f} BPM / {len(downbeats)} bars / key: {keys[0]['key']}")
    fig.tight_layout()
    fig.savefig(outdir / "sections.png", dpi=110)

    print(json.dumps({k: v for k, v in result.items() if k != "bars"}, indent=2, ensure_ascii=False))
    print(f"\nwrote {outdir/'basics.json'} / {outdir/'click.wav'} / {outdir/'sections.png'}")


if __name__ == "__main__":
    main()
