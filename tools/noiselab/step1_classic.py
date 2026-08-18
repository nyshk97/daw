#!/usr/bin/env python3
"""学習目的の軽量比較: 無処理 vs 古典スペクトラルゲート（noisereduce・stationary）。

Step 0 で見送りが確定した後の追加実験（2026-08-18）。採用判定ではないため plan の
72行プロトコルは使わず、3すくみのブラインド比較に簡略化する:

  X/Y/Z（ランダム割当・固定シード）=
    - original      … 無処理
    - classic-ideal … ノイズプロファイル = 頭の意図的無音（古典の理想条件・ベストケース）
    - classic-auto  … ノイズプロファイル = 最静区間の自動検出（製品で使う条件。
                      今回の素材は連続フローで長い隙間が無く、声混入が予想される —
                      それ自体が「製品条件での古典の実力」の観察対象）

処理チェーンは plan の契約に準拠: 同SR（リサンプル不要）→ ラグ検査 → 声区間ゲイン一致 →
残差生成。除去音（残差）は listen/step1-classic/reveal/ に隔離（ブラインド回答前に聴くと
どれが処理済みか割れるため）。割当マップは human-answers 側に保存する。

使い方:
  .venv/bin/python step1_classic.py --src ~/Music/daw/2026-08-18-tundra
"""

import argparse
import json
import random
from pathlib import Path

import numpy as np
import noisereduce as nr
import soundfile as sf

OUT_DIR = Path(__file__).parent / "listen/step1-classic"
REVEAL_DIR = OUT_DIR / "reveal"
MAP_FILE = Path("/Users/d0ne1s/daw/docs/labs/reference-beat-human-answers/2026-08-18-noiselab-step1-classic-map.json")

MATERIALS = {"uta": "clip-008.wav", "rap": "clip-001.wav"}
OKE_START = 384000  # 判定区間の開始（step0 と同じ = オケ開始のタイムライン位置）
CLIP_START = {"uta": 192000, "rap": 288000}
HEAD_SILENCE_S = 5.0  # 意図的無音（ideal プロファイル / 自動検出の除外範囲）
PROP_DECREASE = 1.0
VOICE_THRESH_DB = -45.0
SEED = 20260818002  # 002: revealファイル名がラベルを漏らす版の割当が一度表示されたため振り直し


def db(v: float) -> float:
    return 20.0 * np.log10(max(v, 1e-12))


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x**2))) if len(x) else 0.0


def quietest_window(x: np.ndarray, sr: int, exclude_head_s: float, win_s: float = 1.0):
    """先頭 exclude_head_s を除いた最静窓（製品条件の自動検出）。開始サンプルと窓を返す。"""
    start0 = int(sr * exclude_head_s)
    win, hop = int(sr * win_s), int(sr * 0.1)
    best_start, best_rms = start0, float("inf")
    for s in range(start0, len(x) - win + 1, hop):
        r = rms(x[s : s + win])
        if r < best_rms:
            best_start, best_rms = s, r
    return best_start, x[best_start : best_start + win]


def voice_mask(x: np.ndarray, sr: int) -> np.ndarray:
    win, hop = int(sr * 0.1), int(sr * 0.05)
    mask = np.zeros(len(x), dtype=bool)
    for s in range(0, max(len(x) - win, 0) + 1, hop):
        if db(rms(x[s : s + win])) >= VOICE_THRESH_DB:
            mask[s : s + win] = True
    return mask


def check_lag(orig: np.ndarray, proc: np.ndarray, sr: int) -> int:
    """声区間の相互相関で推定ラグ（サンプル）。±2400（50ms）の範囲で探す。"""
    seg = slice(int(len(orig) * 0.3), int(len(orig) * 0.3) + sr * 3)
    a, b = orig[seg], proc[seg]
    n = 2400
    corr = np.correlate(a, b[n:-n], mode="valid")
    return int(np.argmax(corr)) - n


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, type=Path)
    args = ap.parse_args()
    src = args.src.expanduser()

    # 出力は毎回作り直す（ファイル名にゲイン値が入るため、条件を変えた再実行で旧ファイルが残ると
    # ブラインドセットに残骸が混ざる）
    import shutil
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)
    REVEAL_DIR.mkdir(parents=True)
    rng = random.Random(SEED)
    blind_map = {}

    for name, fname in MATERIALS.items():
        x, sr = sf.read(src / fname, dtype="float64", always_2d=True)
        x = x.mean(axis=1)
        sr = int(sr)

        # プロファイル2種。ideal の終端は「最初の声の立ち上がり - 0.2s」（固定秒数だと
        # 歌い出しの早いテイクで声が混入する。rap は 3.4s から歌が始まっていた）
        mask_full = voice_mask(x, sr)
        onset = int(np.argmax(mask_full)) if mask_full.any() else len(x)
        ideal_end = min(int(sr * (HEAD_SILENCE_S - 1.0)), onset - int(sr * 0.2))
        ideal = x[int(sr * 0.5) : ideal_end]
        assert len(ideal) >= sr, f"{name}: idealプロファイルが1秒未満 ({len(ideal)/sr:.2f}s)"
        auto_start, auto = quietest_window(x, sr, HEAD_SILENCE_S)
        print(f"[{name}] autoプロファイル: {auto_start/sr:.1f}s〜 RMS {db(rms(auto)):.1f}dBFS"
              f"（idealは {db(rms(ideal)):.1f}dBFS）")

        variants = {"original": x}
        for key, prof in (("classic-ideal", ideal), ("classic-auto", auto)):
            y = nr.reduce_noise(y=x, sr=sr, y_noise=prof, stationary=True,
                                prop_decrease=PROP_DECREASE)
            lag = check_lag(x, y, sr)
            assert abs(lag) <= 1, f"{name}/{key}: ラグ {lag} サンプル"
            mask = voice_mask(x, sr)
            gain = rms(x[mask]) / max(rms(y[mask]), 1e-12)
            y = y * gain
            print(f"  {key}: ラグ {lag}spl / 声区間ゲイン一致 {db(gain):+.2f}dB")
            variants[key] = y

        # 判定区間へトリムして匿名出力
        t0 = OKE_START - CLIP_START[name]
        labels = ["X", "Y", "Z"]
        rng.shuffle(labels)
        removed_idx = 1
        for label, (key, y) in zip(labels, variants.items()):
            trimmed = y[t0:]
            assert np.isfinite(trimmed).all() and np.abs(trimmed).max() < 1.0
            sf.write(OUT_DIR / f"{name}-{label}.wav", trimmed, sr, subtype="PCM_24")
            blind_map[f"{name}-{label}"] = key
            # 除去音（reveal 用。original は残差ゼロなのでスキップ）。
            # ファイル名からラベルが逆算できないよう匿名連番にし、対応はマップへ書く
            if key != "original":
                d = (x - y)[t0:]
                gain_db = -30.0 - db(rms(d))
                fname_out = f"{name}-removed-{removed_idx}.wav"
                removed_idx += 1
                sf.write(REVEAL_DIR / fname_out, d * (10 ** (gain_db / 20)), sr, subtype="PCM_24")
                blind_map[f"reveal/{fname_out}"] = {"label": label, "variant": key,
                                                   "amplified_db": round(gain_db, 1)}

    MAP_FILE.write_text(json.dumps(blind_map, indent=2))
    print(f"\n完了: {OUT_DIR}（対応表: {MAP_FILE.name} — 回答完了まで開かない）")


if __name__ == "__main__":
    main()
