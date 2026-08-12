#!/usr/bin/env python
"""正例と近接した否定例の対照から、「作りたいビートの境界」を連続条件として出す。

分類器もクラスタリングも作らない。出すのは4つだけ。

1. 近接性: 否定例が正例からどれだけ離れているか（遠くても捨てず申告する）
2. view別の分離度: 正例内距離と正例×否定例距離が分かれるか。分かれないviewも同格の結果
3. 境界: feature群／個別featureごとの分布の重なりと、正例の目標レンジ
4. 安定性: 1曲抜きで採用軸が変わらないか

しきい値ではなくレンジで書く。confoundタグの付いた軸は分離しても境界候補にしない。
"""
from __future__ import annotations

import argparse
import datetime as dt
import sys
from pathlib import Path
from typing import Any

import numpy as np
from scipy.stats import rankdata

from common import DEFAULT_CORPUS_ROOT, REPO_ROOT, load_json, write_json_atomic
import features as FEATURES
from distances import symmetric_nearest_set_distance, value_distance, weighted_group_distance
from schemas import VIEW_SCHEMAS

# --- 結果を見る前に固定する条件 --------------------------------------------------
PERMUTATIONS = 10000
SEED = 20260810
ALPHA = 0.05
# AUCがこの範囲に収まる軸は「分離しない＝好みの境界ではない」と読む。
NEUTRAL_AUC = (0.40, 0.60)
# LOOで採用/不採用が入れ替わった割合がこれを超えたら不安定とみなす。
MAX_LOO_CHURN = 0.10
# 距離型でないleaf（列・multi-hot）はgroup単位でだけ扱う。
NON_SCALAR_KEYS = {"root_sequence", "section_sequence", "instruments", "hit_count"}
# 採用を止めるタグ。featureの値そのものが測り方の副作用で動くもの。
# selection_biasedはview単位の窓の性格差なので、ここには入れず注記として持ち回る
# （窓の音量が違っても、音量不感なfeatureの値は壊れない）。
BLOCKING_TAGS = {"loudness_sensitive", "bleed_sensitive"}


def _tag_applies(axis: str, tag_path: str) -> bool:
    """viewやgroupに付いたタグを、その配下の軸へも届かせる（両方向のprefix一致）。"""
    return axis == tag_path or axis.startswith(tag_path + ".") or tag_path.startswith(axis + ".")


def _rng() -> np.random.Generator:
    return np.random.default_rng(SEED)


# --- 統計 ----------------------------------------------------------------------

def auc_from_ranks(ranks: np.ndarray, mask_a: np.ndarray) -> float:
    """順位だけで測る2標本AUC。しきい値に依存せず、単調変換で不変。"""
    n_a = int(mask_a.sum())
    n_b = len(ranks) - n_a
    if n_a == 0 or n_b == 0:
        return float("nan")
    u = float(ranks[mask_a].sum()) - n_a * (n_a + 1) / 2
    return u / (n_a * n_b)


def _ranks(values: np.ndarray) -> np.ndarray:
    # 同値は平均順位。並び順で結果が変わらないようにする。
    return rankdata(values)


def permutation_p_two_sided(values: np.ndarray, mask_a: np.ndarray, rng: np.random.Generator) -> tuple[float, float]:
    """AUCの観測値と、ラベルをシャッフルしたnullに対する両側p値。

    順位はラベルの並べ替えで変わらないので、nullは固定した順位ベクトルの
    部分和として一括で作れる。
    """
    ranks = _ranks(values)
    observed = auc_from_ranks(ranks, mask_a)
    n = len(values)
    n_a = int(mask_a.sum())
    if n_a == 0 or n_a == n:
        return observed, 1.0
    picks = rng.random((PERMUTATIONS, n)).argsort(axis=1)[:, :n_a]
    sums = ranks[picks].sum(axis=1)
    null = (sums - n_a * (n_a + 1) / 2) / (n_a * (n - n_a))
    extreme = np.abs(null - 0.5) >= abs(observed - 0.5) - 1e-12
    return observed, float((extreme.sum() + 1) / (PERMUTATIONS + 1))


