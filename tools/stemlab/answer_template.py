#!/usr/bin/env python3
"""Phase 3 の回答テンプレートを listen/ の実在ファイルから生成する。

- 全採点行を0（同等）で事前充填 → 人間は差を感じた行だけ書き換える
- 尺度: -2 悪い / -1 少し悪い / 0 同等 / +1 少し良い / +2 良い（YはXよりどうか）
- 対応表は見ずに埋める。activity判定でN/Aの軸は「NA」と記入済み
出力: docs/labs/reference-beat-human-answers/2026-08-15-blind-answers.md
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).parent
LISTEN = HERE / "listen"
MAP = HERE / "../../docs/labs/reference-beat-human-answers/2026-08-15-blind-map.json"
OUT = HERE / "../../docs/labs/reference-beat-human-answers/2026-08-15-blind-answers.md"

AXES_STEM = ["漏れ", "欠落", "アーティファクト", "アタック"]


def main() -> int:
    m = json.loads(MAP.read_text())
    lines = [
        "# ブラインド試聴 回答（2026-08-15）",
        "",
        "採点: **YはXよりどうか** を -2(悪い) / -1(少し悪い) / 0(同等) / +1(少し良い) / +2(良い)。",
        "差を感じた行だけ0を書き換える。NA行は触らない。ファイルは `tools/stemlab/listen/` 以下。",
        "軸: 漏れ=他楽器の漏れ / 欠落=本来の音の欠落 / アーティファクト=金属的な揺れ・水中感 / アタック=アタックの崩れ / 再加算=remixのX,Yをoriginalと比べた差",
        "",
    ]
    cur_song = None
    for e in m["entries"]:
        if e["song"] != cur_song:
            cur_song = e["song"]
            lines += [f"## {cur_song}", ""]
        seg, pair = e["segment"], e["pair"]
        lines.append(f"### {seg} / {pair}")
        lines.append("")
        lines.append("| ステム | " + " | ".join(AXES_STEM) + " |")
        lines.append("|---|" + "---|" * len(AXES_STEM))
        for stem, info in e["stems"].items():
            cells = []
            for ax in AXES_STEM:
                na = ax in ("欠落", "アタック") and info.get("na_axes")
                cells.append("NA" if na else "0")
            lines.append(f"| {stem} | " + " | ".join(cells) + " |")
        lines.append(f"| remix(再加算) | 0 | — | — | — |")
        lines.append("")
        lines.append("メモ（任意・差の内容を一言）: ")
        lines.append("")
    OUT.write_text("\n".join(lines))
    print(f"回答テンプレート: {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
