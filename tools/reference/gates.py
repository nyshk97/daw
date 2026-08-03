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


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("analysis")
    args = ap.parse_args()
    a = Path(args.analysis)

    basics, groove = load(a, "basics"), load(a, "groove")
    gates: dict = {}

    # --- BPM のオクターブ ---
    # 素点は常に遅い方を好むので、事前分布で重み付けした weighted が採用値(x1)で最大かを見る。
    oc = basics["tempo"].get("octave_check", {})
    best = max(oc.values(), key=lambda d: d["weighted"]) if oc else None
    gates["bpm"] = {
        "value": basics["tempo"]["bpm"],
        "octave_ok": bool(best and abs(best["bpm"] - basics["tempo"]["bpm"]) < 0.01),
        "octave_check": {k: v["weighted"] for k, v in oc.items()},
        "note": "false なら倍/半分を取り違えている。グリッドが違うと以降の全分析が無意味",
    }

    # --- テンポが一定か ---
    gc = basics["tempo"].get("grid_contrast_by_eighth") or []
    first, last = (gc[: len(gc) // 2], gc[len(gc) // 2 :]) if gc else ([], [])
    gates["tempo_stable"] = {
        "ok": bool(gc and min(gc) > 1.0 and (sum(last) / len(last)) > (sum(first) / len(first)) * 0.6),
        "grid_contrast_by_eighth": gc,
        "local_bpm_std": basics["tempo"].get("local_bpm_std"),
        "note": "false ならテンポが揺れており、後半ほど小節番号が信用できない",
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
    usable = [t for t in tops if t["usable"]]
    gates["harmony"] = {
        "ok": bool(usable),
        "source_stem": max(usable, key=lambda t: t["rms_below_mix_db"])["stem"] if usable else None,
        "stems": tops,
        "note": "全ステム false なら上モノがほぼ無い曲。コード進行の節は書かず、何で構成されているかを書く",
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
