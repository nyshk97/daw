from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
from pathlib import Path
from typing import Any

SCHEMA_REVISION = 1
DEFAULT_CORPUS_ROOT = Path.home() / "Music/daw/reference-beat-corpus"
REPO_ROOT = Path(__file__).resolve().parents[2]
PICKS_PATH = REPO_ROOT / "docs/labs/reference-beat-picks.md"


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(chunk_size):
            h.update(chunk)
    return h.hexdigest()


def stable_hash(value: Any) -> str:
    raw = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(raw).hexdigest()


def load_json(path: Path, default: Any = None) -> Any:
    if not path.exists():
        return default
    return json.loads(path.read_text())


def write_json_atomic(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(value, f, ensure_ascii=False, indent=2)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


def canonical_youtube_url(video_id: str) -> str:
    return f"https://www.youtube.com/watch?v={video_id}"


YOUTUBE_RE = re.compile(
    r"https?://(?:www\.)?(?:youtube\.com/(?:watch\?[^\s)]*?v=|shorts/)|youtu\.be/)"
    r"([A-Za-z0-9_-]{11})(?:[^\s)]*)?"
)


def video_id_from_url(url: str) -> str | None:
    m = YOUTUBE_RE.search(url)
    return m.group(1) if m else None


def audio_info(path: Path) -> dict[str, Any]:
    import soundfile as sf

    info = sf.info(str(path))
    return {
        "sha256": sha256_file(path),
        "bytes": path.stat().st_size,
        "duration_s": round(info.frames / info.samplerate, 6),
        "sample_rate": info.samplerate,
        "channels": info.channels,
        "frames": info.frames,
    }


def validate_audio(path: Path, min_duration: float = 10.0) -> dict[str, Any]:
    import numpy as np
    import soundfile as sf

    info = sf.info(str(path))
    if info.frames / info.samplerate < min_duration:
        raise ValueError(f"音声が短すぎる: {path} ({info.frames / info.samplerate:.2f}s)")
    # 全読み込みを避け、先頭・中央・末尾の合計30秒で非無音を確認する。
    points = [0, max(0, info.frames // 2 - info.samplerate * 5), max(0, info.frames - info.samplerate * 10)]
    chunks = []
    with sf.SoundFile(str(path)) as f:
        for p in points:
            f.seek(p)
            chunks.append(f.read(min(info.samplerate * 10, info.frames - p), dtype="float32", always_2d=True))
    rms = float(np.sqrt(np.mean(np.concatenate(chunks) ** 2)))
    if not np.isfinite(rms) or rms < 1e-5:
        raise ValueError(f"音声が無音: {path} (RMS={rms})")
    return {**audio_info(path), "sample_rms": round(rms, 8)}
