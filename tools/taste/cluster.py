#!/usr/bin/env python
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from collections import Counter
from pathlib import Path
from typing import Any

import numpy as np
from sklearn.cluster import AgglomerativeClustering
from sklearn.metrics import adjusted_rand_score, silhouette_score

from common import DEFAULT_CORPUS_ROOT, REPO_ROOT, load_json, write_json_atomic
from provenance import record_root_stage
from distances import symmetric_nearest_set_distance, weighted_group_distance
from schemas import OVERALL_MANDATORY_VIEWS, STAGE_SOURCE_PATHS, VIEW_SCHEMAS

MIN_SILHOUETTE = 0.12
MIN_ABLATION_STABILITY = 0.5
INTERNAL_VARIANT_DISTANCE = 0.42


def _output_schema_revision(data: dict[str, Any]) -> int:
    """距離成果物は、元になった特徴量スキーマの版を引き継ぐ。"""
    return int(data.get("schema_revision", 1))


def _samples(view: str, data: dict) -> list[dict]:
    if view == "arrangement":
        return [data["groups"]]
    return [s["groups"] for s in data["segments"] if s.get("eligible")]


def build_distance(view: str, songs: dict[str, Any], omit_group: str | None = None) -> tuple[list[str], np.ndarray, dict[str, Any]]:
    schema = VIEW_SCHEMAS[view]
    ids = sorted(vid for vid, s in songs.items() if not s.get("excluded") and s.get("views", {}).get(view, {}).get("eligible"))
    n = len(ids)
    matrix = np.zeros((n, n))
    contributions: dict[str, Any] = {}
    for i in range(n):
        for j in range(i + 1, n):
            a, b = _samples(view, songs[ids[i]]["views"][view]), _samples(view, songs[ids[j]]["views"][view])
            if view == "arrangement":
                d, parts = weighted_group_distance(a[0], b[0], schema["weights"], omit_group)
            else:
                d, parts = symmetric_nearest_set_distance(a, b, schema["weights"], omit_group)
            matrix[i, j] = matrix[j, i] = d
            contributions[f"{ids[i]}::{ids[j]}"] = parts
    if np.isnan(matrix).any():
        raise ValueError(f"{view}: NaNを含む距離表")
    return ids, matrix, contributions


def _cluster(matrix: np.ndarray, k: int) -> np.ndarray:
    return AgglomerativeClustering(n_clusters=k, metric="precomputed", linkage="average").fit_predict(matrix)


def candidates_for_view(view: str, ids: list[str], matrix: np.ndarray, songs: dict[str, Any]) -> list[dict[str, Any]]:
    m = len(ids)
    if m < 3:
        return []
    candidates = []
    for k in range(2, min(6, m - 1) + 1):
        labels = _cluster(matrix, k)
        sizes = Counter(map(int, labels))
        if min(sizes.values()) < 2:
            continue
        sil = float(silhouette_score(matrix, labels, metric="precomputed"))
        stabilities = []
        for group in VIEW_SCHEMAS[view]["weights"]:
            _, ablated, _ = build_distance(view, songs, omit_group=group)
            stabilities.append(float(adjusted_rand_score(labels, _cluster(ablated, k))))
        stability = float(np.mean(stabilities)) if stabilities else 1.0
        candidates.append({
            "k": k,
            "silhouette": round(sil, 6),
            "ablation_stability": round(stability, 6),
            "labels": {vid: int(labels[i]) for i, vid in enumerate(ids)},
            "cluster_sizes": dict(sorted((str(k), v) for k, v in sizes.items())),
            "accepted": sil >= MIN_SILHOUETTE and stability >= MIN_ABLATION_STABILITY,
        })
    return sorted(candidates, key=lambda c: (-int(c["accepted"]), -c["silhouette"], -c["ablation_stability"], c["k"]))[:2]


def _neighbors(ids: list[str], matrix: np.ndarray) -> dict[str, list[dict[str, Any]]]:
    result = {}
    for i, vid in enumerate(ids):
        order = sorted((float(matrix[i, j]), ids[j]) for j in range(len(ids)) if j != i)[:3]
        result[vid] = [{"video_id": other, "distance": round(d, 6)} for d, other in order]
    return result


