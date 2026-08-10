from __future__ import annotations

from typing import Any

import librosa
import numpy as np

from features import HOP, SR, _distribution, spectrum


def extract_shared(y: np.ndarray, bpm: float, bars: int | None = None) -> dict[str, Any]:
    """reference上モノとlibrary loopを同じ条件で測る共有subset抽出器。"""
    y = librosa.to_mono(y) if y.ndim == 2 else np.asarray(y)
    harm = librosa.effects.harmonic(y, margin=3)
    chroma = librosa.feature.chroma_cqt(y=harm, sr=SR, hop_length=HOP)
    frames_per_bar = max(1, int(round((4 * 60 / bpm) * SR / HOP)))
    roots, third_clarity = [], []
    for i in range(0, chroma.shape[1], frames_per_bar):
        c = chroma[:, i : i + frames_per_bar].mean(axis=1)
        if c.sum() <= 1e-9:
            continue
        root = int(np.argmax(c))
        roots.append(root)
        rel = np.roll(c, -root)
        third_clarity.append(float(max(rel[3], rel[4]) / max(rel.sum(), 1e-9)))
    intervals = [(b - a) % 12 for a, b in zip(roots, roots[1:])]
    norm = chroma / (np.linalg.norm(chroma, axis=0, keepdims=True) + 1e-9)
    pitch_motion = float(np.mean(np.linalg.norm(np.diff(norm, axis=1), axis=0))) if chroma.shape[1] > 1 else 0
    lag = min(frames_per_bar * 4, max(1, chroma.shape[1] // 2))
    repetition = float(np.mean(np.sum(norm[:, :-lag] * norm[:, lag:], axis=0))) if chroma.shape[1] > lag else 0
    sp = spectrum(y)
    return {
        "harmony": {
            "root_interval_hist": _distribution(np.bincount(intervals, minlength=12)),
            "root_change_ratio": round(sum(a != b for a, b in zip(roots, roots[1:])) / max(1, len(roots) - 1), 6),
            "third_clarity": round(float(np.mean(third_clarity)) if third_clarity else 0, 6),
        },
        "pitch_motion_repetition": {"pitch_motion": round(pitch_motion, 6), "repetition": round(repetition, 6)},
        "onset_rhythm": {"onset_rate": sp["onset_rate"]},
        "spectrum_texture": {k: sp[k] for k in ("band_balance", "centroid_hz", "rolloff95_hz", "hpss_ratio")},
    }
