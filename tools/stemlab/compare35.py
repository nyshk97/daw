#!/usr/bin/env python3
"""Phase 3.5: scratch分析（SW配置）と既存分析（demucs）を突き合わせる。

- uppers: harmonyゲートのループ長・usable（正解があるのはループ長のみ）
- lowend: bassの音名分布・音域、drumsの16分プロファイル・swing、HPR（正解の無い数値は変化量表示）
出力: scratch/comparison.md
"""
import json
from pathlib import Path

HERE = Path(__file__).parent
REFS = Path.home() / "Music/daw/references"
SONGS = {
    "rau-def-freeze": "rau",
    "rip-slyme-楽園ベイベー": "rip",
    "stuts-sikk-o-鈴木真海子-summer-situation": "stuts",
}

out = ["# Phase 3.5 reference回帰比較（SW vs 既存demucs）", "",
       "枠名≠中身に注意: scratchの中身は PLACEMENT.md 参照（uppersは6s枠=SW、lowendは4s枠=SW＋other4加算）。",
       "正解ラベルの無い数値は変化量のみ（改善とは呼ばない）。", ""]

for song, short in SONGS.items():
    orig = json.loads((REFS / song / "analysis/gates.json").read_text())
    out += [f"## {song}", ""]

    # --- uppers: harmony
    sg = json.loads((HERE / f"scratch/{short}-uppers/analysis/gates.json").read_text())
    out += ["### 6s枠（uppers: SW配置）harmonyゲート", "",
            "| stem | demucs既存 usable/loop | SW usable/loop |", "|---|---|---|"]
    o_stems = {x["stem"]: x for x in orig["harmony"]["stems"]}
    s_stems = {x["stem"]: x for x in sg["harmony"]["stems"]}
    for stem in sorted(set(o_stems) | set(s_stems)):
        o = o_stems.get(stem)
        s = s_stems.get(stem)
        f = lambda x: f"{x['usable']}/{x['loop']}" if x else "—"
        out.append(f"| {stem} | {f(o)} | {f(s)} |")
    out += ["",
            f"harmony.ok: 既存={orig['harmony']['ok']} → SW={sg['harmony']['ok']} / "
            f"source: 既存={orig['harmony']['source_stem']} → SW={sg['harmony']['source_stem']}", ""]

    # --- lowend: bass/drums
    lg = json.loads((HERE / f"scratch/{short}-lowend/analysis/groove.json").read_text())
    og = json.loads((REFS / song / "analysis/groove.json").read_text())
    ob, sb = og["bass"], lg["bass"]
    NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

    def topnotes(b, n=7):
        w = b.get("pitch_class_weight") or []
        pairs = list(zip(NOTE_NAMES, w))
        return sorted(pairs, key=lambda kv: -kv[1])[:n]
    out += ["### 4s枠（lowend: SW bass/drums/vocals＋other4加算）", "",
            f"- bass音名上位: 既存={[(k, round(v,3)) for k,v in topnotes(ob)]}",
            f"- bass音名上位: SW  ={[(k, round(v,3)) for k,v in topnotes(sb)]}",
            f"- bass音域: 既存 {ob['pitch_min']}〜{ob['pitch_max']} (p5-95 {ob['pitch_p5']}〜{ob['pitch_p95']}, core {ob['core_range_semitones']}半音)"
            f" → SW {sb['pitch_min']}〜{sb['pitch_max']} (p5-95 {sb['pitch_p5']}〜{sb['pitch_p95']}, core {sb['core_range_semitones']}半音)",
            f"- bass音数/小節: 既存 {ob['notes_per_bar']} → SW {sb['notes_per_bar']}（変化量のみ）",
            f"- bassグリッドずれ: 既存 |dev|平均 {ob['dev_ms_abs_mean']}ms → SW {sb['dev_ms_abs_mean']}ms（変化量のみ）"]
    for band in ("low", "mid"):
        op = og["drums"][band]
        sp = lg["drums"][band]
        if isinstance(op, dict):
            op, sp = op.get("profile_by_16th"), sp.get("profile_by_16th")
        if isinstance(op, list):
            out.append(f"- drums 16分({band}): 既存={[round(x, 2) for x in op[:16]]}")
            out.append(f"- drums 16分({band}): SW  ={[round(x, 2) for x in sp[:16]]}")
    out += [f"- swing: 既存 {og['swing']} → SW {lg['swing']}",
            f"- downbeatゲート: 既存 ok={orig['downbeat']['ok']} backbeat={orig['downbeat'].get('backbeat_strength')}"
            f" → SW ok={json.loads((HERE / f'scratch/{short}-lowend/analysis/gates.json').read_text())['downbeat']['ok']}"
            f" backbeat={json.loads((HERE / f'scratch/{short}-lowend/analysis/gates.json').read_text())['downbeat'].get('backbeat_strength')}", ""]

    # HPR（設計指標: 4s bassの調波打楽器比）
    o4 = json.loads((REFS / song / "analysis/stems-4s.json").read_text())["stems"]["bass"]
    s4 = json.loads((HERE / f"scratch/{short}-lowend/analysis/stems-4s.json").read_text())["stems"]["bass"]
    out += [f"- bass HPR(調波打楽器比): 既存 {o4['harmonic_percussive_ratio']} → SW {s4['harmonic_percussive_ratio']}（大きいほど打楽器成分が少ない）", ""]

(HERE / "scratch/comparison.md").write_text("\n".join(out))
print("scratch/comparison.md 生成")
