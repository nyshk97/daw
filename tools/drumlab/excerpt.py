#!/usr/bin/env python3
"""ドラムステムから耳確認用の抜粋を作る。

曲名が判断へ混ざらないよう、出力はシャッフルした記号ラベル（A, B, ...）にし、
対応表は聴取ディレクトリの外へ書く。ラウドネス差が「良く聞こえる」バイアスに
ならないよう、各抜粋を同じRMSへ揃える。

  python3 tools/drumlab/excerpt.py --role contrast --bars 16
"""
from __future__ import annotations

import argparse
import json
import string
from pathlib import Path

import numpy as np
import soundfile as sf

CORPUS = Path.home() / "Music/daw/reference-beat-corpus"
TARGET_RMS_DB = -20.0
FADE_MS = 20.0


def load_songs(role: str, extra_ids: list[str]) -> list[dict]:
    """指定roleの全曲＋extra_idsの曲を返す。

    extra_idsは「別roleから伏せたまま混ぜる曲」。正例だけを並べると
    「好きな曲なのだから好きと答えるべき」へ倒れるので、判定済みの曲を
    混ぜてその引力を薄め、同時に回答の安定性も測れるようにする。
    """
    manifest = json.loads((CORPUS / "manifest.json").read_text())
    songs = manifest["songs"]
    songs = songs if isinstance(songs, list) else list(songs.values())
    picked = [s for s in songs if s.get("role") == role]
    known = {s["video_id"] for s in picked}
    picked += [s for s in songs if s["video_id"] in extra_ids and s["video_id"] not in known]
    return picked


def pick_window(energy_by_bar: list[dict], bars: int) -> int:
    """ドラムが途切れない16小節を曲の中盤から選ぶ。

    窓内の最小エネルギーが最大になる位置を採る（ブレイクやイントロを避ける）。
    同点は曲の中央に近い方。「一番派手な区間」を選ぶと集団間で選定条件がずれるため、
    最大でなく最小値の最大化にしている。
    """
    e = np.array([b["low"] + b["mid"] + b["high"] for b in energy_by_bar], dtype=float)
    n = len(e)
    if n <= bars:
        return 0
    lo, hi = int(n * 0.35), max(int(n * 0.75) - bars, int(n * 0.35))
    starts = range(lo, max(hi, lo) + 1)
    center = (n - bars) / 2
    best = min(starts, key=lambda s: (-float(e[s:s + bars].min()), abs(s - center)))
    return best


def normalize(y: np.ndarray, sr: int) -> np.ndarray:
    rms = float(np.sqrt(np.mean(y ** 2)))
    if rms > 0:
        y = y * (10 ** (TARGET_RMS_DB / 20.0) / rms)
    fade = int(sr * FADE_MS / 1000.0)
    if fade > 0 and len(y) > 2 * fade:
        ramp = np.linspace(0.0, 1.0, fade)
        y[:fade] *= ramp[:, None] if y.ndim > 1 else ramp
        y[-fade:] *= ramp[::-1, None] if y.ndim > 1 else ramp[::-1]
    return np.clip(y, -1.0, 1.0)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--role", default="contrast")
    ap.add_argument("--bars", type=int, default=16)
    ap.add_argument("--seed", type=int, default=20260822)
    ap.add_argument("--out", default=str(CORPUS / "review/drum-listen"))
    ap.add_argument("--extra-ids", default="", help="別roleから伏せて混ぜるvideo_id（カンマ区切り）")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.wav"):
        old.unlink()

    extra = [x for x in args.extra_ids.split(",") if x]
    songs = load_songs(args.role, extra)
    rng = np.random.default_rng(args.seed)
    order = rng.permutation(len(songs))
    labels = list(string.ascii_uppercase)

    key = []
    for label_idx, song_idx in enumerate(order):
        song = songs[song_idx]
        track = Path(song["active_artifact_path"])
        groove = json.loads((track / "analysis/groove.json").read_text())
        drums = track / "stems/htdemucs/track/drums.wav"

        bar_len = float(groove["bar_len_sec"])
        first_db = float(groove["first_downbeat_sec"])
        start_bar = pick_window(groove["drums"]["energy_by_bar"], args.bars)
        start_s = first_db + start_bar * bar_len
        dur_s = args.bars * bar_len

        info = sf.info(str(drums))
        sr = info.samplerate
        y, _ = sf.read(str(drums), start=int(start_s * sr), frames=int(dur_s * sr), dtype="float32")
        y = normalize(np.asarray(y, dtype=np.float32), sr)

        label = labels[label_idx]
        sf.write(str(out / f"{label}.wav"), y, sr)
        key.append({
            "label": label,
            "video_id": song["video_id"],
            "display_name": song.get("display_name"),
            "role": song.get("role"),
            "start_bar": int(start_bar) + 1,
            "start_sec": round(start_s, 3),
            "bars": args.bars,
            "bpm": groove.get("bpm"),
        })
        mark = "*" if song.get("role") != args.role else " "
        print(f"{label}{mark} {song.get('display_name')}  bar {start_bar + 1}  {dur_s:.1f}s  {groove.get('bpm')}BPM")

    key_path = out.parent / f"{out.name}-key.json"
    key_path.write_text(json.dumps({"seed": args.seed, "bars": args.bars, "items": key},
                                   ensure_ascii=False, indent=2))
    print(f"\n出力: {out}\n対応表: {key_path}")


if __name__ == "__main__":
    main()