def benjamini_hochberg(pvalues: dict[str, float], alpha: float = ALPHA) -> dict[str, bool]:
    if not pvalues:
        return {}
    items = sorted(pvalues.items(), key=lambda kv: kv[1])
    m = len(items)
    passed: dict[str, bool] = {k: False for k in pvalues}
    cutoff = 0
    for i, (_, p) in enumerate(items, 1):
        if p <= alpha * i / m:
            cutoff = i
    for k, _ in items[:cutoff]:
        passed[k] = True
    return passed


def overlap_coefficient(a: np.ndarray, b: np.ndarray) -> float:
    """2標本の分布の重なり。1に近いほど「境界がない」。"""
    pool = np.concatenate([a, b])
    if np.allclose(pool, pool[0]):
        return 1.0
    bins = np.histogram_bin_edges(pool, bins="fd")
    if len(bins) < 3:
        bins = np.histogram_bin_edges(pool, bins=5)
    pa, _ = np.histogram(a, bins=bins, density=False)
    pb, _ = np.histogram(b, bins=bins, density=False)
    pa = pa / max(pa.sum(), 1)
    pb = pb / max(pb.sum(), 1)
    return float(np.minimum(pa, pb).sum())


def _summary(values: np.ndarray) -> dict[str, float]:
    return {
        "n": int(len(values)),
        "median": round(float(np.median(values)), 6),
        "p10": round(float(np.percentile(values, 10)), 6),
        "p90": round(float(np.percentile(values, 90)), 6),
        "iqr": round(float(np.percentile(values, 75) - np.percentile(values, 25)), 6),
    }


# --- 特徴量の取り出し ------------------------------------------------------------

def _segments(view: str, song: dict[str, Any]) -> list[dict[str, Any]]:
    data = song.get("views", {}).get(view, {})
    if not data.get("eligible"):
        return []
    if view == "arrangement":
        return [{"groups": data["groups"]}]
    return [s for s in data.get("segments", []) if s.get("eligible")]


def scalar_features(view: str, song: dict[str, Any]) -> dict[str, float]:
    """曲ごとに1票。区間があれば中央値へ集約し、区間数の多い曲を重くしない。"""
    collected: dict[str, list[float]] = {}
    for seg in _segments(view, song):
        stack = [("", seg["groups"])]
        while stack:
            prefix, node = stack.pop()
            for key, value in node.items():
                path = f"{prefix}.{key}" if prefix else key
                if isinstance(value, dict):
                    stack.append((path, value))
                elif isinstance(value, (int, float)) and not isinstance(value, bool) and key not in NON_SCALAR_KEYS:
                    collected.setdefault(path, []).append(float(value))
    if view == "drum_placement":
        # 既存の16分profileから導けるので、featureの作り直しは要らない。
        # 「ハットが細かい」「キック/スネアが規則的でゆったり」を軸として取り出す。
        for seg in _segments(view, song):
            bars = (seg.get("end_bar") or 0) - (seg.get("start_bar") or 0) + 1
            bar_s = (seg["end_s"] - seg["start_s"]) / bars if bars > 0 else None
            for key, value in FEATURES.drum_shape(seg["groups"]["profiles"], bar_s).items():
                collected.setdefault(f"drum_shape.{key}", []).append(float(value))
    return {k: float(np.median(v)) for k, v in sorted(collected.items()) if v}


def build_group_distance(view: str, songs: dict[str, Any], ids: list[str], group: str) -> tuple[list[str], np.ndarray]:
    """feature群1つだけで曲×曲の距離表を作る。

    「正例のmedoidからの距離」で1次元化すると、medoidを正例から決めている以上
    正例が近くなるのは構成上ほぼ自明で、ラベルを入れ替えるpermutationでは補正できない。
    群の分離はroleを一切見ない距離表の上で測る。
    """
    weights = {group: 1.0}
    # weighted_group_distanceは「群名→中身」のdictを取るので、対象の群だけを残した形で渡す。
    samples = {vid: [{group: seg["groups"][group]} for seg in _segments(view, songs[vid]) if group in seg["groups"]] for vid in ids}
    usable = [vid for vid in ids if samples.get(vid)]
    n = len(usable)
    matrix = np.zeros((n, n))
    for i in range(n):
        for j in range(i + 1, n):
            a, b = samples[usable[i]], samples[usable[j]]
            if view == "arrangement":
                d, _ = weighted_group_distance(a[0], b[0], weights)
            else:
                d, _ = symmetric_nearest_set_distance(a, b, weights)
            matrix[i, j] = matrix[j, i] = d
    return usable, matrix


