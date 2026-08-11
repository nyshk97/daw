#!/usr/bin/env python
"""境界分析の前に、「好みの差と誤認してはいけない差」を洗い出して封じる。

正例23曲はボーカル入り完成曲をDemucsで剥がした残り、否定例10曲は元からインストのtype beat。
この非対称（ボーカル残留・マスタリング音圧）が効くfeatureを先に特定し、分離が出ても
境界として採用しないためのタグを付ける。

測るのは3つ。

(a) loudness依存: 同じ音へgainを掛けて各featureがどれだけ動くか
(b) ボーカルbleed感度: 否定例へ実ボーカルを混ぜてDemucsし直し、非ボーカルstemのfeatureが動く量
(c) 区間選定バイアス: 代表区間の選ばれ方がroleで系統的に違わないか
"""
from __future__ import annotations

import argparse
import datetime as dt
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf

import features as F
from common import DEFAULT_CORPUS_ROOT, REPO_ROOT, load_json, write_json_atomic
from distances import value_distance
from schemas import VIEW_SCHEMAS

# --- 分析前に固定する判定条件 --------------------------------------------------
# gainは実運用で起こりうる幅。-12dBは控えめなラフマスター、+6dBは詰めた商用マスターの側。
GAINS_DB = (-12.0, 6.0)
# schema距離で0.02。view距離の採用差（0.1〜0.4オーダー）に対して十分小さく、
# 数値誤差（1e-6オーダー）よりは十分大きい水準。
SENSITIVE_TOL = 0.02
# bleedは正例内のfeature分散と比べる。IQRの0.5を超える系統差は境界の根拠にしない。
BLEED_IQR_RATIO = 0.5
# 区間選定バイアスの申告閾値。role間のmedian差がプール全体のIQRのこれを超えたら併記する。
SELECTION_IQR_RATIO = 0.5
LOUDNESS_SAMPLE_SONGS = 6
# bleedのprobe条件。ボーカルRMSをbeat RMSに対して-6dB／0dBへ合わせる。
# ラップのミックスで実際に起こる比率の幅を、控えめ側と標準側で挟む。
BLEED_PROBE_SONGS = 3
BLEED_GAINS_DB = (-6.0, 0.0)


def _gain(y: np.ndarray, db: float) -> np.ndarray:
    return (y * (10.0 ** (db / 20.0))).astype(np.float32)


def _flatten(prefix: str, value: Any, out: dict[str, tuple[str, Any]]) -> None:
    """leaf featureを `group.key` のpathへ落とす。距離計算に使うkey名は保持する。"""
    if isinstance(value, dict):
        for k, v in value.items():
            _flatten(f"{prefix}.{k}" if prefix else k, v, out)
        return
    out[prefix] = (prefix.rsplit(".", 1)[-1], value)


def _deviation(a: Any, b: Any, key: str) -> float | None:
    try:
        return float(value_distance(a, b, key))
    except (TypeError, ValueError):
        return None


# --- 各viewのgroupsを音から作り直す（features.pyの関数をそのまま使う） -----------

def _recompute_topline(y: np.ndarray, ctx: dict[str, Any]) -> dict[str, Any]:
    sp = F.spectrum(y)
    return {
        "motion": {"onset_rate": sp["onset_rate"]},
        "texture": {k: sp[k] for k in ("band_balance", "centroid_hz", "rolloff95_hz", "hpss_ratio")},
    }


def _recompute_drum_placement(y: np.ndarray, ctx: dict[str, Any]) -> dict[str, Any]:
    profiles = {
        label: F.onset_profile(y, ctx["start_s"], ctx["first_down"], ctx["beat_s"], band)
        for label, band in {"low": (35, 140), "mid": (160, 1800), "high": (5000, 10500)}.items()
    }
    return {"profiles": profiles, "timing": {"onset_rate": F.spectrum(y)["onset_rate"]}, "drum_shape": F.drum_shape(profiles, 4.0 * ctx["beat_s"])}


