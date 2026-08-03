#!/usr/bin/env python
"""音源の全体像を1枚の画像にする（波形・RMS曲線・スペクトログラム）。

MVエディット（イントロのSE、映像用の尺合わせ、フェード）が混入していないかを
目で確認するための下ごしらえ。曲としての音は 20Hz〜16kHz に広く分布し、
SE や無音区間は帯域とRMSの両方で浮くので、この3段で大体判別できる。

使い方: overview.py <audio> <out.png> [--title T]
"""
import argparse
import warnings

warnings.filterwarnings("ignore")

import librosa
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("out")
    ap.add_argument("--title", default="")
    ap.add_argument("--sr", type=int, default=22050)
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--end", type=float, default=None)
    args = ap.parse_args()

    offset = args.start
    duration = None if args.end is None else args.end - args.start
    y, sr = librosa.load(args.audio, sr=args.sr, mono=True, offset=offset, duration=duration)
    dur = len(y) / sr

    hop = 512
    rms = librosa.feature.rms(y=y, hop_length=hop)[0]
    rms_db = librosa.amplitude_to_db(rms, ref=1.0)
    t_rms = librosa.frames_to_time(np.arange(len(rms)), sr=sr, hop_length=hop)

    S = librosa.amplitude_to_db(np.abs(librosa.stft(y, n_fft=2048, hop_length=hop)), ref=np.max)

    fig, axes = plt.subplots(3, 1, figsize=(18, 10), sharex=True)

    axes[0].plot(np.arange(len(y)) / sr, y, lw=0.2, color="#3b7dd8")
    axes[0].set_ylabel("waveform")
    axes[0].set_ylim(-1, 1)

    axes[1].plot(t_rms, rms_db, lw=0.8, color="#d8663b")
    axes[1].set_ylabel("RMS (dBFS)")
    axes[1].set_ylim(-70, 0)
    axes[1].grid(alpha=0.3)

    librosa.display.specshow(S, sr=sr, hop_length=hop, x_axis="time", y_axis="log", ax=axes[2], cmap="magma")
    axes[2].set_ylabel("spectrogram")

    for ax in axes:
        for t in range(0, int(dur) + 1, 10):
            ax.axvline(t, color="white" if ax is axes[2] else "black", alpha=0.12, lw=0.5)

    axes[0].set_title(f"{args.title or args.audio}  —  {dur:.2f}s ({int(dur // 60)}:{dur % 60:05.2f})")
    fig.tight_layout()
    fig.savefig(args.out, dpi=110)

    # 無音に近い区間（-50dBFS未満が0.3秒以上続く）を列挙する。
    # 曲頭・曲尾のフェードと、途中のブレイク（意図的な抜き）の切り分けに使う。
    quiet = rms_db < -50
    runs = []
    start = None
    for i, q in enumerate(quiet):
        if q and start is None:
            start = i
        elif not q and start is not None:
            if t_rms[i] - t_rms[start] >= 0.3:
                runs.append((t_rms[start], t_rms[i]))
            start = None
    if start is not None:
        runs.append((t_rms[start], t_rms[-1]))

    print(f"duration: {dur:.3f}s  sr(load): {sr}")
    print(f"peak: {librosa.amplitude_to_db(np.array([np.abs(y).max()]), ref=1.0)[0]:.2f} dBFS")
    print(f"mean RMS: {rms_db[rms_db > -70].mean():.2f} dBFS")
    print("near-silence (<-50dBFS, >=0.3s):")
    for a, b in runs:
        print(f"  {a:7.2f} - {b:7.2f}  ({b - a:.2f}s)")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
