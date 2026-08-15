#!/usr/bin/env python3
"""ブラインド試聴セットの生成（planのPhase 2末尾）。

- segments.json の固定区間を切り出し
- ペア: P4 = htdemucs vs 多段4s（vocals/other）
        P6a = htdemucs_6s vs SW（piano/guitar/other）
        P6b = htdemucs_6s vs 多段6s（piano/guitar/other）
- セットA: ステム単体。activity判定→両方有効なら-20LUFSに個別正規化、
  それ以外は「mix区間を-20LUFSにするゲイン」を両者へ共通適用（相対音量を保存）
- セットB: グループ生ステムをそのまま加算→ミックス全体へ共通ゲイン1つ（-16LUFS）＋原曲
- X/Yのdemucs側はsha256(song/segment/pair)の先頭バイト偶奇で決定（再現可能なランダム化）
- 対応表・activity判定・ラウドネス値は blind-map JSON に保存（回答完了まで人間は開かない）

使い方: python blindprep.py <run名>   例: python blindprep.py run1
出力: listen/ 以下と docs/labs/reference-beat-human-answers/<日付>-blind-map.json
"""
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import pyloudnorm
import soundfile as sf
import soxr

HERE = Path(__file__).parent
REFS = Path.home() / "Music/daw/references"
MAP_OUT = HERE / "../../docs/labs/reference-beat-human-answers/2026-08-15-blind-map.json"
LISTEN = HERE / "listen"
TARGET_SR = 48000
STEM_LUFS = -20.0
MIX_LUFS = -16.0
ACTIVITY_DB = -40.0  # 区間内で stemRMS - mixRMS がこれ未満なら「非アクティブ」

PAIRS = {
    "P4": {
        "group": "4stems",
        "stems": ["vocals", "other"],
        "demucs": lambda r, run: {
            "vocals": r / "demucs" / run / "htdemucs/track/vocals.wav",
            "other": r / "demucs" / run / "htdemucs/track/other.wav",
        },
        "chal": lambda r, run: {
            "vocals": r / "multi" / run / "vocals_rf.wav",
            "other": r / "multi" / run / "htdemucs/instrumental/other.wav",
        },
        "group_stems_demucs": lambda r, run: sorted((r / "demucs" / run / "htdemucs/track").glob("*.wav")),
        "group_stems_chal": lambda r, run: [r / "multi" / run / "vocals_rf.wav"]
        + [p for p in sorted((r / "multi" / run / "htdemucs/instrumental").glob("*.wav")) if p.stem != "vocals"],
    },
    "P6a": {
        "group": "6stems",
        "stems": ["piano", "guitar", "other"],
        "demucs": lambda r, run: {
            s: r / "demucs" / run / f"htdemucs_6s/track/{s}.wav" for s in ("piano", "guitar", "other")
        },
        "chal": lambda r, run: {s: r / "sw" / run / f"track_{s}.wav" for s in ("piano", "guitar", "other")},
        "group_stems_demucs": lambda r, run: sorted((r / "demucs" / run / "htdemucs_6s/track").glob("*.wav")),
        "group_stems_chal": lambda r, run: [
            r / "sw" / run / f"track_{s}.wav" for s in ("bass", "drums", "guitar", "other", "piano", "vocals")
        ],
    },
    "P6b": {
        "group": "6stems",
        "stems": ["piano", "guitar", "other"],
        "demucs": lambda r, run: {
            s: r / "demucs" / run / f"htdemucs_6s/track/{s}.wav" for s in ("piano", "guitar", "other")
        },
        "chal": lambda r, run: {
            s: r / "multi" / run / f"htdemucs_6s/instrumental/{s}.wav" for s in ("piano", "guitar", "other")
        },
        "group_stems_demucs": lambda r, run: sorted((r / "demucs" / run / "htdemucs_6s/track").glob("*.wav")),
        "group_stems_chal": lambda r, run: [r / "multi" / run / "vocals_rf.wav"]
        + [p for p in sorted((r / "multi" / run / "htdemucs_6s/instrumental").glob("*.wav")) if p.stem != "vocals"],
    },
}


def load_seg(path: Path, start: float, end: float) -> np.ndarray:
    """区間を切り出して48kモノラルでなくステレオfloat64で返す（必要ならresample）"""
    info = sf.info(str(path))
    sr = info.samplerate
    x, _ = sf.read(str(path), start=int(start * sr), stop=int(end * sr), always_2d=True, dtype="float64")
    if sr != TARGET_SR:
        x = soxr.resample(x, sr, TARGET_SR)
    return x


def rms_db(x: np.ndarray) -> float:
    r = float(np.sqrt(np.mean(np.square(x), dtype=np.float64)))
    return -120.0 if r <= 0 else 20 * float(np.log10(r))


def lufs(x: np.ndarray) -> float:
    meter = pyloudnorm.Meter(TARGET_SR)
    try:
        v = meter.integrated_loudness(x)
        return float(v) if np.isfinite(v) else -120.0
    except ValueError:
        return -120.0


