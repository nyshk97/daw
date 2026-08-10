#!/usr/bin/env python
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from common import (
    DEFAULT_CORPUS_ROOT,
    PICKS_PATH,
    REPO_ROOT,
    canonical_youtube_url,
    load_json,
    sha256_file,
    validate_audio,
    video_id_from_url,
    write_json_atomic,
)
from provenance import artifact_manifest, build_stage, load_song_provenance, save_song_provenance, stage_valid, validate_recorded_outputs
from schemas import MATERIALIZATION_STATES, PER_SONG_STAGES

REF_DIR = REPO_ROOT / "tools/reference"
MANIFEST_REVISION = 1


def parse_picks(path: Path) -> tuple[list[dict[str, str]], list[str]]:
    entries, errors = [], []
    seen: dict[str, str] = {}
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or not re.match(r"^[-*+]\s+", line):
            continue
        body = re.sub(r"^[-*+]\s+", "", line).strip()
        urls = list(re.finditer(r"https?://\S+", body))
        if not urls:
            errors.append(f"line {lineno}: URLなし: {body}")
            continue
        url = urls[-1].group(0).rstrip(").,、。")
        video_id = video_id_from_url(url)
        if not video_id:
            errors.append(f"line {lineno}: YouTube URLを解釈できない: {url}")
            continue
        name = body[: urls[-1].start()].rstrip(" \t-–—：:").strip()
        if not name:
            errors.append(f"line {lineno}: 表示名なし: {url}")
            continue
        if video_id in seen:
            kind = "同一IDの別名" if seen[video_id] != name else "重複URL"
            errors.append(f"line {lineno}: {kind}: {video_id} ({seen[video_id]} / {name})")
            continue
        seen[video_id] = name
        entries.append({"video_id": video_id, "display_name": name, "url": canonical_youtube_url(video_id), "source_line": lineno})
    return entries, errors


def discover_external(search_root: Path) -> dict[str, list[dict[str, Any]]]:
    found: dict[str, list[dict[str, Any]]] = {}
    if not search_root.exists():
        return found
    for info in search_root.glob("**/references/*/source.info.json"):
        try:
            meta = json.loads(info.read_text())
            vid = str(meta.get("id") or "")
            if len(vid) != 11:
                continue
            root = info.parent.resolve()
            prov = load_json(root / "analysis-provenance.json", {})
            found.setdefault(vid, []).append({
                "path": str(root),
                "title": meta.get("title"),
                "has_source": (root / "source.wav").is_file(),
                "has_track": (root / "track.wav").is_file(),
                "provenance_stages": sorted(prov.get("stages", {})),
            })
        except (OSError, ValueError, json.JSONDecodeError):
            continue
    return found


def _reuse_decision(candidates: list[dict[str, Any]]) -> tuple[str | None, str | None]:
    complete = [c for c in candidates if set(c["provenance_stages"]) >= set(PER_SONG_STAGES)]
    if len(complete) == 1:
        return complete[0]["path"], None
    if candidates:
        why = "multiple_complete_candidates" if len(complete) > 1 else "legacy_or_incomplete_provenance"
        return None, why
    return None, None