# --- (1) 近接性 -----------------------------------------------------------------

def proximity(ids: list[str], matrix: np.ndarray, roles: dict[str, str]) -> dict[str, Any]:
    index = {vid: i for i, vid in enumerate(ids)}
    pos = [v for v in ids if roles.get(v) == "positive"]
    con = [v for v in ids if roles.get(v) == "contrast"]
    if len(pos) < 3 or not con:
        return {"error": "参加曲不足", "n_positive": len(pos), "n_contrast": len(con)}
    internal = np.array([min(matrix[index[a], index[b]] for b in pos if b != a) for a in pos])
    q1, q3 = np.percentile(internal, [25, 75])
    threshold = float(q3 + 1.5 * (q3 - q1))
    rows = []
    for vid in con:
        nearest = sorted((float(matrix[index[vid], index[p]]), p) for p in pos)
        rows.append({
            "video_id": vid,
            "nearest_positive": nearest[0][1],
            "nearest_distance": round(nearest[0][0], 6),
            "distant_candidate": bool(nearest[0][0] > threshold),
        })
    return {
        "positive_internal_nearest": _summary(internal),
        "distant_threshold": round(threshold, 6),
        "contrast": sorted(rows, key=lambda r: -r["nearest_distance"]),
    }


# --- (2) view別の分離度 ----------------------------------------------------------

