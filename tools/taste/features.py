#!/usr/bin/env python
"""代表区間をview別に選び、正規化前の横断特徴量を生成する。"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import sys
from pathlib import Path
from typing import Any

import librosa
import numpy as np
import soundfile as sf

from common import DEFAULT_CORPUS_ROOT, REPO_ROOT, load_json, write_json_atomic
from provenance import record_root_stage
from schemas import STAGE_SOURCE_PATHS
from schemas import METER_DEPENDENT_ELIGIBLE, VIEW_SCHEMAS
from segments import Candidate, common_bar_candidates, fixed_time_candidates, select_representative_diverse

SR = 22050
HOP = 512
WINDOW_BARS = 8
MIN_ACTIVE_RMS_DB = -48.0
FEATURE_SCHEMA_REVISION = 2


def _slice(path: Path, start: float, end: float, sr: int = SR) -> np.ndarray:
    y, _ = librosa.load(str(path), sr=sr, mono=True, offset=max(0, start), duration=max(0.01, end - start))
    return y


def _sum_slices(paths: list[Path], start: float, end: float) -> np.ndarray:
    ys = [_slice(p, start, end) for p in paths if p.is_file()]
    if not ys:
        return np.zeros(1, dtype=np.float32)
    n = min(map(len, ys))
    return np.sum([y[:n] for y in ys], axis=0)


def _rms_db(y: np.ndarray) -> float:
    return round(float(20 * np.log10(max(float(np.sqrt(np.mean(y * y))), 1e-9))), 4)


def _distribution(v: np.ndarray) -> list[float]:
    v = np.maximum(np.asarray(v, dtype=float), 0)
    s = v.sum()
    return [round(float(x), 6) for x in (v / s if s else np.zeros_like(v))]


def spectrum(y: np.ndarray) -> dict[str, Any]:
    S = np.abs(librosa.stft(y, n_fft=2048, hop_length=HOP))
    power = S * S
    freqs = librosa.fft_frequencies(sr=SR, n_fft=2048)
    bands = [(20, 80), (80, 250), (250, 2000), (2000, 6000), (6000, 11025)]
    energy = [float(power[(freqs >= lo) & (freqs < hi)].sum()) for lo, hi in bands]
    harm, perc = librosa.effects.hpss(y)
    return {
        "band_balance": _distribution(np.array(energy)),
        "centroid_hz": round(float(np.median(librosa.feature.spectral_centroid(S=S, sr=SR))), 3),
        "rolloff95_hz": round(float(np.median(librosa.feature.spectral_rolloff(S=S, sr=SR, roll_percent=0.95))), 3),
        "hpss_ratio": round(float(np.sqrt(np.mean(harm * harm)) / max(np.sqrt(np.mean(perc * perc)), 1e-9)), 6),
        "rms_db": _rms_db(y),
        "onset_rate": round(float(len(librosa.onset.onset_detect(y=y, sr=SR, hop_length=HOP)) / max(len(y) / SR, 1e-9)), 6),
    }


def onset_profile(y: np.ndarray, start: float, first_down: float, beat_s: float, bands: tuple[float, float] | None = None) -> list[float]:
    if bands:
        y = librosa.effects.preemphasis(y) if bands[0] >= 2000 else y
        y = librosa.resample(y, orig_sr=SR, target_sr=SR)  # dtypeを揃えるだけ。filterはmelspec側で行う
    lo, hi = bands if bands else (30, 11000)
    # 狭いlow bandへ64 filterを押し込むと空filterが生まれprofileが歪む。
    width = hi - lo
    n_mels = 3 if width < 200 else (16 if width < 2000 else 32)
    n_fft = 8192 if width < 200 else 2048
    S = librosa.feature.melspectrogram(y=y, sr=SR, n_fft=n_fft, n_mels=n_mels, fmin=lo, fmax=hi)
    env = librosa.onset.onset_strength(S=librosa.power_to_db(S + 1e-12), sr=SR, hop_length=HOP)
    times = start + librosa.frames_to_time(np.arange(len(env)), sr=SR, hop_length=HOP)
    pos = ((times - first_down) / (beat_s / 4.0)) % 16
    profile = np.zeros(16)
    for i in range(16):
        mask = np.minimum((pos - i) % 16, (i - pos) % 16) <= 0.5
        profile[i] = float(env[mask].mean()) if mask.any() else 0
    return _distribution(profile)


def _chord_root(name: str | None) -> int | None:
    if not name:
        return None
    names = {"C": 0, "C#": 1, "D": 2, "D#": 3, "E": 4, "F": 5, "F#": 6, "G": 7, "G#": 8, "A": 9, "A#": 10, "B": 11}
    return names.get(name[:2] if len(name) > 1 and name[1] == "#" else name[:1])


def _best_topline(analysis: Path) -> dict:
    options = []
    for label in ("6s-piano", "6s-guitar", "6s-other", "other"):
        d = load_json(analysis / f"topline-{label}.json", {})
        if d.get("chord_estimate_usable") and d.get("chords"):
            options.append((float(d.get("stem_rms_db", -120)), label, d))
    return max(options, default=(-999, "", {}), key=lambda x: (x[0], x[1]))[2]


def harmony_group(topline: dict, start: float, end: float) -> dict[str, Any] | None:
    chords = [c for c in topline.get("chords", []) if start <= float(c.get("t", -1)) < end and c.get("chord")]
    if len(chords) < 2:
        return None
    roots = [_chord_root(c["chord"]) for c in chords]
    roots = [r for r in roots if r is not None]
    if len(roots) < 2:
        return None
    intervals = [(b - a) % 12 for a, b in zip(roots, roots[1:])]
    suffixes = [c["chord"][2 if len(c["chord"]) > 1 and c["chord"][1] == "#" else 1 :] for c in chords]
    ambiguous = sum(s in {"5", "sus4", "7sus4"} for s in suffixes) / len(suffixes)
    color = sum(any(x in s for x in ("7", "9", "6", "add", "sus")) for s in suffixes) / len(suffixes)
    conf = float(np.mean([c.get("conf", 0) for c in chords]))
    if conf < 0.35:
        return None
    return {
        "root_sequence": roots,
        "root_interval_hist": _distribution(np.bincount(intervals, minlength=12)),
        "change_ratio": round(sum(a != b for a, b in zip(roots, roots[1:])) / max(1, len(roots) - 1), 6),
        "third_ambiguous_ratio": round(ambiguous, 6),
        "color_chord_ratio": round(color, 6),
        "return_ratio": round(sum(r == roots[0] for r in roots[1:]) / max(1, len(roots) - 1), 6),
        "confidence": round(conf, 6),
    }


def hit_character(y: np.ndarray) -> dict[str, Any] | None:
    bands = {"low": (35, 140), "mid": (160, 1800), "high": (5000, 10500)}
    result = {}
    for label, (lo, hi) in bands.items():
        S = librosa.feature.melspectrogram(y=y, sr=SR, n_mels=32, fmin=lo, fmax=hi)
        env = librosa.onset.onset_strength(S=librosa.power_to_db(S + 1e-12), sr=SR, hop_length=HOP)
        frames = librosa.onset.onset_detect(onset_envelope=env, sr=SR, hop_length=HOP, backtrack=False)
        rows = []
        for frame in frames[:200]:
            center = int(frame * HOP)
            seg = y[center : center + int(SR * 0.18)]
            if len(seg) < int(SR * 0.1):
                continue
            peak = float(np.max(np.abs(seg)))
            if peak < 1e-5:
                continue
            seg = seg / peak  # ヒット単位正規化。元の音量を音色距離へ混ぜない。
            spec = np.abs(librosa.stft(seg, n_fft=1024, hop_length=128))
            centroid = float(np.median(librosa.feature.spectral_centroid(S=spec, sr=SR)))
            rolloff = float(np.median(librosa.feature.spectral_rolloff(S=spec, sr=SR, roll_percent=0.95)))
            amp = np.abs(seg)
            pk = int(np.argmax(amp))
            attack = pk / SR * 1000
            decay = float(np.mean(amp[int(SR * 0.1) :]) / max(amp[pk], 1e-9))
            rows.append((centroid, rolloff, attack, decay))
        if len(rows) < 3:
            return None
        a = np.asarray(rows)
        result[label] = {
            "median": [round(float(x), 6) for x in np.median(a, axis=0)],
            "iqr": [round(float(x), 6) for x in (np.percentile(a, 75, axis=0) - np.percentile(a, 25, axis=0))],
            "hit_count": len(rows),
        }
    return result


def bass_motion_group(y: np.ndarray, bpm: float) -> dict[str, float]:
    """ベースが反復する土台か、音高を動かす支えかを表す。"""
    from shared_features import extract_shared

    motion = extract_shared(y, bpm)["pitch_motion_repetition"]
    return {"pitch_motion": motion["pitch_motion"], "repetition": motion["repetition"]}


def _candidate_audio_descriptors(paths: list[Path], candidates: list[Candidate]) -> tuple[list[float], list[list[float]]]:
    scores, descriptors = [], []
    for c in candidates:
        y = _sum_slices(paths, c.start_s, c.end_s)
        spec = spectrum(y)
        score = spec["rms_db"]
        scores.append(score)
        descriptors.append([score, spec["onset_rate"], spec["centroid_hz"], spec["hpss_ratio"]])
    return scores, descriptors


def _segments_for_view(view: str, paths: list[Path], candidates: list[Candidate], topline: dict | None = None) -> list[dict[str, Any]]:
    scores, desc = _candidate_audio_descriptors(paths, candidates)
    if view in {"topline_harmony", "bass_harmony"} and topline:
        for i, c in enumerate(candidates):
            h = harmony_group(topline, c.start_s, c.end_s)
            if not h:
                scores[i] = -120
            else:
                desc[i].extend([h["change_ratio"], h["third_ambiguous_ratio"]])
    eligible = [i for i, score in enumerate(scores) if score >= MIN_ACTIVE_RMS_DB]
    subset = [candidates[i] for i in eligible]
    picks = select_representative_diverse(subset, [scores[i] for i in eligible], [desc[i] for i in eligible], 3)
    result = []
    for local_i, slot in picks:
        i = eligible[local_i]
        c = candidates[i]
        result.append({
            "slot": slot,
            "start_bar": c.start_bar,
            "end_bar": c.end_bar,
            "start_s": round(c.start_s, 6),
            "end_s": round(c.end_s, 6),
            "activity_rms_db": scores[i],
            "selection_reason": "high recurring activity" if slot == "representative" else "within-song diversity",
        })
    return result


def _extract_song(entry: dict[str, Any]) -> dict[str, Any]:
    ref = Path(entry["active_artifact_path"])
    basics = load_json(ref / "analysis/basics.json")
    grid = entry.get("grid", {})
    status = grid.get("grid_status", "unknown")
    paths = {
        "top": [ref / f"stems/htdemucs_6s/track/{x}.wav" for x in ("piano", "guitar", "other")],
        "drums": [ref / "stems/htdemucs/track/drums.wav"],
        "bass": [ref / "stems/htdemucs/track/bass.wav"],
    }
    top_line = _best_topline(ref / "analysis")
    duration = float(basics["duration_sec"])
    meter_ok = status in METER_DEPENDENT_ELIGIBLE
    if meter_ok:
        bar_duration = float(grid.get("bar_duration_s") or 4 * 60 / float(grid["tempo_bpm"]))
        candidates = common_bar_candidates(float(grid["first_downbeat_s"]), bar_duration, int(basics["grid"]["n_bars"]), WINDOW_BARS)
    else:
        candidates = []
    drum_time_candidates = candidates if candidates else fixed_time_candidates(duration)
    selected = {
        "topline_harmony": _segments_for_view("topline_harmony", paths["top"], candidates, top_line) if meter_ok else [],
        "drum_placement": _segments_for_view("drum_placement", paths["drums"], candidates) if meter_ok else [],
        "drum_audio": _segments_for_view("drum_audio", paths["drums"], drum_time_candidates),
        "bass_harmony": _segments_for_view("bass_harmony", paths["bass"], candidates, top_line) if meter_ok else [],
    }
    views: dict[str, Any] = {}
    beat_s = 60.0 / float(grid.get("tempo_bpm") or basics["tempo"]["bpm"])
    first_down = float(grid.get("first_downbeat_s") or basics["grid"]["first_downbeat_sec"])

    def segment_result(view: str, seg: dict[str, Any], groups: dict[str, Any] | None, reason: str | None = None):
        if groups is None:
            return {**seg, "eligible": False, "reason": reason or "required_features_missing"}
        return {**seg, "eligible": True, "groups": groups}

    top_segments = []
    for seg in selected["topline_harmony"]:
        y = _sum_slices(paths["top"], seg["start_s"], seg["end_s"])
        h = harmony_group(top_line, seg["start_s"], seg["end_s"])
        sp = spectrum(y)
        instruments = [p.stem for p in paths["top"] if _rms_db(_slice(p, seg["start_s"], seg["end_s"])) >= MIN_ACTIVE_RMS_DB]
        groups = None if not h else {
            "harmony": h,
            "motion": {"onset_rate": sp["onset_rate"], "repetition": round(1 - min(1, h["change_ratio"]), 6)},
            "instruments": instruments,
            "texture": {k: sp[k] for k in ("band_balance", "centroid_hz", "rolloff95_hz", "hpss_ratio")},
        }
        item = segment_result("topline_harmony", seg, groups)
        if item.get("eligible"):
            from shared_features import extract_shared
            item["comparison_shared"] = extract_shared(y, 60.0 / beat_s)
        top_segments.append(item)
    views["topline_harmony"] = {"eligible": bool(top_segments) and meter_ok, "reason": None if top_segments and meter_ok else ("unsupported_meter" if status == "human_verified_non_4_4" else "unverified_grid" if not meter_ok else "required_features_missing"), "segments": top_segments}

    dp_segments = []
    for seg in selected["drum_placement"]:
        y = _slice(paths["drums"][0], seg["start_s"], seg["end_s"])
        profiles = {label: onset_profile(y, seg["start_s"], first_down, beat_s, band) for label, band in {"low": (35, 140), "mid": (160, 1800), "high": (5000, 10500)}.items()}
        onset_rate = spectrum(y)["onset_rate"]
        dp_segments.append(segment_result("drum_placement", seg, {"profiles": profiles, "timing": {"onset_rate": onset_rate, "swing_ratio": float(load_json(ref / "analysis/groove.json", {}).get("swing", {}).get("ratio", 1.0) or 1.0)}}))
    views["drum_placement"] = {"eligible": bool(dp_segments) and meter_ok, "reason": None if dp_segments and meter_ok else ("unsupported_meter" if status == "human_verified_non_4_4" else "unverified_grid" if not meter_ok else "required_features_missing"), "segments": dp_segments}

    da_segments = []
    for seg in selected["drum_audio"]:
        y = _slice(paths["drums"][0], seg["start_s"], seg["end_s"])
        hit = hit_character(y)
        sp = spectrum(y)
        groups = None if not hit else {"hit_character": hit, "production": {k: sp[k] for k in ("band_balance", "centroid_hz", "rolloff95_hz", "hpss_ratio")}}
        da_segments.append(segment_result("drum_audio", seg, groups, "insufficient_isolated_hits"))
    da_segments = [s for s in da_segments if s["eligible"]]
    views["drum_audio"] = {"eligible": bool(da_segments), "reason": None if da_segments else "required_features_missing", "segments": da_segments}

    bass_segments = []
    for seg in selected["bass_harmony"]:
        y = _slice(paths["bass"][0], seg["start_s"], seg["end_s"])
        h = harmony_group(top_line, seg["start_s"], seg["end_s"])
        if not h:
            bass_segments.append(segment_result("bass_harmony", seg, None))
            continue
        chroma = librosa.feature.chroma_cqt(y=librosa.effects.harmonic(y), sr=SR).mean(axis=1)
        root = h["root_sequence"][0]
        rel = np.roll(chroma, -root)
        prof = onset_profile(y, seg["start_s"], first_down, beat_s)
        sp = spectrum(y)
        groups = {
            "placement": {"profile": prof},
            "pitch_relation": {"relative_pitch_class": _distribution(rel), "root_ratio": round(float(rel[0] / max(rel.sum(), 1e-9)), 6), "third_ratio": round(float((rel[3] + rel[4]) / max(rel.sum(), 1e-9)), 6), "fifth_ratio": round(float(rel[7] / max(rel.sum(), 1e-9)), 6)},
            "density": {"onset_rate": sp["onset_rate"], "rms_db": sp["rms_db"]},
            "motion": bass_motion_group(y, float(grid["tempo_bpm"])),
        }
        bass_segments.append(segment_result("bass_harmony", seg, groups))
    bass_segments = [s for s in bass_segments if s["eligible"]]
    views["bass_harmony"] = {"eligible": bool(bass_segments) and meter_ok, "reason": None if bass_segments and meter_ok else ("unsupported_meter" if status == "human_verified_non_4_4" else "unverified_grid" if not meter_ok else "required_features_missing"), "segments": bass_segments}

    arrangement = load_json(ref / "analysis/arrangement.json", {})
    sections = arrangement.get("sections", [])
    if meter_ok and sections:
        stem_names = ["drums", "bass", "piano", "guitar", "other"]
        total = sum(s["length_bars"] for s in sections) or 1
        occupancy = {name: round(sum(s["length_bars"] for s in sections if name in s["active"]) / total, 6) for name in stem_names}
        seq = [{"length_ratio": round(s["length_bars"] / total, 6), "active": [x for x in s["active"] if x != "vocals"]} for s in sections]
        groups = {"occupancy": occupancy, "shape": {"section_count": len(sections), "mean_length_bars": round(float(np.mean([s["length_bars"] for s in sections])), 6), "reentry_count": sum(sections[i]["active"] == sections[j]["active"] for i in range(len(sections)) for j in range(i))}, "section_sequence": seq}
        views["arrangement"] = {"eligible": True, "reason": None, "groups": groups, "sections": sections}
    else:
        views["arrangement"] = {"eligible": False, "reason": "unsupported_meter" if status == "human_verified_non_4_4" else "unverified_grid"}

    return {
        "video_id": entry["video_id"],
        "display_name": entry["display_name"],
        "grid_status": status,
        "gates": load_json(ref / "analysis/gates.json", {}),
        "views": views,
    }


def _upgrade_bass_motion(entry: dict[str, Any], song: dict[str, Any]) -> dict[str, Any]:
    """既存の選定区間を変えず、schema v2のbass motionだけを差分計算する。"""
    view = song.get("views", {}).get("bass_harmony", {})
    if not view.get("eligible"):
        return song
    ref = Path(entry["active_artifact_path"])
    bass = ref / "stems/htdemucs/track/bass.wav"
    bpm = float(entry["grid"]["tempo_bpm"])
    for segment in view.get("segments", []):
        if not segment.get("eligible"):
            continue
        y = _slice(bass, float(segment["start_s"]), float(segment["end_s"]))
        segment["groups"]["motion"] = bass_motion_group(y, bpm)
    return song


def validate_view_contract(song: dict[str, Any]) -> list[str]:
    errors = []
    for view, schema in VIEW_SCHEMAS.items():
        data = song["views"].get(view, {})
        if not data.get("eligible"):
            if not data.get("reason"):
                errors.append(f"{view}: ineligible reasonなし")
            continue
        samples = data.get("segments") if view != "arrangement" else [data]
        for i, sample in enumerate(samples):
            groups = sample.get("groups", {})
            missing = set(schema["required_groups"]) - set(groups)
            if missing:
                errors.append(f"{view}[{i}]: missing groups {sorted(missing)}")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS_ROOT)
    ap.add_argument("--only")
    ap.add_argument("--upgrade-bass-motion", action="store_true", help="既存区間へschema v2のbass motionだけを追加")
    ap.add_argument("--out", type=Path, default=REPO_ROOT / "docs/labs/reference-beat-taste-data.json")
    args = ap.parse_args()
    root = args.corpus.expanduser().resolve()
    manifest = load_json(root / "manifest.json")
    if not manifest:
        print("ERROR: manifestなし", file=sys.stderr)
        return 2
    existing = load_json(args.out, {"songs": {}})
    songs = dict(existing.get("songs", {}))
    failures = 0
    for vid, entry in manifest["songs"].items():
        if args.only and vid != args.only:
            continue
        if entry.get("status") != "analyzed":
            songs[vid] = {"video_id": vid, "display_name": entry["display_name"], "excluded": True, "reason": f"status:{entry.get('status')}"}
            continue
        try:
            if args.upgrade_bass_motion:
                if vid not in songs:
                    raise ValueError("差分upgrade元のsongがありません。通常のtaste:featuresを実行してください")
                song = _upgrade_bass_motion(entry, songs[vid])
            else:
                song = _extract_song(entry)
            errors = validate_view_contract(song)
            if errors:
                raise ValueError("; ".join(errors))
            songs[vid] = song
            print(f"{vid}: " + ", ".join(f"{v}={int(d['eligible'])}" for v, d in song["views"].items()))
        except Exception as e:
            failures += 1
            songs[vid] = {"video_id": vid, "display_name": entry["display_name"], "excluded": True, "reason": str(e)}
            print(f"FAILED {vid}: {e}", file=sys.stderr)
    output = {"schema_revision": FEATURE_SCHEMA_REVISION, "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(), "window_bars": WINDOW_BARS, "songs": dict(sorted(songs.items()))}
    write_json_atomic(args.out, output)
    taste_prov = load_json(root / "taste-provenance.json", {})
    grid_fp = taste_prov.get("stages", {}).get("grid_audit", {}).get("fingerprint")
    if grid_fp:
        feature_contract = {v: {"meter_dependent": s["meter_dependent"], "required_groups": s["required_groups"]} for v, s in VIEW_SCHEMAS.items()}
        record_root_stage(root, "taste_features", list(STAGE_SOURCE_PATHS["taste_features"]), {"grid_audit": grid_fp}, {"window_bars": WINDOW_BARS, "view_contract": feature_contract}, [args.out])
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