def sync_manifest(picks: Path, corpus_root: Path, search_root: Path, dry_run: bool = False) -> tuple[dict[str, Any], list[str]]:
    parsed, diagnostics = parse_picks(picks)
    old = load_json(corpus_root / "manifest.json", {"songs": {}, "orphans": {}})
    external = discover_external(search_root)
    songs: dict[str, Any] = {}
    for item in parsed:
        vid = item["video_id"]
        previous = old.get("songs", {}).get(vid, {})
        candidates = external.get(vid, [])
        external_path, conflict = _reuse_decision(candidates)
        artifact = previous.get("active_artifact_path") or str((corpus_root / "tracks" / vid).resolve())
        ownership = previous.get("ownership", "external" if external_path else "corpus")
        if ownership == "external" and not external_path:
            ownership, artifact = "corpus", str((corpus_root / "tracks" / vid).resolve())
        songs[vid] = {
            **item,
            "ownership": ownership,
            "active_artifact_path": external_path or artifact,
            "external_candidates": candidates,
            "reuse_conflict": conflict,
            "status": previous.get("status", "pending"),
            "stage_status": previous.get("stage_status", {}),
            "grid": previous.get("grid", {"grid_status": "unknown"}),
            "analysis_summary": previous.get("analysis_summary"),
            "last_error": previous.get("last_error"),
        }
    current = set(songs)
    old_songs = old.get("songs", {})
    orphans = {**old.get("orphans", {})}
    for vid in sorted(set(old_songs) - current):
        orphans[vid] = {**old_songs[vid], "orphaned_at": dt.datetime.now(dt.timezone.utc).isoformat()}
    for vid in current:
        orphans.pop(vid, None)
    manifest = {
        "schema_revision": MANIFEST_REVISION,
        "input": str(picks.resolve()),
        "corpus_root": str(corpus_root.resolve()),
        "updated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "songs": songs,
        "orphans": orphans,
        "diagnostics": diagnostics,
    }
    if not dry_run:
        write_json_atomic(corpus_root / "manifest.json", manifest)
    return manifest, diagnostics


def _copy_external_source(entry: dict[str, Any], dst: Path) -> str | None:
    candidates = [Path(c["path"]) for c in entry.get("external_candidates", []) if c.get("has_source")]
    if not candidates:
        return None
    # source.infoのIDが一致する候補なら、bytes hashを記録してcorpus所有先へcopyできる。
    hashes = [(c, sha256_file(c / "source.wav")) for c in candidates]
    unique = {h for _, h in hashes}
    if len(unique) != 1:
        return None
    src = hashes[0][0]
    dst.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "source.wav", dst / "source.wav")
    shutil.copy2(src / "source.info.json", dst / "source.info.json")
    return str(src)


def _record_per_song_provenance(ref: Path, entry: dict[str, Any], imported_from: str | None) -> dict[str, Any]:
    source = validate_audio(ref / "source.wav")
    track = validate_audio(ref / "track.wav")
    analysis = ref / "analysis"
    stem4 = sorted((ref / "stems/htdemucs/track").glob("*.wav"))
    stem6 = sorted((ref / "stems/htdemucs_6s/track").glob("*.wav"))
    required_json = [analysis / n for n in ("basics.json", "gates.json", "stems-4s.json", "stems-6s.json", "groove.json", "arrangement.json")]
    missing = [str(p) for p in required_json if not p.is_file()]
    if len(stem4) != 4 or len(stem6) != 6:
        missing.append(f"stems 4s={len(stem4)} 6s={len(stem6)}")
    for p in required_json:
        if p.is_file():
            json.loads(p.read_text())
    if missing:
        raise ValueError("分析成果物不足: " + ", ".join(missing))

    stages: dict[str, dict[str, Any]] = {}
    stages["acquire"] = build_stage(
        "acquire",
        {"video_id": entry["video_id"], "canonical_url": entry["url"], "format": "bestaudio/wav", "imported_from_external_path": imported_from},
        stages,
        [{"path": "source.wav", **{k: source[k] for k in ("sha256", "bytes")}}, {"path": "source.info.json", "sha256": sha256_file(ref / "source.info.json"), "bytes": (ref / "source.info.json").stat().st_size}],
    )
    stages["acquire"]["materialization"] = "present"
    overview = load_json(analysis / "source-overview.json", {})
    source_duration = float(overview.get("duration_sec", source["duration_s"]))
    gaps = overview.get("near_silence", [])
    head = [float(g["end"]) for g in gaps if float(g["start"]) < 30]
    tail = [float(g["start"]) for g in gaps if float(g["end"]) > source_duration - 30]
    trim_start = max(head) if head else 0.0
    trim_end = min(tail) if tail else source_duration
    stages["trim"] = build_stage(
        "trim",
        {"trim_start_s": round(trim_start, 6), "trim_end_s": round(trim_end, 6), "codec": "pcm_s16le"},
        stages,
        artifact_manifest([ref / "track.wav"], ref),
    )
    stages["demucs"] = build_stage(
        "demucs",
        {"models": ["htdemucs", "htdemucs_6s"], "device": "mps"},
        stages,
        artifact_manifest(stem4 + stem6, ref),
    )
    json_outputs = required_json + sorted(analysis.glob("topline-*.json"))
    stages["reference_analysis"] = build_stage(
        "reference_analysis",
        {"pipeline": "tools/reference/analyze.py", "gates_schema": 1},
        stages,
        artifact_manifest(json_outputs, ref),
    )
    provenance = {"schema_revision": 1, "stages": stages, "cleanup_status": "pending_grid_audit"}
    save_song_provenance(ref, provenance)
    return provenance


