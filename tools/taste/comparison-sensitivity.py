#!/usr/bin/env python
"""library loopへ混入を加えDemucs前後の共有feature感度を固定基準で評価する。"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import librosa
import numpy as np
import soundfile as sf
from scipy.stats import spearmanr

from common import DEFAULT_CORPUS_ROOT, REPO_ROOT, load_json, write_json_atomic
from distances import value_distance
from provenance import record_root_stage
from shared_features import extract_shared

SCALAR_RHO = 0.8
SCALAR_MAE_IQR = 0.25
DIST_MEDIAN = 0.15
DIST_P90 = 0.35
GROUPS_REQUIRED = 3


def select_eight(entries: list[dict[str, Any]]) -> list[dict[str, Any]]:
    candidates = [e for e in entries if e.get("kind") == "loop" and not e.get("is_contrast") and e.get("features")]
    if len(candidates) < 8:
        candidates = [e for e in entries if e.get("kind") == "loop" and e.get("features")]
    cents = np.array([e["features"]["spectral_centroid_hz_median"] for e in candidates])
    onsets = np.array([e["features"]["onset_rate_per_s"] for e in candidates])
    cm, om = float(np.median(cents)), float(np.median(onsets))
    result = []
    for ch, oh in ((False, False), (False, True), (True, False), (True, True)):
        bucket = [e for e in candidates if (e["features"]["spectral_centroid_hz_median"] >= cm) == ch and (e["features"]["onset_rate_per_s"] >= om) == oh]
        bucket.sort(key=lambda e: (abs(e["features"]["spectral_centroid_hz_median"] - cm) + 200 * abs(e["features"]["onset_rate_per_s"] - om), e["path"]), reverse=True)
        result.extend(bucket[:2])
    return result


def _fit(y: np.ndarray, n: int) -> np.ndarray:
    if not len(y):
        return np.zeros(n)
    return np.tile(y, int(np.ceil(n / len(y))))[:n]


def _stem_contaminants(manifest: dict, n: int, offset: float = 20.0) -> dict[str, np.ndarray]:
    entry = next(e for e in manifest["songs"].values() if e.get("status") == "analyzed")
    ref = Path(entry["active_artifact_path"])
    stems = {}
    for name in ("drums", "bass", "vocals"):
        model = "htdemucs" if name in {"drums", "bass"} else "htdemucs_6s"
        y, _ = librosa.load(str(ref / f"stems/{model}/track/{name}.wav"), sr=22050, mono=True, offset=offset)
        stems[name] = _fit(y, n)
    return stems


def _mix_at(clean: np.ndarray, other: np.ndarray, relative_db: float) -> np.ndarray:
    cr = np.sqrt(np.mean(clean * clean)); oor = np.sqrt(np.mean(other * other))
    gain = cr * 10 ** (relative_db / 20) / max(oor, 1e-9)
    mix = clean + other * gain
    peak = max(1.0, float(np.max(np.abs(mix))))
    return mix / peak


def _flatten(d: dict, prefix: str = "") -> dict[str, Any]:
    out = {}
    for k, v in d.items():
        key = f"{prefix}.{k}" if prefix else k
        if isinstance(v, dict): out.update(_flatten(v, key))
        else: out[key] = v
    return out


def evaluate(rows: list[dict[str, Any]]) -> dict[str, Any]:
    features = sorted(set.intersection(*[set(_flatten(r["clean"])) for r in rows]))
    result, accepted = {}, []
    for key in features:
        group = key.split(".")[0]
        clean = [_flatten(r["clean"])[key] for r in rows]
        dirty = [_flatten(r["separated"])[key] for r in rows]
        if all(isinstance(x, (int, float)) for x in clean + dirty):
            rho = float(spearmanr(clean, dirty).statistic) if len(set(clean)) > 1 and len(set(dirty)) > 1 else 1.0
            mae = float(np.median(np.abs(np.asarray(clean) - np.asarray(dirty))))
            iqr = float(np.subtract(*np.percentile(clean, [75, 25])))
            ok = rho >= SCALAR_RHO and mae <= SCALAR_MAE_IQR * max(iqr, 1e-9)
            metrics = {"rho": round(rho, 6), "median_absolute_error": round(mae, 6), "clean_iqr": round(iqr, 6)}
        else:
            distances = [value_distance(a, b, key.split(".")[-1]) for a, b in zip(clean, dirty)]
            med, p90 = float(np.median(distances)), float(np.percentile(distances, 90))
            ok = med <= DIST_MEDIAN and p90 <= DIST_P90
            metrics = {"distance_median": round(med, 6), "distance_p90": round(p90, 6)}
        result[key] = {"group": group, "accepted": ok, **metrics}
        if ok: accepted.append(key)
    stable_groups = sorted({result[k]["group"] for k in accepted})
    return {"features": result, "comparison_required_features": accepted, "stable_groups": stable_groups, "comparison_supported": len(stable_groups) >= GROUPS_REQUIRED}


def _select_flat(d: dict[str, Any], keys: list[str]) -> dict[str, Any]:
    flat = _flatten(d)
    return {k: flat[k] for k in keys}


def compare_library_to_corpus(result: dict[str, Any], corpus_root: Path) -> dict[str, Any] | None:
    if not result["comparison_supported"]:
        return None
    data = load_json(REPO_ROOT / "docs/labs/reference-beat-taste-data.json")
    draft = load_json(corpus_root / "analysis/machine-draft.json")
    if not data or not draft:
        return None
    keys = result["comparison_required_features"]
    unique_loops = {}
    for row in result["rows"]:
        unique_loops.setdefault(row["loop"], _select_flat(row["clean"], keys))
    corpus = {}
    for vid, song in data["songs"].items():
        segments = song.get("views", {}).get("topline_harmony", {}).get("segments", [])
        values = [_select_flat(s["comparison_shared"], keys) for s in segments if s.get("eligible") and s.get("comparison_shared")]
        if values:
            corpus[vid] = values
    def dist(a, b):
        return float(np.mean([value_distance(a[k], b[k], k.split(".")[-1]) for k in keys]))
    nearest = {}
    for vid, samples in corpus.items():
        rows = []
        for path, loop in unique_loops.items():
            # 曲内2〜3窓は平均featureにせず、loopから最も近い実在窓で比較。最終的に1曲1票。
            rows.append((min(dist(s, loop) for s in samples), path))
        nearest[vid] = [{"loop": p, "distance": round(d, 6)} for d, p in sorted(rows)[:3]]
    candidate = next((c for c in draft["views"]["topline_harmony"]["candidates"] if c.get("accepted")), None)
    groups = {}
    if candidate:
        for label in sorted(set(candidate["labels"].values())):
            members = [v for v, g in candidate["labels"].items() if g == label and v in corpus]
            per_feature = {}
            for key in keys:
                cv = [_flatten(corpus[v][0])[key] if key not in corpus[v][0] else corpus[v][0][key] for v in members]
                lv = [x[key] for x in unique_loops.values()]
                if all(isinstance(x, (int, float)) for x in cv + lv):
                    cmed, lmed = float(np.median(cv)), float(np.median(lv))
                    pooled = max(float(np.subtract(*np.percentile(cv + lv, [75, 25]))), 1e-9)
                    per_feature[key] = {"corpus_median": round(cmed, 6), "library_median": round(lmed, 6), "standardized_median_difference": round((cmed-lmed)/pooled, 6)}
                else:
                    ds = [value_distance(a, b, key.split(".")[-1]) for a in cv for b in lv]
                    per_feature[key] = {"cross_set_distance_median": round(float(np.median(ds)), 6)}
            groups[str(label)] = {"corpus_members": members, "corpus_n": len(members), "library_n": len(unique_loops), "features": per_feature}
    return {"schema_revision": 1, "required_features": keys, "corpus_n": len(corpus), "library_n": len(unique_loops), "groups": groups, "nearest_library_loops": nearest}


def comparison_markdown(result: dict[str, Any]) -> str:
    comp = result.get("corpus_library_comparison")
    lines = ["# 現行ループライブラリとの上モノ比較 — 機械ドラフト", "", "同じ共有抽出器を通し、Demucs混入感度の固定基準を通ったfeatureだけを比較する。日本／海外の因果ではなく、この23曲の参加曲と現行Cymatics系素材の差として読む。", "", f"- sensitivity通過群: {', '.join(result['stable_groups']) or 'なし'}", f"- 比較可否: {result['comparison_supported']}", ""]
    if not comp:
        lines += ["安定feature群が3群未満のため、距離比較は根拠不足として生成しない。", ""]
        return "\n".join(lines)
    lines += [f"- corpus参加: {comp['corpus_n']}曲（各曲1票）", f"- library参加: {comp['library_n']} loops", "", "## 上モノ群ごとの差", ""]
    for label, group in comp["groups"].items():
        lines += [f"### group {label}", "", f"corpus {group['corpus_n']}曲 / library {group['library_n']} loops", ""]
        ranked = []
        for key, metrics in group["features"].items():
            effect = abs(float(metrics.get("standardized_median_difference", metrics.get("cross_set_distance_median", 0))))
            ranked.append((effect, key, metrics))
        for _, key, metrics in sorted(ranked, reverse=True)[:8]:
            lines.append(f"- `{key}`: `{json.dumps(metrics, ensure_ascii=False)}`")
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS_ROOT)
    ap.add_argument("--library", type=Path, default=Path.home() / "Music/daw/library")
    ap.add_argument("--markdown", type=Path, default=REPO_ROOT / "docs/labs/reference-beat-taste-comparison-draft.md")
    ap.add_argument("--reuse", action="store_true", help="既存の分離結果だけで再集計")
    args = ap.parse_args()
    root = args.corpus.expanduser().resolve()
    index = load_json(args.library / "index.json")
    manifest = load_json(root / "manifest.json")
    if not index or not manifest:
        print("ERROR: library indexまたはcorpus manifestなし", file=sys.stderr); return 2
    loops = select_eight(index["entries"])
    if len(loops) != 8:
        print(f"ERROR: 感度用loopが8本揃わない: {len(loops)}", file=sys.stderr); return 2
    work = root / "comparison-sensitivity"
    work.mkdir(parents=True, exist_ok=True)
    rows = []
    py = REPO_ROOT / "tools/reference/.venv/bin/python"
    for li, entry in enumerate(loops):
        path = args.library / entry["path"]
        clean, _ = librosa.load(str(path), sr=22050, mono=True)
        contaminants = _stem_contaminants(manifest, len(clean), offset=20 + li * 3)
        conditions = {
            "drums-minus6": _mix_at(clean, contaminants["drums"], -6),
            "drums-bass-0": _mix_at(clean, contaminants["drums"] + contaminants["bass"], 0),
            "drums-bass-vocals-plus6": _mix_at(clean, contaminants["drums"] + contaminants["bass"] + contaminants["vocals"], 6),
        }
        bpm = float(entry.get("bpm") or 90)
        clean_feat = extract_shared(clean, bpm)
        for cond, mix in conditions.items():
            stem_root = work / f"loop-{li}-{cond}"
            wav = stem_root / "track.wav"
            wav.parent.mkdir(parents=True, exist_ok=True)
            if not wav.exists(): sf.write(str(wav), mix, 22050, subtype="PCM_16")
            outdir = stem_root / "stems"
            expected = outdir / "htdemucs_6s/track"
            if not args.reuse and not expected.is_dir():
                proc = subprocess.run([str(py), "-m", "demucs", "-d", "mps", "-n", "htdemucs_6s", "-o", str(outdir), str(wav)], cwd=REPO_ROOT / "tools/reference")
                if proc.returncode: return proc.returncode
            upper = sum((librosa.load(str(expected / f"{x}.wav"), sr=22050, mono=True)[0] for x in ("piano", "guitar", "other")), start=np.zeros_like(clean))
            rows.append({"loop": entry["path"], "condition": cond, "clean": clean_feat, "separated": extract_shared(upper[:len(clean)], bpm)})
    result = {"schema_revision": 1, "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(), "thresholds": {"scalar_rho": SCALAR_RHO, "scalar_mae_iqr": SCALAR_MAE_IQR, "distribution_median": DIST_MEDIAN, "distribution_p90": DIST_P90, "minimum_stable_groups": GROUPS_REQUIRED}, "selected_loops": [e["path"] for e in loops], "rows": rows}
    result.update(evaluate(rows))
    result["corpus_library_comparison"] = compare_library_to_corpus(result, root)
    output = root / "analysis/comparison-sensitivity.json"
    write_json_atomic(output, result)
    args.markdown.parent.mkdir(parents=True, exist_ok=True)
    args.markdown.write_text(comparison_markdown(result))
    taste_prov = load_json(root / "taste-provenance.json", {})
    parent = taste_prov.get("stages", {}).get("taste_features", {}).get("fingerprint")
    if parent:
        record_root_stage(root, "comparison_sensitivity", ["tools/taste/comparison-sensitivity.py", "tools/taste/distances.py", "tools/taste/shared_features.py"], {"taste_features": parent}, result["thresholds"], [output, args.markdown])
    print(f"stable groups={result['stable_groups']} supported={result['comparison_supported']}")
    return 0


if __name__ == "__main__": raise SystemExit(main())
