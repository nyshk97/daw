#!/usr/bin/env python
"""おすすめ5の効きを個人ライブラリで検証するレポート（CI 対象外・人間が読む）。

検証基準（docs/plans/2026-08-07-2319-loop-track.md）:
  (a) 対照群（_contrast/ のジャンル外素材）がランキングの下位に沈む
  (b) リファレンスを替えると上位5本が入れ替わる

キー/BPM の足切りは通さない（skip_filters）— 検証したいのは類似度の質で、
足切りに救われた「実は鈍いランキング」を見逃さないため。

使い方: evaluate.py <リファレンスフォルダ...> [--library DIR]
"""
import argparse
import json
import sys
from itertools import combinations
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from index import DEFAULT_LIBRARY, SCHEMA_VERSION  # noqa: E402
from recommend import load_ref_meta, rank, upper_features  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("refdirs", nargs="+", type=Path)
    ap.add_argument("--library", type=Path, default=DEFAULT_LIBRARY)
    args = ap.parse_args()

    library = args.library.expanduser()
    index = json.loads((library / "index.json").read_text())
    if index.get("schema_version") != SCHEMA_VERSION:
        raise SystemExit("ERROR: index の版が合いません（lib:index --force で作り直し）")
    entries = index["entries"]
    n_contrast = sum(1 for e in entries if e.get("is_contrast"))
    n_real = sum(1 for e in entries if not e.get("is_contrast") and e.get("kind") == "loop")
    print(f"ライブラリ: 本命 {n_real}本 / 対照群 {n_contrast}本")
    if n_contrast == 0:
        print("注意: 対照群が空です。基準(a)は判定できません（_contrast/loops/ に投入を）")

    top5_by_ref: dict[str, list[str]] = {}
    for refdir in args.refdirs:
        refdir = refdir.expanduser()
        ref_meta = load_ref_meta(refdir)
        ref_features = upper_features(refdir)
        ranked = rank(entries, ref_meta, ref_features, include_contrast=True, skip_filters=True)
        if not ranked:
            print(f"\n== {refdir.name}: ループが1本もありません")
            continue

        print(f"\n== {refdir.name}（全 {len(ranked)}本を類似順に）")
        real_top5 = [c for c in ranked if not c["is_contrast"]][:5]
        top5_by_ref[refdir.name] = [c["path"] for c in real_top5]
        for i, c in enumerate(real_top5, 1):
            print(f"  {i}. {c['path']}  距離 {c['score']} — {c['reason']}")

        # 基準(a): 対照群の順位。中央値が下位側（percentile 0.5 超）に居るかを見る
        positions = [i for i, c in enumerate(ranked, 1) if c["is_contrast"]]
        if positions:
            positions.sort()
            median = positions[len(positions) // 2]
            pct = median / len(ranked)
            verdict = "沈んでいる" if pct > 0.6 else ("やや沈む" if pct > 0.5 else "沈んでいない — 特徴量を疑う")
            print(f"  対照群 {len(positions)}本: 最上位 {positions[0]}位 / 中央値 {median}位"
                  f"（下位 {round(pct * 100)}%地点）→ {verdict}")
            intruders = [c for c in ranked[:5] if c["is_contrast"]]
            for c in intruders:
                print(f"  ⚠ 対照群が top5 に侵入: {c['path']}（距離 {c['score']}）")

    # 基準(b): リファレンス間で上位5本が入れ替わるか
    if len(top5_by_ref) >= 2:
        print("\n== リファレンス間の top5 重複（少ないほど識別できている）")
        for (a, ta), (b, tb) in combinations(top5_by_ref.items(), 2):
            overlap = len(set(ta) & set(tb))
            note = "" if overlap <= 2 else " ← ほぼ同じ並び。特徴量がリファレンスを見ていない疑い"
            print(f"  {a} × {b}: {overlap}/5 重複{note}")


if __name__ == "__main__":
    main()