def separation(ids: list[str], matrix: np.ndarray, roles: dict[str, str], rng: np.random.Generator) -> dict[str, Any]:
    pos = [v for v in ids if roles.get(v) == "positive"]
    con = [v for v in ids if roles.get(v) == "contrast"]
    if len(pos) < 3 or len(con) < 2:
        return {"error": "参加曲不足", "n_positive": len(pos), "n_contrast": len(con)}

    def stats(pos_mask: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        con_mask = ~pos_mask
        block = matrix[np.ix_(pos_mask, pos_mask)]
        within = block[np.triu_indices(int(pos_mask.sum()), 1)]
        across = matrix[np.ix_(pos_mask, con_mask)].ravel()
        return within, across

    def auc_for(pos_mask: np.ndarray) -> float:
        w, a = stats(pos_mask)
        vals = np.concatenate([w, a])
        m = np.zeros(len(vals), dtype=bool)
        m[len(w) :] = True  # across側が「大きいほど分離」
        return auc_from_ranks(_ranks(vals), m)

    base_mask = np.array([roles.get(v) == "positive" for v in ids])
    within, across = stats(base_mask)
    observed = auc_for(base_mask)

    n = len(ids)
    n_pos = int(base_mask.sum())
    picks = rng.random((PERMUTATIONS, n)).argsort(axis=1)[:, :n_pos]
    null = np.empty(PERMUTATIONS)
    for i in range(PERMUTATIONS):
        m = np.zeros(n, dtype=bool)
        m[picks[i]] = True
        null[i] = auc_for(m)
    p = float((np.sum(np.abs(null - 0.5) >= abs(observed - 0.5) - 1e-12) + 1) / (PERMUTATIONS + 1))
    return {
        "n_positive": len(pos),
        "n_contrast": len(con),
        "auc": round(float(observed), 6),
        "p_permutation": round(p, 6),
        "null_p95": round(float(np.percentile(np.abs(null - 0.5), 95) + 0.5), 6),
        "within_positive": _summary(within),
        "positive_to_contrast": _summary(across),
        "separates": bool(not (NEUTRAL_AUC[0] <= observed <= NEUTRAL_AUC[1])),
    }


# --- (3) 境界（重なりと目標レンジ） -----------------------------------------------

def _axis_row(name: str, values: dict[str, float], roles: dict[str, str], rng: np.random.Generator) -> dict[str, Any] | None:
    pos = np.array([v for k, v in values.items() if roles.get(k) == "positive"])
    con = np.array([v for k, v in values.items() if roles.get(k) == "contrast"])
    if len(pos) < 5 or len(con) < 4:
        return None
    allv = np.concatenate([pos, con])
    mask = np.zeros(len(allv), dtype=bool)
    mask[: len(pos)] = True
    auc, p = permutation_p_two_sided(allv, mask, rng)
    return {
        "axis": name,
        "positive": _summary(pos),
        "contrast": _summary(con),
        "auc_positive_higher": round(float(auc), 6),
        "rank_biserial": round(float(2 * auc - 1), 6),
        "overlap": round(overlap_coefficient(pos, con), 6),
        "p_permutation": round(p, 6),
        "direction": "positive_higher" if auc > 0.5 else "positive_lower",
    }


def _separates(auc: float) -> bool:
    return not (NEUTRAL_AUC[0] <= auc <= NEUTRAL_AUC[1])


def _loo_churn(values: dict[str, float], roles: dict[str, str]) -> float:
    """1曲抜きで判定が変わる割合。1曲の性質で立った軸を落とす。"""
    base = _plain_auc(values, roles)
    if base is None:
        return 1.0
    flips = 0
    for drop in values:
        trimmed = {k: v for k, v in values.items() if k != drop}
        auc = _plain_auc(trimmed, roles)
        if auc is None or _separates(auc) != _separates(base):
            flips += 1
        elif _separates(base) and (auc - 0.5) * (base - 0.5) <= 0:
            # 分離している軸だけ、向きの反転も不安定として数える。
            flips += 1
    return flips / max(1, len(values))


def _plain_auc(values: dict[str, float], roles: dict[str, str]) -> float | None:
    pos = np.array([v for k, v in values.items() if roles.get(k) == "positive"])
    con = np.array([v for k, v in values.items() if roles.get(k) == "contrast"])
    if len(pos) < 4 or len(con) < 3:
        return None
    allv = np.concatenate([pos, con])
    mask = np.zeros(len(allv), dtype=bool)
    mask[: len(pos)] = True
    return auc_from_ranks(_ranks(allv), mask)


def _distance_auc(ids: list[str], matrix: np.ndarray, roles: dict[str, str]) -> float | None:
    mask = np.array([roles.get(v) == "positive" for v in ids])
    if mask.sum() < 4 or (~mask).sum() < 3:
        return None
    block = matrix[np.ix_(mask, ~mask)]
    within = matrix[np.ix_(mask, mask)][np.triu_indices(int(mask.sum()), 1)]
    values = np.concatenate([within, block.ravel()])
    m = np.zeros(len(values), dtype=bool)
    m[len(within):] = True
    return auc_from_ranks(_ranks(values), m)


def _distance_loo_churn(ids: list[str], matrix: np.ndarray, roles: dict[str, str]) -> float:
    base = _distance_auc(ids, matrix, roles)
    if base is None:
        return 1.0
    flips = 0
    for i in range(len(ids)):
        keep = [j for j in range(len(ids)) if j != i]
        auc = _distance_auc([ids[j] for j in keep], matrix[np.ix_(keep, keep)], roles)
        if auc is None or _separates(auc) != _separates(base):
            flips += 1
    return flips / max(1, len(ids))


def omnibus_view_test(view: str, songs: dict[str, Any], ids: list[str], roles: dict[str, str], rng: np.random.Generator) -> dict[str, Any] | None:
    """viewの全scalar軸をまとめて1回だけ検定する。

    軸ごとにp値を出して補正すると、相関した軸（明るさとロールオフ、コード変化と戻る率など）を
    独立な検定として数えることになり、厳しすぎる方向にも緩すぎる方向にも歪む。
    同じラベルのシャッフルを全軸へ同時に当てれば、軸間の相関はnull側にそのまま入る。
    統計量は「軸ごとの|AUC-0.5|の平均」＝viewとしてどれだけ偏っているか。
    """
    per_song = {vid: scalar_features(view, songs[vid]) for vid in ids}
    keys = sorted({k for f in per_song.values() for k in f})
    keys = [k for k in keys if all(k in per_song[vid] for vid in ids)]
    if len(keys) < 2:
        return None
    labels = np.array([roles.get(v) == "positive" for v in ids])
    if labels.sum() < 5 or (~labels).sum() < 4:
        return None
    ranks = np.array([_ranks(np.array([per_song[vid][k] for vid in ids])) for k in keys])
    n, n_pos = len(ids), int(labels.sum())

    def mean_abs_dev(mask: np.ndarray) -> float:
        aucs = (ranks[:, mask].sum(axis=1) - n_pos * (n_pos + 1) / 2) / (n_pos * (n - n_pos))
        return float(np.mean(np.abs(aucs - 0.5)))

    observed = mean_abs_dev(labels)
    picks = rng.random((PERMUTATIONS, n)).argsort(axis=1)[:, :n_pos]
    null = np.empty(PERMUTATIONS)
    for i in range(PERMUTATIONS):
        m = np.zeros(n, dtype=bool)
        m[picks[i]] = True
        null[i] = mean_abs_dev(m)
    p = float((np.sum(null >= observed - 1e-12) + 1) / (PERMUTATIONS + 1))
    return {
        "n_axes": len(keys),
        "axes": keys,
        "mean_abs_auc_deviation": round(observed, 6),
        "null_median": round(float(np.median(null)), 6),
        "null_p95": round(float(np.percentile(null, 95)), 6),
        "p_permutation": round(p, 6),
        "significant": bool(p <= ALPHA),
    }


def boundaries(songs: dict[str, Any], draft: dict[str, Any], roles: dict[str, str], rng: np.random.Generator) -> dict[str, Any]:
    group_rows: list[dict[str, Any]] = []
    scalar_rows: list[dict[str, Any]] = []
    for view, schema in VIEW_SCHEMAS.items():
        ids = draft["views"][view]["participants"]
        for group in schema["weights"]:
            usable, matrix = build_group_distance(view, songs, ids, group)
            result = separation(usable, matrix, roles, rng)
            if "error" in result:
                continue
            group_rows.append({
                "axis": f"{view}.{group}",
                "kind": "group_distance_separation",
                "n_positive": result["n_positive"],
                "n_contrast": result["n_contrast"],
                "auc_positive_higher": result["auc"],
                "rank_biserial": round(2 * result["auc"] - 1, 6),
                "overlap": round(overlap_coefficient(
                    np.array([result["within_positive"]["p10"], result["within_positive"]["median"], result["within_positive"]["p90"]]),
                    np.array([result["positive_to_contrast"]["p10"], result["positive_to_contrast"]["median"], result["positive_to_contrast"]["p90"]]),
                ), 6),
                "within_positive": result["within_positive"],
                "positive_to_contrast": result["positive_to_contrast"],
                "p_permutation": result["p_permutation"],
                "direction": "contrast_farther" if result["auc"] > 0.5 else "contrast_closer",
                "loo_churn": round(_distance_loo_churn(usable, matrix, roles), 6),
            })
        per_song: dict[str, dict[str, float]] = {vid: scalar_features(view, songs[vid]) for vid in ids}
        keys = sorted({k for f in per_song.values() for k in f})
        for key in keys:
            values = {vid: f[key] for vid, f in per_song.items() if key in f}
            row = _axis_row(f"{view}.{key}", values, roles, rng)
            if row:
                row["kind"] = "scalar"
                row["loo_churn"] = round(_loo_churn(values, roles), 6)
                scalar_rows.append(row)

    for rows in (group_rows, scalar_rows):
        passed = benjamini_hochberg({r["axis"]: r["p_permutation"] for r in rows})
        for r in rows:
            r["bh_significant"] = bool(passed.get(r["axis"], False))
            r["stable"] = bool(r["loo_churn"] <= MAX_LOO_CHURN)
            r["separates"] = bool(_separates(r["auc_positive_higher"]))
            r["selected"] = bool(r["separates"] and r["bh_significant"] and r["stable"])
    return {
        "criteria": {"permutations": PERMUTATIONS, "alpha": ALPHA, "neutral_auc": list(NEUTRAL_AUC), "max_loo_churn": MAX_LOO_CHURN},
        "groups": sorted(group_rows, key=lambda r: -abs(r["rank_biserial"])),
        "scalars": sorted(scalar_rows, key=lambda r: -abs(r["rank_biserial"])),
    }


# --- 出力 ------------------------------------------------------------------------

def render_markdown(report: dict[str, Any], names: dict[str, str]) -> str:
    lines = ["# 対照分析の機械ドラフト", "", f"生成: {report['generated_at']}", ""]
    lines += ["これは機械の中間出力。耳確認を通るまで「境界」とは書かない。", ""]

    lines += ["## view別の分離度", "", "7つ検定しているのでBenjamini–Hochberg補正後の判定を採る。", "",
              "| view | n(+/-) | AUC | p | BH | 分離 |", "|---|---|---:|---:|---|---|"]
    for view, row in report["separation"].items():
        if "error" in row:
            lines.append(f"| {view} | - | - | - | - | {row['error']} |")
            continue
        bh = "通過" if row.get("bh_significant") else "落ち"
        verdict = "分離" if row.get("separates_after_bh") else "分離しない"
        lines.append(f"| {view} | {row['n_positive']}/{row['n_contrast']} | {row['auc']:.3f} | {row['p_permutation']:.4f} | {bh} | {verdict} |")
    lines.append("")

    lines += ["## 近接性（否定例が正例からどれだけ離れているか）", ""]
    ov = report["proximity"].get("overall", {})
    if "contrast" in ov:
        lines += [f"正例内の最近傍距離: median {ov['positive_internal_nearest']['median']:.3f} / 遠い判定閾値 {ov['distant_threshold']:.3f}", "", "| 曲 | 最近傍の正例 | 距離 | 遠い候補 |", "|---|---|---:|---|"]
        for row in ov["contrast"]:
            lines.append(f"| {names.get(row['video_id'], row['video_id'])} | {names.get(row['nearest_positive'], row['nearest_positive'])} | {row['nearest_distance']:.3f} | {'はい' if row['distant_candidate'] else ''} |")
        lines.append("")

    groups = [r for r in report["boundaries"]["groups"] if r["selected"]]
    lines += ["## 採用された軸（feature群・距離で判定）", ""]
    if groups:
        lines += ["| 軸 | 正例内距離 median | 正例→否定例 median | AUC | p | confound |", "|---|---:|---:|---:|---:|---|"]
        for r in groups:
            tags = ", ".join(r.get("confound_tags", [])) or "-"
            lines.append(
                f"| {r['axis']} | {r['within_positive']['median']:.3f} | {r['positive_to_contrast']['median']:.3f} | "
                f"{r['auc_positive_higher']:.3f} | {r['p_permutation']:.4f} | {tags} |"
            )
    else:
        lines.append("採用ゼロ。この粒度では境界が立たなかった。")
    lines.append("")

    scalars = [r for r in report["boundaries"]["scalars"] if r["selected"]]
    lines += ["## 採用された軸（個別feature・目標レンジ）", ""]
    if scalars:
        lines += ["| 軸 | 正例 p10–p90 | 否定例 p10–p90 | AUC | 重なり | confound |", "|---|---|---|---:|---:|---|"]
        for r in scalars:
            tags = ", ".join(r.get("confound_tags", [])) or "-"
            lines.append(
                f"| {r['axis']} | {r['positive']['p10']:.3f}–{r['positive']['p90']:.3f} | "
                f"{r['contrast']['p10']:.3f}–{r['contrast']['p90']:.3f} | {r['auc_positive_higher']:.3f} | {r['overlap']:.3f} | {tags} |"
            )
    else:
        lines.append("採用ゼロ。この粒度では境界が立たなかった。")
    lines.append("")

    neutral = [r["axis"] for r in report["boundaries"]["scalars"] if not r["separates"]]
    lines += ["## 分離しなかった軸（＝好みの境界ではない／自由に振ってよい）", "", ", ".join(neutral) or "なし", ""]

    blocked = [r for rows in (report["boundaries"]["groups"], report["boundaries"]["scalars"]) for r in rows if r.get("blocking_tags")]
    lines += ["## confoundで採用しなかった軸", ""]
    if blocked:
        lines += ["| 軸 | AUC | 理由 |", "|---|---:|---|"]
        for r in sorted(blocked, key=lambda x: -abs(x["rank_biserial"])):
            lines.append(f"| {r['axis']} | {r['auc_positive_higher']:.3f} | {', '.join(r['blocking_tags'])} |")
    else:
        lines.append("なし")
    lines.append("")

    noted = sorted({t for rows in (report["boundaries"]["groups"], report["boundaries"]["scalars"]) for r in rows for t in r.get("confound_tags", []) if t not in ("loudness_sensitive", "bleed_sensitive")})
    if noted:
        lines += ["## 注記として持ち回るconfound", "", ", ".join(noted), "", "採用は止めないが、レポートでは必ず併記する。", ""]
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description="正例と近接した否定例の境界を連続条件として出す")
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS_ROOT)
    ap.add_argument("--data", type=Path, default=REPO_ROOT / "docs/labs/reference-beat-taste-data.json")
    ap.add_argument("--markdown", type=Path, default=REPO_ROOT / "docs/labs/reference-beat-contrast-draft.md")
    args = ap.parse_args()
    root = args.corpus.expanduser().resolve()
    data = load_json(args.data)
    draft = load_json(root / "analysis/machine-draft.json")
    confound = load_json(root / "analysis/contrast-confound.json", {})
    if not data or not draft:
        print("ERROR: taste-data / machine-draft不足", file=sys.stderr)
        return 2
    songs = data["songs"]
    roles = {vid: s.get("role", "positive") for vid, s in songs.items()}
    names = {vid: s.get("display_name", vid) for vid, s in songs.items()}
    if "contrast" not in set(roles.values()):
        print("ERROR: 否定例がtaste-dataに無い。features生成をやり直す", file=sys.stderr)
        return 2

    rng = _rng()
    report: dict[str, Any] = {
        "schema_revision": 1,
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "feature_schema_revision": data.get("schema_revision"),
        "roles": {"positive": sum(v == "positive" for v in roles.values()), "contrast": sum(v == "contrast" for v in roles.values())},
        "proximity": {},
        "separation": {},
    }
    for view in list(VIEW_SCHEMAS) + ["overall"]:
        block = draft["views"][view] if view in VIEW_SCHEMAS else draft["overall"]
        ids = block["participants"]
        matrix = np.asarray(block["distance_matrix"])
        report["proximity"][view] = proximity(ids, matrix, roles)
        report["separation"][view] = separation(ids, matrix, roles, rng)
        print(f"{view}: AUC={report['separation'][view].get('auc')} p={report['separation'][view].get('p_permutation')}", flush=True)

    view_p = {v: r["p_permutation"] for v, r in report["separation"].items() if "error" not in r}
    view_bh = benjamini_hochberg(view_p)
    for v, r in report["separation"].items():
        if "error" in r:
            continue
        r["bh_significant"] = bool(view_bh.get(v, False))
        r["separates_after_bh"] = bool(r["separates"] and r["bh_significant"])

    print("view単位のomnibus検定（軸をまとめて1回）...", flush=True)
    report["omnibus"] = {}
    for view in VIEW_SCHEMAS:
        result = omnibus_view_test(view, songs, draft["views"][view]["participants"], roles, rng)
        if result:
            report["omnibus"][view] = result
            print(f"  {view}: 軸{result['n_axes']}本 統計量={result['mean_abs_auc_deviation']:.4f} null95%={result['null_p95']:.4f} p={result['p_permutation']:.4f}", flush=True)

    print("境界（feature群・個別feature）を計算中...", flush=True)
    report["boundaries"] = boundaries(songs, draft, roles, rng)

    tags = confound.get("confound_tags", {})
    for rows in (report["boundaries"]["groups"], report["boundaries"]["scalars"]):
        for r in rows:
            hits = sorted({t for path, why in tags.items() if _tag_applies(r["axis"], path) for t in why})
            r["confound_tags"] = hits
            blocking = sorted(set(hits) & BLOCKING_TAGS)
            r["blocking_tags"] = blocking
            if blocking:
                # 測り方の副作用で動くfeatureは、分離が出ても境界候補にしない。
                r["selected"] = False

    distant = sorted({row["video_id"] for block in report["proximity"].values() for row in block.get("contrast", []) if row["distant_candidate"]})
    report["distant_candidates"] = distant
    if distant:
        print(f"遠い候補を除いた再計算: {distant}", flush=True)
        kept = {vid: s for vid, s in songs.items() if vid not in set(distant)}
        trimmed_roles = {k: v for k, v in roles.items() if k not in set(distant)}
        trimmed_draft = {"views": {v: {**b, "participants": [i for i in b["participants"] if i not in set(distant)]} for v, b in draft["views"].items()}}
        report["boundaries_without_distant"] = boundaries(kept, trimmed_draft, trimmed_roles, _rng())

    out = root / "analysis/contrast.json"
    write_json_atomic(out, report)
    args.markdown.parent.mkdir(parents=True, exist_ok=True)
    args.markdown.write_text(render_markdown(report, names))
    print(f"\n出力: {out}\n      {args.markdown}")
    selected = [r["axis"] for rows in (report["boundaries"]["groups"], report["boundaries"]["scalars"]) for r in rows if r["selected"]]
    print(f"採用軸: {len(selected)}件")
    for axis in selected:
        print(f"  {axis}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