def gain_to(x: np.ndarray, current: float, target: float) -> np.ndarray:
    if current <= -119:
        return x
    return x * (10 ** ((target - current) / 20.0))


def write(path: Path, x: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    if peak > 0.999:  # クリップ回避（発生したらmapに残るLUFS値で追える）
        x = x * (0.999 / peak)
    sf.write(str(path), x.astype(np.float32), TARGET_SR, subtype="FLOAT")


def main() -> int:
    run = sys.argv[1] if len(sys.argv) > 1 else "run1"
    segments = json.loads((HERE / "segments.json").read_text())
    segments.pop("_comment", None)
    blind_map = {
        "created": "2026-08-15",
        "run_used": run,
        "activity_threshold_db_rel_mix": ACTIVITY_DB,
        "stem_target_lufs": STEM_LUFS,
        "mix_target_lufs": MIX_LUFS,
        "resample": f"全試聴コピーを{TARGET_SR}Hzへsoxrで統一（demucsは44.1k出力・Salva本番も元SRへresampleするため条件一致）",
        "entries": [],
    }

    for song, sconf in segments.items():
        rdir = HERE / "runs" / song
        track = REFS / song / "track.wav"
        for segname, seg in sconf["segments"].items():
            start, end = seg["start"], seg["end"]
            mix = load_seg(track, start, end)
            mix_rms = rms_db(mix)
            mix_gain_db = MIX_LUFS - lufs(mix)
            for pair, conf in PAIRS.items():
                # X/Y割当: hashの先頭バイト偶数ならX=demucs
                h = hashlib.sha256(f"{song}/{segname}/{pair}".encode()).digest()[0]
                demucs_is_x = h % 2 == 0
                d_paths = conf["demucs"](rdir, run)
                c_paths = conf["chal"](rdir, run)
                if not all(p.exists() for p in list(d_paths.values()) + list(c_paths.values())):
                    print(f"skip {song}/{segname}/{pair}: 出力不足")
                    continue
                entry = {
                    "song": song,
                    "segment": segname,
                    "pair": pair,
                    "X": "demucs" if demucs_is_x else "challenger",
                    "Y": "challenger" if demucs_is_x else "demucs",
                    "challenger": {"P4": "multi4s", "P6a": "sw", "P6b": "multi6s"}[pair],
                    "stems": {},
                }
                outdir = LISTEN / song / segname / pair
                # --- セットA: ステム単体
                for stem in conf["stems"]:
                    d = load_seg(d_paths[stem], start, end)
                    c = load_seg(c_paths[stem], start, end)
                    d_rel = rms_db(d) - mix_rms
                    c_rel = rms_db(c) - mix_rms
                    d_act = d_rel >= ACTIVITY_DB
                    c_act = c_rel >= ACTIVITY_DB
                    if d_act and c_act:
                        mode = "lufs_match"
                        d_out = gain_to(d, lufs(d), STEM_LUFS)
                        c_out = gain_to(c, lufs(c), STEM_LUFS)
                    else:
                        # 片方でも非アクティブ→共通ゲイン（mix基準）で相対音量を保存
                        mode = "common_gain"
                        g = 10 ** (mix_gain_db / 20.0)
                        d_out, c_out = d * g, c * g
                    x_wav, y_wav = (d_out, c_out) if demucs_is_x else (c_out, d_out)
                    write(outdir / stem / "X.wav", x_wav)
                    write(outdir / stem / "Y.wav", y_wav)
                    entry["stems"][stem] = {
                        "mode": mode,
                        "demucs_rel_db": round(d_rel, 1),
                        "chal_rel_db": round(c_rel, 1),
                        "demucs_active": d_act,
                        "chal_active": c_act,
                        "na_axes": [] if (d_act or c_act) else ["欠落", "アタック"],
                    }
                # --- セットB: 再加算
                def remix(paths):
                    acc = None
                    for p in paths:
                        x = load_seg(p, start, end)
                        if acc is None:
                            acc = x
                        else:
                            n = min(len(acc), len(x))
                            acc = acc[:n] + x[:n]
                    return acc

                d_mix = remix(conf["group_stems_demucs"](rdir, run))
                c_mix = remix(conf["group_stems_chal"](rdir, run))
                d_mix = gain_to(d_mix, lufs(d_mix), MIX_LUFS)
                c_mix = gain_to(c_mix, lufs(c_mix), MIX_LUFS)
                orig = gain_to(mix, lufs(mix), MIX_LUFS)
                x_mix, y_mix = (d_mix, c_mix) if demucs_is_x else (c_mix, d_mix)
                write(outdir / "remix" / "X.wav", x_mix)
                write(outdir / "remix" / "Y.wav", y_mix)
                write(outdir / "remix" / "original.wav", orig)
                blind_map["entries"].append(entry)

    MAP_OUT.parent.mkdir(parents=True, exist_ok=True)
    MAP_OUT.write_text(json.dumps(blind_map, ensure_ascii=False, indent=2))
    print(f"listen/ 生成完了。対応表: {MAP_OUT}（回答完了まで開かない）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
