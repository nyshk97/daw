#!/usr/bin/env python
"""looproots.py の回帰テスト。合成の三和音ループで抽出と契約検証を固定する。

実行: tools/reference/.venv/bin/python tools/library/tests/test_looproots.py
"""
import json
import shutil
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from looproots import (  # noqa: E402
    CONTRACT_VERSION, extract, load_contract, validate_contract, validate_roots_core,
)

SR = 22050
TRIADS = {  # ルートpc → 三和音の周波数（低めの音域。ルートを1本重ねて重心を明確に）
    9: (110.0, 130.81, 164.81, 220.0),   # Am
    5: (87.31, 110.0, 130.81, 174.61),   # F
    0: (130.81, 164.81, 196.0, 261.63),  # C
    7: (98.0, 123.47, 146.83, 196.0),    # G
}


def chord_loop(progression: list[int], bpm: float, slots_per_bar: int = 1) -> np.ndarray:
    """スロットごとに三和音を鳴らす合成ループ（振幅一定・純音の和）。"""
    slot_sec = 240.0 / bpm / slots_per_bar
    parts = []
    for pc in progression:
        t = np.arange(int(slot_sec * SR)) / SR
        freqs = TRIADS[pc]
        amps = (1.0, 0.7, 0.7, 0.5)
        y = sum(a * np.sin(2 * np.pi * f * t) for f, a in zip(freqs, amps))
        parts.append(y / np.abs(y).max() * 0.5)
    return np.concatenate(parts)


def test_extract_progression() -> None:
    prog = [9, 5, 0, 7]  # Am F C G を1小節ずつ
    y = chord_loop(prog, bpm=80, slots_per_bar=1)
    c = extract(y, SR, bpm=80, bars=4, slots_per_bar=1, key_root=9, key_mode="minor", source="t.wav")
    assert c["roots"] == prog, f"進行の抽出: {c['roots']} != {prog}"
    assert c["degraded"] is False
    assert all(v > 0.5 for v in c["confidence"]), f"三和音の照合スコアが低すぎ: {c['confidence']}"
    assert c["loop_bars"] == 4 and c["slots_per_bar"] == 1 and c["version"] == CONTRACT_VERSION
    print("OK extract progression:", c["roots"], c["confidence"])


def test_extract_slots_per_bar_2() -> None:
    # 1小節保持のコードをスロット2分割で読む → 各小節で同じルートが2回並ぶ
    y = chord_loop([9, 9, 5, 5], bpm=80, slots_per_bar=2)  # 2小節ぶん（スロット4個）
    c = extract(y, SR, bpm=80, bars=2, slots_per_bar=2, key_root=9, key_mode="minor")
    assert c["roots"] == [9, 9, 5, 5], c["roots"]
    print("OK extract slots_per_bar=2")


def test_single_note_degrades_to_tonic() -> None:
    """単音はテンプレート一致のスコア自体は高く出る（A単音が "A5" に0.85）ので、
    有効ピッチクラス数の判定で退化することを固定する。"""
    # 持続単音: A を4小節
    t = np.arange(int(4 * 240.0 / 80 * SR)) / SR
    y = 0.5 * np.sin(2 * np.pi * 110.0 * t)
    c = extract(y, SR, bpm=80, bars=4, slots_per_bar=2, key_root=0, key_mode="major")
    assert c["degraded"] is True, f"持続単音が退化しない: {c['confidence']}"
    assert c["roots"] == [0] * 8, f"トニック(C)でなく単音(A)を採用した: {c['roots']}"

    # 単音メロディ: 小節ごとに別の単音（A → B → C → E）
    bar = int(240.0 / 80 * SR)
    tb = np.arange(bar) / SR
    y = np.concatenate([0.5 * np.sin(2 * np.pi * f * tb) for f in (110.0, 123.47, 130.81, 164.81)])
    c = extract(y, SR, bpm=80, bars=4, slots_per_bar=1, key_root=0, key_mode="major")
    assert c["degraded"] is True, f"単音メロディが退化しない: {c['confidence']}"
    assert c["roots"] == [0] * 4, c["roots"]

    # スロット**内**を動く単音メロディ（4分音符×4/小節）。スロット平均のクロマで数えると
    # 3音以上に見えて和音扱いになる（レビューで実測）— フレーム単位の同時発音数なら退化する
    beat = bar // 4
    tq = np.arange(beat) / SR
    seq = [110.0, 123.47, 130.81, 146.83, 164.81, 174.61, 196.0, 220.0]
    y = np.concatenate([0.5 * np.sin(2 * np.pi * f * tq) for f in seq])  # 2小節ぶん
    c = extract(y, SR, bpm=80, bars=2, slots_per_bar=1, key_root=0, key_mode="major")
    assert c["degraded"] is True, f"スロット内を動く単音メロディが和音扱いされた: {c['confidence']}"
    assert c["roots"] == [0] * 2, c["roots"]
    print("OK 単音 → tonic 退化")


