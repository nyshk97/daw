#!/usr/bin/env python
"""既存4/4分析から独立してtrim・拍子・小節gridを監査する。

4/4だけを自動確定する。3/4・6/8・曖昧・途中ずれはneeds_reviewへ送り、
先頭／中盤／終盤のclick付き素材を作る。人間回答はreview/grid/answers.jsonで取り込む。
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import sys
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf

from common import DEFAULT_CORPUS_ROOT, load_json, sha256_file, validate_audio, write_json_atomic
from provenance import load_song_provenance, record_root_stage, save_song_provenance
from schemas import GRID_STATUSES, MATERIALIZATION_STATES

SR = 22050
HOP = 512
AUTO_MARGIN = 0.075
MIN_FOUR_SCORE = 0.34
MIN_GRID_COHERENCE = 0.42


def _norm_autocorr(env: np.ndarray, lag: float) -> float:
    n = int(round(lag))
    if n <= 0 or n >= len(env) // 2:
        return 0.0
    a, b = env[:-n], env[n:]
    if a.std() < 1e-9 or b.std() < 1e-9:
        return 0.0
    return float(np.clip(np.corrcoef(a, b)[0, 1], -1, 1))


def meter_scores(onset_env: np.ndarray, frames_per_beat: float) -> dict[str, float]:
    """四分音符beatを基準にaccent周期仮説を比較する純粋関数。"""
    ac = lambda beats: max(0.0, _norm_autocorr(onset_env, frames_per_beat * beats))
    # bar反復を主、拍・半小節の骨格を補助にする。6/8は8分3つ（1.5 quarter）の
    # compound pulseを持つ点で3/4と分ける。
    return {
        "3/4": round(0.70 * ac(3.0) + 0.20 * ac(1.0) + 0.10 * ac(1.5), 6),
        "4/4": round(0.65 * ac(4.0) + 0.20 * ac(2.0) + 0.15 * ac(1.0), 6),
        "6/8": round(0.60 * ac(3.0) + 0.30 * ac(1.5) + 0.10 * ac(0.5), 6),
    }


def _grid_coherence(env: np.ndarray, first_down: float, beat_s: float, duration: float) -> tuple[float, list[dict[str, float]]]:
    times = librosa.frames_to_time(np.arange(len(env)), sr=SR, hop_length=HOP)
    windows = [("head", 0.05, 0.28), ("middle", 0.40, 0.60), ("tail", 0.72, 0.95)]
    evidence = []
    vals = []
    for name, lo, hi in windows:
        t0, t1 = duration * lo, duration * hi
        ticks = np.arange(first_down, duration, beat_s)
        ticks = ticks[(ticks >= t0) & (ticks < t1)]
        if not len(ticks):
            score = 0.0
        else:
            local = []
            for tick in ticks:
                mask = np.abs(times - tick) <= min(0.09, beat_s * 0.18)
                local.append(float(env[mask].max()) if mask.any() else 0.0)
            denom = float(np.percentile(env, 90)) + 1e-9
            score = float(np.clip(np.mean(local) / denom, 0, 1))
        vals.append(score)
        evidence.append({"window": name, "start_s": round(t0, 3), "end_s": round(t1, 3), "coherence": round(score, 4)})
    return float(min(vals)), evidence


def audit_audio(track: Path, drums: Path, basics: dict, gates: dict) -> dict:
    y, _ = librosa.load(drums if drums.is_file() else track, sr=SR, mono=True)
    env = librosa.onset.onset_strength(y=y, sr=SR, hop_length=HOP)
    bpm = float(basics["tempo"]["bpm"])
    beat_s = 60.0 / bpm
    fpb = beat_s * SR / HOP
    scores = meter_scores(env, fpb)
    ranked = sorted(scores.items(), key=lambda x: (-x[1], x[0]))
    first_down = float(basics["grid"]["first_downbeat_sec"])
    duration = len(y) / SR
    coherence, windows = _grid_coherence(env, first_down, beat_s, duration)
    # _summary.noteには説明文として常に"failed"という単語が入るため文字列検索しない。
    # meter/gridに直接効く3 gateだけを見る。swing/harmony/key欠損はgrid確定を妨げない。
    bpm_gate = gates.get("bpm", {})
    gates_ok = bool(bpm_gate.get("octave_ok", bpm_gate.get("ok", True))) and bool(gates.get("tempo_stable", {}).get("ok", False)) and bool(gates.get("downbeat", {}).get("ok", False))
    auto = (
        ranked[0][0] == "4/4"
        and ranked[0][1] >= MIN_FOUR_SCORE
        and ranked[0][1] - ranked[1][1] >= AUTO_MARGIN
        and coherence >= MIN_GRID_COHERENCE
        and gates_ok
    )
    return {
        "grid_status": "auto_verified_4_4" if auto else "needs_review",
        "meter_numerator": 4 if auto else None,
        "meter_denominator": 4 if auto else None,
        "tempo_bpm": round(bpm, 6),
        "first_downbeat_s": round(first_down, 6),
        "bar_duration_s": round(beat_s * 4, 6) if auto else None,
        "evidence": {
            "meter_scores": scores,
            "best_hypothesis": ranked[0][0],
            "margin": round(ranked[0][1] - ranked[1][1], 6),
            "minimum_segment_coherence": round(coherence, 6),
            "segment_coherence": windows,
            "gates_ok": gates_ok,
            "thresholds": {"four_score": MIN_FOUR_SCORE, "margin": AUTO_MARGIN, "coherence": MIN_GRID_COHERENCE},
        },
        "schema_revision": 1,
        "checked_at": dt.datetime.now(dt.timezone.utc).isoformat(),
    }


def _mix_click(y: np.ndarray, sr: int, start: float, end: float, first_down: float, beat_s: float) -> np.ndarray:
    i0, i1 = int(start * sr), min(len(y), int(end * sr))
    out = np.array(y[i0:i1], dtype=np.float32, copy=True)
    click_len = max(1, int(sr * 0.025))
    x = np.arange(click_len) / sr
    ticks = np.arange(first_down, len(y) / sr, beat_s)
    for tick in ticks[(ticks >= start) & (ticks < end)]:
        j = int((tick - start) * sr)
        bar_pos = int(round((tick - first_down) / beat_s)) % 4
        freq, amp = (1760, 0.23) if bar_pos == 0 else (1100, 0.15)
        click = amp * np.sin(2 * np.pi * freq * x) * np.exp(-x * 90)
        n = min(len(click), len(out) - j)
        if n > 0:
            out[j : j + n] += click[:n]
    return np.clip(out, -1, 1)


def make_review_clips(entry: dict, ref: Path, corpus_root: Path, grid: dict) -> list[str]:
    y, sr = sf.read(str(ref / "track.wav"), dtype="float32", always_2d=False)
    if y.ndim == 2:
        y = y.mean(axis=1)
    duration = len(y) / sr
    beat = 60.0 / grid["tempo_bpm"]
    outdir = corpus_root / "review/grid" / entry["video_id"]
    outdir.mkdir(parents=True, exist_ok=True)
    written = []
    for label, center in (("head", duration * 0.12), ("middle", duration * 0.5), ("tail", duration * 0.86)):
        start = max(0.0, center - 8 * beat)
        end = min(duration, start + 16 * beat)
        out = outdir / f"{label}-{start:.2f}-{end:.2f}.wav"
        sf.write(str(out), _mix_click(y, sr, start, end, grid["first_downbeat_s"], beat), sr, subtype="PCM_16")
        validate_audio(out, min_duration=2.0)
        written.append(str(out.relative_to(corpus_root / "review/grid")))
    return written


HUMAN_DECIDED_STATUSES = {"human_verified_4_4", "human_verified_non_4_4"}


def human_decided(grid: dict) -> bool:
    """この曲のgridを人が判断済みか。再監査で消してはいけない印。"""
    return bool(grid.get("human_review")) or grid.get("grid_status") in HUMAN_DECIDED_STATUSES


def _apply_answers(manifest: dict, answers_path: Path) -> None:
    answers = load_json(answers_path, {})
    for vid, answer in answers.items():
        if vid not in manifest["songs"]:
            raise ValueError(f"answers.jsonに未知のvideo id: {vid}")
        status = answer.get("grid_status")
        if status not in {"human_verified_4_4", "human_verified_non_4_4", "unknown"}:
            raise ValueError(f"{vid}: 人間回答のgrid_statusが不正: {status}")
        grid = manifest["songs"][vid].setdefault("grid", {})
        grid.update(answer)
        grid["checked_at"] = dt.datetime.now(dt.timezone.utc).isoformat()
        grid["schema_revision"] = 1


def cleanup_verified_source(entry: dict, ref: Path) -> None:
    if entry.get("ownership") != "corpus":
        return
    if entry.get("grid", {}).get("grid_status") not in {"auto_verified_4_4", "human_verified_4_4", "human_verified_non_4_4"}:
        return
    prov = load_song_provenance(ref)
    acquire = prov.get("stages", {}).get("acquire", {})
    source = ref / "source.wav"
    state = acquire.get("materialization", "present")
    if state not in MATERIALIZATION_STATES:
        raise ValueError(f"不正なmaterialization: {state}")
    if state == "disposed_after_verified" and not source.exists():
        return
    if state == "dispose_pending" and source.exists():
        acquire["materialization"] = "present"
    if not source.exists():
        cleanup = prov.get("cleanup", {})
        if state == "dispose_pending" and cleanup.get("source_sha256") == next((o["sha256"] for o in acquire.get("outputs", []) if o["path"] == "source.wav"), None):
            acquire["materialization"] = "disposed_after_verified"
            save_song_provenance(ref, prov)
            return
        acquire["materialization"] = "missing_unexpected"
        save_song_provenance(ref, prov)
        raise ValueError(f"source.wavが意図せず消失: {ref}")
    recorded = next((o["sha256"] for o in acquire.get("outputs", []) if o["path"] == "source.wav"), None)
    actual = sha256_file(source)
    if recorded != actual:
        raise ValueError(f"cleanup前source hash不一致: {ref}")
    validate_audio(ref / "track.wav")
    prov["cleanup"] = {"intent_at": dt.datetime.now(dt.timezone.utc).isoformat(), "source_sha256": actual, "grid_status": entry["grid"]["grid_status"]}
    acquire["materialization"] = "dispose_pending"
    save_song_provenance(ref, prov)
    source.unlink()
    prov["cleanup"]["completed_at"] = dt.datetime.now(dt.timezone.utc).isoformat()
    acquire["materialization"] = "disposed_after_verified"
    prov["cleanup_status"] = "complete"
    save_song_provenance(ref, prov)


def write_review_readme(manifest: dict, corpus_root: Path) -> None:
    review = corpus_root / "review/grid"
    review.mkdir(parents=True, exist_ok=True)
    lines = ["# Grid監査 — 人間確認が必要な曲", "", "clickの高い音が小節頭、低い音が拍です。先頭・中盤・終盤で、同じ4/4の小節頭に聞こえるか確認してください。", "", "回答は `answers.json` にvideo IDごとに記入します。", ""]
    for vid, e in manifest["songs"].items():
        if e.get("grid", {}).get("grid_status") != "needs_review":
            continue
        ev = e["grid"].get("evidence", {})
        lines += [f"## [ ] {e['display_name']} (`{vid}`)", "", f"- 機械best: {ev.get('best_hypothesis')} / scores: `{json.dumps(ev.get('meter_scores', {}), ensure_ascii=False)}`", f"- clips: `{vid}/`", "- 判定: 4/4 / 3/4 / 6/8 / other / unknown", "- click位置: OK / 修正値", ""]
    (review / "README.md").write_text("\n".join(lines))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS_ROOT)
    ap.add_argument("--only")
    ap.add_argument("--apply-answers", type=Path)
    ap.add_argument("--no-cleanup", action="store_true")
    ap.add_argument("--force-reaudit", action="store_true", help="人間確認済みの曲も機械判定でやり直す（回答を捨てる）")
    args = ap.parse_args()
    root = args.corpus.expanduser().resolve()
    manifest_path = root / "manifest.json"
    manifest = load_json(manifest_path)
    if not manifest:
        print(f"ERROR: manifestなし: {manifest_path}", file=sys.stderr)
        return 2
    if args.apply_answers:
        _apply_answers(manifest, args.apply_answers)
    answered = set(load_json(args.apply_answers, {})) if args.apply_answers else set()
    failures = 0
    for vid, entry in manifest["songs"].items():
        if args.only and vid != args.only:
            continue
        if entry.get("status") != "analyzed":
            print(f"SKIP {vid}: status={entry.get('status')}")
            continue
        ref = Path(entry["active_artifact_path"])
        try:
            # 曲を足して再監査したときに、前回の人間回答を機械判定で上書きしない。
            protected = human_decided(entry.get("grid", {})) and not args.force_reaudit
            note = ""
            if vid in answered:
                note = " (回答適用)"
            elif protected:
                note = " (人間確認済みのため再監査しない)"
            else:
                grid = audit_audio(ref / "track.wav", ref / "stems/htdemucs/track/drums.wav", load_json(ref / "analysis/basics.json"), load_json(ref / "analysis/gates.json"))
                song_prov = load_song_provenance(ref)
                trim_config = song_prov.get("stages", {}).get("trim", {}).get("config", {})
                grid["trim_start_s"] = trim_config.get("trim_start_s")
                grid["trim_end_s"] = trim_config.get("trim_end_s")
                if grid["grid_status"] == "needs_review":
                    grid["review_clips"] = make_review_clips(entry, ref, root, grid)
                entry["grid"] = grid
            if not args.no_cleanup:
                cleanup_verified_source(entry, ref)
            print(f"{vid}: {entry['grid']['grid_status']} ({entry['grid'].get('evidence', {}).get('best_hypothesis')}){note}")
        except Exception as e:
            failures += 1
            entry["grid"] = {"grid_status": "unknown", "error": str(e), "schema_revision": 1}
            print(f"FAILED {vid}: {e}", file=sys.stderr)
        write_json_atomic(manifest_path, manifest)
    write_review_readme(manifest, root)
    parents = {}
    for vid, entry in manifest["songs"].items():
        prov = load_song_provenance(Path(entry["active_artifact_path"]))
        fp = prov.get("stages", {}).get("reference_analysis", {}).get("fingerprint")
        if fp:
            parents[vid] = fp
    grid_snapshot = {vid: entry.get("grid", {}) for vid, entry in manifest["songs"].items()}
    record_root_stage(
        root,
        "grid_audit",
        ["tools/taste/common.py", "tools/taste/grid-audit.py", "tools/taste/schemas.py"],
        parents,
        {"thresholds": {"four_score": MIN_FOUR_SCORE, "margin": AUTO_MARGIN, "coherence": MIN_GRID_COHERENCE}, "grid_snapshot_hash": __import__("common").stable_hash(grid_snapshot)},
        [root / "review/grid/README.md"],
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
