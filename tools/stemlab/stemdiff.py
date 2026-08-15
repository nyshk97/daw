#!/usr/bin/env python3
"""2つの分離runの同名ステムを突き合わせ、run間の揺れを数値化する。

labs 2026-08-03 の再現: 差のRMS(dBFS)・ステム比(差RMS - ステムRMS, dB)を出す。
使い方: python stemdiff.py <runA_dir> <runB_dir>
（各dirに同名の *.wav がある前提。長さが違えば短い方に切り揃える）
"""
import sys
import numpy as np
import soundfile as sf
from pathlib import Path


def rms_db(x: np.ndarray) -> float:
    r = float(np.sqrt(np.mean(np.square(x), dtype=np.float64)))
    return -np.inf if r <= 0 else 20 * np.log10(r)


def main() -> int:
    a_dir, b_dir = Path(sys.argv[1]), Path(sys.argv[2])
    names = sorted(p.name for p in a_dir.glob("*.wav"))
    if not names:
        print(f"no wav in {a_dir}", file=sys.stderr)
        return 1
    print(f"{'stem':<12} {'diffRMS(dB)':>12} {'stem比(dB)':>12} {'stemRMS(dB)':>12}")
    for name in names:
        b = b_dir / name
        if not b.exists():
            print(f"{name:<12} (B側に無し)")
            continue
        xa, sra = sf.read(a_dir / name, always_2d=True, dtype="float64")
        xb, srb = sf.read(b, always_2d=True, dtype="float64")
        assert sra == srb, f"SR不一致 {name}: {sra} vs {srb}"
        n = min(len(xa), len(xb))
        diff = xa[:n] - xb[:n]
        d, s = rms_db(diff), rms_db(xa[:n])
        ratio = d - s if np.isfinite(d) and np.isfinite(s) else float("-inf")
        print(f"{Path(name).stem:<12} {d:>12.1f} {ratio:>12.1f} {s:>12.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
