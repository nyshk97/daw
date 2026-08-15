#!/usr/bin/env python3
"""試聴区間の候補を既存demucsステムのRMS推移から提示する。

3種の区間（各8〜16小節）を選ぶための材料出し:
  ① ボーカルと上モノが重なる区間（vocals活発 かつ 上モノ活発）
  ② ボーカルなしの薄い区間（vocals非活発 かつ 全体RMS低め）
  ③ 編成が厚い区間（全体RMS上位）

使い方: python segpick.py <ref_dir> <bpm>
（ref_dir は track.wav と stems/htdemucs/track/, stems/htdemucs_6s/track/ を持つ）
4小節単位のグリッドで各窓のRMSを出し、条件に合う窓を印付きで一覧する。
最終的な区間は人間/AIが表を見て固定し、blind-map に記録する。
"""
import sys
import numpy as np
import soundfile as sf
from pathlib import Path


def window_rms_db(path: Path, win_sec: float) -> list:
    # ファイルごとにSRが違う（demucsステムは44.1k、trackは48k）ので窓は秒で切る
    x, sr = sf.read(path, always_2d=True, dtype="float32")
    mono = x.mean(axis=1)
    win = int(win_sec * sr)
    out = []
    for i in range(0, len(mono) - win + 1, win):
        w = mono[i : i + win]
        r = float(np.sqrt(np.mean(np.square(w, dtype=np.float64))))
        out.append(-120.0 if r <= 0 else 20 * float(np.log10(r)))
    return out, sr


def main() -> int:
    ref = Path(sys.argv[1])
    bpm = float(sys.argv[2])
    track = ref / "track.wav"
    info = sf.info(str(track))
    sr = info.samplerate
    bars4_sec = 4 * 4 * 60.0 / bpm  # 4小節（4/4前提）

    stems4 = ref / "stems" / "htdemucs" / "track"
    stems6 = ref / "stems" / "htdemucs_6s" / "track"
    mix_rms, _ = window_rms_db(track, bars4_sec)
    voc_rms, _ = window_rms_db(stems4 / "vocals.wav", bars4_sec)
    uppers = []
    for name in ("piano.wav", "guitar.wav", "other.wav"):
        p = stems6 / name
        if p.exists():
            r, _ = window_rms_db(p, bars4_sec)
            uppers.append(r)
    # ステム側はSR違い（demucsは44.1k出力）で窓数がずれるので最短に切り揃える
    n = min([len(mix_rms), len(voc_rms)] + [len(r) for r in uppers])
    mix_rms, voc_rms = mix_rms[:n], voc_rms[:n]
    upper = np.full(n, -120.0)
    for r in uppers:
        upper = np.maximum(upper, r[:n])

    mix_med = float(np.median(mix_rms))
    print(f"# {ref.name}  bpm={bpm}  4小節={bars4_sec:.1f}s  mix中央値={mix_med:.1f}dB")
    print(f"{'窓':>3} {'開始(s)':>8} {'mix':>7} {'vocals':>7} {'上モノ':>7}  候補")
    for i, (m, v, u) in enumerate(zip(mix_rms, voc_rms, upper)):
        t = i * bars4_sec
        tags = []
        if v > m - 18 and u > m - 18:
            tags.append("①vo+上モノ")
        if v < m - 30 and m < mix_med:
            tags.append("②vo無し薄")
        if m >= mix_med + 1.5:
            tags.append("③厚い")
        print(f"{i:>3} {t:>8.1f} {m:>7.1f} {v:>7.1f} {u:>7.1f}  {' '.join(tags)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
