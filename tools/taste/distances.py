from __future__ import annotations

import math
from collections.abc import Sequence
from typing import Any

import numpy as np


def jensen_shannon(a: Sequence[float], b: Sequence[float]) -> float:
    a, b = np.maximum(np.asarray(a, float), 0), np.maximum(np.asarray(b, float), 0)
    sa, sb = float(a.sum()), float(b.sum())
    if sa == 0 and sb == 0:
        return 0.0
    if sa == 0 or sb == 0:
        return 1.0
    a, b = a / sa, b / sb
    m = (a + b) / 2
    def kl(x):
        mask = x > 0
        return float(np.sum(x[mask] * np.log2(x[mask] / np.maximum(m[mask], 1e-12))))
    return float(np.sqrt(max(0, (kl(a) + kl(b)) / 2)))


def jaccard(a: Sequence[str], b: Sequence[str]) -> float:
    aa, bb = set(a), set(b)
    if not aa and not bb:
        return 0.0
    return 1 - len(aa & bb) / len(aa | bb)


def circular_pitch_distance(a: float, b: float) -> float:
    d = abs(float(a) - float(b)) % 12
    return min(d, 12 - d) / 6.0


def dtw_distance(a: Sequence[Any], b: Sequence[Any], element_distance, cyclic: bool = False) -> float:
    if not a and not b:
        return 0.0
    if not a or not b:
        return 1.0

    def run(x, y):
        dp = np.full((len(x) + 1, len(y) + 1), np.inf)
        dp[0, 0] = 0
        for i in range(1, len(x) + 1):
            for j in range(1, len(y) + 1):
                dp[i, j] = element_distance(x[i - 1], y[j - 1]) + min(dp[i - 1, j], dp[i, j - 1], dp[i - 1, j - 1])
        return float(dp[-1, -1] / max(len(x), len(y)))

    if not cyclic:
        return run(a, b)
    return min(run(a, list(b[k:]) + list(b[:k])) for k in range(len(b)))


def section_distance(a: dict, b: dict) -> float:
    return 0.65 * jaccard(a.get("active", []), b.get("active", [])) + 0.35 * min(1, abs(float(a.get("length_ratio", 0)) - float(b.get("length_ratio", 0))) * 5)


DISTRIBUTION_KEYS = {"band_balance", "profile", "profiles", "root_interval_hist", "relative_pitch_class", "median", "iqr"}
SEQUENCE_KEYS = {"root_sequence"}


def value_distance(a: Any, b: Any, key: str = "") -> float:
    if isinstance(a, bool) or isinstance(b, bool):
        if not isinstance(a, bool) or not isinstance(b, bool):
            raise TypeError(f"{key}: 型不一致")
        return float(a != b)
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if not math.isfinite(float(a)) or not math.isfinite(float(b)):
            raise ValueError(f"{key}: 非finite")
        # 正値の大きい尺度はlog1pし、対称相対差で0..1にclip。
        aa, bb = float(a), float(b)
        if min(aa, bb) >= 0 and max(abs(aa), abs(bb)) > 2:
            aa, bb = math.log1p(aa), math.log1p(bb)
        return min(1.0, abs(aa - bb) / max(1.0, abs(aa), abs(bb)))
    if isinstance(a, list) and isinstance(b, list):
        if all(isinstance(x, str) for x in a + b):
            return jaccard(a, b)
        if key in SEQUENCE_KEYS:
            return dtw_distance(a, b, circular_pitch_distance, cyclic=True)
        if key == "section_sequence":
            return dtw_distance(a, b, section_distance, cyclic=False)
        if all(isinstance(x, (int, float)) for x in a + b):
            if len(a) != len(b):
                raise ValueError(f"{key}: 固定長分布の長さ不一致")
            return jensen_shannon(a, b) if key in DISTRIBUTION_KEYS or len(a) in {5, 12, 16} else float(np.mean(np.minimum(1, np.abs(np.asarray(a) - np.asarray(b)) / np.maximum(1, np.maximum(np.abs(a), np.abs(b))))))
        raise TypeError(f"{key}: 未定義list型")
    if isinstance(a, dict) and isinstance(b, dict):
        if set(a) != set(b):
            raise ValueError(f"{key}: 固定feature集合不一致 {sorted(set(a)^set(b))}")
        if not a:
            return 0.0
        return float(np.mean([value_distance(a[k], b[k], k) for k in sorted(a)]))
    raise TypeError(f"{key}: schema未定義型 {type(a).__name__}/{type(b).__name__}")


def group_distances(a: dict[str, Any], b: dict[str, Any], weights: dict[str, float]) -> dict[str, float]:
    if set(a) != set(weights) or set(b) != set(weights):
        raise ValueError(f"required group不一致: a={sorted(a)} b={sorted(b)} schema={sorted(weights)}")
    return {g: value_distance(a[g], b[g], g) for g in sorted(weights)}


def weighted_group_distance(a: dict[str, Any], b: dict[str, Any], weights: dict[str, float], omit: str | None = None) -> tuple[float, dict[str, float]]:
    parts = group_distances(a, b, weights)
    used = {g: w for g, w in weights.items() if g != omit}
    total = sum(used.values())
    return sum(parts[g] * w for g, w in used.items()) / total, parts


def symmetric_nearest_set_distance(samples_a: list[dict], samples_b: list[dict], weights: dict[str, float], omit: str | None = None) -> tuple[float, dict[str, float]]:
    matrix = []
    contributions = []
    for a in samples_a:
        row = []
        crow = []
        for b in samples_b:
            d, parts = weighted_group_distance(a, b, weights, omit)
            row.append(d)
            crow.append(parts)
        matrix.append(row)
        contributions.append(crow)
    arr = np.asarray(matrix)
    nearest_a = [(i, int(np.argmin(arr[i]))) for i in range(arr.shape[0])]
    nearest_b = [(int(np.argmin(arr[:, j])), j) for j in range(arr.shape[1])]
    pairs = nearest_a + nearest_b
    distance = float(np.mean([arr[i, j] for i, j in pairs]))
    parts = {g: float(np.mean([contributions[i][j][g] for i, j in pairs])) for g in weights}
    return distance, parts
