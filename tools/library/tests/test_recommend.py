#!/usr/bin/env python
"""recommend.py の回帰テスト。ランキングは純関数（rank）を合成の特徴量で固定する。

実行: tools/reference/.venv/bin/python tools/library/tests/test_recommend.py
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
from recommend import (  # noqa: E402
    load_ref_meta, rank, reason_text, tempo_relation, transpose_semitones, upper_features,
)

BB = {"sub_20_80": 0.1, "low_80_250": 0.2, "mid_250_2k": 0.5, "hi_2k_6k": 0.15, "air_6k_11k": 0.05}


def feats(centroid=800.0, bb=None, hp=3.0, onset=2.0):
    return {"spectral_centroid_hz_median": centroid, "band_balance": bb or dict(BB),
            "harmonic_percussive_ratio": hp, "onset_rate_per_s": onset, "duration_s": 6.0,
            "rms_db": -12.0, "spectral_rolloff95_hz": 4000.0}


def entry(path, bpm=80.0, root=9, mode="minor", contrast=False, **fkw):
    return {"path": path, "kind": "loop", "is_contrast": contrast, "pack": "P",
            "bpm": bpm, "bpm_source": "filename", "key_root": root, "key_mode": mode,
            "key_source": "filename", "key_confidence": None, "loop_bars_estimate": 2,
            "features": feats(**fkw)}


REF = {"bpm": 80.0, "key_root": 9, "key_mode": "minor", "key_trusted": True}  # A minor


def test_transpose_semitones() -> None:
    # A minor のリファレンスに対して
    assert transpose_semitones(0, "major", 9, "minor") == 0    # C major は平行調 = そのまま
    assert transpose_semitones(9, "minor", 9, "minor") == 0    # 同キー
    assert transpose_semitones(11, "minor", 9, "minor") == -2  # B minor → 2半音下げ
    assert transpose_semitones(10, "major", 9, "minor") == 2   # Bb major → 2半音上げ
    assert transpose_semitones(1, "minor", 9, "minor") is None  # C# minor は4半音 → 圏外
    assert transpose_semitones(7, "major", 9, "minor") is None  # G major は5半音 → 圏外
    print("OK transpose_semitones")


def test_tempo_relation() -> None:
    assert tempo_relation(84, 80) == ("same", 1.05)
    assert tempo_relation(92, 80) is None    # +15% は圏外
    assert tempo_relation(160, 80) is None   # 倍テンポ表記は許さない（±10%のみ = 設計文書どおり）
    assert tempo_relation(40, 80) is None    # 半分テンポ表記も同様
    # 境界: ±10% ちょうどは含む（1/1.1 を下限にすると −10% を弾く、の再発防止）
    assert tempo_relation(72, 80) == ("same", 0.9)
    assert tempo_relation(88, 80) is not None
    assert tempo_relation(71, 80) is None
    assert tempo_relation(89, 80) is None
    print("OK tempo_relation")


def test_rank_ordering_and_filters() -> None:
    ref_f = feats(centroid=800.0)
    entries = [
        entry("loops/P/bright.wav", centroid=3200.0),          # 2オクターブ明るい
        entry("loops/P/close.wav", centroid=850.0),            # ほぼ同じ
        entry("loops/P/dark.wav", centroid=400.0),             # 1オクターブ暗い
        entry("loops/P/wrongkey.wav", root=1, mode="minor"),   # C#m → キー圏外
        entry("loops/P/wrongbpm.wav", bpm=100.0),              # +25% → BPM圏外
        entry("_c/edm.wav", bpm=142.0, root=5, mode="minor", contrast=True, centroid=5000.0),
    ]
    ranked = rank(entries, REF, ref_f)
    paths = [c["path"] for c in ranked]
    assert paths == ["loops/P/close.wav", "loops/P/dark.wav", "loops/P/bright.wav"], paths
    assert all(not c["is_contrast"] for c in ranked)  # 対照群は既定で候補に出ない

    # skip_filters: 検証用にはキー/BPM 圏外も対照群も土俵に乗る
    ranked_all = rank(entries, REF, ref_f, include_contrast=True, skip_filters=True)
    assert len(ranked_all) == 6
    assert ranked_all[-1]["path"] == "_c/edm.wav"  # ジャンル外（明るすぎ）が最下位に沈む
    print("OK rank ordering/filters")


def test_variant_grouping() -> None:
    # 同曲のテイク番号・_LOFI 質感違いは距離最小の1本に集約される（1曲1枠）
    entries = [
        entry("loops/APS/LHG_80_Am_Song_Electric_Guitar_1.wav", centroid=850.0),
        entry("loops/APS/LHG_80_Am_Song_Electric_Guitar_2.wav", centroid=900.0),
        entry("loops/APS/LHG_80_Am_Song_Electric_Guitar_1_LOFI.wav", centroid=950.0),
        entry("loops/APS/LHG_80_Am_Other_Electric_Guitar_1.wav", centroid=1200.0),
    ]
    ranked = rank(entries, REF, feats(centroid=800.0))
    paths = [c["path"] for c in ranked]
    assert paths == ["loops/APS/LHG_80_Am_Song_Electric_Guitar_1.wav",
                     "loops/APS/LHG_80_Am_Other_Electric_Guitar_1.wav"], paths
    assert ranked[0]["group_size"] == 3   # _2 と _1_LOFI が集約された
    assert ranked[1]["group_size"] == 1

    # 末尾が Min/Maj で終わる命名（Cymatics）はサフィックスが剥がれない = 全件独立のまま
    cyma = [
        entry("loops/C/Cymatics - Oracle Melody Loop 2 - 80 BPM A Min.wav", centroid=850.0),
        entry("loops/C/Cymatics - Oracle Melody Loop 4 - 80 BPM A Min.wav", centroid=900.0),
    ]
    assert len(rank(cyma, REF, feats(centroid=800.0))) == 2

    # 名前が同じでもキーや BPM が違えば別グループ（同曲とみなす根拠が崩れるため）
    keyed = [
        entry("loops/APS/LHG_80_Am_Song_Electric_Guitar_1.wav", root=9, centroid=850.0),
        entry("loops/APS/LHG_80_Bm_Song_Electric_Guitar_1.wav", root=11, centroid=900.0),
    ]
    assert len(rank(keyed, REF, feats(centroid=800.0))) == 2
    print("OK variant grouping")


def test_rank_deterministic_tiebreak() -> None:
    ref_f = feats()
    entries = [entry("loops/P/b.wav"), entry("loops/P/a.wav")]  # 特徴量が完全に同じ
    r1 = rank(entries, REF, ref_f)
    r2 = rank(list(reversed(entries)), REF, ref_f)
    assert [c["path"] for c in r1] == [c["path"] for c in r2] == ["loops/P/a.wav", "loops/P/b.wav"]
    print("OK deterministic tiebreak")


def test_reason_text_plain_words() -> None:
    ref_f = feats()
    ranked = rank([entry("loops/P/x.wav", centroid=850.0)], REF, ref_f)
    reason = ranked[0]["reason"]
    assert reason, "理由が空"
    for banned in ("重心", "centroid", "HPSS", "スペクトル", "オンセット", "hp_ratio"):
        assert banned not in reason, f"内部用語が理由に漏れた: {banned} in {reason}"
    print("OK reason plain words:", reason)


def test_rank_end_to_end_with_wavs() -> None:
    """特徴抽出（compute_features）→ index → rank を実 wav で通す。
    特徴量の実装が壊れたら順位で検出する（合成特徴量のテストだけだと素通りする）。"""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from index import build_index, compute_features  # noqa: E402

    sr = 22050

    def wav(path: Path, freqs, bpm=80.0, bars=2, notes_per_beat=1.0):
        dur = bars * 240.0 / bpm
        t = np.arange(int(dur * sr)) / sr
        y = sum(np.sin(2 * np.pi * f * t) for f in freqs)
        phase = (t * bpm / 60.0 * notes_per_beat) % 1.0  # 音数（オンセット密度）を作り分ける
        y = 0.5 * y * np.clip(1.0 - phase * 2.5, 0.1, 1.0) / np.abs(y).max()
        path.parent.mkdir(parents=True, exist_ok=True)
        sf.write(path, y.astype(np.float32), sr)

    tmp = Path(tempfile.mkdtemp(prefix="rece2e-"))
    try:
        wav(tmp / "loops/P/dark_Am_80bpm.wav", (110.0, 165.0, 220.0), notes_per_beat=0.5)
        wav(tmp / "loops/P/bright_Am_80bpm.wav", (1760.0, 2640.0, 3520.0), notes_per_beat=4.0)
        index, summary = build_index(tmp)
        assert summary["new"] == 2, summary

        # リファレンスの上モノ相当: 暗く疎（dark 側と同系だが同一ではない音）
        t = np.arange(int(6.0 * sr)) / sr
        ref_y = 0.5 * (np.sin(2 * np.pi * 130.0 * t) + np.sin(2 * np.pi * 195.0 * t))
        ref_y *= np.clip(1.0 - ((t * 80 / 60.0 * 0.5) % 1.0) * 2.5, 0.1, 1.0)
        ref_f = compute_features(ref_y, sr)["features"]

        ranked = rank(index["entries"], REF, ref_f)
        assert [c["path"] for c in ranked] == \
            ["loops/P/dark_Am_80bpm.wav", "loops/P/bright_Am_80bpm.wav"], \
            [(c["path"], c["score"]) for c in ranked]
        print("OK end-to-end rank with real wavs")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_cli_page_size() -> None:
    """--page-size がスライスと JSON 出力に効く（LaLa は画面に合わせて 10 を渡す）。"""
    import subprocess
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from index import build_index  # noqa: E402

    sr = 22050
    tmp = Path(tempfile.mkdtemp(prefix="recps-"))
    try:
        t = np.arange(int(6.0 * sr)) / sr
        for name, f in (("dark", 220.0), ("mid", 660.0), ("bright", 1760.0)):
            p = tmp / f"loops/P/{name}_Am_80bpm.wav"
            p.parent.mkdir(parents=True, exist_ok=True)
            sf.write(p, (0.4 * np.sin(2 * np.pi * f * t)).astype(np.float32), sr)
        index, _ = build_index(tmp)
        (tmp / "index.json").write_text(json.dumps(index))
        refdir = make_refdir(tmp)

        script = Path(__file__).resolve().parent.parent / "recommend.py"
        out = subprocess.run(
            [sys.executable, str(script), str(refdir), "--library", str(tmp),
             "--json", "--page-size", "2", "--page", "2"],
            capture_output=True, text=True, check=True).stdout
        result = json.loads(out)
        assert result["page_size"] == 2 and result["total"] == 3, result
        assert len(result["candidates"]) == 1  # 3本を2件/頁で割った2頁目は1本
        print("OK cli page size")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def make_refdir(root: Path, with_key: bool = True) -> Path:
    refdir = root / "ref"
    (refdir / "analysis").mkdir(parents=True)
    g = {"bpm": 80.0}
    if with_key:
        g["key"] = {"root": "A", "mode": "minor"}
    (refdir / "card.json").write_text(json.dumps({"global": g}))
    (refdir / "analysis" / "basics.json").write_text(json.dumps(
        {"key": {"top5": [{"key": "C major", "corr": 0.4}]}}))
    stems = refdir / "stems" / "htdemucs_6s" / "track"  # demucs の実出力と同じ1階層深い形
    stems.mkdir(parents=True)
    sr = 22050
    t = np.arange(sr // 2) / sr
    for name, f in (("piano", 220.0), ("guitar", 330.0), ("other", 440.0)):
        sf.write(stems / f"{name}.wav", (0.3 * np.sin(2 * np.pi * f * t)).astype(np.float32), sr)
    return refdir


def test_load_ref_meta_and_upper_features_cache() -> None:
    tmp = Path(tempfile.mkdtemp(prefix="rectest-"))
    try:
        refdir = make_refdir(tmp)
        meta = load_ref_meta(refdir)
        assert meta == {"bpm": 80.0, "key_root": 9, "key_mode": "minor", "key_trusted": True}

        f1 = upper_features(refdir)
        cache = refdir / "analysis" / "upper-features.json"
        assert cache.exists() and f1["spectral_centroid_hz_median"] > 0

        # キャッシュに目印を仕込む → sig が同じなら再計算せずキャッシュが返る証明
        data = json.loads(cache.read_text())
        data["features"]["spectral_centroid_hz_median"] = 12345.0
        cache.write_text(json.dumps(data))
        assert upper_features(refdir)["spectral_centroid_hz_median"] == 12345.0

        # ステムの mtime が変わったらキャッシュは無効（再計算されて目印が消える）
        stem = refdir / "stems" / "htdemucs_6s" / "track" / "piano.wav"
        ns = stem.stat().st_mtime_ns + 1_000_000
        os.utime(stem, ns=(ns, ns))
        assert upper_features(refdir)["spectral_centroid_hz_median"] != 12345.0

        # キーがゲート落ちしたカード → basics の推定1位に退避し、信頼フラグが折れる
        refdir2 = make_refdir(tmp / "2", with_key=False)
        meta2 = load_ref_meta(refdir2)
        assert (meta2["key_root"], meta2["key_mode"], meta2["key_trusted"]) == (0, "major", False)

        # カード無し（分析未完）は明示エラー
        try:
            load_ref_meta(tmp / "nocard")
            raise AssertionError("SystemExit が出ていない")
        except SystemExit:
            pass
        print("OK load_ref_meta / upper_features cache")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_cli_warm_cache() -> None:
    """--warm-cache はカード・ライブラリ無しでキャッシュだけ作る（analyze.py が分析中に呼ぶ）。"""
    import subprocess

    tmp = Path(tempfile.mkdtemp(prefix="recwarm-"))
    try:
        refdir = make_refdir(tmp)
        (refdir / "card.json").unlink()  # ゲート落ちでカードが無くても温められること
        script = Path(__file__).resolve().parent.parent / "recommend.py"
        subprocess.run([sys.executable, str(script), str(refdir), "--warm-cache"],
                       capture_output=True, text=True, check=True)
        cache = refdir / "analysis" / "upper-features.json"
        assert cache.exists(), "upper-features.json が作られていない"
        assert json.loads(cache.read_text())["features"]["spectral_centroid_hz_median"] > 0
        print("OK cli warm cache")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    test_transpose_semitones()
    test_tempo_relation()
    test_rank_ordering_and_filters()
    test_variant_grouping()
    test_rank_deterministic_tiebreak()
    test_reason_text_plain_words()
    test_rank_end_to_end_with_wavs()
    test_cli_page_size()
    test_cli_warm_cache()
    test_load_ref_meta_and_upper_features_cache()
    print("all tests passed")
