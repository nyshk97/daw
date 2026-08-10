#!/usr/bin/env python
"""Grid監査READMEのNG/?だけ、聞き分けやすい再確認素材へ変換する。"""
from __future__ import annotations

import argparse
import importlib.util
import re
from pathlib import Path

import numpy as np
import soundfile as sf

from common import DEFAULT_CORPUS_ROOT, load_json, validate_audio


def _load_grid_audit():
    path = Path(__file__).with_name("grid-audit.py")
    spec = importlib.util.spec_from_file_location("taste_grid_audit", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"grid-audit.pyを読み込めません: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GRID_AUDIT = _load_grid_audit()
HEADING = re.compile(r"^## \[(?P<mark>x|NG|\?)\].*\(`(?P<video_id>[^`]+)`\)$")
ANSWER = re.compile(r"^- 回答: (?P<answer>.+)$")
TRACK_GAIN = 10 ** (-7.0 / 20.0)
CLICK_GAIN = 2.4


def parse_marks(readme: Path) -> dict[str, str]:
    marks: dict[str, str] = {}
    for line in readme.read_text().splitlines():
        match = HEADING.match(line)
        if match:
            marks[match.group("video_id")] = match.group("mark")
    return marks


def parse_reviews(readme: Path) -> dict[str, dict[str, str]]:
    reviews: dict[str, dict[str, str]] = {}
    current: str | None = None
    for line in readme.read_text().splitlines():
        heading = HEADING.match(line)
        if heading:
            current = heading.group("video_id")
            reviews[current] = {"mark": heading.group("mark")}
            continue
        answer = ANSWER.match(line)
        if current and answer:
            reviews[current]["answer"] = answer.group("answer").strip()
    return reviews


def review_modes(readme: Path) -> dict[str, str]:
    modes: dict[str, str] = {}
    for video_id, review in parse_reviews(readme).items():
        answer = review.get("answer")
        if answer == "NG":
            modes[video_id] = "NG"
        elif answer is None and review["mark"] in {"NG", "?"}:
            modes[video_id] = review["mark"]
    return modes


def _windows(duration: float, beat_s: float):
    for label, center in (("head", duration * 0.12), ("middle", duration * 0.5), ("tail", duration * 0.86)):
        start = max(0.0, center - 8 * beat_s)
        yield label, start, min(duration, start + 16 * beat_s)


def _loud_mix(y: np.ndarray, sr: int, start: float, end: float, first_down: float, beat_s: float) -> np.ndarray:
    # first_downを整数拍ずらしても低いclickの時刻列は同じで、高い小節頭だけが回転する。
    clicks = GRID_AUDIT._mix_click(np.zeros_like(y), sr, start, end, first_down, beat_s)
    i0, i1 = int(start * sr), min(len(y), int(end * sr))
    return np.clip(np.asarray(y[i0:i1]) * TRACK_GAIN + clicks * CLICK_GAIN, -1.0, 1.0)


def write_clip(path: Path, audio: np.ndarray, sr: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), audio, sr, subtype="PCM_16")
    validate_audio(path, min_duration=2.0)


def generate(corpus: Path, annotations: Path, out_root: Path | None = None) -> tuple[int, int]:
    manifest = load_json(corpus / "manifest.json")
    selected = review_modes(annotations)
    unknown = sorted(set(selected) - set(manifest.get("songs", {})))
    if unknown:
        raise ValueError(f"manifestにないvideo ID: {', '.join(unknown)}")

    out_root = out_root or corpus / "review/grid-followup"
    lines = [
        "# Grid監査 — 再確認",
        "",
        "曲を-7 dBへ下げ、clickを約+7.6 dBにしています。低いclickは拍、高いclickは小節頭候補です。",
        "",
        "- `?`: 3本とも聞き、4/4で合えば `[x]`、合わなければ `[NG]`、まだ不明なら `[?]`。",
        "- `NG`: A〜Dはclickの速さが同じで、高い音の位置だけ1拍ずつ違います。最も自然な案を1つ選びます。",
        "- A〜Dのどれも曲中でずれる場合は `drift`。高い音だけ合わない場合は最も自然な `A/B/C/D`。",
        "",
    ]
    clips = 0
    for video_id, mark in selected.items():
        entry = manifest["songs"][video_id]
        grid = entry["grid"]
        track = Path(entry["active_artifact_path"]) / "track.wav"
        y, sr = sf.read(str(track), dtype="float32", always_2d=False)
        if y.ndim == 2:
            y = y.mean(axis=1)
        duration = len(y) / sr
        beat_s = 60.0 / float(grid["tempo_bpm"])
        first_down = float(grid["first_downbeat_s"])
        lines += [f"## [{mark}] {entry['display_name']} (`{video_id}`)", ""]
        if mark == "?":
            for label, start, end in _windows(duration, beat_s):
                rel = Path(video_id) / "louder" / f"{label}-{start:.2f}-{end:.2f}.wav"
                write_clip(out_root / rel, _loud_mix(y, sr, start, end, first_down, beat_s), sr)
                clips += 1
            lines += [f"- clips: `{video_id}/louder/`", "- 回答: x / NG / ?", ""]
        else:
            for offset, variant in enumerate("ABCD"):
                for label, start, end in _windows(duration, beat_s):
                    rel = Path(video_id) / f"variant-{variant}" / f"{label}-{start:.2f}-{end:.2f}.wav"
                    shifted_down = first_down + offset * beat_s
                    write_clip(out_root / rel, _loud_mix(y, sr, start, end, shifted_down, beat_s), sr)
                    clips += 1
            lines += [f"- clips: `{video_id}/variant-A/` 〜 `variant-D/`", "- 回答: A / B / C / D / drift / ?", ""]
    (out_root / "README.md").write_text("\n".join(lines))
    return len(selected), clips


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS_ROOT)
    parser.add_argument("--annotations", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    corpus = args.corpus.expanduser().resolve()
    annotations = (args.annotations or corpus / "review/grid/README.md").expanduser().resolve()
    output = args.output.expanduser().resolve() if args.output else corpus / "review/grid-followup"
    songs, clips = generate(corpus, annotations, output)
    print(f"generated songs={songs} clips={clips} output={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
