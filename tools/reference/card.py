#!/usr/bin/env python
"""制約カード card.json — 分析結果のうち「ガチャの生成器が読む値」だけを1枚に厳選する。

analysis/*.json は分析器の都合で分かれていて、信用できない値・検算用の素点も混ざっている。
生成器にそのまま読ませると壊れた値を食う。ここで gates.json の判定を通し、
信頼できた値だけをパーツ別スライス（global / drums / bass / chords / arrangement）に転記する。

方針（docs/design/reference-beat.md）:
- コード進行そのものは載せない。借りるのは性格（ループ長・動きの少なさ・方向）だけ
- ゲート落ちの項目はスライス/フィールドごと省略し、meta.excluded に理由を残す。
  生成器は「無いなら自分のデフォルト」と解釈する
- 拍子は測定値でなく4/4前提（meta.assumptions）。現行分析は3拍子をサイレントに取り違える

出力: <ref>/card.json（atomic 書き込み。ゲート落ちで生成しないときは旧カードを削除する）

使い方: card.py <リファレンスフォルダ>
"""
import argparse
import json
import math
import os
import re
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

CARD_VERSION = 1
NOTE_PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def normalize_chord(label: str) -> tuple[int, str]:
    """コード名を（ルート音程クラス, 基本クオリティ）の骨格に落とす。

    テンション・sus・5 は maj 族に吸収する。コードの細部は chroma のにじみで
    同じ小節が maj9/maj7/sus4 に揺れることが実測で分かっており、骨格しか信用しない。
    """
    m = re.match(r"^([A-G])([#♯b♭]?)(.*)$", label)
    if not m:
        raise ValueError(f"コード名を読めない: {label}")
    pc = NOTE_PC[m.group(1)] + {"#": 1, "♯": 1, "b": -1, "♭": -1, "": 0}[m.group(2)]
    rest = m.group(3)
    if "dim" in rest or "ø" in rest:
        quality = "dim"
    elif rest.startswith("m") and not rest.startswith("maj"):
        quality = "min"
    else:
        quality = "maj"
    return pc % 12, quality


def chord_character(progression: list[dict]) -> dict:
    """畳んだ進行のスロット列から、進行の性格3値を導出する。

    - changes_per_loop: 骨格レベルで隣接比較（循環＝末尾→先頭を含む）した変化の回数
    - root_motion: 変化ごとのルート移動を半音数 (-6, +6] で取り（トライトーンは +6）、
      0（クオリティのみの変化）は方向の集計から除外。2/3 以上が同方向なら up/down、
      非ゼロの移動が無い・変化が1回以下なら static、それ以外は mixed
    """
    labels = [p["chord"] for p in progression if p.get("chord")]
    slots = [normalize_chord(c) for c in labels]
    has_7th = any(re.search(r"(7|9|11|13)", c) for c in labels)
    changes, moves = 0, []
    for i in range(len(slots)):
        a, b = slots[i], slots[(i + 1) % len(slots)]
        if a != b:
            changes += 1
            iv = (b[0] - a[0]) % 12
            moves.append(iv - 12 if iv > 6 else iv)
    nonzero = [m for m in moves if m != 0]
    up, down = sum(1 for m in nonzero if m > 0), sum(1 for m in nonzero if m < 0)
    if changes <= 1 or not nonzero:
        motion = "static"
    elif up >= 2 * len(nonzero) / 3:
        motion = "up"
    elif down >= 2 * len(nonzero) / 3:
        motion = "down"
    else:
        motion = "mixed"
    return {"has_7th_or_more": has_7th, "changes_per_loop": changes, "root_motion": motion}


def fold_kick_by_beat(profile_16th: list[float]) -> list[float]:
    """低域の16分プロファイルを拍単位に畳む（4スロットずつ max、最大=1.0 に正規化）。

    低域は帯域通過の時間分解能が原理的に足りず、16分単位の位置は1〜2個ぶん滲む。
    拍単位までは信用できる（README「設計上の注意」）。
    """
    folded = [max(profile_16th[i * 4 : (i + 1) * 4]) for i in range(4)]
    top = max(folded) or 1.0
    return [round(v / top, 3) for v in folded]


def parse_key(value: str) -> dict:
    root, mode = value.rsplit(" ", 1)
    return {"root": root, "mode": mode}


