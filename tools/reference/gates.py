#!/usr/bin/env python
"""分析結果の「どこまで信じてよいか」を機械的な判定にまとめる。

この分析器は、入力が想定外でも何かしらの数値を返し、壊れたことを自分から申告しない。
判定条件は README「既知の壊れ方」と report-template.md の閾値表に散らばっているが、
**読む側に判定させると再検証の迷路に入る**（レポート生成をAIに投げたとき、
octave_check を読めば済むところを DSP から再導出しにいって28ターン迷走した）。
判定はここで1回やって、結果だけを渡す。

出力: analysis/gates.json

使い方: gates.py <analysisフォルダ>
"""
import argparse
import json
from pathlib import Path


def load(a: Path, name: str):
    p = a / f"{name}.json"
    return json.loads(p.read_text()) if p.exists() else None


# コードを読んでよいステムは6分割の上モノだけ。4分割 other は1本きりで合議できず、
# 実際にループ長を 8小節 と誤答した実績がある。usable の閾値(-25dB)も6分割の実測で
# 引いた線なので、4分割に当てはめない。
HARMONY_STEMS = ("6s-piano", "6s-guitar", "6s-other")


def bpm_octave_ok(octave_check: dict, value) -> bool:
    """採用BPMが、事前分布込みで最大の重みを持つオクターブ候補と同じオクターブか。

    数値の厳密一致では判定できない: octave_check の梯子は乗り換え前の基準BPMから
    作られ、乗り換え時は候補の近傍±0.3BPMで精密化し直すため、採用値は梯子の値から
    最大0.3ずれる（実測: うそさ で梯子 double=137.98 vs 採用 138.0。旧判定 <0.01 が
    差0.02 を「取り違え」と誤判定した。GOAT は差0.002 で偶然通っていた）。
    オクターブ違いは ×1.5/×2 離れているので、比率5%以内なら同一オクターブと言い切れる。
    """
    if not octave_check or not value:
        return False
    best = max(octave_check.values(), key=lambda d: d["weighted"])
    return abs(best["bpm"] - value) / value < 0.05