def _outliers(ids: list[str], matrix: np.ndarray) -> list[dict[str, Any]]:
    if len(ids) < 2:
        return []
    nearest = np.array([min(matrix[i, j] for j in range(len(ids)) if i != j) for i in range(len(ids))])
    q1, q3 = np.percentile(nearest, [25, 75])
    threshold = q3 + 1.5 * (q3 - q1)
    return [{"video_id": ids[i], "nearest_distance": round(float(v), 6)} for i, v in enumerate(nearest) if v > threshold]


def _internal_variants(view: str, ids: list[str], songs: dict[str, Any]) -> list[dict[str, Any]]:
    if view == "arrangement":
        return []
    result = []
    weights = VIEW_SCHEMAS[view]["weights"]
    for vid in ids:
        samples = _samples(view, songs[vid]["views"][view])
        if len(samples) < 2:
            continue
        farthest = max(weighted_group_distance(a, b, weights)[0] for i, a in enumerate(samples) for b in samples[i + 1 :])
        if farthest >= INTERNAL_VARIANT_DISTANCE:
            result.append({"video_id": vid, "max_segment_distance": round(farthest, 6)})
    return result


def _overall(view_results: dict[str, Any]) -> dict[str, Any]:
    eligible = sorted(set.intersection(*(set(view_results[v]["participants"]) for v in OVERALL_MANDATORY_VIEWS))) if view_results else []
    n = len(eligible)
    matrix = np.zeros((n, n))
    for view in OVERALL_MANDATORY_VIEWS:
        vr = view_results[view]
        index = {vid: i for i, vid in enumerate(vr["participants"])}
        src = np.asarray(vr["distance_matrix"])
        for i, a in enumerate(eligible):
            for j, b in enumerate(eligible):
                matrix[i, j] += src[index[a], index[b]] / len(OVERALL_MANDATORY_VIEWS)
    # overall candidate stabilityはbase viewを1つ抜く意味になるが、mandatory契約を変えないため
    # 候補説明だけに記録し、正式overall距離は常に5view等重み。
    cands = []
    if n >= 3:
        for k in range(2, min(6, n - 1) + 1):
            labels = _cluster(matrix, k)
            sizes = Counter(map(int, labels))
            if min(sizes.values()) < 2:
                continue
            sil = float(silhouette_score(matrix, labels, metric="precomputed"))
            st = []
            for omitted in OVERALL_MANDATORY_VIEWS:
                m2 = np.zeros_like(matrix)
                for view in OVERALL_MANDATORY_VIEWS:
                    if view == omitted:
                        continue
                    vr = view_results[view]; index = {vid: i for i, vid in enumerate(vr["participants"])}; src = np.asarray(vr["distance_matrix"])
                    for i, a in enumerate(eligible):
                        for j, b in enumerate(eligible):
                            m2[i, j] += src[index[a], index[b]] / (len(OVERALL_MANDATORY_VIEWS) - 1)
                st.append(float(adjusted_rand_score(labels, _cluster(m2, k))))
            stability = float(np.mean(st))
            cands.append({"k": k, "silhouette": round(sil, 6), "view_ablation_stability": round(stability, 6), "labels": {vid: int(labels[i]) for i, vid in enumerate(eligible)}, "accepted": sil >= MIN_SILHOUETTE and stability >= MIN_ABLATION_STABILITY})
    cands = sorted(cands, key=lambda c: (-int(c["accepted"]), -c["silhouette"], -c["view_ablation_stability"], c["k"]))[:2]
    return {"mandatory_views": OVERALL_MANDATORY_VIEWS, "participants": eligible, "distance_matrix": matrix.round(8).tolist(), "neighbors": _neighbors(eligible, matrix), "candidates": cands, "outliers": _outliers(eligible, matrix)}


def _common_core(songs: dict[str, Any]) -> dict[str, Any]:
    total = len(songs)
    # 数値の全曲測定だけを共通核と呼ばない。全曲supportを定義できるbinary事実だけ調べる。
    facts = []
    checks = {
        "drum_audio_measurable": lambda s: s.get("views", {}).get("drum_audio", {}).get("eligible", False),
        "verified_4_4_grid": lambda s: s.get("grid_status") in {"auto_verified_4_4", "human_verified_4_4"},
        "topline_harmony_measurable": lambda s: s.get("views", {}).get("topline_harmony", {}).get("eligible", False),
    }
    for name, check in checks.items():
        measured = sum(not s.get("excluded") for s in songs.values())
        support = sum(bool(check(s)) for s in songs.values() if not s.get("excluded"))
        item = {"feature": name, "measured": f"{measured}/{total}", "support": f"{support}/{total}"}
        if measured == total and support == total:
            facts.append(item)
    return {"definition": "measured=23/23 and support=23/23", "facts": facts, "distributions_are_not_common_core": True}


