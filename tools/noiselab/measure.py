#!/usr/bin/env python3
"""Step 0: 録音クリップのノイズフロア実測。

使い方:
    .venv/bin/python measure.py <clip.wav> [--head 5.0]

前提（plan 2026-08-18-1123-noise-removal-lab.md）:
- クリップ先頭 --head 秒は「何も喋らず動かない意図的無音」（ノイズフロア実測専用）
- 最静区間の自動検出は先頭 --head 秒を除外する（古典方式のプロファイル取得の試験条件。
  実運用のテイクに意図的無音は無いため、歌の隙間から拾わせる）
"""

import argparse
import sys

import numpy as np
import soundfile as sf


def db(x: float) -> float:
    return 20.0 * np.log10(max(x, 1e-12))


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x**2))) if len(x) else 0.0


def sliding_rms_db(x: np.ndarray, sr: int, win_s: float, hop_s: float = 0.1):
    """(開始秒, RMS dBFS) のリスト。"""
    win, hop = int(sr * win_s), int(sr * hop_s)
    out = []
    for start in range(0, max(len(x) - win, 0) + 1, hop):
        out.append((start / sr, db(rms(x[start : start + win]))))
    return out


def band_levels(x: np.ndarray, sr: int):
    """帯域別のRMSレベル（dBFS）。Welch風の平均パワースペクトルから積分する。"""
    nfft = 8192
    if len(x) < nfft:
        return {}
    hop = nfft // 2
    window = np.hanning(nfft)
    acc = np.zeros(nfft // 2 + 1)
    n = 0
    for start in range(0, len(x) - nfft + 1, hop):
        seg = x[start : start + nfft] * window
        acc += np.abs(np.fft.rfft(seg)) ** 2
        n += 1
    psd = acc / n / (np.sum(window**2) * sr)  # V^2/Hz 相当
    freqs = np.fft.rfftfreq(nfft, 1 / sr)
    bands = [(20, 80), (80, 300), (300, 2000), (2000, 8000), (8000, sr / 2)]
    out = {}
    for lo, hi in bands:
        m = (freqs >= lo) & (freqs < hi)
        power = np.trapezoid(psd[m], freqs[m]) if m.any() else 0.0
        out[f"{lo}-{int(hi)}Hz"] = db(np.sqrt(power))
    return out, psd, freqs


def detect_hum(psd: np.ndarray, freqs: np.ndarray):
    """50/60Hz系の電源ハム候補。周辺中央値より10dB以上高いピークを報告する。"""
    hits = []
    for base in (50.0, 60.0):
        for k in range(1, 6):
            f = base * k
            if f >= freqs[-1]:
                break
            near = (freqs >= f - 2) & (freqs <= f + 2)
            around = (freqs >= f - 20) & (freqs <= f + 20) & ~near
            if not near.any() or not around.any():
                continue
            peak = db(np.sqrt(psd[near].max()))
            floor = db(np.sqrt(np.median(psd[around])))
            if peak - floor >= 10.0:
                hits.append((f, peak - floor))
    return hits


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--head", type=float, default=5.0, help="先頭の意図的無音の秒数")
    args = ap.parse_args()

    x, sr = sf.read(args.wav, dtype="float64", always_2d=True)
    x = x.mean(axis=1)  # 録音はモノラル前提だが、念のためモノ化
    head_n = int(sr * args.head)
    if len(x) <= head_n:
        sys.exit(f"error: クリップが --head {args.head}s より短い ({len(x)/sr:.2f}s)")

    head, rest = x[:head_n], x[head_n:]

    floor_rms = db(rms(head))
    floor_peak = db(float(np.abs(head).max()))
    bands, psd, freqs = band_levels(head, sr)
    hum = detect_hum(psd, freqs)

    # 声側: 0.4s窓RMSの95パーセンタイルを「声のレベル」、最大を「声のピーク」とする
    voice_windows = [v for _, v in sliding_rms_db(rest, sr, 0.4)]
    voice_p95 = float(np.percentile(voice_windows, 95))
    peak = db(float(np.abs(rest).max()))

    # 最静区間（先頭 head 秒を除外・1.5s窓）。古典プロファイルの取得試験条件と同じ
    quiet = min(sliding_rms_db(rest, sr, 1.5), key=lambda p: p[1])
    quiet_pos = quiet[0] + args.head

    print(f"# {args.wav}")
    print(f"  SR {sr}Hz / 長さ {len(x)/sr:.1f}s（うち先頭 {args.head:.0f}s を無音として計測）")
    print()
    print(f"  ノイズフロア（先頭無音）: RMS {floor_rms:+.1f} dBFS / ピーク {floor_peak:+.1f} dBFS")
    print(f"  声のレベル（0.4s窓 p95）: {voice_p95:+.1f} dBFS / 波形ピーク {peak:+.1f} dBFS")
    print(f"  SNR（声p95 − フロアRMS）: {voice_p95 - floor_rms:.1f} dB")
    print()
    print("  ノイズの帯域分布（先頭無音・dBFS）:")
    for name, level in bands.items():
        print(f"    {name:>14}: {level:+.1f}")
    if hum:
        print("  電源ハム候補（周辺より10dB以上のピーク）:")
        for f, d in hum:
            print(f"    {f:.0f}Hz: +{d:.1f}dB")
    else:
        print("  電源ハム候補: なし")
    print()
    print(
        f"  最静区間（先頭{args.head:.0f}s除外・1.5s窓）: "
        f"{quiet_pos:.1f}s〜 RMS {quiet[1]:+.1f} dBFS"
        f"（フロアとの差 {quiet[1] - floor_rms:+.1f} dB）"
    )


if __name__ == "__main__":
    main()
