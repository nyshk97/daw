#!/usr/bin/env python
from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

import numpy as np


@dataclass(frozen=True)
class Candidate:
    start_bar: int | None
    end_bar: int | None
    start_s: float
    end_s: float
    activity: dict[str, float]
    descriptor: tuple[float, ...]


def common_bar_candidates(first_down: float, bar_duration: float, n_bars: int, window_bars: int = 8) -> list[Candidate]:
    """1小節頭から4小節刻みの共通候補プール。intro/outro 1回物を避ける。"""
    result = []
    first = 5 if n_bars >= 24 else 1
    last_start = max(first, n_bars - window_bars - 3)
    for bar in range(first, last_start + 1, 4):
        start = first_down + (bar - 1) * bar_duration
        result.append(Candidate(bar, bar + window_bars - 1, start, start + window_bars * bar_duration, {}, ()))
    return result


def fixed_time_candidates(duration: float, window_s: float = 12.0) -> list[Candidate]:
    if duration <= window_s:
        return [Candidate(None, None, 0.0, duration, {}, ())]
    centers = np.linspace(window_s / 2, duration - window_s / 2, min(9, max(3, int(duration / window_s))))
    return [Candidate(None, None, float(c - window_s / 2), float(c + window_s / 2), {}, ()) for c in centers]


def select_representative_diverse(candidates: list[Candidate], scores: list[float], descriptors: list[Iterable[float]], count: int = 3) -> list[tuple[int, str]]:
    """activity代表2枠＋代表から最も離れた多様性1枠。決定的なtie break。"""
    if not candidates:
        return []
    desc = np.asarray(list(descriptors), dtype=float)
    if desc.ndim == 1:
        desc = desc[:, None]
    order = sorted(range(len(candidates)), key=lambda i: (-float(scores[i]), candidates[i].start_s))
    chosen = order[: min(2, count, len(order))]
    if len(chosen) < count and len(chosen) < len(order):
        scale = np.ptp(desc, axis=0)
        scale[scale < 1e-9] = 1
        norm = (desc - np.median(desc, axis=0)) / scale
        remaining = [i for i in order if i not in chosen]
        diverse = max(remaining, key=lambda i: (min(float(np.linalg.norm(norm[i] - norm[j])) for j in chosen), -candidates[i].start_s))
        chosen.append(diverse)
    return [(i, "diversity" if j == 2 else "representative") for j, i in enumerate(chosen)]