def _recompute_drum_audio(y: np.ndarray, ctx: dict[str, Any]) -> dict[str, Any] | None:
    hit = F.hit_character(y)
    if not hit:
        return None
    sp = F.spectrum(y)
    return {"hit_character": hit, "production": {k: sp[k] for k in ("band_balance", "centroid_hz", "rolloff95_hz", "hpss_ratio")}}


def _recompute_bass(y: np.ndarray, ctx: dict[str, Any]) -> dict[str, Any]:
    sp = F.spectrum(y)
    return {
        "placement": {"profile": F.onset_profile(y, ctx["start_s"], ctx["first_down"], ctx["beat_s"])},
        "density": {"onset_rate": sp["onset_rate"], "rms_db": sp["rms_db"]},
        "motion": F.bass_motion_group(y, ctx["bpm"]),
    }


AUDIO_VIEWS: dict[str, dict[str, Any]] = {
    "topline_harmony": {"stems": "top", "recompute": _recompute_topline},
    "drum_placement": {"stems": "drums", "recompute": _recompute_drum_placement},
    "drum_audio": {"stems": "drums", "recompute": _recompute_drum_audio},
    "bass_harmony": {"stems": "bass", "recompute": _recompute_bass},
}
# harmony群はtopline JSONのコード推定から作るので音のgainでは動かない。
# instruments群は在不在のmulti-hotで、活性判定のRMS閾値経由でのみgainに依存する。
NON_AUDIO_GROUPS = {"topline_harmony": ["harmony", "instruments"], "bass_harmony": ["pitch_relation"], "drum_placement": ["timing.swing_ratio"]}


def _stem_paths(ref: Path) -> dict[str, list[Path]]:
    top = [ref / f"stems/htdemucs_6s/track/{x}.wav" for x in ("piano", "guitar", "other")]
    drums6 = [ref / "stems/htdemucs_6s/track/drums.wav"]
    bass6 = [ref / "stems/htdemucs_6s/track/bass.wav"]
    return {
        "top": top,
        "drums": [ref / "stems/htdemucs/track/drums.wav"],
        "bass": [ref / "stems/htdemucs/track/bass.wav"],
        "drums6s": drums6,
        "bass6s": bass6,
        "beat": drums6 + bass6 + top,
    }


def _context(entry: dict[str, Any], ref: Path, seg: dict[str, Any]) -> dict[str, Any]:
    basics = load_json(ref / "analysis/basics.json", {})
    grid = entry.get("grid", {})
    bpm = float(grid.get("tempo_bpm") or basics.get("tempo", {}).get("bpm") or 90.0)
    return {
        "start_s": seg["start_s"],
        "beat_s": 60.0 / bpm,
        "bpm": bpm,
        "first_down": float(grid.get("first_downbeat_s") or basics.get("grid", {}).get("first_downbeat_sec") or 0.0),
    }


# --- (a) loudness依存 ----------------------------------------------------------

