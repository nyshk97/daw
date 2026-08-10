#!/usr/bin/env python
"""Phase 5の人間確認で判明した不明点だけ、追加試聴素材へする。"""
from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

import numpy as np
import soundfile as sf

from common import DEFAULT_CORPUS_ROOT, REPO_ROOT, load_json
from review import TARGET_DRUM_RMS_DB, _normalize, _paths, _safe, _segment, _sum, inspect_wav


FOLLOWUP_SPECS = [
    {
        "view": "topline_harmony", "video_id": "6LbPC4ZWFok", "kinds": ["top"],
        "role": "unique-group1-member",
        "reason": "group 1 boundaryがLOVE representativeと重複したため、未試聴のgroup 1曲を補う",
    },
    {
        "view": "drum_audio", "video_id": "0eCFLpkrqSw", "kinds": ["drums"],
        "role": "made-machine-neighbor",
        "reason": "Made my dayの機械距離1位。耳でMade型が複数曲に再現するか確認",
    },
    {
        "view": "drum_audio", "video_id": "AH7tY4nIcp0", "kinds": ["drums"],
        "role": "made-machine-neighbor",
        "reason": "Made my dayの機械距離2位。耳でMade型が複数曲に再現するか確認",
    },
    {
        "view": "drum_audio", "video_id": "8YqiaNYwoMY", "kinds": ["drums"],
        "role": "memory-machine-neighbor",
        "reason": "memory triggerの機械距離1位。中心側の音響型が再現するか確認",
    },
    {
        "view": "drum_audio", "video_id": "O0L9rJdbFGE", "kinds": ["drums"],
        "role": "memory-machine-neighbor",
        "reason": "memory triggerの機械距離2位。中心側の音響型が再現するか確認",
    },
    {
        "view": "bass_harmony", "video_id": "O0L9rJdbFGE", "kinds": ["bass", "top-bass"],
        "role": "ambiguous-machine-neighbor",
        "reason": "シグナル距離1位かつLOVE距離2位。現featureが人間の2型を分けられるか確認",
    },
    {
        "view": "bass_harmony", "video_id": "nE1nzTE5ESA", "kinds": ["bass", "top-bass"],
        "role": "ambiguous-machine-neighbor",
        "reason": "LOVE距離1位かつシグナル距離2位。現featureが人間の2型を分けられるか確認",
    },
    {
        "view": "bass_harmony", "video_id": "MC6jsEPYzIE", "kinds": ["bass", "top-bass"],
        "role": "repetition-side-neighbor",
        "reason": "夏の魔法'18の機械距離1位。反復する土台型が再現するか確認",
    },
    {
        "view": "bass_harmony", "video_id": "HIZzYz1xk18", "kinds": ["bass", "top-bass"],
        "role": "repetition-side-neighbor",
        "reason": "夏の魔法'18の機械距離2位。反復する土台型が再現するか確認",
    },
]


def expected_file_count() -> int:
    return 1 + sum(len(spec["kinds"]) for spec in FOLLOWUP_SPECS)


def _write(out: Path, y: np.ndarray, sr: int, meta: dict) -> dict:
    sf.write(str(out), y, sr, subtype="PCM_16")
    return {"path": out.name, **meta, **inspect_wav(out)}


