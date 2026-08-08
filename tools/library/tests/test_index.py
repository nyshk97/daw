#!/usr/bin/env python
"""index.py の回帰テスト。合成 wav のみ・数十秒で完走する。

実行: tools/reference/.venv/bin/python tools/library/tests/test_index.py
"""
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from index import (  # noqa: E402
    SCHEMA_VERSION, build_index, estimate_bars, parse_filename_meta, scan_files, write_atomic,
)

SR = 22050


def tonal_loop(path: Path, bpm: float = 80, bars: int = 2,
               freqs=(110.0, 130.81, 164.81, 220.0), amps=(1.0, 0.6, 0.6, 0.8)) -> None:
    """Aマイナー系の音程内容＋拍ごとの音量パルス（オンセット/テンポ推定の材料）を持つループ。"""
    dur = bars * 240.0 / bpm
    t = np.arange(int(dur * SR)) / SR
    y = sum(a * np.sin(2 * np.pi * f * t) for f, a in zip(freqs, amps))
    beat_phase = (t * bpm / 60.0) % 1.0
    env = np.clip(1.0 - beat_phase * 2.5, 0.15, 1.0)  # 各拍頭で立ち上がる減衰エンベロープ
    y = 0.5 * y * env / np.abs(y).max()
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(path, y.astype(np.float32), SR)


def test_parse_filename_meta() -> None:
    cases = [
        # (ファイル名, bpm, key_root, key_mode)
        ("Cymatics - Lofi Melody Loop 3 - 140 BPM G Min", 140.0, 7, "minor"),
        ("guitar_melody_Am_82bpm", 82.0, 9, "minor"),
        ("F#m 95 BPM pad", 95.0, 6, "minor"),
        ("Keys_Bbmaj_090bpm", 90.0, 10, "major"),
        ("A Minor 74bpm", 74.0, 9, "minor"),
        ("piano Dm7 loop 82 bpm", 82.0, 2, "minor"),
        ("bpm 96 E min keys", 96.0, 4, "minor"),
        ("Gm", None, 7, "minor"),
        ("16 bars 82bpm A#min", 82.0, 10, "minor"),
        ("Cymatics Gems Vol 1 - Purple Dreams - 126 A Min", 126.0, 9, "minor"),  # BPM単位なし
        ("Cymatics Gems Vol 3 - Memories - 160 BPM F min", 160.0, 5, "minor"),   # 小文字 min
        # 拾ってはいけないもの
        ("drum loop 128bpm", 128.0, None, None),      # キー表記なし
        ("album track 12", None, None, None),          # album の Bm / 裸の数字
        ("Ambient Emotional Camp", None, None, None),  # 音名＋m に見える英単語
        ("808 bpm mixup", None, None, None),           # 範囲外 BPM
        ("Ab summer", None, None, None),               # モード表記の無い裸の音名は拾わない
    ]
    for stem, bpm, root, mode in cases:
        got = parse_filename_meta(stem)
        assert got["bpm"] == bpm, f"{stem}: bpm {got['bpm']} != {bpm}"
        assert got["key_root"] == root, f"{stem}: root {got['key_root']} != {root}"
        assert got["key_mode"] == mode, f"{stem}: mode {got['key_mode']} != {mode}"
    print("OK parse_filename_meta")


def test_estimate_bars() -> None:
    assert estimate_bars(6.0, 80) == 2       # ちょうど2小節
    assert estimate_bars(6.2, 80) == 2       # 8%以内の誤差は丸める
    assert estimate_bars(7.0, 80) is None    # 2.33小節 — 余韻付き書き出し等は信用しない
    assert estimate_bars(12.0, 80) == 4
    assert estimate_bars(1.0, 80) is None    # 0.33小節
    print("OK estimate_bars")


def make_library(root: Path) -> None:
    tonal_loop(root / "loops/PackA/lofi_Am_80bpm.wav", bpm=80, bars=2)
    tonal_loop(root / "loops/PackA/noname.wav", bpm=120, bars=4)  # メタ無し → 推定経路
    tonal_loop(root / "_contrast/loops/PackB/edm_Fmin_142bpm.wav", bpm=142, bars=2)
    # インデックス対象外であるべきもの
    tonal_loop(root / "oneshots/PackC/kick_C_100bpm.wav", bpm=100, bars=1)
    # 壊れたファイル
    (root / "loops/PackA").mkdir(parents=True, exist_ok=True)
    (root / "loops/PackA/broken.wav").write_bytes(b"not a wav at all")