def render_markdown(result: dict[str, Any], songs: dict[str, Any]) -> str:
    names = {vid: s.get("display_name", vid) for vid, s in songs.items()}
    lines = ["# 好きなビート23曲 — 機械ドラフト", "", "これは耳確認前の機械案です。群名はまだ付けず、Phase 5で代表・境界を聴いて確定します。", ""]
    for view, vr in result["views"].items():
        lines += [f"## {view}", "", f"参加: {len(vr['participants'])}/{len(songs)} / 不参加: {len(vr['excluded'])}", ""]
        for i, cand in enumerate(vr["candidates"], 1):
            lines.append(f"- 案{i}: k={cand['k']}, silhouette={cand['silhouette']}, stability={cand['ablation_stability']}, accepted={cand['accepted']}")
            groups: dict[int, list[str]] = {}
            for vid, label in cand["labels"].items():
                groups.setdefault(label, []).append(names[vid])
            for label, members in sorted(groups.items()):
                lines.append(f"  - group {label}: " + " / ".join(members))
        if not vr["candidates"]:
            lines.append("- 群なし／距離地図のみ")
        lines.append("")
    ov = result["overall"]
    lines += ["## overall", "", f"5 base viewすべてに参加: {len(ov['participants'])}/{len(songs)}", ""]
    for i, cand in enumerate(ov["candidates"], 1):
        lines.append(f"- 案{i}: k={cand['k']}, silhouette={cand['silhouette']}, view-ablation stability={cand['view_ablation_stability']}, accepted={cand['accepted']}")
    lines += ["", "## 全曲共通核", ""]
    if result["common_core"]["facts"]:
        lines += [f"- {x['feature']} (measured={x['measured']}, support={x['support']})" for x in result["common_core"]["facts"]]
    else:
        lines.append("- 定義を満たす全曲共通核は現時点では無い。数値が全曲で測れただけの項目は分布として扱う。")
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS_ROOT)
    ap.add_argument("--data", type=Path, default=REPO_ROOT / "docs/labs/reference-beat-taste-data.json")
    ap.add_argument("--markdown", type=Path, default=REPO_ROOT / "docs/labs/reference-beat-taste-machine-draft.md")
    args = ap.parse_args()
    data = load_json(args.data)
    if not data:
        print(f"ERROR: dataなし: {args.data}", file=sys.stderr)
        return 2
    songs = data["songs"]
    views = {}
    for view in VIEW_SCHEMAS:
        ids, matrix, contributions = build_distance(view, songs)
        excluded = {vid: s.get("views", {}).get(view, {}).get("reason", s.get("reason", "excluded")) for vid, s in songs.items() if vid not in ids}
        views[view] = {
            "participants": ids,
            "excluded": excluded,
            "required_features": VIEW_SCHEMAS[view]["required_groups"],
            "distance_matrix": matrix.round(8).tolist(),
            "neighbors": _neighbors(ids, matrix),
            "outliers": _outliers(ids, matrix),
            "internal_variants": _internal_variants(view, ids, songs),
            "contributions": contributions,
            "candidates": candidates_for_view(view, ids, matrix, songs),
        }
        print(f"{view}: M={len(ids)} candidates={len(views[view]['candidates'])}")
    result = {
        "schema_revision": _output_schema_revision(data),
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "views": views,
    }
    result["overall"] = _overall(views)
    result["common_core"] = _common_core(songs)
    out = args.corpus.expanduser().resolve() / "analysis/machine-draft.json"
    write_json_atomic(out, result)
    args.markdown.parent.mkdir(parents=True, exist_ok=True)
    args.markdown.write_text(render_markdown(result, songs))
    taste_prov = load_json(args.corpus.expanduser().resolve() / "taste-provenance.json", {})
    taste_fp = taste_prov.get("stages", {}).get("taste_features", {}).get("fingerprint")
    if taste_fp:
        record_root_stage(args.corpus.expanduser().resolve(), "distance_cluster", list(STAGE_SOURCE_PATHS["distance_cluster"]), {"taste_features": taste_fp}, {"view_schemas": VIEW_SCHEMAS, "overall_mandatory": OVERALL_MANDATORY_VIEWS, "linkage": "average", "min_silhouette": MIN_SILHOUETTE, "min_stability": MIN_ABLATION_STABILITY}, [out, args.markdown])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
