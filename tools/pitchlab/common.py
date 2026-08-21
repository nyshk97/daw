"""pitchlab 共通: 入出力・音程変換・カーブ JSON。

カーブの単位は全スクリプトで統一する:
- hop = 5ms（48kHz なら 240 サンプル）。f0[Hz]（無声=0）・voiced(0/1)・prob(有声度 0..1)・rms
- フレーム k の中心時刻 = k * hop / sr（フレーム境界は WAV 先頭基準＝C++ の PitchCurve と同じ約束）
"""
from __future__ import annotations

import json
from dataclasses import dataclass, asdict, field
from pathlib import Path

import numpy as np
import soundfile as sf

HERE = Path(__file__).parent
WORK = HERE / "work"
LISTEN = HERE / "listen"
ANSWERS = HERE / "../../docs/labs/reference-beat-human-answers"
HOP_MS = 5.0
FMIN, FMAX = 60.0, 1000.0  # ボーカル（ラップの低い語尾〜サビの高音）を覆う範囲


def hop_samples(sr: int) -> int:
    return int(round(sr * HOP_MS / 1000.0))


def load_mono(path: Path) -> tuple[np.ndarray, int]:
    """ステレオは Mid（L+R の平均）で解析する（plan: 両chに同じ変換を掛ける前提）"""
    x, sr = sf.read(str(path), dtype="float32", always_2d=True)
    return x.mean(axis=1).astype(np.float32), int(sr)


def write_wav(path: Path, x: np.ndarray, sr: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), np.clip(x, -1.0, 1.0), sr, subtype="PCM_24")


def hz_to_midi(f):
    f = np.asarray(f, dtype=np.float64)
    with np.errstate(divide="ignore"):
        m = 69.0 + 12.0 * np.log2(np.where(f > 0, f, np.nan) / 440.0)
    return m


def midi_to_hz(m):
    return 440.0 * 2.0 ** ((np.asarray(m, dtype=np.float64) - 69.0) / 12.0)


@dataclass
class Curve:
    sr: int
    hop: int
    f0: list[float]          # Hz, 無声は 0
    voiced: list[int]        # 0/1
    prob: list[float]        # 有声度 0..1
    rms: list[float]
    algo: str = ""
    meta: dict = field(default_factory=dict)

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(asdict(self)))

    @staticmethod
    def load(path: Path) -> "Curve":
        return Curve(**json.loads(path.read_text()))

    def times(self) -> np.ndarray:
        return np.arange(len(self.f0)) * self.hop / self.sr

    def midi(self) -> np.ndarray:
        return hz_to_midi(self.f0)


def frame_rms(x: np.ndarray, hop: int, n: int) -> np.ndarray:
    """フレーム中心 ±hop の RMS（長さ n）"""
    pad = np.pad(x, (hop, hop))
    out = np.zeros(n)
    for k in range(n):
        seg = pad[k * hop : k * hop + 2 * hop]
        out[k] = np.sqrt(np.mean(seg * seg)) if len(seg) else 0.0
    return out