def test_build_index() -> None:
    tmp = Path(tempfile.mkdtemp(prefix="libtest-"))
    try:
        make_library(tmp)

        # --- 初回: 対象の列挙・メタ・特徴量 ---
        assert [p.as_posix() for p in scan_files(tmp)] == [
            "_contrast/loops/PackB/edm_Fmin_142bpm.wav",
            "loops/PackA/broken.wav",
            "loops/PackA/lofi_Am_80bpm.wav",
            "loops/PackA/noname.wav",
        ], "oneshots が混ざったか、_contrast/loops が漏れた"

        index, summary = build_index(tmp)
        assert summary == {"new": 3, "reused": 0, "skipped": 1, "removed": 0}, summary
        by_path = {e["path"]: e for e in index["entries"]}
        assert set(by_path) == {
            "loops/PackA/lofi_Am_80bpm.wav", "loops/PackA/noname.wav",
            "_contrast/loops/PackB/edm_Fmin_142bpm.wav",
        }

        e = by_path["loops/PackA/lofi_Am_80bpm.wav"]
        assert e["kind"] == "loop" and e["is_contrast"] is False and e["pack"] == "PackA"
        assert e["bpm"] == 80.0 and e["bpm_source"] == "filename"
        assert e["key_root"] == 9 and e["key_mode"] == "minor" and e["key_source"] == "filename"
        assert e["loop_bars_estimate"] == 2
        f = e["features"]
        assert 0 < f["spectral_centroid_hz_median"] < 2000  # 低域寄りの合成音
        assert abs(sum(f["band_balance"].values()) - 1.0) < 0.05
        assert f["onset_rate_per_s"] > 0.5  # 拍パルスがオンセットとして見えている

        c = by_path["_contrast/loops/PackB/edm_Fmin_142bpm.wav"]
        assert c["is_contrast"] is True and c["pack"] == "PackB"
        assert c["key_root"] == 5 and c["bpm"] == 142.0

        # --- メタ無しファイルは推定にフォールバック ---
        n = by_path["loops/PackA/noname.wav"]
        assert n["bpm_source"] == "estimate" and n["bpm"] is not None
        assert 108 <= n["bpm"] <= 132, f"120bpm パルスの推定が外れすぎ: {n['bpm']}"
        assert n["key_source"] == "estimate" and n["key_confidence"] is not None
        assert n["key_root"] == 9 and n["key_mode"] == "minor", \
            f"Aマイナー素材の推定: {n['key_root']} {n['key_mode']}"

        # --- 原子的書き込みと読み戻し ---
        write_atomic(tmp / "index.json", index)
        loaded = json.loads((tmp / "index.json").read_text())
        assert loaded["schema_version"] == SCHEMA_VERSION
        assert all(not Path(e["path"]).is_absolute() for e in loaded["entries"])

        # --- 2回目: 全件再利用（再分析しない） ---
        index2, summary2 = build_index(tmp)
        assert summary2 == {"new": 0, "reused": 3, "skipped": 1, "removed": 0}, summary2

        # --- 変更されたファイルだけ再分析 ---
        tonal_loop(tmp / "loops/PackA/lofi_Am_80bpm.wav", bpm=80, bars=4)  # 尺が変わる=サイズ変化
        index3, summary3 = build_index(tmp)
        assert summary3["new"] == 1 and summary3["reused"] == 2, summary3
        write_atomic(tmp / "index.json", index3)

        # --- 同サイズ・同一整数秒の上書きも検出する（mtime を ns で持っているから） ---
        target = tmp / "loops/PackA/lofi_Am_80bpm.wav"
        old_ns = next(e for e in index3["entries"]
                      if e["path"] == "loops/PackA/lofi_Am_80bpm.wav")["mtime_ns"]
        tonal_loop(target, bpm=80, bars=4,
                   freqs=(220.0, 261.63, 329.63, 440.0))  # 同じ尺=同じバイト数・中身だけ違う
        ns = old_ns + 1000  # 同じ整数秒のまま ns だけ進める（秒精度だと再利用扱いになる条件）
        os.utime(target, ns=(ns, ns))
        index3b, summary3b = build_index(tmp)
        assert summary3b["new"] == 1 and summary3b["reused"] == 2, \
            f"同サイズ・同秒の上書きが再利用扱いになった（mtime の精度不足）: {summary3b}"
        write_atomic(tmp / "index.json", index3b)

        # --- 消えたファイルは index から落ちる ---
        (tmp / "loops/PackA/noname.wav").unlink()
        index4, summary4 = build_index(tmp)
        assert summary4["removed"] == 1, summary4
        assert "loops/PackA/noname.wav" not in {e["path"] for e in index4["entries"]}

        print("OK build_index")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    test_parse_filename_meta()
    test_estimate_bars()
    test_build_index()
    print("all tests passed")