def test_silence_degrades_to_tonic() -> None:
    y = np.zeros(int(4 * 240.0 / 80 * SR))
    c = extract(y, SR, bpm=80, bars=4, slots_per_bar=2, key_root=2, key_mode="minor")
    assert c["degraded"] is True
    assert c["roots"] == [2] * 8, c["roots"]
    assert all(v == 0.0 for v in c["confidence"])
    print("OK silence → tonic 退化")


def test_validate() -> None:
    good = {"version": 1, "slots_per_bar": 2, "loop_bars": 2, "roots": [9, 9, 5, 7],
            "confidence": [0.9, 0.9, 0.9, 0.9], "degraded": False,
            "key_root": 9, "key_mode": "minor", "source": "x.wav"}
    assert validate_contract(dict(good)) == good

    def bad(mutate: dict, needle: str) -> None:
        d = {**good, **mutate}
        try:
            validate_contract(d)
            raise AssertionError(f"{mutate}: ValueError が出ていない")
        except ValueError as e:
            assert needle in str(e), f"{mutate}: {e}"

    bad({"version": 2}, "version")
    bad({"slots_per_bar": 3}, "slots_per_bar")
    bad({"loop_bars": 0}, "loop_bars")
    bad({"loop_bars": True}, "loop_bars")             # bool は int 扱いしない
    bad({"roots": [9, 9, 5]}, "roots の長さ")
    bad({"roots": [9, 9, 5, -1]}, "0..11")
    bad({"confidence": [0.9, 0.9, 0.9, 1.5]}, "confidence")
    bad({"degraded": "no"}, "degraded")
    bad({"key_root": 12}, "key_root")
    bad({"key_mode": "dorian"}, "key_mode")

    try:
        validate_roots_core(2, 2, [9, 9, 5, "G"])
        raise AssertionError("型違いが通った")
    except ValueError:
        pass
    print("OK validate")


def test_load_contract_roundtrip() -> None:
    tmp = Path(tempfile.mkdtemp(prefix="lrtest-"))
    try:
        y = chord_loop([9, 5, 0, 7], bpm=80)
        c = extract(y, SR, bpm=80, bars=4, slots_per_bar=1, key_root=9, key_mode="minor")
        p = tmp / "roots.json"
        p.write_text(json.dumps(c))
        assert load_contract(p) == c
        (tmp / "broken.json").write_text("{oops")
        try:
            load_contract(tmp / "broken.json")
            raise AssertionError("壊れたJSONが通った")
        except ValueError:
            pass
        print("OK load_contract roundtrip")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_cli_bars_omitted() -> None:
    """--bars 省略時は音声から小節数を推定する（estimate_bars のシグネチャ変更で
    TypeError になっていた回帰。アプリは常に --bars を渡すので CLI 経由でしか踏めない）。"""
    import json
    import subprocess
    import soundfile as sf
    tmp = Path(tempfile.mkdtemp(prefix="lrcli-"))
    try:
        wav = tmp / "loop_Am_120bpm.wav"
        sf.write(wav, chord_loop([9, 5, 0, 7], bpm=120.0), SR)  # 4スロット=4小節ちょうど
        out = tmp / "roots.json"
        script = Path(__file__).resolve().parent.parent / "looproots.py"
        subprocess.run([sys.executable, str(script), str(wav), "--out", str(out)],
                       capture_output=True, text=True, check=True)
        contract = json.loads(out.read_text())
        assert contract["loop_bars"] == 4, contract
        print("OK cli --bars omitted")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    test_extract_progression()
    test_extract_slots_per_bar_2()
    test_single_note_degrades_to_tonic()
    test_silence_degrades_to_tonic()
    test_validate()
    test_load_contract_roundtrip()
    test_cli_bars_omitted()
    print("all tests passed")