def build_card(name: str, gates: dict, groove: dict, arrangement: dict, topline: dict | None) -> dict:
    """カード本体を組み立てる。ゲート落ちは省略して meta.excluded に記録する。"""
    excluded: list[dict] = []

    def drop(path: str, gate: str, reason: str) -> None:
        excluded.append({"path": path, "gate": gate, "reason": reason})

    downbeat_ok = gates["downbeat"]["ok"]

    card: dict = {
        "card_version": CARD_VERSION,
        "meta": {
            "reference": name,
            "generated_at": datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds"),
            "assumptions": {"time_signature": "4/4"},
            "excluded": excluded,
        },
        "global": {"bpm": gates["bpm"]["value"]},
    }

    if gates["key"]["confidence"] != "低":
        card["global"]["key"] = parse_key(gates["key"]["value"])
    else:
        drop("global.key", "key.confidence", "ベースの音名分布でキーが決まらない曲")

    # --- drums ---
    drums: dict = {}
    if gates["swing"]["ok"]:
        drums["swing_ratio"] = round(groove["swing"]["ratio"], 3)
    else:
        drop("drums.swing_ratio", "swing.ok", "ハットが無く高域が別の音を拾っている")
    devs = [abs(v) for band in ("mid", "high") for v in groove["drums"][band]["dev_ms_by_16th"] if v is not None]
    if devs:
        drums["quantize_dev_ms"] = round(sum(devs) / len(devs), 1)
    else:
        drop("drums.quantize_dev_ms", "n/a", "mid/high の dev_ms_by_16th が全て None")
    if downbeat_ok:
        drums["kick_profile_by_beat"] = fold_kick_by_beat(groove["drums"]["low"]["profile_by_16th"])
        drums["snare_profile_by_16th"] = [round(v, 3) for v in groove["drums"]["mid"]["profile_by_16th"]]
        if gates["swing"]["ok"]:
            drums["hat_profile_by_16th"] = [round(v, 3) for v in groove["drums"]["high"]["profile_by_16th"]]
        else:
            drop("drums.hat_profile_by_16th", "swing.ok", "スウィングと同じ壊れた高域を見ている")
    else:
        for f in ("kick_profile_by_beat", "snare_profile_by_16th", "hat_profile_by_16th"):
            drop(f"drums.{f}", "downbeat.ok", "小節頭の位相が取れておらず小節内の位置が無意味")
    card["drums"] = drums

    # --- bass ---
    gb = groove["bass"]
    bass: dict = {
        "notes_per_bar": round(gb["notes_per_bar"], 2),
        "core_range_semitones": gb["core_range_semitones"],
        "pitch_center": gb["pitch_median"],
        "note_length_16ths": round(gb["dur_median_in_16ths"], 2),
        "quantize_dev_ms": round(gb["dev_ms_abs_mean"], 1),
    }
    if downbeat_ok:
        bass["onset_rate_by_16th"] = [round(v, 3) for v in gb["onset_rate_by_16th"]]
    else:
        drop("bass.onset_rate_by_16th", "downbeat.ok", "小節頭の位相が取れておらず小節内の位置が無意味")
    card["bass"] = bass

    # --- chords（進行そのものは載せない。性格だけ） ---
    if not gates["harmony"]["ok"]:
        drop("chords", "harmony.ok", "上モノがほぼ無く、6分割の全ステムがかぶりノイズしか見ていない")
    elif not downbeat_ok:
        drop("chords", "downbeat.ok", "スロットの区切りが小節グリッドに依存する")
    else:
        prog = topline["loop_progression"]
        card["chords"] = {"loop_bars": prog["loop_bars"], **chord_character(prog["progression"])}

    # --- arrangement ---
    arr: dict = {"n_bars": arrangement["n_bars"]}
    arr["instrumentation"] = sorted({s for sec in arrangement["sections"] for s in sec["active"]})
    if downbeat_ok:
        arr["sections"] = [
            {"bars": [s["bar_start"], s["bar_end"]], "active": sorted(s["active"]), "rms_db": round(s["rms_db_mean"], 1)}
            for s in arrangement["sections"]
        ]
    else:
        drop("arrangement.sections", "downbeat.ok", "小節番号が信用できない")
    card["arrangement"] = arr

    return card


# スキーマ上の型（存在すれば検査。省略は excluded 側で検査する）。
# bool は int のサブクラスなので isinstance でなく type で厳密に見る
NUM = (int, float)
FIELD_TYPES = {
    "card_version": (int,),
    "meta.reference": (str,),
    "meta.generated_at": (str,),
    "global.bpm": NUM,
    "global.key.root": (str,),
    "global.key.mode": (str,),
    "drums.swing_ratio": NUM,
    "drums.quantize_dev_ms": NUM,
    "bass.notes_per_bar": NUM,
    "bass.core_range_semitones": NUM,
    "bass.pitch_center": (str,),
    "bass.note_length_16ths": NUM,
    "bass.quantize_dev_ms": NUM,
    "chords.loop_bars": (int,),
    "chords.has_7th_or_more": (bool,),
    "chords.changes_per_loop": (int,),
    "chords.root_motion": (str,),
    "arrangement.n_bars": (int,),
}
PROFILE_LENGTHS = {
    "drums.kick_profile_by_beat": 4,
    "drums.snare_profile_by_16th": 16,
    "drums.hat_profile_by_16th": 16,
    "bass.onset_rate_by_16th": 16,
}


# 「フィールドが無い」（＝ゲート落ちの省略。正当）と「値が None」（＝バグ）を区別する sentinel
MISSING = object()


def get_path(card: dict, path: str):
    node = card
    for part in path.split("."):
        if not (isinstance(node, dict) and part in node):
            return MISSING
        node = node[part]
    return node


