#!/usr/bin/env python3
"""logs/ の /usr/bin/time -l 出力から速度・ピークメモリ表を作る。"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).parent


def parse(path: Path):
    """1ログ内の (real秒, 最大RSS GB) のリスト（demucsログは2モデル分で2エントリ）"""
    text = path.read_text(errors="replace")
    reals = [float(m) for m in re.findall(r"([\d.]+) real", text)]
    rss = [int(m) / 1e9 for m in re.findall(r"(\d+)  maximum resident set size", text)]
    return list(zip(reals, rss))


def main() -> int:
    rows = []
    for log in sorted(HERE.glob("logs/*/*.log")):
        song = log.parent.name
        name = log.stem  # 例: demucs4-run1 / sw-run1 / multi-vocal-run1
        for i, (real, gb) in enumerate(parse(log)):
            stage = name if len(parse(log)) == 1 else f"{name}#{i+1}"
            rows.append((song, stage, real, gb))
    print(f"{'song':<44} {'stage':<22} {'real(s)':>8} {'peakRSS(GB)':>12}")
    for song, stage, real, gb in rows:
        print(f"{song:<44} {stage:<22} {real:>8.1f} {gb:>12.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
