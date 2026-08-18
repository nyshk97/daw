#!/usr/bin/env python3
"""Step 0: 試聴ファイル生成（plan 2026-08-18-1123-noise-removal-lab.md Phase 1）。

scratch プロジェクト（元プロジェクトの複製）の project.json を変異させ、LaLa-dev の
検証フック（--open / --bounce）で実経路バウンスを回して以下を作る:

  (a)  ボーカルソロ           …… オケと他ボーカルをミュートしてバウンス
  (b)  ミックス中             …… 他ボーカルだけミュートしてバウンス
  (b') 既存編集で隙間整理     …… (b) に加え、ボーカルの無音区間をクリップ分割・削除相当に編集
  (c)  無音部の増幅           …… クリップ頭の意図的無音を聴取音量へ増幅（判定には使わない）

判定区間は plan の取り決めどおり先頭の意図的無音を除外する（トリム開始 = オケ開始時刻）。

使い方:
  .venv/bin/python step0.py --src ~/Music/daw/2026-08-18-tundra \
      --scratch ~/Music/daw/0-0-noiselab-tundra [--dry-gaps]
"""

import argparse
import copy
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import soundfile as sf

APP = Path("/Users/d0ne1s/daw/build/daw_artefacts/Debug/LaLa-dev.app")
LOG_DIR = Path.home() / "Library/Logs/daw"
OUT_DIR = Path(__file__).parent / "listen/step0"

# 素材の定義（トラック名で引く。track/clip の対応は project.json から解決する）
MATERIALS = ["uta", "rap"]
OKE = "oke"

# 隙間検出（(b') 用）: 100ms窓RMSが VOICE_THRESH_DB 未満の区間を隙間とみなす。
# 250ms未満の隙間は歌のフレーズ内として残し、声区間の前後に PAD_MS の余白を付ける
# （ユーザーが手で分割・削除するときの自然な粒度に合わせる）
VOICE_THRESH_DB = -45.0
MIN_GAP_MS = 250
PAD_MS = 150

# (c) の増幅目標（RMS）。声の平均レベル相当まで持ち上げて「ノイズの正体」を聴く
AMPLIFY_TARGET_DB = -30.0


def db(v: float) -> float:
    return 20.0 * np.log10(max(v, 1e-12))


def detect_voice_spans(wav: Path, sr_expected: float):
    """クリップ内の声区間 [(開始サンプル, 終了サンプル), ...] を返す。"""
    x, sr = sf.read(wav, dtype="float64", always_2d=True)
    assert sr == sr_expected, f"{wav}: SR {sr} != project {sr_expected}"
    x = x.mean(axis=1)
    win, hop = int(sr * 0.1), int(sr * 0.05)
    voiced = []
    for start in range(0, max(len(x) - win, 0) + 1, hop):
        seg = x[start : start + win]
        voiced.append((start, db(float(np.sqrt(np.mean(seg**2)))) >= VOICE_THRESH_DB))
    spans = []
    cur = None
    for start, v in voiced:
        if v and cur is None:
            cur = [start, start + win]
        elif v:
            cur[1] = start + win
        elif cur is not None and start - cur[1] >= int(sr * MIN_GAP_MS / 1000):
            spans.append(cur)
            cur = None
    if cur is not None:
        spans.append(cur)
    pad = int(sr * PAD_MS / 1000)
    spans = [[max(0, s - pad), min(len(x), e + pad)] for s, e in spans]
    # pad で重なった区間を結合
    merged = []
    for s, e in spans:
        if merged and s <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], e)
        else:
            merged.append([s, e])
    return merged, len(x)


def find_track(project: dict, name: str) -> dict:
    for t in project["tracks"]:
        if t["name"] == name:
            return t
    sys.exit(f"error: トラック '{name}' が見つからない")


def wait_no_app():
    for _ in range(20):
        if subprocess.run(["pgrep", "-x", "LaLa-dev"], capture_output=True).returncode != 0:
            return
        time.sleep(1)
    sys.exit("error: LaLa-dev が終了しない（ユーザーが使用中なら中止が正しい）")


def newest_log() -> Path:
    logs = sorted(LOG_DIR.glob("*"), key=lambda p: p.stat().st_mtime)
    return logs[-1] if logs else None