def validate(card: dict) -> list[str]:
    """自分の出力を検算する。壊れたカードを黙って公開しない（「何かしら返して申告しない」を作らない）。"""
    errors: list[str] = []

    def walk(v, path: str) -> None:
        if isinstance(v, dict):
            for k, x in v.items():
                walk(x, f"{path}.{k}" if path else k)
        elif isinstance(v, list):
            for i, x in enumerate(v):
                walk(x, f"{path}[{i}]")
        elif isinstance(v, float) and not math.isfinite(v):
            errors.append(f"{path} が有限値でない: {v}")

    walk(card, "")
    for path, types in FIELD_TYPES.items():
        v = get_path(card, path)
        if v is not MISSING and type(v) not in types:
            errors.append(f"{path} の型が不正: {v!r}")
    if (v := get_path(card, "chords.root_motion")) is not MISSING and v not in ("static", "up", "down", "mixed"):
        errors.append(f"chords.root_motion の値が不正: {v!r}")
    for path, n in PROFILE_LENGTHS.items():
        v = get_path(card, path)
        if v is MISSING:
            continue
        if not isinstance(v, list) or len(v) != n:
            errors.append(f"{path} が {n} 件のリストでない: {v!r}")
        elif any(type(x) not in NUM for x in v):
            errors.append(f"{path} に数値でない要素がある")
    if (v := get_path(card, "arrangement.instrumentation")) is not MISSING and (
        not isinstance(v, list) or any(type(x) is not str for x in v)
    ):
        errors.append(f"arrangement.instrumentation が文字列のリストでない: {v!r}")
    if (secs := get_path(card, "arrangement.sections")) is not MISSING:
        for i, s in enumerate(secs):
            bars, active, rms = s.get("bars"), s.get("active"), s.get("rms_db")
            if not (isinstance(bars, list) and len(bars) == 2 and all(type(b) is int for b in bars)):
                errors.append(f"sections[{i}].bars が int 2件のリストでない: {bars!r}")
            if not (isinstance(active, list) and all(type(x) is str for x in active)):
                errors.append(f"sections[{i}].active が文字列のリストでない: {active!r}")
            if type(rms) not in NUM:
                errors.append(f"sections[{i}].rms_db が数値でない: {rms!r}")
    for e in card["meta"]["excluded"]:
        if set(e) != {"path", "gate", "reason"} or not all(isinstance(x, str) for x in e.values()):
            errors.append(f"excluded の形式が不正: {e}")
            continue
        node, ok = card, True
        for part in e["path"].split("."):
            if not (isinstance(node, dict) and part in node):
                ok = False
                break
            node = node[part]
        if ok:
            errors.append(f"excluded の {e['path']} が本体にも存在する")
    return errors


def generate(ref: Path) -> int:
    a = ref / "analysis"
    gates = json.loads((a / "gates.json").read_text())
    out = ref / "card.json"

    # グリッド系のゲートが落ちていたら全分析が無意味なのでカードは作らない。
    # 旧カードが正常品の顔で残らないよう消す。想定内の結果なので exit 0。
    if not gates["bpm"]["octave_ok"] or not gates["tempo_stable"]["ok"]:
        reason = "BPMのオクターブ" if not gates["bpm"]["octave_ok"] else "テンポの安定"
        out.unlink(missing_ok=True)
        print(f"カードを生成しない（{reason}のゲートが落ちている＝グリッドごと信用できない）。既存の card.json は削除した")
        return 0

    groove = json.loads((a / "groove.json").read_text())
    arrangement = json.loads((a / "arrangement.json").read_text())
    topline = None
    if gates["harmony"]["ok"]:
        topline = json.loads((a / f"topline-{gates['harmony']['source_stem']}.json").read_text())

    card = build_card(ref.name, gates, groove, arrangement, topline)
    errors = validate(card)
    if errors:
        print("ERROR: カードが検算に落ちた（card.py のバグ。既存の card.json には触っていない）", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    # 一時ファイル→os.replace で、不正・書きかけのファイルを一瞬も公開しない
    fd, tmp = tempfile.mkstemp(dir=ref, prefix=".card-", suffix=".json")
    with os.fdopen(fd, "w") as f:
        f.write(json.dumps(card, indent=2, ensure_ascii=False) + "\n")
    os.replace(tmp, out)

    slices = [k for k in ("global", "drums", "bass", "chords", "arrangement") if k in card]
    print(f"card.json を書いた: {', '.join(slices)}" + (f" / 省略 {len(card['meta']['excluded'])}件" if card["meta"]["excluded"] else ""))
    for e in card["meta"]["excluded"]:
        print(f"  [省略] {e['path']} ({e['gate']}): {e['reason']}")
    return 0


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("ref", help="リファレンスフォルダ（analysis/ がある場所）")
    args = ap.parse_args()
    sys.exit(generate(Path(args.ref).expanduser()))


if __name__ == "__main__":
    main()
