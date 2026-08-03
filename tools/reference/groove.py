#!/usr/bin/env python
"""ドラム・ベースの「性格」を数値にする。

借りるのはパターンそのものではなく統計（どの16分に何が来やすいか・どれだけ食う/もたつくか・
どのくらい詰まっているか）。ここで出す数字がそのままガチャの制約になる。

出力:
  analysis/groove.json  16分グリッド上のプロファイル・マイクロタイミング・密度・ベースの音域
  analysis/groove.png   帯域ごとの16分プロファイル＋ベースのピアノロール

使い方: groove.py <stemdir> <basics.json> <outdir>
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
from scipy.signal import butter, medfilt, sosfiltfilt

SR = 22050
STEP16 = 16  # 1小節を16分割（docs/design/grid-resolution.md のタイムライン上限と同じ粒度）
LABELS16 = ["1", "e", "&", "a", "2", "e", "&", "a", "3", "e", "&", "a", "4", "e", "&", "a"]

# ドラムの3要素をどの帯域で見るか。
# 注意: 高域はハットだけでなくクラップ/スネアのアタックも入る（クラップは広帯域なので
# 分離できない）。「ハットの刻み」を読むときはこの混入を前提にする。
BANDS = {
    "low": {"range": (35, 120), "means": "kick / 808 のアタック"},
    "mid": {"range": (180, 900), "means": "スネア・クラップの胴"},
    "high": {"range": (6500, 10500), "means": "ハット・シンバル（＋クラップのアタック）"},
}


def onset_env(y: np.ndarray, sr: int, lo: float, hi: float, smooth_ms: float = 6.0) -> np.ndarray:
    """帯域通過 → 振幅包絡 → 立ち上がりだけ残す。サンプル解像度のオンセット強度。

    STFT ベースの onset_strength は窓長ぶん時間分解能が鈍り、特に低域（キック）で
    アタック位置が数十msぼやける。マイクロタイミングを ms 単位で見たいのでこちらを使う。
    """
    sos = butter(4, [lo / (sr / 2), min(hi, sr / 2 - 1) / (sr / 2)], btype="band", output="sos")
    x = np.abs(sosfiltfilt(sos, y))
    n = max(1, int(sr * smooth_ms / 1000))
    x = np.convolve(x, np.ones(n) / n, mode="same")
    d = np.diff(x, prepend=x[0])
    d[d < 0] = 0
    return d / (d.max() + 1e-12)


def refine_origin(env: np.ndarray, t: np.ndarray, first_down: float, step: float, search_ms: float = 45) -> float:
    """basics の小節頭を、ドラムの実際のアタック位置に合わせて微調整する。

    basics 側は全ミックス（ボーカル・上モノ込み）で合わせているので数十msずれる。
    小節頭の同定はそのままに、ズレだけ吸収する。
    """
    best, best_s = first_down, -1.0
    k = np.arange(int((t[-1] - first_down - 0.2) / step))
    for d in np.arange(-search_ms / 1000, search_ms / 1000, 0.0005):
        s = np.interp(first_down + d + k * step, t, env).mean()
        if s > best_s:
            best, best_s = first_down + d, s
    return float(best)


def slot_profile(env: np.ndarray, t: np.ndarray, first_down: float, bar_len: float, n_bars: int):
    """16分スロットごとの平均オンセット強度と、その中でのピーク位置のズレ（ms）。

    閾値でヒット判定せずに連続値のまま出す。閾値はパラメータ1つで結果が動いてしまうので、
    「どの16分がどれだけ強いか」の形をそのまま渡す方が読み違えが少ない。
    """
    step = bar_len / STEP16
    tol = step * 0.4  # スロットの取り合いを避ける範囲でズレを許容
    vals = np.zeros((n_bars, STEP16))
    devs = np.full((n_bars, STEP16), np.nan)
    for b in range(n_bars):
        for s in range(STEP16):
            c = first_down + b * bar_len + s * step
            i0, i1 = np.searchsorted(t, [c - tol, c + tol])
            if i1 <= i0:
                continue
            seg = env[i0:i1]
            j = int(np.argmax(seg))
            vals[b, s] = seg[j]
            devs[b, s] = (t[i0 + j] - c) * 1000
    prof = vals.mean(axis=0)
    # ズレは「強く鳴っている小節」だけで平均する（無音スロットのノイズ位置は意味がない）
    strong = vals > np.percentile(vals, 75)
    dev_by_slot = [
        round(float(np.nanmean(devs[strong[:, s], s])), 1) if strong[:, s].sum() >= 5 else None for s in range(STEP16)
    ]
    return prof, vals, dev_by_slot


def _wpercentile(values: np.ndarray, weights: np.ndarray, q: float) -> float:
    order = np.argsort(values)
    v, w = values[order], weights[order]
    c = np.cumsum(w) / w.sum() * 100
    return float(v[np.searchsorted(c, q)])


def bass_notes(y: np.ndarray, sr: int, first_down: float, bar_len: float) -> dict:
    hop = 128
    f0, voiced, _ = librosa.pyin(y, fmin=30, fmax=400, sr=sr, hop_length=hop, frame_length=2048)
    t = librosa.frames_to_time(np.arange(len(f0)), sr=sr, hop_length=hop)
    midi = np.where(voiced & np.isfinite(f0), librosa.hz_to_midi(np.nan_to_num(f0, nan=1.0)), np.nan)
    # pyin は単発フレームでオクターブを取り違えることがある（音域が実際より広く出る）。
    # 中央値フィルタで1〜2フレームの飛びを潰す。
    fin = np.isfinite(midi)
    if fin.sum() > 5:
        sm = midi.copy()
        sm[fin] = medfilt(midi[fin], 5)
        midi = sm

    # 半音に丸めて、同じ音が 60ms 以上続いたら1音とみなす
    notes = []
    cur, start = None, None
    for i, m in enumerate(midi):
        q = None if not np.isfinite(m) else int(round(m))
        if q != cur:
            if cur is not None and t[i] - start >= 0.06:
                notes.append({"pitch": cur, "start": float(start), "dur": float(t[i] - start)})
            cur, start = q, t[i]
    if cur is not None and t[-1] - start >= 0.06:
        notes.append({"pitch": cur, "start": float(start), "dur": float(t[-1] - start)})
    if not notes:
        return {"count": 0}

    pitches = np.array([n["pitch"] for n in notes])
    starts = np.array([n["start"] for n in notes])
    durs = np.array([n["dur"] for n in notes])
    step = bar_len / STEP16
    rel = (starts - first_down) / step
    slot = np.round(rel).astype(int) % STEP16
    dev = (rel - np.round(rel)) * step * 1000
    n_bars = max(1, int((starts[-1] - first_down) / bar_len) + 1)
    return {
        "count": len(notes),
        "notes_per_bar": round(len(notes) / n_bars, 2),
        "pitch_min": librosa.midi_to_note(int(pitches.min())),
        "pitch_max": librosa.midi_to_note(int(pitches.max())),
        "pitch_median": librosa.midi_to_note(int(np.median(pitches))),
        "range_semitones": int(pitches.max() - pitches.min()),
        # min/max は単発のオクターブ誤検出で広がるので、長さで重みづけした 5-95% を主に見る
        "pitch_p5": librosa.midi_to_note(int(_wpercentile(pitches, durs, 5))),
        "pitch_p95": librosa.midi_to_note(int(_wpercentile(pitches, durs, 95))),
        "core_range_semitones": int(_wpercentile(pitches, durs, 95) - _wpercentile(pitches, durs, 5)),
        "pitch_class_weight": [
            round(float(x), 3) for x in np.bincount(pitches % 12, weights=durs, minlength=12) / max(durs.sum(), 1e-9)
        ],
        "dur_median_in_16ths": round(float(np.median(durs) / step), 2),
        "onset_rate_by_16th": [round(float((slot == s).sum() / n_bars), 3) for s in range(STEP16)],
        "dev_ms_mean": round(float(dev.mean()), 1),
        "dev_ms_abs_mean": round(float(np.abs(dev).mean()), 1),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("stemdir")
    ap.add_argument("basics")
    ap.add_argument("outdir")
    args = ap.parse_args()

    basics = json.loads(Path(args.basics).read_text())
    bpm = basics["tempo"]["bpm"]
    bar_len = 4 * 60.0 / bpm
    step = bar_len / STEP16
    outdir, stemdir = Path(args.outdir), Path(args.stemdir)

    drums, sr = librosa.load(stemdir / "drums.wav", sr=SR, mono=True)
    t = np.arange(len(drums)) / sr

    # マスターグリッド: 中高域（アタックがはっきりしている）で原点を詰める
    master = onset_env(drums, sr, 180, 10500)
    first_down = refine_origin(master, t, basics["grid"]["first_downbeat_sec"], step)
    n_bars = int((len(drums) / sr - first_down) / bar_len)

    result = {
        "bpm": bpm,
        "bar_len_sec": round(bar_len, 4),
        "first_downbeat_sec": round(first_down, 4),
        "first_downbeat_shift_ms": round((first_down - basics["grid"]["first_downbeat_sec"]) * 1000, 1),
        "n_bars": n_bars,
        "sixteenth_ms": round(step * 1000, 1),
        "drums": {},
    }

    envs = {}
    for name, spec in BANDS.items():
        env = onset_env(drums, sr, *spec["range"])
        envs[name] = env
        prof, vals, dev = slot_profile(env, t, first_down, bar_len, n_bars)
        norm = prof / (prof.max() + 1e-12)
        # 「鳴っている」16分＝プロファイルが最大値の35%以上。パターンの骨格を言葉にするため
        active = [LABELS16[s] for s in range(STEP16) if norm[s] >= 0.35]
        result["drums"][name] = {
            "means": spec["means"],
            "band_hz": list(spec["range"]),
            "profile_by_16th": [round(float(v), 3) for v in norm],
            "active_16ths": active,
            "dev_ms_by_16th": dev,
        }

    # スウィング: 高域（ハット）の 8分裏がどれだけ後ろにずれているか
    # 0.50=イーブン / 0.67=3連。裏拍だけの平均ズレを 8分長で割って比に直す。
    hi_dev = result["drums"]["high"]["dev_ms_by_16th"]
    on8 = [hi_dev[s] for s in (0, 4, 8, 12) if hi_dev[s] is not None]
    off8 = [hi_dev[s] for s in (2, 6, 10, 14) if hi_dev[s] is not None]
    if on8 and off8:
        d = float(np.mean(off8) - np.mean(on8))
        ratio = round(0.5 + d / (step * 2 * 1000) / 2, 4)
        # 0.5 を下回る＝裏拍が表より早い、はまず起きない。ハットの無い曲で高域が
        # 別の音（歪んだ808のノイズ等）を拾っているサイン。0.8超も同様に測定の破綻。
        # 音楽的にありうるのは 0.50〜0.75（0.67が3連）。範囲外なら数値を採用しない
        result["swing"] = {
            "offbeat_lag_ms": round(d, 1),
            "ratio": ratio,
            "plausible": bool(0.45 <= ratio <= 0.80),
            "note": "0.50=イーブン / 0.67=3連スウィング。高域(ハット)の8分裏 vs 8分表の平均ズレ。"
            "plausible=false なら高域にハットが無く別の音を拾っている（ハネ無しとは読まない）",
        }

    # 小節ごとの帯域エネルギー（抜き差しの検出用）
    density = []
    for b in range(n_bars):
        i0, i1 = int((first_down + b * bar_len) * sr), int((first_down + (b + 1) * bar_len) * sr)
        row = {"bar": b + 1}
        for name, env in envs.items():
            row[name] = round(float(env[i0:i1].sum()), 2)
        density.append(row)
    result["drums"]["energy_by_bar"] = density

    bass, _ = librosa.load(stemdir / "bass.wav", sr=SR, mono=True)
    result["bass"] = bass_notes(bass, sr, first_down, bar_len)

    (outdir / "groove.json").write_text(json.dumps(result, indent=2, ensure_ascii=False))

    # --- 図 ---
    fig, ax = plt.subplots(2, 1, figsize=(14, 8), gridspec_kw={"height_ratios": [1, 1.2]})
    x = np.arange(STEP16)
    w = 0.27
    for i, (name, color) in enumerate((("low", "#c0392b"), ("mid", "#27ae60"), ("high", "#2980b9"))):
        ax[0].bar(x + (i - 1) * w, result["drums"][name]["profile_by_16th"], width=w, label=name, color=color)
    ax[0].set_xticks(x)
    ax[0].set_xticklabels(LABELS16)
    ax[0].set_ylabel("relative onset strength")
    ax[0].set_title(f"drum profile by 16th  —  {bpm} BPM / {n_bars} bars")
    ax[0].legend()
    ax[0].grid(axis="y", alpha=0.3)

    f0, voiced, _ = librosa.pyin(bass, fmin=30, fmax=400, sr=sr, hop_length=512, frame_length=2048)
    tt = librosa.frames_to_time(np.arange(len(f0)), sr=sr, hop_length=512)
    m = np.where(voiced, librosa.hz_to_midi(np.nan_to_num(f0, nan=1.0)), np.nan)
    # ベースが最も鳴っている8小節を選ぶ（イントロは基本ベース無しなので先頭固定だと空になる）
    best_bar, best_n = 0, -1
    for b in range(max(1, n_bars - 8)):
        s = (tt >= first_down + b * bar_len) & (tt < first_down + (b + 8) * bar_len)
        n = int(np.isfinite(m[s]).sum())
        if n > best_n:
            best_bar, best_n = b, n
    t0 = first_down + best_bar * bar_len
    sel = (tt >= t0) & (tt < t0 + 8 * bar_len)
    ax[1].plot(tt[sel], m[sel], lw=2.5, color="#8e44ad")
    for b in range(9):
        ax[1].axvline(t0 + b * bar_len, color="black", alpha=0.45)
    for b in range(8 * 4):
        ax[1].axvline(t0 + b * bar_len / 4, color="gray", alpha=0.15, lw=0.6)
    ax[1].set_ylabel("bass pitch (MIDI)")
    ax[1].set_xlabel(f"time (s)  —  bars {best_bar + 1}-{best_bar + 8}")
    ax[1].grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(outdir / "groove.png", dpi=110)

    slim = json.loads(json.dumps(result))
    slim["drums"].pop("energy_by_bar")
    print(json.dumps(slim, indent=2, ensure_ascii=False))
    print(f"\nwrote {outdir/'groove.json'} / {outdir/'groove.png'}")


if __name__ == "__main__":
    main()
