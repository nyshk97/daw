from __future__ import annotations

import datetime as dt
import subprocess
from pathlib import Path
from typing import Any

from common import REPO_ROOT, audio_info, load_json, sha256_file, stable_hash, write_json_atomic
from schemas import STAGE_PARENTS, STAGE_SOURCE_PATHS


def source_manifest(stage: str, repo_root: Path = REPO_ROOT) -> list[dict[str, str]]:
    result = []
    for rel in sorted(STAGE_SOURCE_PATHS[stage]):
        path = repo_root / rel
        if not path.is_file():
            raise FileNotFoundError(f"{stage} のsource hash対象が無い: {rel}")
        result.append({"path": rel, "sha256": sha256_file(path)})
    return result


def stage_source_hash(stage: str, repo_root: Path = REPO_ROOT) -> str:
    return stable_hash(source_manifest(stage, repo_root))


def git_metadata(repo_root: Path = REPO_ROOT) -> dict[str, Any]:
    def run(*args: str) -> str:
        return subprocess.run(args, cwd=repo_root, text=True, capture_output=True, check=False).stdout.strip()

    return {
        "commit": run("git", "rev-parse", "HEAD") or None,
        "dirty": bool(run("git", "status", "--porcelain")),
    }


def artifact_manifest(paths: list[Path], root: Path) -> list[dict[str, Any]]:
    out = []
    for path in sorted(paths, key=lambda p: p.relative_to(root).as_posix()):
        if not path.is_file():
            raise FileNotFoundError(path)
        out.append({
            "path": path.relative_to(root).as_posix(),
            "sha256": sha256_file(path),
            "bytes": path.stat().st_size,
        })
    return out


def build_stage(
    name: str,
    config: dict[str, Any],
    parent_stages: dict[str, dict[str, Any]],
    outputs: list[dict[str, Any]],
    repo_root: Path = REPO_ROOT,
) -> dict[str, Any]:
    parents = {p: parent_stages[p]["fingerprint"] for p in STAGE_PARENTS[name]}
    identity = {
        "stage": name,
        "schema_revision": 1,
        "parent_fingerprints": parents,
        "config": config,
        "stage_source_hash": stage_source_hash(name, repo_root),
        "outputs": outputs,
    }
    return {
        **identity,
        "source_files": source_manifest(name, repo_root),
        "fingerprint": stable_hash(identity),
        "completed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "git": git_metadata(repo_root),
    }


def validate_recorded_outputs(stage: dict[str, Any], root: Path) -> list[str]:
    errors = []
    for item in stage.get("outputs", []):
        path = root / item["path"]
        if not path.is_file():
            errors.append(f"missing:{item['path']}")
        elif sha256_file(path) != item["sha256"]:
            errors.append(f"hash:{item['path']}")
    return errors


def stage_valid(name: str, stage: dict[str, Any] | None, parents: dict[str, dict[str, Any]], root: Path) -> tuple[bool, str]:
    if not stage:
        return False, "provenance_missing"
    try:
        if stage.get("stage_source_hash") != stage_source_hash(name):
            return False, "stage_source_changed"
    except FileNotFoundError as e:
        return False, str(e)
    expected_parents = {p: parents[p]["fingerprint"] for p in STAGE_PARENTS[name]}
    if stage.get("parent_fingerprints") != expected_parents:
        return False, "parent_fingerprint_changed"
    errors = validate_recorded_outputs(stage, root)
    if errors:
        # acquire source.wav is allowed to be intentionally disposed after a verified trim.
        if name == "acquire" and stage.get("materialization") in {"dispose_pending", "disposed_after_verified"}:
            errors = [e for e in errors if e != "missing:source.wav"]
        if errors:
            return False, ",".join(errors)
    identity = {k: stage[k] for k in ("stage", "schema_revision", "parent_fingerprints", "config", "stage_source_hash", "outputs")}
    if stable_hash(identity) != stage.get("fingerprint"):
        return False, "fingerprint_mismatch"
    return True, "valid"


def load_song_provenance(path: Path) -> dict[str, Any]:
    return load_json(path / "analysis-provenance.json", {"schema_revision": 1, "stages": {}})


def save_song_provenance(path: Path, value: dict[str, Any]) -> None:
    write_json_atomic(path / "analysis-provenance.json", value)


def record_root_stage(
    corpus_root: Path,
    name: str,
    source_paths: list[str],
    parent_fingerprints: dict[str, str],
    config: dict[str, Any],
    outputs: list[Path],
) -> dict[str, Any]:
    """corpus全体stageをtaste-provenance.jsonへ記録する。

    output pathはcorpus外の小さいrepo派生dataもあるため絶対pathで記録するが、
    repoへ出すdata本体には絶対pathを含めない。
    """
    sources = []
    for rel in sorted(source_paths):
        path = REPO_ROOT / rel
        sources.append({"path": rel, "sha256": sha256_file(path)})
    out = [{"path": str(p.resolve()), "sha256": sha256_file(p), "bytes": p.stat().st_size} for p in sorted(outputs)]
    identity = {
        "stage": name,
        "schema_revision": 1,
        "parent_fingerprints": dict(sorted(parent_fingerprints.items())),
        "config": config,
        "stage_source_hash": stable_hash(sources),
        "outputs": out,
    }
    stage = {**identity, "source_files": sources, "fingerprint": stable_hash(identity), "completed_at": dt.datetime.now(dt.timezone.utc).isoformat(), "git": git_metadata()}
    path = corpus_root / "taste-provenance.json"
    root = load_json(path, {"schema_revision": 1, "stages": {}})
    root["stages"][name] = stage
    write_json_atomic(path, root)
    return stage