def tempo_stable_ok(gc: list) -> bool:
    """剛体グリッドが曲全体で成立しているか。

    主判定は min(gc) > 1.0（全区間で表拍がオフビート位置に勝つ）。実戦根拠は
    docs/labs/reference-beat.md 2026-08-04: ライブ演奏の実テンポ揺れ（Jinmenusagi - GOAT
    ライブ映像。ドラムは全区間均一に鳴っているのにグリッドが合わない、と切り分け済み）は
    **後半減衰も local_bpm_std の増大（0.64 と小さい）も出さず**、中盤の対比 <1.0 だけが
    捕まえた。「揺れ＝後半だけ落ちる/std大」という直感で min を緩めると、この既知の
    揺れ曲を通してしまう（一度やりかけたレビュー指摘済みの誤り）。後半減衰チェックは補助。

    local_bpm_std は判定に使わない（一度 std<2.0 ガードを足して撤回した）:
    std はビートトラッカー由来で、トラッカーがグリッドの×1.5（付点/三連の読み）に
    ロックすると健全な打ち込み曲でも膨らむ（実測: うそさ で grid_contrast 全区間>1.0 なのに
    トラッカーが 92.3BPM に迷い std=2.13）。グリッド妥当性の直接証拠は grid_contrast だけ。
    """
    if not gc:
        return False
    if min(gc) <= 1.0:  # どこかの区間で表がオフビート位置に負ける＝その区間のグリッドが信用できない
        return False
    if len(gc) >= 2:
        first, last = gc[: len(gc) // 2], gc[len(gc) // 2 :]
        if sum(last) / len(last) <= sum(first) / len(first) * 0.6:  # 後半減衰＝ドリフト
            return False
    return True


def pick_harmony_source(tops: list[dict]) -> str | None:
    """usable かつ進行が抽出できた6分割ステムのうち一番大きいものを返す。無ければ None。

    usable=None（chord_estimate_usable が無い古い分析）は false 扱い。
    進行が空（曲が短くループ2周ぶん取れない）のステムも候補にしない — usable だけ見て
    選ぶと、下流（card.py / excerpts.py）が空の loop_progression を読んで落ちる。
    複数ステムの進行が食い違うときの合議は入れていない（v1 は最大音量で決める。
    性格は骨格レベルまで落として読むので揺れが小さい。必要になったら Phase 2 の生成器の要求で決める）。
    """
    # any() で「実コードが1つ以上」を見る（リストの truthiness だと [None, None] が通り、
    # コードを1つも読めていないのに changes=0/static のカードができる）
    cands = [t for t in tops if t["stem"] in HARMONY_STEMS and t.get("usable") and any(t.get("progression") or [])]
    return max(cands, key=lambda t: t["rms_below_mix_db"])["stem"] if cands else None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("analysis")
    args = ap.parse_args()
    a = Path(args.analysis)

    basics, groove = load(a, "basics"), load(a, "groove")
    gates: dict = {}

    # --- BPM のオクターブ ---
    # 素点は常に遅い方を好むので、事前分布で重み付けした weighted が採用値と同オクターブかを見る。
    oc = basics["tempo"].get("octave_check", {})
    gates["bpm"] = {
        "value": basics["tempo"]["bpm"],
        "octave_ok": bpm_octave_ok(oc, basics["tempo"]["bpm"]),
        "octave_check": {k: v["weighted"] for k, v in oc.items()},
        "note": "false なら倍/半分を取り違えている。グリッドが違うと以降の全分析が無意味",
    }

    # --- テンポが一定か ---
    gc = basics["tempo"].get("grid_contrast_by_eighth") or []
    gates["tempo_stable"] = {
        "ok": tempo_stable_ok(gc),
        "grid_contrast_by_eighth": gc,
        "local_bpm_std": basics["tempo"].get("local_bpm_std"),
        "note": "false ならどこかの区間で剛体グリッドが成立していない（後半とは限らない）。その区間以降の小節番号は信用できない",
    }

    # --- 小節頭（2拍・4拍にバックビートが立っているか） ---
    mid = groove["drums"]["mid"]["profile_by_16th"]
    back = (mid[4] + mid[12]) / 2
    other = sum(v for i, v in enumerate(mid) if i not in (0, 4, 8, 12)) / 12
    gates["downbeat"] = {
        "ok": bool(back > other * 1.5),
        "backbeat_strength": round(back, 3),
        "offbeat_mean": round(other, 3),
        "note": "2拍・4拍（16分の index 4 と 12）が他より立っていれば位相は正しい",
    }

    # --- スウィング ---
    sw = groove.get("swing") or {}
    gates["swing"] = {
        "ok": bool(sw.get("plausible")),
        "ratio": sw.get("ratio"),
        "note": "false ならハットが無く高域が別の音を拾っている。「ハネ無し」と書かない",
    }

    # --- キー（ベースの音名分布で裏取り） ---
    # 「上位7音と8番目の比」ではなく「上位7音が全体の何割か」で見る。
    # 音階外の音も1つくらいは鳴る（実測: Db major の曲で Cb が8位に 0.045 あった）ので、
    # 隣接する2つの比だと本来の差を捉え損ねる。
    w = sorted(groove["bass"]["pitch_class_weight"], reverse=True)
    mass = sum(w[:7]) / max(sum(w), 1e-9)
    gates["key"] = {
        "value": basics["key"]["top5"][0]["key"],
        "confidence": "高" if mass > 0.90 else ("中" if mass > 0.80 else "低"),
        "top7_mass": round(mass, 3),
        "top5": [k["key"] for k in basics["key"]["top5"]],
        "note": "ベースの音名の上位7音と8番目に差があるほど確か。差が小さいとキーが決まらない曲",
    }

    # --- 上モノ（コード進行を読んでよいか） ---
    tops = []
    for p in sorted(a.glob("topline-*.json")):
        d = json.loads(p.read_text())
        tops.append(
            {
                "stem": d["stem"],
                "usable": d.get("chord_estimate_usable"),
                "rms_below_mix_db": d.get("stem_rms_below_mix_db"),
                "loop": d.get("loop", {}).get("most_likely"),
                "progression": [x["chord"] for x in d.get("loop_progression", {}).get("progression", [])],
            }
        )
    source = pick_harmony_source(tops)
    gates["harmony"] = {
        "ok": source is not None,
        "source_stem": source,
        "stems": tops,
        "note": "ok/source は6分割の上モノだけで判定（4分割 other は合議できない）。"
        "6分割が全 false なら上モノがほぼ無い曲。コード進行の節は書かず、何で構成されているかを書く",
    }

    failed = [k for k, v in gates.items() if isinstance(v, dict) and v.get("ok") is False or v.get("octave_ok") is False]
    gates["_summary"] = {
        "all_ok": not failed,
        "failed": failed,
        "note": "failed に入った項目は数値を書かず『測れなかった』と書く。bpm が入っていたらレポートを書かない",
    }

    (a / "gates.json").write_text(json.dumps(gates, indent=2, ensure_ascii=False))
    for k, v in gates.items():
        if k == "_summary":
            continue
        mark = v.get("ok", v.get("octave_ok"))
        state = "OK " if mark else ("NG " if mark is False else "-  ")
        extra = v.get("confidence") or v.get("source_stem") or v.get("value") or ""
        print(f"  [{state}] {k:14s} {extra}")
    print(f"  => {'全部OK' if gates['_summary']['all_ok'] else '要注意: ' + ', '.join(gates['_summary']['failed'])}")


if __name__ == "__main__":
    main()