def audit_loudness(manifest: dict[str, Any], data: dict[str, Any], sample: int) -> dict[str, Any]:
    songs = [
        (vid, s) for vid, s in sorted(data["songs"].items())
        if not s.get("excluded") and any(s["views"].get(v, {}).get("segments") for v in AUDIO_VIEWS)
    ]
    if not songs:
        return {"error": "対象曲なし"}
    step = max(1, len(songs) // sample)
    picked = songs[::step][:sample]
    per_feature: dict[str, list[float]] = {}
    unmeasurable: list[str] = []
    for vid, song in picked:
        entry = manifest["songs"][vid]
        ref = Path(entry["active_artifact_path"])
        paths = _stem_paths(ref)
        for view, spec in AUDIO_VIEWS.items():
            segments = song["views"].get(view, {}).get("segments") or []
            if not segments:
                continue
            seg = segments[0]
            ctx = _context(entry, ref, seg)
            base_audio = F._sum_slices(paths[spec["stems"]], seg["start_s"], seg["end_s"])
            reference = spec["recompute"](base_audio, ctx)
            if reference is None:
                unmeasurable.append(f"{vid}/{view}")
                continue
            flat_ref: dict[str, tuple[str, Any]] = {}
            _flatten("", reference, flat_ref)
            for db in GAINS_DB:
                moved = spec["recompute"](_gain(base_audio, db), ctx)
                if moved is None:
                    unmeasurable.append(f"{vid}/{view}@{db}dB")
                    continue
                flat_moved: dict[str, tuple[str, Any]] = {}
                _flatten("", moved, flat_moved)
                for path, (key, ref_value) in flat_ref.items():
                    if path not in flat_moved:
                        continue
                    d = _deviation(ref_value, flat_moved[path][1], key)
                    if d is not None:
                        per_feature.setdefault(f"{view}.{path}", []).append(d)
    result = {
        path: {"max_deviation": round(max(values), 6), "n_observations": len(values), "sensitive": max(values) > SENSITIVE_TOL}
        for path, values in sorted(per_feature.items())
    }
    return {
        "gains_db": list(GAINS_DB),
        "tolerance": SENSITIVE_TOL,
        "songs": [vid for vid, _ in picked],
        "unmeasurable": sorted(set(unmeasurable)),
        "features": result,
        "non_audio_groups": NON_AUDIO_GROUPS,
    }


# --- (b) ボーカルbleed感度 ------------------------------------------------------

def _groups_for_windows(paths: dict[str, list[Path]], windows: dict[str, tuple[float, float]], ctx: dict[str, Any]) -> dict[str, Any]:
    """同じ窓・同じ抽出器で、全viewのgroupsを作る。baselineとprobeの差だけを見るため。"""
    out: dict[str, Any] = {}
    for view, spec in AUDIO_VIEWS.items():
        if view not in windows:
            continue
        start, end = windows[view]
        local = {**ctx, "start_s": start}
        groups = spec["recompute"](F._sum_slices(paths[spec["stems"]], start, end), local)
        if groups is not None:
            out[view] = groups
    if "vocal_space" in windows:
        start, end = windows["vocal_space"]
        groups = F.vocal_space_group(
            F._sum_slices(paths["beat"], start, end),
            F._sum_slices(paths["drums6s"], start, end),
            F._sum_slices(paths["bass6s"], start, end),
            F._sum_slices(paths["top"], start, end),
        )
        if groups is not None:
            out["vocal_space"] = groups
    return out


def _run_demucs(wav: Path, outdir: Path) -> dict[str, list[Path]]:
    py = REPO_ROOT / "tools/reference/.venv/bin/python"
    for model in ("htdemucs", "htdemucs_6s"):
        proc = subprocess.run(
            [str(py), "-m", "demucs", "-d", "mps", "-n", model, "-o", str(outdir), str(wav)],
            cwd=REPO_ROOT / "tools/reference", capture_output=True, text=True,
        )
        if proc.returncode:
            raise RuntimeError(f"demucs {model} 失敗: {proc.stderr[-400:]}")
    base4 = outdir / "htdemucs" / wav.stem
    base6 = outdir / "htdemucs_6s" / wav.stem
    top = [base6 / f"{x}.wav" for x in ("piano", "guitar", "other")]
    return {
        "top": top,
        "drums": [base4 / "drums.wav"],
        "bass": [base4 / "bass.wav"],
        "drums6s": [base6 / "drums.wav"],
        "bass6s": [base6 / "bass.wav"],
        "beat": [base6 / "drums.wav", base6 / "bass.wav"] + top,
    }


def audit_bleed_by_injection(manifest: dict[str, Any], data: dict[str, Any], songs_n: int, gains_db: tuple[float, ...]) -> dict[str, Any]:
    """否定例へ実際のボーカルを混ぜてDemucsし直し、非ボーカルstemのfeatureがどれだけ動くか測る。

    正例はボーカル入り完成曲を剥がした残り、否定例は元からインスト。この非対称そのものを
    再現するので、区間の音楽的な違いとbleedを取り違えない。
    """
    contrast = [
        (vid, s) for vid, s in sorted(data["songs"].items())
        if s.get("role") == "contrast" and not s.get("excluded")
    ]
    donors = [
        vid for vid, s in sorted(data["songs"].items())
        if s.get("role") == "positive" and not s.get("excluded")
        and (Path(manifest["songs"][vid]["active_artifact_path"]) / "stems/htdemucs_6s/track/vocals.wav").is_file()
    ]
    if not contrast or not donors:
        return {"error": "対照曲またはdonorボーカルなし"}
    step = max(1, len(contrast) // songs_n)
    picked = contrast[::step][:songs_n]

    probe_root = DEFAULT_CORPUS_ROOT / "analysis/bleed-probe"
    per_feature: dict[str, list[float]] = {}
    trials: list[dict[str, Any]] = []
    try:
        for i, (vid, song) in enumerate(picked):
            entry = manifest["songs"][vid]
            ref = Path(entry["active_artifact_path"])
            windows = {}
            for view in list(AUDIO_VIEWS) + ["vocal_space"]:
                segments = song["views"].get(view, {}).get("segments") or []
                seg = next((s for s in segments if s.get("eligible")), None)
                if seg:
                    windows[view] = (seg["start_s"], seg["end_s"])
            if not windows:
                continue
            ctx = _context(entry, ref, {"start_s": 0.0})
            baseline = _groups_for_windows(_stem_paths(ref), windows, ctx)

            donor = donors[i % len(donors)]
            donor_ref = Path(manifest["songs"][donor]["active_artifact_path"])
            beat = F._slice(ref / "track.wav", 0.0, 10_000.0)
            vocal = F._slice(donor_ref / "stems/htdemucs_6s/track/vocals.wav", 0.0, 10_000.0)
            n = min(len(beat), len(vocal))
            if n < F.SR * 30:
                continue
            beat_rms = max(float(np.sqrt(np.mean(beat[:n] ** 2))), 1e-9)
            vocal_rms = max(float(np.sqrt(np.mean(vocal[:n] ** 2))), 1e-9)
            for db in gains_db:
                scale = (beat_rms / vocal_rms) * (10 ** (db / 20.0))
                mixed = np.clip(beat[:n] + vocal[:n] * scale, -1.0, 1.0).astype(np.float32)
                work = probe_root / f"{vid}_{int(db)}"
                if work.exists():
                    shutil.rmtree(work)
                work.mkdir(parents=True)
                wav = work / "track.wav"
                sf.write(str(wav), mixed, F.SR)
                probe_paths = _run_demucs(wav, work)
                probed = _groups_for_windows(probe_paths, windows, ctx)
                moved = 0
                for view, groups in baseline.items():
                    if view not in probed:
                        continue
                    flat_a: dict[str, tuple[str, Any]] = {}
                    flat_b: dict[str, tuple[str, Any]] = {}
                    _flatten("", groups, flat_a)
                    _flatten("", probed[view], flat_b)
                    for path, (key, a) in flat_a.items():
                        if path not in flat_b:
                            continue
                        d = _deviation(a, flat_b[path][1], key)
                        if d is not None:
                            per_feature.setdefault(f"{view}.{path}", []).append(d)
                            moved += 1
                trials.append({"video_id": vid, "donor": donor, "vocal_gain_db": db, "features_compared": moved})
                shutil.rmtree(work, ignore_errors=True)
    finally:
        shutil.rmtree(probe_root, ignore_errors=True)

    spread = _corpus_spread(data)
    features = {}
    for path, values in sorted(per_feature.items()):
        median = float(np.median(values))
        iqr = spread.get(path)
        ratio = None if not iqr else round(median / iqr, 6)
        features[path] = {
            "median_shift": round(median, 6),
            "n_trials": len(values),
            "corpus_iqr": None if iqr is None else round(iqr, 6),
            "shift_over_iqr": ratio,
            "sensitive": bool(ratio is not None and ratio > BLEED_IQR_RATIO),
        }
    return {
        "method": "否定例へ実ボーカルを混ぜてDemucsし直し、同じ窓・同じ抽出器でfeatureの移動量を測る",
        "criterion": {"shift_over_iqr": BLEED_IQR_RATIO},
        "vocal_gains_db": list(gains_db),
        "trials": trials,
        "features": features,
    }


def _bar_windows(sections: list[dict[str, Any]], first_down: float, bar_s: float, want_vocals: bool, window_bars: int) -> list[tuple[float, float]]:
    out = []
    for section in sections:
        has_vocals = "vocals" in section.get("active", [])
        if has_vocals != want_vocals:
            continue
        length = int(section.get("length_bars", 0))
        if length < window_bars:
            continue
        start_bar = int(section["bar_start"])
        start = first_down + (start_bar - 1) * bar_s
        out.append((start, start + window_bars * bar_s))
    return out


def audit_bleed(manifest: dict[str, Any], data: dict[str, Any]) -> dict[str, Any]:
    """正例だけで、ボーカルが鳴っている区間と鳴っていない区間の同じfeatureを比べる。

    否定例には残留が無いので、ここで動くfeatureは正例側だけに乗る系統差になる。
    """
    per_feature: dict[str, list[float]] = {}
    measured, skipped = [], []
    for vid, song in sorted(data["songs"].items()):
        entry = manifest["songs"].get(vid, {})
        if song.get("excluded") or entry.get("role") != "positive":
            continue
        grid = entry.get("grid", {})
        if not grid.get("bar_duration_s"):
            skipped.append({"video_id": vid, "reason": "no_bar_grid"})
            continue
        ref = Path(entry["active_artifact_path"])
        sections = load_json(ref / "analysis/arrangement.json", {}).get("sections", [])
        bar_s = float(grid["bar_duration_s"])
        first = float(grid["first_downbeat_s"])
        with_v = _bar_windows(sections, first, bar_s, True, F.WINDOW_BARS)
        without_v = _bar_windows(sections, first, bar_s, False, F.WINDOW_BARS)
        if not with_v or not without_v:
            skipped.append({"video_id": vid, "reason": "no_paired_windows", "with_vocals": len(with_v), "without_vocals": len(without_v)})
            continue
        paths = _stem_paths(ref)
        song_moved = False
        for view, spec in AUDIO_VIEWS.items():
            ctx_seed = {"start_s": with_v[0][0]}
            ctx = _context(entry, ref, ctx_seed)
            groups = {}
            for label, windows in (("with", with_v), ("without", without_v)):
                start, end = windows[0]
                ctx["start_s"] = start
                y = F._sum_slices(paths[spec["stems"]], start, end)
                groups[label] = spec["recompute"](y, ctx)
            if groups["with"] is None or groups["without"] is None:
                continue
            flat_a: dict[str, tuple[str, Any]] = {}
            flat_b: dict[str, tuple[str, Any]] = {}
            _flatten("", groups["with"], flat_a)
            _flatten("", groups["without"], flat_b)
            for path, (key, a) in flat_a.items():
                if path not in flat_b:
                    continue
                d = _deviation(a, flat_b[path][1], key)
                if d is not None:
                    per_feature.setdefault(f"{view}.{path}", []).append(d)
                    song_moved = True
        # vocal_spaceは正例のbleedを一番受けやすいので必ず測る。
        vs = song["views"].get("vocal_space", {})
        if vs.get("segments"):
            ctx = _context(entry, ref, {"start_s": with_v[0][0]})
            pair = {}
            for label, windows in (("with", with_v), ("without", without_v)):
                start, end = windows[0]
                pair[label] = F.vocal_space_group(
                    F._sum_slices(paths["beat"], start, end),
                    F._sum_slices(paths["drums6s"], start, end),
                    F._sum_slices(paths["bass6s"], start, end),
                    F._sum_slices(paths["top"], start, end),
                )
            if pair["with"] and pair["without"]:
                flat_a, flat_b = {}, {}
                _flatten("", pair["with"], flat_a)
                _flatten("", pair["without"], flat_b)
                for path, (key, a) in flat_a.items():
                    d = _deviation(a, flat_b[path][1], key)
                    if d is not None:
                        per_feature.setdefault(f"vocal_space.{path}", []).append(d)
                        song_moved = True
        if song_moved:
            measured.append(vid)

    spread = _corpus_spread(data)
    features = {}
    for path, values in sorted(per_feature.items()):
        median = float(np.median(values))
        iqr = spread.get(path)
        ratio = None if not iqr else round(median / iqr, 6)
        features[path] = {
            "median_shift": round(median, 6),
            "n_songs": len(values),
            "corpus_iqr": None if iqr is None else round(iqr, 6),
            "shift_over_iqr": ratio,
            "sensitive": bool(ratio is not None and ratio > BLEED_IQR_RATIO),
        }
    return {"criterion": {"shift_over_iqr": BLEED_IQR_RATIO}, "measured_songs": measured, "skipped": skipped, "features": features}


def _corpus_spread(data: dict[str, Any]) -> dict[str, float]:
    """各leaf featureの、正例内でのばらつき（曲間distanceのIQR）。bleed差の物差しにする。"""
    collected: dict[str, list[Any]] = {}
    for song in data["songs"].values():
        if song.get("excluded"):
            continue
        for view, view_data in song["views"].items():
            segments = view_data.get("segments") or ([view_data] if view_data.get("groups") else [])
            for seg in segments[:1]:
                groups = dict(seg.get("groups", {}))
                # 保存済みprofileから導く派生featureも分母を持たせる。
                # 無いとshift_over_iqrがNoneになり、検証していないのに「感応なし」に見える。
                if view == "drum_placement" and "profiles" in groups:
                    bars = (seg.get("end_bar") or 0) - (seg.get("start_bar") or 0) + 1
                    bar_s = (seg["end_s"] - seg["start_s"]) / bars if bars > 0 else None
                    groups["drum_shape"] = F.drum_shape(groups["profiles"], bar_s)
                flat: dict[str, tuple[str, Any]] = {}
                _flatten("", groups, flat)
                for path, (_, value) in flat.items():
                    collected.setdefault(f"{view}.{path}", []).append(value)
    spread = {}
    for path, values in collected.items():
        key = path.rsplit(".", 1)[-1]
        pairs = [
            d for i in range(len(values)) for j in range(i + 1, len(values))
            if (d := _deviation(values[i], values[j], key)) is not None
        ]
        if len(pairs) >= 3:
            spread[path] = float(np.percentile(pairs, 75) - np.percentile(pairs, 25))
    return spread


# --- (c) 区間選定バイアス --------------------------------------------------------

def audit_selection(manifest: dict[str, Any], data: dict[str, Any]) -> dict[str, Any]:
    rows: dict[str, dict[str, list[float]]] = {}
    reasons: dict[str, dict[str, int]] = {}
    for vid, song in data["songs"].items():
        role = manifest["songs"].get(vid, {}).get("role", "positive")
        if song.get("excluded"):
            continue
        duration = float(load_json(Path(manifest["songs"][vid]["active_artifact_path"]) / "analysis/basics.json", {}).get("duration_sec") or 0)
        for view, view_data in song["views"].items():
            for seg in view_data.get("segments") or []:
                bucket = rows.setdefault(view, {}).setdefault(role, [])
                bucket.append(seg["activity_rms_db"])
                if duration > 0:
                    rows.setdefault(f"{view}.position", {}).setdefault(role, []).append(round(seg["start_s"] / duration, 6))
                key = f"{view}:{seg.get('selection_reason', '?')}"
                reasons.setdefault(key, {}).setdefault(role, 0)
                reasons[key][role] += 1
    result = {}
    for metric, by_role in sorted(rows.items()):
        pool = [v for values in by_role.values() for v in values]
        iqr = float(np.percentile(pool, 75) - np.percentile(pool, 25)) if len(pool) >= 4 else None
        medians = {role: round(float(np.median(v)), 6) for role, v in by_role.items()}
        gap = abs(medians.get("positive", 0.0) - medians.get("contrast", 0.0)) if len(medians) == 2 else None
        result[metric] = {
            "medians": medians,
            "counts": {role: len(v) for role, v in by_role.items()},
            "pool_iqr": None if iqr is None else round(iqr, 6),
            "median_gap": None if gap is None else round(gap, 6),
            "biased": bool(gap is not None and iqr not in (None, 0) and gap / iqr > SELECTION_IQR_RATIO),
        }
    return {"criterion": {"median_gap_over_pool_iqr": SELECTION_IQR_RATIO}, "metrics": result, "selection_reasons": reasons}


def main() -> int:
    ap = argparse.ArgumentParser(description="境界分析の前にloudness・vocal bleed・区間選定バイアスを監査する")
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS_ROOT)
    ap.add_argument("--data", type=Path, default=REPO_ROOT / "docs/labs/reference-beat-taste-data.json")
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--sample", type=int, default=LOUDNESS_SAMPLE_SONGS)
    ap.add_argument("--bleed-songs", type=int, default=BLEED_PROBE_SONGS)
    ap.add_argument("--skip", nargs="*", default=[], choices=["loudness", "bleed", "selection"])
    args = ap.parse_args()
    root = args.corpus.expanduser().resolve()
    manifest = load_json(root / "manifest.json")
    data = load_json(args.data)
    if not manifest or not data:
        print("ERROR: manifest/taste-data不足", file=sys.stderr)
        return 2

    out = args.out or (root / "analysis/contrast-confound.json")
    # --skipしたセクションは前回の結果を引き継ぐ。高価な測定を消さずに一部だけ回し直せる。
    previous = load_json(out, {})
    report: dict[str, Any] = {
        "schema_revision": 1,
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "feature_schema_revision": data.get("schema_revision"),
    }
    for key in ("loudness", "bleed", "bleed_section_diff_unused", "selection"):
        section = key.split("_")[0]
        if section in args.skip and key in previous:
            report[key] = previous[key]
            report.setdefault("carried_over", []).append(key)
    if "loudness" not in args.skip:
        print("(a) loudness依存を測定中...", flush=True)
        report["loudness"] = audit_loudness(manifest, data, args.sample)
    if "bleed" not in args.skip:
        print("(b) ボーカルbleed感度を測定中（否定例へボーカルを混ぜてDemucsし直す）...", flush=True)
        report["bleed"] = audit_bleed_by_injection(manifest, data, args.bleed_songs, BLEED_GAINS_DB)
        # 区間ベースの旧測定は、ボーカル有無の差と区間そのものの音楽的な差を分離できない。
        # 判定には使わないが、記録として残す。
        report["bleed_section_diff_unused"] = audit_bleed(manifest, data)
    if "selection" not in args.skip:
        print("(c) 区間選定バイアスを集計中...", flush=True)
        report["selection"] = audit_selection(manifest, data)

    tags: dict[str, list[str]] = {}
    for path, row in report.get("loudness", {}).get("features", {}).items():
        if row["sensitive"]:
            tags.setdefault(path, []).append("loudness_sensitive")
    for path, row in report.get("bleed", {}).get("features", {}).items():
        if row["sensitive"]:
            tags.setdefault(path, []).append("bleed_sensitive")
    for metric, row in report.get("selection", {}).get("metrics", {}).items():
        if row["biased"]:
            tags.setdefault(metric, []).append("selection_biased")
    report["confound_tags"] = {k: sorted(v) for k, v in sorted(tags.items())}

    write_json_atomic(out, report)
    print(f"\n出力: {out}")
    print(f"confoundタグ付きfeature: {len(report['confound_tags'])}件")
    for path, why in report["confound_tags"].items():
        print(f"  {path}: {', '.join(why)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