def _summarize(ref: Path, elapsed: float) -> dict[str, Any]:
    basics = load_json(ref / "analysis/basics.json", {})
    gates = load_json(ref / "analysis/gates.json", {})
    source_meta = load_json(ref / "source.info.json", {})
    return {
        "bpm": basics.get("tempo", {}).get("bpm"),
        "key": basics.get("key"),
        "gates": gates.get("_summary", gates),
        "bytes": sum(p.stat().st_size for p in ref.rglob("*") if p.is_file()),
        "elapsed_s": round(elapsed, 1),
        "youtube": {k: source_meta.get(k) for k in ("title", "availability", "age_limit", "duration", "channel")},
    }


def analyze_one(entry: dict[str, Any], corpus_root: Path, dry_run: bool = False) -> tuple[dict[str, Any], str]:
    if entry["ownership"] == "external":
        # 完全一致のexternalはread-onlyで検証だけする。現状のlegacy候補はsyncでcorpusへ落ちる。
        return entry, "external_reused"
    ref = Path(entry["active_artifact_path"])
    ref.mkdir(parents=True, exist_ok=True)
    prov = load_song_provenance(ref)
    parents: dict[str, dict[str, Any]] = {}
    stage_status = {}
    for name in PER_SONG_STAGES:
        valid, why = stage_valid(name, prov.get("stages", {}).get(name), parents, ref)
        stage_status[name] = {"valid": valid, "reason": why}
        if valid:
            parents[name] = prov["stages"][name]
        else:
            break
    if len(parents) == len(PER_SONG_STAGES):
        entry.update(status="analyzed", stage_status=stage_status, analysis_summary=_summarize(ref, 0.0), last_error=None)
        return entry, "unchanged"
    if dry_run:
        return entry, f"would_analyze_from:{PER_SONG_STAGES[len(parents)]}"

    imported = None
    if not (ref / "source.wav").is_file():
        imported = _copy_external_source(entry, ref)
    logdir = corpus_root / "logs"
    logdir.mkdir(parents=True, exist_ok=True)
    logfile = logdir / f"{entry['video_id']}.log"
    cmd = [str(REF_DIR / "analyze-url.sh"), entry["url"], entry["video_id"], "--dir", str(corpus_root / "tracks")]
    t0 = time.monotonic()
    with logfile.open("a") as log:
        log.write(f"\n== {dt.datetime.now(dt.timezone.utc).isoformat()} ==\n$ {' '.join(cmd)}\n")
        proc = subprocess.run(cmd, cwd=REPO_ROOT, stdout=log, stderr=subprocess.STDOUT, text=True)
    warning = None
    if proc.returncode:
        # 既存pipelineの末尾にある耳確認clip/cardだけが失敗しても、Phase 2の成功契約
        # （track・両stem・主要JSON・再計算可能provenance）を満たすなら曲分析は有効。
        # 逆に主要成果物が欠ければ_recordが例外にして失敗のまま残す。
        warning = f"analyze-url.sh exit {proc.returncode}; optional tail step failed; log={logfile}"
    _record_per_song_provenance(ref, entry, imported)
    entry.update(status="analyzed", stage_status={s: {"valid": True, "reason": "completed"} for s in PER_SONG_STAGES}, analysis_summary=_summarize(ref, time.monotonic() - t0), last_error=None, warnings=([warning] if warning else []))
    return entry, "analyzed_with_warning" if warning else "analyzed"


