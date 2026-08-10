#!/usr/bin/env python
"""機械案からPhase 5用のview別短尺WAVとチェックシートを生成する。"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from pathlib import Path
from typing import Any

import librosa
import numpy as np
import soundfile as sf

from common import DEFAULT_CORPUS_ROOT, REPO_ROOT, load_json
from provenance import record_root_stage

TARGET_DRUM_RMS_DB = -18.0


def _safe(value: str) -> str:
    return re.sub(r"[^0-9A-Za-zぁ-んァ-ヶ一-龥ー]+", "-", value).strip("-")[:42]


def _load(path: Path, start: float, end: float, sr: int = 44100) -> np.ndarray:
    y, _ = librosa.load(str(path), sr=sr, mono=False, offset=start, duration=max(0.05, end - start))
    return y.T if y.ndim == 2 else y[:, None]


def _sum(paths: list[Path], start: float, end: float) -> tuple[np.ndarray, int]:
    ys = [_load(p, start, end) for p in paths if p.is_file()]
    n = min(map(len, ys)); ch = max(y.shape[1] for y in ys)
    padded = [np.repeat(y[:n], ch, axis=1) if y.shape[1] == 1 and ch == 2 else y[:n, :ch] for y in ys]
    return np.sum(padded, axis=0), 44100


def _normalize(y: np.ndarray, target_db: float) -> np.ndarray:
    rms = float(np.sqrt(np.mean(y * y))); gain = 10 ** (target_db / 20) / max(rms, 1e-9)
    y = y * gain
    return y / max(1.0, float(np.max(np.abs(y))) / 0.98)


def inspect_wav(path: Path) -> dict[str, float]:
    y, sr = sf.read(str(path), always_2d=True, dtype="float32")
    duration = len(y) / sr; rms = float(np.sqrt(np.mean(y * y))); peak = float(np.max(np.abs(y)))
    if duration < 1 or rms < 1e-5 or peak > 1.0001:
        raise ValueError(f"review WAV不正: {path} duration={duration} rms={rms} peak={peak}")
    return {"duration_s": round(duration, 3), "rms_db": round(20*np.log10(rms), 3), "peak_db": round(20*np.log10(max(peak,1e-9)), 3)}


def _group_selection(ids: list[str], matrix: np.ndarray, labels: dict[str, int]) -> list[tuple[str, str, int]]:
    index = {vid: i for i, vid in enumerate(ids)}
    result = []
    groups = sorted(set(labels.values()))
    for label in groups:
        members = sorted(v for v, g in labels.items() if g == label)
        medoids = sorted(members, key=lambda v: (sum(matrix[index[v], index[o]] for o in members), v))[:2]
        result.extend((v, "representative", label) for v in medoids)
        others = [v for v in ids if labels.get(v) != label]
        if others:
            boundary = min(members, key=lambda v: (min(matrix[index[v], index[o]] for o in others), v))
            result.append((boundary, "boundary", label))
    return result


def _map_selection(ids: list[str], matrix: np.ndarray) -> list[tuple[str, str, int]]:
    """群なしのviewでも連続体／外れ値判断用に地図の中心と両端を3曲出す。"""
    if not ids:
        return []
    mean = matrix.mean(axis=1)
    center_i = min(range(len(ids)), key=lambda i: (float(mean[i]), ids[i]))
    if len(ids) == 1:
        return [(ids[0], "map-center", 0)]
    edge_a, edge_b = max(((i, j) for i in range(len(ids)) for j in range(i + 1, len(ids))), key=lambda ij: (float(matrix[ij]), ids[ij[0]], ids[ij[1]]))
    chosen = []
    for i, role in ((center_i, "map-center"), (edge_a, "map-edge"), (edge_b, "map-edge")):
        if ids[i] not in {x[0] for x in chosen}:
            chosen.append((ids[i], role, 0))
    return chosen


def _accepted_candidate(candidates: list[dict]) -> dict | None:
    """安定性gateを通った群だけを耳確認へ渡す。棄却案は距離地図として確認する。"""
    return next((candidate for candidate in candidates if candidate.get("accepted") is True), None)


def _segment(song: dict, view: str) -> dict:
    segments = song["views"][view].get("segments", [])
    return next((s for s in segments if s.get("slot") == "representative" and s.get("eligible")), segments[0])


def _paths(ref: Path, kind: str) -> list[Path]:
    upper = [ref / f"stems/htdemucs_6s/track/{x}.wav" for x in ("piano", "guitar", "other")]
    if kind == "top": return upper
    if kind == "drums": return [ref / "stems/htdemucs/track/drums.wav"]
    if kind == "bass": return [ref / "stems/htdemucs/track/bass.wav"]
    if kind == "top-bass": return upper + [ref / "stems/htdemucs/track/bass.wav"]
    if kind == "beat": return [ref / f"stems/htdemucs_6s/track/{x}.wav" for x in ("drums", "bass", "piano", "guitar", "other")]
    raise ValueError(kind)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS_ROOT)
    ap.add_argument("--data", type=Path, default=REPO_ROOT / "docs/labs/reference-beat-taste-data.json")
    ap.add_argument("--view", help="誤分類群だけ追加生成するときのview")
    args = ap.parse_args()
    root = args.corpus.expanduser().resolve(); manifest = load_json(root / "manifest.json"); data = load_json(args.data); draft = load_json(root / "analysis/machine-draft.json")
    if not all((manifest, data, draft)):
        print("ERROR: manifest/data/machine-draft不足", file=sys.stderr); return 2
    review = root / "review/phase5"
    if review.exists():
        shutil.rmtree(review)
    review.mkdir(parents=True)
    generated = []
    lines = ["# Phase 5 — 好みの型を耳で確認", "", "同じgroupの代表2曲が同じタイプに聞こえるか、boundaryがどちら寄りかをviewごとに確認します。音量差そのものをdrum audioの差と誤認しないよう、ドラム素材は-18 dBFS RMSへ揃えています。", ""]
    view_kind = {"topline_harmony": ["top"], "drum_placement": ["drums"], "drum_audio": ["drums"], "bass_harmony": ["top-bass", "bass"], "arrangement": ["beat"]}
    for view, kinds in view_kind.items():
        if args.view and view != args.view: continue
        vr = draft["views"][view]; cand = _accepted_candidate(vr["candidates"])
        lines += [f"## {view}", "", "- [ ] group内の共通説明が聴感と合う", "- [ ] boundaryの寄り先／別タイプを記録", ""]
        ids = vr["participants"]; matrix = np.asarray(vr["distance_matrix"])
        if not cand:
            lines += ["群候補なし。連続体／外れ値でよいかを距離地図の中心1曲＋両端2曲で確認します。", ""]
            selections = _map_selection(ids, matrix)
        else:
            selections = _group_selection(ids, matrix, cand["labels"])
        seen = set()
        for vid, role, group in selections:
            for kind in kinds:
                key = (view, vid, kind, role, group)
                if key in seen: continue
                seen.add(key)
                song = data["songs"][vid]; entry = manifest["songs"][vid]; ref = Path(entry["active_artifact_path"])
                if view == "arrangement":
                    sections = song["views"][view]["sections"]
                    pieces = []
                    bar_s = float(entry["grid"]["bar_duration_s"]); first = float(entry["grid"]["first_downbeat_s"])
                    for section in sections[:8]:
                        boundary = first + (section["bar_start"] - 1) * bar_s
                        piece, sr = _sum(_paths(ref, "beat"), max(0, boundary - bar_s), boundary + bar_s)
                        pieces += [piece, np.zeros((int(sr * .5), piece.shape[1]))]
                    y = np.concatenate(pieces[:-1]); start, end = 0.0, len(y)/sr
                    timerange = "digest"
                else:
                    seg = _segment(song, view); start, end = seg["start_s"], seg["end_s"]
                    y, sr = _sum(_paths(ref, kind), start, end); timerange = f"{start:.2f}-{end:.2f}"
                if kind == "drums": y = _normalize(y, TARGET_DRUM_RMS_DB)
                else: y = y / max(1.0, float(np.max(np.abs(y))) / .98)
                name = f"{view}-a1-g{group}-{role}-{_safe(entry['display_name'])}-{kind}-{timerange}.wav"
                out = review / name; sf.write(str(out), y, sr, subtype="PCM_16")
                stats = inspect_wav(out); generated.append({"path": name, "view": view, "video_id": vid, "role": role, "group": group, "source_time": timerange, **stats})
                lines.append(f"- [ ] group {group} / {role}: `{name}` — 元時刻 {timerange}")
        lines.append("")
    # overallは同じ選定をボーカル抜きbeatで生成。
    ov = draft["overall"]; cand = _accepted_candidate(ov["candidates"])
    lines += ["## overall（ボーカル抜きビート）", "", "- [ ] 5 viewを合わせた総合候補として納得する", ""]
    if ov["participants"]:
        ids = ov["participants"]; matrix = np.asarray(ov["distance_matrix"])
        selections = _group_selection(ids, matrix, cand["labels"]) if cand else _map_selection(ids, matrix)
        if not cand:
            lines += ["総合群候補なし。地図の中心と両端で、1群の連続体として妥当かを確認します。", ""]
        for vid, role, group in selections:
            song = data["songs"][vid]; entry = manifest["songs"][vid]; ref = Path(entry["active_artifact_path"])
            # overall用時刻はtopline代表、無ければdrum audio代表。
            base_view = "topline_harmony" if song["views"]["topline_harmony"].get("segments") else "drum_audio"
            seg = _segment(song, base_view); start, end = seg["start_s"], seg["end_s"]
            y, sr = _sum(_paths(ref, "beat"), start, end); y = y / max(1.0, float(np.max(np.abs(y))) / .98)
            name = f"overall-a1-g{group}-{role}-{_safe(entry['display_name'])}-beat-{start:.2f}-{end:.2f}.wav"; out = review/name
            sf.write(str(out), y, sr, subtype="PCM_16"); stats = inspect_wav(out); generated.append({"path": name, "view": "overall", "video_id": vid, "role": role, "group": group, "source_time": f"{start:.2f}-{end:.2f}", **stats})
            lines.append(f"- [ ] group {group} / {role}: `{name}` — 元時刻 {start:.2f}-{end:.2f}")
    (review / "README.md").write_text("\n".join(lines) + "\n")
    (review / "manifest.json").write_text(json.dumps({"schema_revision": 1, "files": generated}, ensure_ascii=False, indent=2) + "\n")
    taste_prov = load_json(root / "taste-provenance.json", {})
    parent = taste_prov.get("stages", {}).get("distance_cluster", {}).get("fingerprint")
    if parent:
        record_root_stage(root, "review_materials", ["tools/taste/review.py"], {"distance_cluster": parent}, {"target_drum_rms_db": TARGET_DRUM_RMS_DB}, [review / "manifest.json", review / "README.md"] + sorted(review.glob("*.wav")))
    print(f"review WAV={len(generated)}: {review}")
    return 0


if __name__ == "__main__": raise SystemExit(main())
