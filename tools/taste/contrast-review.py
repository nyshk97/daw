#!/usr/bin/env python
"""対照分析の耳確認素材を作る。

Round B: 機械が分離したと言う軸ごとに、正例代表と否定例代表のA/B
Round C: その軸の値が中間にある曲。境界がどこに引かれるかを耳で確かめる
Round D: omnibus検定を通ったviewを、view全体と強い軸で聴き分ける
Round E: 軸の値とroleを食い違わせた交差ペア。haloか本当の軸かを判定する

否定例そのものの妥当性を聴き直すroundは作らない。否定例として選んだ曲を「否定例として妥当か」と
聞き直すだけで、対照曲は元がインストなので素材が元音源とほぼ同じになり、選定時点の情報を超えない。

全クリップを同じRMSへ揃える。音圧差そのものを「好みの差」と誤認させないため。
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import shutil
import sys
from pathlib import Path
from typing import Any

import numpy as np

from common import DEFAULT_CORPUS_ROOT, REPO_ROOT, load_json

_review_spec = importlib.util.spec_from_file_location("taste_review", Path(__file__).with_name("review.py"))
REVIEW = importlib.util.module_from_spec(_review_spec)
_review_spec.loader.exec_module(REVIEW)

_contrast_spec = importlib.util.spec_from_file_location("taste_contrast", Path(__file__).with_name("contrast.py"))
CONTRAST = importlib.util.module_from_spec(_contrast_spec)
_contrast_spec.loader.exec_module(CONTRAST)

import soundfile as sf

TARGET_RMS_DB = -18.0
# viewごとに、その軸を判断できる最小のステム構成。
VIEW_KIND = {
    "topline_harmony": "top",
    "drum_placement": "drums",
    "drum_audio": "drums",
    "bass_harmony": "bass",
    "arrangement": "beat",
    "vocal_space": "beat",
}


def _write(path: Path, audio: np.ndarray, sr: int = 44100) -> dict[str, float]:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), REVIEW._normalize(audio, TARGET_RMS_DB), sr, subtype="PCM_16")
    return REVIEW.inspect_wav(path)


def _arrangement_digest(manifest: dict, songs: dict, vid: str) -> tuple[np.ndarray, int] | None:
    """構成の軸は8小節の窓では聴けない。セクション境界の前後を並べたダイジェストを作る。

    「ドラムが鳴っている時間の割合」は曲全体の性質なので、代表区間を1つ切り出しても
    その軸を判断できない。境界の前後1小節を時系列順に並べ、0.5秒の無音で区切る。
    """
    entry = manifest["songs"][vid]
    sections = songs[vid]["views"].get("arrangement", {}).get("sections") or []
    if not sections:
        return None
    ref = Path(entry["active_artifact_path"])
    bar_s = float(entry["grid"]["bar_duration_s"])
    first = float(entry["grid"]["first_downbeat_s"])
    pieces, sr = [], 44100
    for section in sections[:10]:
        boundary = first + (section["bar_start"] - 1) * bar_s
        piece, sr = REVIEW._sum(REVIEW._paths(ref, "beat"), max(0.0, boundary - bar_s), boundary + bar_s)
        pieces += [piece, np.zeros((int(sr * 0.5), piece.shape[1]))]
    if not pieces:
        return None
    return np.concatenate(pieces[:-1]), sr


def _window(song: dict[str, Any], view: str, seconds: float | None = None) -> tuple[float, float] | None:
    data = song.get("views", {}).get(view, {})
    segments = [s for s in data.get("segments", []) if s.get("eligible")]
    if not segments:
        return None
    seg = next((s for s in segments if s.get("slot") == "representative"), segments[0])
    start, end = float(seg["start_s"]), float(seg["end_s"])
    if seconds and end - start > seconds:
        end = start + seconds
    return start, end


def _axis_view(axis: str) -> str:
    return axis.split(".", 1)[0]


def _axis_values(axis: str, songs: dict, draft: dict, roles: dict) -> dict[str, float]:
    view = _axis_view(axis)
    rest = axis.split(".", 1)[1]
    ids = draft["views"][view]["participants"]
    positives = [v for v in ids if roles.get(v) == "positive"]
    if rest in CONTRAST.VIEW_SCHEMAS[view]["weights"]:
        # 群の軸は距離表しか無いので、試聴例を選ぶためだけに「正例群への平均距離」で並べる。
        # 統計はrole非依存の距離表側で済んでおり、ここはどれを聴かせるかの選択にしか使わない。
        usable, matrix = CONTRAST.build_group_distance(view, songs, ids, rest)
        index = {vid: i for i, vid in enumerate(usable)}
        anchors = [v for v in positives if v in index]
        if len(anchors) < 3:
            return {}
        return {
            vid: float(np.mean([matrix[index[vid], index[a]] for a in anchors if a != vid]))
            for vid in usable
        }
    return {vid: f[rest] for vid in ids if rest in (f := CONTRAST.scalar_features(view, songs[vid]))}


def _material(manifest: dict, songs: dict, vid: str, view: str, kind: str) -> tuple[np.ndarray, int] | None:
    if view == "arrangement":
        return _arrangement_digest(manifest, songs, vid)
    window = _window(songs[vid], view)
    if not window:
        return None
    ref = Path(manifest["songs"][vid]["active_artifact_path"])
    return REVIEW._sum(REVIEW._paths(ref, kind), *window)


# 単体ステムのA/Bは「複雑な方が面白く聞こえる」に引きずられる。
# 同じ窓を文脈つきでも出して、単体での印象が全体でも残るかを確かめる。
CONTEXT_KINDS = {
    "bass_harmony": ["top-bass", "beat"],
    "drum_audio": ["beat"],
    "drum_placement": ["beat"],
    "topline_harmony": ["beat"],
    "vocal_space": ["beat"],
}


def round_b_context(manifest: dict, data: dict, report: dict, out: Path, max_axes: int) -> list[dict[str, Any]]:
    """Round Bと同じ2曲・同じ窓を、単体ステムでなく文脈つきで鳴らす素材。"""
    songs = data["songs"]
    roles = {vid: s.get("role", "positive") for vid, s in songs.items()}
    draft = load_json(Path(manifest["corpus_root"]) / "analysis/machine-draft.json")
    selected = [r for rows in (report["boundaries"]["groups"], report["boundaries"]["scalars"]) for r in rows if r["selected"]]
    selected = sorted(selected, key=lambda r: -abs(r["rank_biserial"]))[:max_axes]
    rows = []
    for rank, row in enumerate(selected, 1):
        axis = row["axis"]
        view = _axis_view(axis)
        kinds = CONTEXT_KINDS.get(view)
        if not kinds:
            continue
        values = _axis_values(axis, songs, draft, roles)
        pos = {v: x for v, x in values.items() if roles.get(v) == "positive"}
        con = {v: x for v, x in values.items() if roles.get(v) == "contrast"}
        if not pos or not con:
            continue
        pick_pos = min(pos, key=lambda v: (abs(pos[v] - float(np.median(list(pos.values())))), v))
        pick_con = min(con, key=lambda v: (abs(con[v] - float(np.median(list(con.values())))), v))
        items = []
        for kind in kinds:
            for label, vid in (("positive", pick_pos), ("contrast", pick_con)):
                made = _material(manifest, songs, vid, view, kind)
                if made is None:
                    continue
                audio, sr = made
                rel = Path("round-b-context") / f"{rank:02d}-{REVIEW._safe(axis)}" / f"{kind}-{label}-{REVIEW._safe(songs[vid]['display_name'])}.wav"
                info = _write(out / rel, audio, sr)
                items.append({"kind": kind, "side": label, "video_id": vid, "display_name": songs[vid]["display_name"], "value": round(values[vid], 6), "file": rel.as_posix(), **info})
        if items:
            rows.append({"rank": rank, "axis": axis, "items": items})
    return rows


def round_b_and_c(manifest: dict, data: dict, report: dict, out: Path, max_axes: int) -> tuple[list, list]:
    songs = data["songs"]
    roles = {vid: s.get("role", "positive") for vid, s in songs.items()}
    draft = load_json(Path(manifest["corpus_root"]) / "analysis/machine-draft.json")
    selected = [r for rows in (report["boundaries"]["groups"], report["boundaries"]["scalars"]) for r in rows if r["selected"]]
    selected = sorted(selected, key=lambda r: -abs(r["rank_biserial"]))[:max_axes]
    b_rows, c_rows = [], []
    for rank, row in enumerate(selected, 1):
        axis = row["axis"]
        view = _axis_view(axis)
        kind = VIEW_KIND[view]
        values = _axis_values(axis, songs, draft, roles)
        if not values:
            continue
        pos = {v: x for v, x in values.items() if roles.get(v) == "positive"}
        con = {v: x for v, x in values.items() if roles.get(v) == "contrast"}
        if not pos or not con:
            continue
        pos_center = float(np.median(list(pos.values())))
        con_center = float(np.median(list(con.values())))
        pick_pos = min(pos, key=lambda v: (abs(pos[v] - pos_center), v))
        pick_con = min(con, key=lambda v: (abs(con[v] - con_center), v))
        pair = []
        for label, vid in (("positive", pick_pos), ("contrast", pick_con)):
            made = _material(manifest, songs, vid, view, kind)
            if made is None:
                raise ValueError(f"{axis}: {vid} の素材を作れない（無言で欠落させない）")
            audio, sr = made
            rel = Path("round-b") / f"{rank:02d}-{REVIEW._safe(axis)}" / f"{label}-{REVIEW._safe(songs[vid]['display_name'])}.wav"
            info = _write(out / rel, audio, sr)
            pair.append({"side": label, "video_id": vid, "display_name": songs[vid]["display_name"], "value": round(values[vid], 6), "file": rel.as_posix(), **info})
        b_rows.append({"rank": rank, "axis": axis, "kind": kind, "direction": row["direction"], "auc": row["auc_positive_higher"], "overlap": row["overlap"], "pair": pair})

        midpoint = (pos_center + con_center) / 2
        mid_vid = min(values, key=lambda v: (abs(values[v] - midpoint), v))
        made = _material(manifest, songs, mid_vid, view, kind)
        if made is not None:
            audio, sr = made
            rel = Path("round-c") / f"{rank:02d}-{REVIEW._safe(axis)}.wav"
            info = _write(out / rel, audio, sr)
            c_rows.append({"rank": rank, "axis": axis, "kind": kind, "video_id": mid_vid, "display_name": songs[mid_vid]["display_name"], "value": round(values[mid_vid], 6), "midpoint": round(midpoint, 6), "true_role": roles.get(mid_vid), "file": rel.as_posix(), **info})
    return b_rows, c_rows


def round_omnibus(manifest: dict, data: dict, report: dict, out: Path, top_axes: int) -> list[dict[str, Any]]:
    """omnibus検定を通ったviewを、view全体の性格と、その中の強い軸で聴き分ける素材。

    軸を1本ずつBHで採るとviewごと落ちるが、viewとしては分離している場合に使う。
    まずview全体の代表2曲（role別のmedoid）、次に強い軸ごとのA/B。
    """
    songs = data["songs"]
    roles = {vid: s.get("role", "positive") for vid, s in songs.items()}
    draft = load_json(Path(manifest["corpus_root"]) / "analysis/machine-draft.json")
    rows = []
    for view, om in sorted(report.get("omnibus", {}).items(), key=lambda kv: kv[1]["p_permutation"]):
        if not om.get("significant"):
            continue
        kind = VIEW_KIND[view]
        ids = draft["views"][view]["participants"]
        matrix = np.asarray(draft["views"][view]["distance_matrix"])
        index = {vid: i for i, vid in enumerate(ids)}
        items = []
        # view全体の性格: role別に、同じroleの他曲へ最も近い曲＝その側の典型
        for label in ("positive", "contrast"):
            members = [v for v in ids if roles.get(v) == label]
            if len(members) < 2:
                continue
            medoid = min(members, key=lambda v: (sum(matrix[index[v], index[o]] for o in members if o != v), v))
            made = _material(manifest, songs, medoid, view, kind)
            if made is None:
                continue
            audio, sr = made
            rel = Path("round-d") / view / f"view-{label}-{REVIEW._safe(songs[medoid]['display_name'])}.wav"
            items.append({"scope": "view", "side": label, "video_id": medoid, "display_name": songs[medoid]["display_name"], "file": rel.as_posix(), **_write(out / rel, audio, sr)})

        axes = sorted(
            [r for r in report["boundaries"]["scalars"] if r["axis"].startswith(view + ".") and not r["blocking_tags"]],
            key=lambda r: r["p_permutation"],
        )[:top_axes]
        for r in axes:
            values = _axis_values(r["axis"], songs, draft, roles)
            pos = {v: x for v, x in values.items() if roles.get(v) == "positive"}
            con = {v: x for v, x in values.items() if roles.get(v) == "contrast"}
            if not pos or not con:
                continue
            for label, pool in (("positive", pos), ("contrast", con)):
                center = float(np.median(list(pool.values())))
                vid = min(pool, key=lambda v: (abs(pool[v] - center), v))
                made = _material(manifest, songs, vid, view, kind)
                if made is None:
                    continue
                audio, sr = made
                rel = Path("round-d") / view / f"{REVIEW._safe(r['axis'].split('.', 1)[1])}-{label}-{REVIEW._safe(songs[vid]['display_name'])}.wav"
                items.append({"scope": "axis", "axis": r["axis"], "side": label, "video_id": vid, "display_name": songs[vid]["display_name"], "value": round(values[vid], 6), "auc": r["auc_positive_higher"], "p": r["p_permutation"], "file": rel.as_posix(), **_write(out / rel, audio, sr)})
        rows.append({"view": view, "omnibus_p": om["p_permutation"], "n_axes": om["n_axes"], "items": items})
    return rows


def round_crossed(manifest: dict, data: dict, report: dict, out: Path, top_axes: int) -> list[dict[str, Any]]:
    """軸の値とroleが食い違うペア。halo（全体の好みが軸へ投影される）を判定する。

    「否定例なのに正例側の値」対「正例なのに否定例側の値」を聴いて、
    軸の値の側を選ぶならその軸が効いている。roleの側を選ぶなら、
    聴いているのは軸ではなく曲全体の性格。
    """
    songs = data["songs"]
    roles = {vid: s.get("role", "positive") for vid, s in songs.items()}
    draft = load_json(Path(manifest["corpus_root"]) / "analysis/machine-draft.json")
    rows = []
    for view, om in sorted(report.get("omnibus", {}).items(), key=lambda kv: kv[1]["p_permutation"]):
        if not om.get("significant"):
            continue
        kind = VIEW_KIND[view]
        axes = sorted(
            [r for r in report["boundaries"]["scalars"] if r["axis"].startswith(view + ".") and not r["blocking_tags"]],
            key=lambda r: r["p_permutation"],
        )[:top_axes]
        for r in axes:
            values = _axis_values(r["axis"], songs, draft, roles)
            pos = {v: x for v, x in values.items() if roles.get(v) == "positive"}
            con = {v: x for v, x in values.items() if roles.get(v) == "contrast"}
            if len(pos) < 3 or len(con) < 3:
                continue
            higher_is_positive = r["auc_positive_higher"] > 0.5
            crossed_con = max(con, key=con.get) if higher_is_positive else min(con, key=con.get)
            crossed_pos = min(pos, key=pos.get) if higher_is_positive else max(pos, key=pos.get)
            # 交差していない（値の重なりが無い）なら作らない
            if higher_is_positive and not (con[crossed_con] > np.median(list(pos.values()))):
                continue
            if not higher_is_positive and not (con[crossed_con] < np.median(list(pos.values()))):
                continue
            items = []
            for label, vid in (("contrast_with_positive_value", crossed_con), ("positive_with_contrast_value", crossed_pos)):
                made = _material(manifest, songs, vid, view, kind)
                if made is None:
                    continue
                audio, sr = made
                rel = Path("round-e") / view / f"{REVIEW._safe(r['axis'].split('.', 1)[1])}-{label}-{REVIEW._safe(songs[vid]['display_name'])}.wav"
                items.append({"side": label, "video_id": vid, "role": roles[vid], "display_name": songs[vid]["display_name"], "value": round(values[vid], 6), "file": rel.as_posix(), **_write(out / rel, audio, sr)})
            if len(items) == 2:
                rows.append({"view": view, "axis": r["axis"], "higher_is_positive": higher_is_positive,
                             "positive_median": round(float(np.median(list(pos.values()))), 6),
                             "contrast_median": round(float(np.median(list(con.values()))), 6), "items": items})
    return rows


def crossed_pair(manifest: dict, data: dict, axis: str, out: Path, exclude: list[str]) -> list[dict[str, Any]]:
    """1軸ぶんの交差ペア。既に聴いた曲を除外して組み直せる。"""
    songs = data["songs"]
    roles = {vid: s.get("role", "positive") for vid, s in songs.items()}
    draft = load_json(Path(manifest["corpus_root"]) / "analysis/machine-draft.json")
    view = _axis_view(axis)
    kind = VIEW_KIND[view]
    values = {k: v for k, v in _axis_values(axis, songs, draft, roles).items() if k not in set(exclude)}
    pos = {v: x for v, x in values.items() if roles.get(v) == "positive"}
    con = {v: x for v, x in values.items() if roles.get(v) == "contrast"}
    report = load_json(Path(manifest["corpus_root"]) / "analysis/contrast.json")
    row = next(r for r in report["boundaries"]["scalars"] if r["axis"] == axis)
    higher_is_positive = row["auc_positive_higher"] > 0.5
    crossed_con = max(con, key=con.get) if higher_is_positive else min(con, key=con.get)
    crossed_pos = min(pos, key=pos.get) if higher_is_positive else max(pos, key=pos.get)
    rows = []
    for label, vid in (("contrast_with_positive_value", crossed_con), ("positive_with_contrast_value", crossed_pos)):
        made = _material(manifest, songs, vid, view, kind)
        if made is None:
            raise ValueError(f"{axis}/{vid}: 素材を作れない")
        audio, sr = made
        rel = Path("crossed") / REVIEW._safe(axis) / f"{label}-{values[vid]:.3f}-{REVIEW._safe(songs[vid]['display_name'])}.wav"
        rows.append({"axis": axis, "side": label, "role": roles[vid], "video_id": vid,
                     "display_name": songs[vid]["display_name"], "value": round(values[vid], 6),
                     "file": rel.as_posix(), **_write(out / rel, audio, sr)})
    return rows


def axis_extremes(manifest: dict, data: dict, axis: str, out: Path) -> list[dict[str, Any]]:
    """指定した軸の、role別の最大・最小の4曲。軸の意味そのものを耳で掴むための素材。

    中央値どうしのA/Bだと軸が弱く出るので、値域の端を聴いて「この軸は何を測っているか」を
    先に理解してから判断したいとき用。
    """
    songs = data["songs"]
    roles = {vid: s.get("role", "positive") for vid, s in songs.items()}
    draft = load_json(Path(manifest["corpus_root"]) / "analysis/machine-draft.json")
    view = _axis_view(axis)
    kind = VIEW_KIND[view]
    values = _axis_values(axis, songs, draft, roles)
    if not values:
        raise ValueError(f"{axis}: 値を取れない")
    rows = []
    for role in ("positive", "contrast"):
        pool = {v: x for v, x in values.items() if roles.get(v) == role}
        if not pool:
            continue
        for label, vid in (("max", max(pool, key=pool.get)), ("min", min(pool, key=pool.get))):
            made = _material(manifest, songs, vid, view, kind)
            if made is None:
                raise ValueError(f"{axis}/{vid}: 素材を作れない")
            audio, sr = made
            rel = Path("axis-extremes") / REVIEW._safe(axis) / f"{role}-{label}-{values[vid]:.3f}-{REVIEW._safe(songs[vid]['display_name'])}.wav"
            rows.append({"axis": axis, "role": role, "extreme": label, "video_id": vid,
                         "display_name": songs[vid]["display_name"], "value": round(values[vid], 6),
                         "file": rel.as_posix(), **_write(out / rel, audio, sr)})
    return rows


def render_readme(b_rows: list, c_rows: list, d_rows: list | None = None) -> str:
    lines = [
        "# 対照分析 — 耳確認",
        "",
        "全クリップを同じRMSへ揃えています。音圧差そのものを好みの差として判断しないでください。",
        "回答は再生成で消えない `review/contrast-human-review.md` へ書きます。",
        "",
    ]

    lines += ["## Round B — 分離した軸を耳で確かめる", ""]
    if not b_rows:
        lines += ["採用軸なし。Round Bは生成していません。", ""]
    for row in b_rows:
        lines += [
            f"### {row['rank']}. {row['axis']}（{row['kind']}のみ・AUC {row['auc']:.3f}・重なり {row['overlap']:.3f}）",
            "",
            "この違いは「作りたい／作りたくない」の差か、それとも単に違うだけか。",
            "",
        ]
        for item in row["pair"]:
            side = "正例側" if item["side"] == "positive" else "否定例側"
            lines.append(f"- [ ] `{item['file']}` — {side}: {item['display_name']}（値 {item['value']:.3f}）")
        lines.append("")

    lines += ["## Round C — 境界の位置", "", "その軸の値が正例と否定例の中間にある曲です。どちら側に感じますか。", ""]
    for row in c_rows:
        lines.append(f"- [ ] `{row['file']}` — {row['axis']}: {row['display_name']}（値 {row['value']:.3f} / 中間 {row['midpoint']:.3f}）")
    lines += ["", "回答: 正例側 / 否定例側 / どちらでもない", ""]

    for row in (d_rows or []):
        lines += [f"## Round D — {row['view']}（view全体で分離。omnibus p={row['omnibus_p']:.4f}・軸{row['n_axes']}本）", "",
                  "まずview全体の代表2曲、次に軸ごとのA/B。軸に対応したステムだけ・同一RMSです。", ""]
        for item in row["items"]:
            side = "正例側" if item["side"] == "positive" else "否定例側"
            if item["scope"] == "view":
                lines.append(f"- [ ] `{item['file']}` — [view全体] {side}: {item['display_name']}")
            else:
                lines.append(f"- [ ] `{item['file']}` — [{item['axis'].split('.', 1)[1]}] {side}: {item['display_name']}（値 {item['value']:.3f}）")
        lines.append("")
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description="対照分析の耳確認素材を作る")
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS_ROOT)
    ap.add_argument("--data", type=Path, default=REPO_ROOT / "docs/labs/reference-beat-taste-data.json")
    ap.add_argument("--max-axes", type=int, default=6, help="Round B/Cを作る軸の上限（多すぎる一問一答を避ける）")
    ap.add_argument("--top-axes", type=int, default=3, help="omnibus通過viewで素材を作る軸の数")
    ap.add_argument("--axis-extremes", help="この軸のrole別max/min 4曲だけを作る（例: topline_harmony.texture.hpss_ratio）")
    ap.add_argument("--crossed-axis", help="この軸の交差ペアだけを作る（軸の値とroleが食い違う2曲）")
    ap.add_argument("--exclude", nargs="*", default=[], help="交差ペアから外すvideo id（既に聴いて印象が残っている曲）")
    args = ap.parse_args()
    root = args.corpus.expanduser().resolve()
    manifest = load_json(root / "manifest.json")
    data = load_json(args.data)
    report = load_json(root / "analysis/contrast.json")
    if not manifest or not data or not report:
        print("ERROR: manifest / taste-data / contrast.json 不足", file=sys.stderr)
        return 2

    out = root / "review/contrast"
    if args.crossed_axis:
        rows = crossed_pair(manifest, data, args.crossed_axis, out, args.exclude)
        print(f"{args.crossed_axis} の交差ペア（除外: {args.exclude or 'なし'}）")
        for r in rows:
            print(f"  [{r['role']:8s}] 値={r['value']:.3f} {r['display_name'][:46]}")
            print(f"      {r['file']}")
        return 0
    if args.axis_extremes:
        rows = axis_extremes(manifest, data, args.axis_extremes, out)
        print(f"{args.axis_extremes} の端 {len(rows)}本")
        for r in rows:
            print(f"  {r['role']:8s} {r['extreme']:3s} 値={r['value']:9.3f} {r['display_name'][:44]}")
            print(f"      {r['file']}")
        return 0
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    b_rows, c_rows = round_b_and_c(manifest, data, report, out, args.max_axes)
    context_rows = round_b_context(manifest, data, report, out, args.max_axes)
    d_rows = round_omnibus(manifest, data, report, out, args.top_axes)
    e_rows = round_crossed(manifest, data, report, out, args.top_axes)
    (out / "README.md").write_text(render_readme(b_rows, c_rows, d_rows))
    (out / "manifest.json").write_text(json.dumps({"schema_revision": 3, "round_b": b_rows, "round_b_context": context_rows, "round_c": c_rows, "round_d": d_rows, "round_e_crossed": e_rows}, ensure_ascii=False, indent=2) + "\n")

    total = len(list(out.rglob("*.wav")))
    selected_axes = len(b_rows)
    print(f"Round B={selected_axes}軸 Round C={len(c_rows)}本 Round D={len(d_rows)}view Round E={len(e_rows)}軸 / 合計{total}本")
    print(f"出力: {out}")
    if not selected_axes:
        print("採用軸ゼロのため、Round B/Cは空です。境界が立たなかったこと自体が結果です。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
