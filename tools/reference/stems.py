#!/usr/bin/env python
"""ステムの編成インベントリと分離品質の当たりをつける。

「どの楽器が・どのくらいの音量で・どんな帯域で鳴っているか」を数値にする（＝編成の把握）。
あわせて、耳で分離品質を判定するための**聴きどころのタイムスタンプ**を出す。
分離品質そのものは耳でしか判定できないので、ここでやるのは「どこを聴けばいいか」の絞り込み。

出力:
  analysis/stems.json  ステムごとの音量・帯域バランス・調波打楽器比・聴きどころ
  analysis/stems.png   ステムごとの小節エネルギー曲線

使い方: stems.py <stemdir> <mix.wav> <basics.json> <outdir> [--label htdemucs]
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

SR = 22050


def db(x: float) -> float:
    return round(float(20 * np.log10(max(x, 1e-9))), 2)


def analyze(path: Path, first_down: float, bar_len: float, n_bars: int) -> dict:
    y, sr = librosa.load(path, sr=SR, mono=True)
    ys, _ = librosa.load(path, sr=SR, mono=False)

    S = np.abs(librosa.stft(y, n_fft=2048, hop_length=512))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=2048)
    total = S.sum() + 1e-9
    bands = {
        "sub_20_80": (20, 80),
        "low_80_250": (80, 250),
        "mid_250_2k": (250, 2000),
        "hi_2k_6k": (2000, 6000),
        "air_6k_11k": (6000, 11000),
    }
    balance = {k: round(float(S[(freqs >= lo) & (freqs < hi)].sum() / total), 3) for k, (lo, hi) in bands.items()}

    # 調波(伸びる音)と打楽器(叩く音)の比。ドラムなのに調波が多い＝上モノの被り、
    # ピアノなのに打楽器が多い＝アタックだけ持っていかれている、の当たりがつく。
    h, p = librosa.effects.hpss(y, margin=2.0)
    hp = float(np.sqrt((h**2).mean()) / (np.sqrt((p**2).mean()) + 1e-9))

    per_bar = []
    for b in range(n_bars):
        seg = y[int((first_down + b * bar_len) * sr) : int((first_down + (b + 1) * bar_len) * sr)]
        per_bar.append(db(np.sqrt((seg**2).mean())) if len(seg) else -99.0)
    per_bar_arr = np.array(per_bar)

    # 聴きどころ: 一番大きい小節（その楽器の素性が出る）と、鳴っているが小さい小節
    # （分離の破綻・幽霊音が一番聴こえるのはここ）。
    # 後者は「一番小さい小節」ではなくピーク -18dB に**一番近い**小節を選ぶ。
    # 単純に小さい順で取ると、その楽器が休んでいる区間（-60dB）を掴んで
    # 聴いても何も分からないクリップになる（実際にそうなった）。
    loudest = [int(i) + 1 for i in np.argsort(-per_bar_arr)[:4]]
    target = float(np.percentile(per_bar_arr, 90)) - 18
    quietest = [int(i) + 1 for i in np.argsort(np.abs(per_bar_arr - target))[:4]]
    live = per_bar_arr[per_bar_arr > per_bar_arr.max() - 40]

    if ys.ndim == 2:
        mid = (ys[0] + ys[1]) / 2
        side = (ys[0] - ys[1]) / 2
        width = float(np.sqrt((side**2).mean()) / (np.sqrt((mid**2).mean()) + 1e-9))
    else:
        width = 0.0

    return {
        "rms_db": db(np.sqrt((y**2).mean())),
        "peak_db": db(np.abs(y).max()),
        "band_balance": balance,
        # 重心は中央値を主に使う。平均は「その楽器が鳴っていないフレーム」（無音・減衰の
        # 尻尾＝広帯域ノイズ）に引っ張られて高く出る（piano で中央値1036Hz に対し平均1480Hz）。
        "spectral_centroid_hz_median": round(float(np.median(librosa.feature.spectral_centroid(S=S, sr=sr))), 1),
        "spectral_centroid_hz_mean": round(float(librosa.feature.spectral_centroid(S=S, sr=sr).mean()), 1),
        "spectral_rolloff95_hz": round(float(librosa.feature.spectral_rolloff(S=S, sr=sr, roll_percent=0.95).mean()), 1),
        "harmonic_percussive_ratio": round(hp, 2),
        "stereo_width": round(width, 3),
        "rms_db_by_bar": per_bar,
        "listen_bars_loudest": loudest,
        "listen_bars_quiet": quietest,
        # そのステムが休んでいる区間に残る音の量。ピークとの差が大きいほど分離がきれい
        "floor_db": round(float(per_bar_arr.min()), 2),
        "floor_below_peak_db": round(float(per_bar_arr.min() - per_bar_arr.max()), 2),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("stemdir")
    ap.add_argument("mix")
    ap.add_argument("basics")
    ap.add_argument("outdir")
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    basics = json.loads(Path(args.basics).read_text())
    bpm = basics["tempo"]["bpm"]
    bar_len = 4 * 60.0 / bpm
    first_down = basics["grid"]["first_downbeat_sec"]
    outdir, stemdir = Path(args.outdir), Path(args.stemdir)

    mix, sr = librosa.load(args.mix, sr=SR, mono=True)
    n_bars = int((len(mix) / sr - first_down) / bar_len)

    stems = sorted(p for p in stemdir.glob("*.wav"))
    result = {"model": args.label or stemdir.parent.name, "n_bars": n_bars, "bar_len_sec": round(bar_len, 4), "stems": {}}
    for p in stems:
        result["stems"][p.stem] = analyze(p, first_down, bar_len, n_bars)

    # ステムの合計がミックスをどれだけ再現しているか（demucs は加算的なのでほぼ一致するはず。
    # 大きく残るなら読み込み・トリムの取り違えを疑う）
    acc = np.zeros_like(mix)
    for p in stems:
        y, _ = librosa.load(p, sr=SR, mono=True)
        acc[: len(y)] += y[: len(acc)]
    res = mix[: len(acc)] - acc[: len(mix)]
    result["reconstruction"] = {
        "mix_rms_db": db(np.sqrt((mix**2).mean())),
        "residual_rms_db": db(np.sqrt((res**2).mean())),
        "residual_below_mix_db": round(db(np.sqrt((res**2).mean())) - db(np.sqrt((mix**2).mean())), 2),
    }

    (outdir / f"stems{('-' + args.label) if args.label else ''}.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False)
    )

    fig, ax = plt.subplots(figsize=(16, 6))
    x = np.arange(1, n_bars + 1)
    for name, d in result["stems"].items():
        ax.plot(x, d["rms_db_by_bar"], lw=1.3, label=f"{name} ({d['rms_db']}dB)")
    ax.set_xlabel("bar")
    ax.set_ylabel("RMS (dBFS)")
    ax.set_ylim(-70, 0)
    ax.grid(alpha=0.3)
    ax.legend(ncol=3, fontsize=9)
    ax.set_title(f"stem energy by bar — {result['model']}")
    fig.tight_layout()
    fig.savefig(outdir / f"stems{('-' + args.label) if args.label else ''}.png", dpi=110)

    slim = json.loads(json.dumps(result))
    for d in slim["stems"].values():
        d.pop("rms_db_by_bar")
    print(json.dumps(slim, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