def command_validate(args: argparse.Namespace) -> int:
    manifest = load_json(args.corpus / "manifest.json")
    if not manifest:
        print("ERROR: manifestなし", file=sys.stderr)
        return 2
    failures = 0
    for vid, entry in manifest["songs"].items():
        if entry.get("ownership") != "corpus" or entry.get("status") != "analyzed":
            continue
        ref = Path(entry["active_artifact_path"])
        try:
            old = load_song_provenance(ref)
            previous = old.get("stages", {})
            rebuilt = {}
            for name in PER_SONG_STAGES:
                stage = previous.get(name)
                if not stage:
                    raise ValueError(f"{name}: recorded provenanceなし")
                errors = validate_recorded_outputs(stage, ref)
                if name == "acquire" and stage.get("materialization") in {"dispose_pending", "disposed_after_verified"}:
                    errors = [e for e in errors if e != "missing:source.wav"]
                if errors:
                    raise ValueError(f"{name}: recorded output不整合: {errors}")
                rebuilt[name] = build_stage(name, stage["config"], rebuilt, stage["outputs"])
                if name == "acquire":
                    rebuilt[name]["materialization"] = stage.get("materialization", "present")
            old["stages"] = rebuilt
            save_song_provenance(ref, old)
            entry["stage_status"] = {s: {"valid": True, "reason": "restamped_after_full_validation"} for s in PER_SONG_STAGES}
            print(f"{vid}: valid")
        except Exception as e:
            failures += 1
            entry["status"] = "failed"; entry["last_error"] = str(e)
            print(f"{vid}: FAILED {e}", file=sys.stderr)
        write_json_atomic(args.corpus / "manifest.json", manifest)
    return 1 if failures else 0


def command_sync(args: argparse.Namespace) -> int:
    manifest, diagnostics = sync_manifest(args.picks, args.corpus, args.search_root, args.dry_run)
    for d in diagnostics:
        print(f"DIAG: {d}")
    counts = {"pending": 0, "analyzed": 0, "failed": 0, "external": 0, "conflict": 0}
    for e in manifest["songs"].values():
        counts[e.get("status", "pending")] = counts.get(e.get("status", "pending"), 0) + 1
        counts["external"] += e["ownership"] == "external"
        counts["conflict"] += bool(e.get("reuse_conflict"))
    print(f"songs={len(manifest['songs'])} orphans={len(manifest['orphans'])} " + " ".join(f"{k}={v}" for k, v in counts.items()))
    return 1 if diagnostics else 0


def command_analyze(args: argparse.Namespace) -> int:
    manifest, diagnostics = sync_manifest(args.picks, args.corpus, args.search_root, False)
    if diagnostics:
        for d in diagnostics:
            print(f"DIAG: {d}", file=sys.stderr)
        return 2
    selected = list(manifest["songs"].values())
    if args.only:
        selected = [e for e in selected if e["video_id"] == args.only]
        if not selected:
            print(f"ERROR: video idなし: {args.only}", file=sys.stderr)
            return 2
    if args.retry_failed:
        selected = [e for e in selected if e.get("status") == "failed"]
    failures = 0
    for i, entry in enumerate(selected, 1):
        print(f"[{i}/{len(selected)}] {entry['display_name']} ({entry['video_id']})", flush=True)
        try:
            updated, action = analyze_one(entry, args.corpus, args.dry_run)
            manifest["songs"][entry["video_id"]] = updated
            print(f"  {action}", flush=True)
        except Exception as e:
            failures += 1
            entry.update(status="failed", last_error=str(e))
            manifest["songs"][entry["video_id"]] = entry
            print(f"  FAILED: {e}", file=sys.stderr, flush=True)
        write_json_atomic(args.corpus / "manifest.json", manifest)
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(description="23曲taste corpusの差分同期・分析")
    sub = ap.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS_ROOT)
    common.add_argument("--picks", type=Path, default=PICKS_PATH)
    common.add_argument("--search-root", type=Path, default=Path.home() / "Music/daw")
    p = sub.add_parser("sync", parents=[common])
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(func=command_sync)
    p = sub.add_parser("analyze", parents=[common])
    p.add_argument("--only")
    p.add_argument("--retry-failed", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(func=command_analyze)
    p = sub.add_parser("validate", parents=[common])
    p.set_defaults(func=command_validate)
    args = ap.parse_args()
    args.corpus = args.corpus.expanduser().resolve()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