def run_bounce(scratch: Path, out_wav: Path, timeout_s: int = 120):
    """LaLa-dev を起動して --bounce し、bounce.done を待って終了させる。"""
    if subprocess.run(["pgrep", "-x", "LaLa-dev"], capture_output=True).returncode == 0:
        sys.exit("error: LaLa-dev が既に起動している（ユーザーの作業を壊さないため中止）")
    before = newest_log()
    subprocess.run(
        ["open", "-g", "-n", str(APP), "--args",
         "--open", str(scratch), "--bounce", str(out_wav)],
        check=True,
    )
    deadline = time.time() + timeout_s
    status = None
    while time.time() < deadline and status is None:
        time.sleep(1)
        log = newest_log()
        if log is None or (before is not None and log == before):
            continue
        text = log.read_text(errors="replace")
        if "bounce.done" in text:
            status = "done"
        elif "bounce.failed" in text:
            status = "failed"
    subprocess.run(["pkill", "-x", "LaLa-dev"], capture_output=True)
    wait_no_app()
    if status != "done":
        sys.exit(f"error: bounce が {status or 'タイムアウト'}（log: {newest_log()}）")
    if not out_wav.exists():
        sys.exit(f"error: bounce.done なのに出力が無い: {out_wav}")


def trim(in_wav: Path, out_wav: Path, start_sample: int):
    x, sr = sf.read(in_wav, dtype="float64", always_2d=True)
    info = sf.info(in_wav)
    sf.write(out_wav, x[start_sample:], sr, subtype=info.subtype)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, type=Path)
    ap.add_argument("--scratch", required=True, type=Path)
    ap.add_argument("--dry-gaps", action="store_true", help="隙間検出の結果だけ表示して終了")
    args = ap.parse_args()
    src, scratch = args.src.expanduser(), args.scratch.expanduser()

    project = json.loads((src / "project.json").read_text())
    sr = project["sampleRate"]
    oke_track = find_track(project, OKE)
    oke_start = min(c["startSample"] for c in oke_track["clips"])

    # 隙間検出（(b') の編集内容を先に決める）
    spans_by_material = {}
    for name in MATERIALS:
        track = find_track(project, name)
        assert len(track["clips"]) == 1, f"{name} はクリップ1個の前提"
        clip = track["clips"][0]
        spans, total = detect_voice_spans(src / clip["file"], sr)
        spans_by_material[name] = spans
        print(f"[{name}] {clip['file']}: 声区間 {len(spans)}個 / クリップ長 {total/sr:.1f}s")
        for s, e in spans:
            print(f"    {s/sr:7.2f}s 〜 {e/sr:7.2f}s  ({(e-s)/sr:.2f}s)")
    if args.dry_gaps:
        return

    # scratch 作成（毎回作り直す。元プロジェクトは読み取りのみ）
    if scratch.exists():
        shutil.rmtree(scratch)
    shutil.copytree(src, scratch)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    work = Path(__file__).parent / "work/step0"
    work.mkdir(parents=True, exist_ok=True)

    def write_variant(mutes: dict, edited_material: str | None) -> None:
        p = copy.deepcopy(project)
        for tname, mute in mutes.items():
            find_track(p, tname)["mute"] = mute
        if edited_material is not None:
            track = find_track(p, edited_material)
            clip = track["clips"][0]
            segments = []
            for s, e in spans_by_material[edited_material]:
                seg = copy.deepcopy(clip)
                seg["startSample"] = clip["startSample"] + s
                seg["offsetSamples"] = clip["offsetSamples"] + s
                seg["lengthSamples"] = e - s
                segments.append(seg)
            track["clips"] = segments
        (scratch / "project.json").write_text(json.dumps(p))

    for name in MATERIALS:
        other = [m for m in MATERIALS if m != name][0]
        variants = [
            (f"{name}-a-solo", {OKE: True, other: True, name: False}, None),
            (f"{name}-b-mix", {OKE: False, other: True, name: False}, None),
            (f"{name}-b2-mix-edited", {OKE: False, other: True, name: False}, name),
        ]
        for label, mutes, edited in variants:
            raw = work / f"{label}-full.wav"
            raw.unlink(missing_ok=True)
            write_variant(mutes, edited)
            print(f"bounce: {label} ...")
            run_bounce(scratch, raw)
            trim(raw, OUT_DIR / f"{label}.wav", oke_start)

        # (c) 無音部の増幅（クリップ頭の静かな2秒を採る。判定には使わない）
        track = find_track(project, name)
        clip = track["clips"][0]
        x, _ = sf.read(src / clip["file"], dtype="float64", always_2d=True)
        x = x.mean(axis=1)
        head = x[int(sr * 0.5) : int(sr * 2.5)]
        gain_db = AMPLIFY_TARGET_DB - db(float(np.sqrt(np.mean(head**2))))
        sf.write(OUT_DIR / f"{name}-c-noise-x{gain_db:.0f}dB.wav",
                 np.tile(head * (10 ** (gain_db / 20)), 4), int(sr), subtype="PCM_24")
        print(f"[{name}] (c) 増幅 {gain_db:.1f}dB")

    print(f"\n完了: {OUT_DIR}")


if __name__ == "__main__":
    main()