def _fall_timeline(root: Path, manifest: dict, data: dict, outdir: Path) -> dict:
    video_id = "3umJ9yXFwho"
    entry = manifest["songs"][video_id]
    song = data["songs"][video_id]
    ref = Path(entry["active_artifact_path"])
    bar_s = float(entry["grid"]["bar_duration_s"])
    first = float(entry["grid"]["first_downbeat_s"])
    total_bars = max(section["bar_end"] for section in song["views"]["arrangement"]["sections"])
    bars = sorted(set(int(round(value)) for value in np.linspace(1, max(1, total_bars - 1), 8)))
    pieces = []
    source_ranges = []
    sr = 44100
    for bar in bars:
        start = first + (bar - 1) * bar_s
        end = min(first + total_bars * bar_s, start + 2 * bar_s)
        piece, sr = _sum(_paths(ref, "beat"), start, end)
        pieces += [piece, np.zeros((int(sr * 0.5), piece.shape[1]))]
        source_ranges.append({"bar": bar, "start_s": round(start, 3), "end_s": round(end, 3)})
    y = np.concatenate(pieces[:-1])
    y = y / max(1.0, float(np.max(np.abs(y))) / 0.98)
    out = outdir / "arrangement-fall-in-love-fixed-timeline-8x2bars.wav"
    return _write(out, y, sr, {
        "view": "arrangement", "video_id": video_id, "role": "fixed-timeline",
        "reason": "section検出が2区間しか出さなかったため、全曲を等間隔8地点×2小節で再確認",
        "source_ranges": source_ranges,
    })


def generate(corpus: Path, data_path: Path) -> tuple[Path, list[dict]]:
    manifest = load_json(corpus / "manifest.json")
    data = load_json(data_path)
    if not manifest or not data:
        raise ValueError("manifestまたはtaste dataがありません")
    outdir = corpus / "review/phase5-followup"
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)

    files = [_fall_timeline(corpus, manifest, data, outdir)]
    lines = [
        "# Phase 5 — 追加確認素材", "",
        "初回耳確認で機械案と食い違った箇所だけを補う。全曲総当たりには戻らない。", "",
        "- arrangement: FALL IN LOVEを等間隔8地点×2小節で再確認",
        "- topline: 重複したgroup 1 boundaryの代わりに未試聴のOneを確認",
        "- drum audio: Made側／memory側の機械近傍が耳でも同型か確認",
        "- bass: 人間が感じた2型を現featureの近傍曲で再確認", "",
        "## arrangement — TEN'S UNIQUE - FALL IN LOVE", "",
        "理由: section検出が2区間しか出なかったため、全曲を等間隔8地点×2小節で再確認", "",
        "- `arrangement-fall-in-love-fixed-timeline-8x2bars.wav`", "",
    ]
    for spec in FOLLOWUP_SPECS:
        video_id = spec["video_id"]
        entry = manifest["songs"][video_id]
        song = data["songs"][video_id]
        ref = Path(entry["active_artifact_path"])
        seg = _segment(song, spec["view"])
        start, end = float(seg["start_s"]), float(seg["end_s"])
        lines += [f"## {spec['view']} — {entry['display_name']}", "", f"理由: {spec['reason']}", ""]
        for kind in spec["kinds"]:
            y, sr = _sum(_paths(ref, kind), start, end)
            if kind == "drums":
                y = _normalize(y, TARGET_DRUM_RMS_DB)
            else:
                y = y / max(1.0, float(np.max(np.abs(y))) / 0.98)
            name = f"{spec['view']}-{spec['role']}-{_safe(entry['display_name'])}-{kind}-{start:.2f}-{end:.2f}.wav"
            meta = {
                "view": spec["view"], "video_id": video_id, "role": spec["role"],
                "kind": kind, "reason": spec["reason"], "source_time": f"{start:.2f}-{end:.2f}",
            }
            files.append(_write(outdir / name, y, sr, meta))
            lines.append(f"- `{name}`")
        lines.append("")
    (outdir / "README.md").write_text("\n".join(lines) + "\n")
    (outdir / "manifest.json").write_text(json.dumps({"schema_revision": 1, "files": files}, ensure_ascii=False, indent=2) + "\n")
    return outdir, files


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS_ROOT)
    parser.add_argument("--data", type=Path, default=REPO_ROOT / "docs/labs/reference-beat-taste-data.json")
    args = parser.parse_args()
    outdir, files = generate(args.corpus.expanduser().resolve(), args.data.expanduser().resolve())
    print(f"followup WAV={len(files)}: {outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
