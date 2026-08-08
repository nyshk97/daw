#!/usr/bin/env python
"""bass.py --roots（ループ追従モード）の回帰テスト。

契約: 進行は looproots 契約 JSON のルート列で固定され、振り直しで変わるのはリズムだけ。
契約の形式違反は即エラー（黙った退化はしない）。--from はループ追従込みで完全再現できる。

使い方: tools/reference/.venv/bin/python tools/gacha/tests/test_bass_roots.py
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import mido

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import bass  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "fixtures"
BASS_PY = Path(__file__).resolve().parent.parent / "bass.py"
CHILL = FIXTURES / "bass-chill.json"
checks = 0


def ok(cond: bool, msg: str) -> None:
    global checks
    assert cond, msg
    checks += 1


def run_cli(*args: str, expect_rc: int = 0) -> subprocess.CompletedProcess:
    r = subprocess.run([sys.executable, str(BASS_PY), *args], capture_output=True, text=True)
    assert r.returncode == expect_rc, f"exit {r.returncode} (期待 {expect_rc}): {r.stderr}\n{r.stdout}"
    return r


def midi_notes(path: Path) -> list[tuple[int, int, int]]:
    mid = mido.MidiFile(path)
    notes, now = [], 0
    for m in mid.tracks[0]:
        now += m.time
        if m.type == "note_on" and m.velocity > 0:
            notes.append((now, m.note, m.velocity))
    return notes


CONTRACT = {
    "version": 1,
    "slots_per_bar": 2,
    "loop_bars": 2,
    "roots": [9, 9, 5, 7],  # Am Am F G
    "confidence": [0.9, 0.85, 0.8, 0.88],
    "degraded": False,
    "key_root": 9,
    "key_mode": "minor",
    "source": "guitar_Am_82bpm.wav",
}
HARMONY_TICKS = bass.TICKS_BAR // CONTRACT["slots_per_bar"]
PATTERN_TICKS = CONTRACT["loop_bars"] * bass.TICKS_BAR


def first_pc_per_slot(notes: list[tuple[int, int, int]]) -> dict[int, int]:
    """ハーモニースロットごとに最初のノートのピッチクラス（=ルートのはず）。"""
    seen: dict[int, int] = {}
    for tick, note, _vel in sorted(notes):
        hslot = (tick % PATTERN_TICKS) // HARMONY_TICKS
        if hslot not in seen:
            seen[hslot] = note % 12
    return seen


def write_contract(d: dict, path: Path) -> Path:
    path.write_text(json.dumps(d, ensure_ascii=False))
    return path


def test_roots_fix_progression_rhythm_gacha() -> None:
    tmp = Path(tempfile.mkdtemp(prefix="roots-"))
    ct = write_contract(CONTRACT, tmp / "roots.json")
    out = tmp / "out"
    run_cli(str(CHILL), "--key", "A:minor", "--seed", "7", "--count", "3",
            "--roots", str(ct), "--out", str(out))
    mids = sorted(out.glob("*.mid"))
    ok(len(mids) == 3, f"候補3件のはず: {len(mids)}")

    tick_sets = []
    for mid in mids:
        notes = midi_notes(mid)
        ok(len(notes) > 0, f"{mid.name}: ノートが無い")
        for hslot, pc in first_pc_per_slot(notes).items():
            ok(pc == CONTRACT["roots"][hslot],
               f"{mid.name}: slot{hslot} の頭 pc={pc} が契約 roots={CONTRACT['roots'][hslot]} でない")
        tick_sets.append(tuple(t for t, _n, _v in sorted(notes)))
    ok(len(set(tick_sets)) >= 2, "3候補のリズムが全部同じ（rhythm レーンがガチャされていない）")

    # sidecar には roots の最小セットだけが入る（confidence 等は同一性の外）
    sc = json.loads(sorted(out.glob("*.json"))[0].read_text())
    ok(sc["config"]["roots"] == {"slots_per_bar": 2, "loop_bars": 2, "roots": [9, 9, 5, 7]},
       f"sidecar の roots が最小セットでない: {sc['config']['roots']}")

    # --from で完全再現（ループ追従込み）
    sc_path = sorted(out.glob("*.json"))[0]
    redo = tmp / "redo"
    run_cli("--from", str(sc_path), "--out", str(redo))
    orig_mid = sc_path.with_suffix(".mid")
    redo_mid = redo / orig_mid.name
    ok(redo_mid.read_bytes() == orig_mid.read_bytes(), "--from の .mid がバイト一致しない")
    print(f"OK roots 固定・rhythm ガチャ・sidecar・--from（checks={checks}）")


def test_bars_repeat_and_multiple() -> None:
    tmp = Path(tempfile.mkdtemp(prefix="roots-"))
    ct = write_contract(CONTRACT, tmp / "roots.json")
    out = tmp / "out"
    # 契約 loop_bars=2 に対して --bars 3 は倍数でない → エラー
    r = run_cli(str(CHILL), "--key", "A:minor", "--seed", "1", "--count", "1",
                "--roots", str(ct), "--bars", "3", "--out", str(out), expect_rc=1)
    ok("loop_bars=2" in r.stderr, f"倍数エラーの文言: {r.stderr}")
    # --bars 4 はパターンの完全な繰り返し
    run_cli(str(CHILL), "--key", "A:minor", "--seed", "1", "--count", "1",
            "--roots", str(ct), "--bars", "4", "--out", str(out))
    notes = sorted(midi_notes(next(out.glob("*.mid"))))
    half = len(notes) // 2
    ok(len(notes) % 2 == 0, "4小節 = 2小節パターン×2 でノート数が偶数のはず")
    shifted = [(t + PATTERN_TICKS, n, v) for t, n, v in notes[:half]]
    ok(shifted == notes[half:], "繰り返しがパターンの完全なコピーでない")
    print(f"OK --bars 検証と繰り返し（checks={checks}）")


def test_prog_seed_normalized() -> None:
    """追従時は prog レーンが生成に効かないので seed を 0 に正規化する —
    rhythm ロック時に「同じ音の候補が別名で複数生成される」のを防ぐ。"""
    tmp = Path(tempfile.mkdtemp(prefix="roots-"))
    ct = write_contract(CONTRACT, tmp / "roots.json")
    out = tmp / "out"
    r = run_cli(str(CHILL), "--key", "A:minor", "--seed", "5", "--count", "3",
                "--roots", str(ct), "--lock", "rhythm=00000001", "--out", str(out))
    mids = list(out.glob("*.mid"))
    ok(len(mids) == 1, f"rhythm ロック時は1候補に畳まれるはず: {len(mids)}")
    ok("p00000000" in mids[0].name, f"prog seed が 0 に正規化されていない: {mids[0].name}")
    ok(r.stdout.count("[スキップ]") == 2, f"重複2件がスキップ申告されるはず:\n{r.stdout}")
    print(f"OK prog seed 正規化と重複排除（checks={checks}）")


def test_invalid_contracts_and_lock() -> None:
    tmp = Path(tempfile.mkdtemp(prefix="roots-"))
    out = tmp / "out"

    def expect_error(mutate: dict, needle: str) -> None:
        d = {**CONTRACT, **mutate}
        ct = write_contract(d, tmp / "bad.json")
        r = run_cli(str(CHILL), "--key", "A:minor", "--seed", "1", "--count", "1",
                    "--roots", str(ct), "--out", str(out), expect_rc=1)
        ok(needle in r.stderr, f"{mutate}: エラー文言に {needle!r} が無い: {r.stderr}")

    expect_error({"roots": [9, 9, 5]}, "roots の長さ")                     # 長さ不一致
    expect_error({"roots": [9, 9, 5, 12]}, "0..11")                        # 範囲外
    expect_error({"roots": [9, 9, 5, 7.0]}, "0..11")                       # 型違い（float）
    expect_error({"slots_per_bar": 3}, "slots_per_bar")                    # 不正な分割
    expect_error({"version": 2}, "version")                                # 版違い
    expect_error({"confidence": [0.9]}, "confidence")                      # 長さ不一致

    # --lock prog は追従と矛盾するので拒否
    ct = write_contract(CONTRACT, tmp / "roots.json")
    r = run_cli(str(CHILL), "--key", "A:minor", "--seed", "1", "--count", "1",
                "--roots", str(ct), "--lock", "prog=00000001", "--out", str(out), expect_rc=1)
    ok("prog ロック" in r.stderr, f"prog ロック拒否の文言: {r.stderr}")

    # 退化契約（トニック連打）はそのまま通る
    degraded = {**CONTRACT, "roots": [9, 9, 9, 9], "degraded": True}
    ct2 = write_contract(degraded, tmp / "deg.json")
    run_cli(str(CHILL), "--key", "A:minor", "--seed", "2", "--count", "1",
            "--roots", str(ct2), "--out", str(out / "deg"))
    notes = midi_notes(next((out / "deg").glob("*.mid")))
    for hslot, pc in first_pc_per_slot(notes).items():
        ok(pc == 9, f"退化契約: slot{hslot} の頭が トニック(9) でない: {pc}")
    print(f"OK 契約検証・lock 拒否・退化契約（checks={checks}）")


if __name__ == "__main__":
    test_roots_fix_progression_rhythm_gacha()
    test_bars_repeat_and_multiple()
    test_prog_seed_normalized()
    test_invalid_contracts_and_lock()
    print(f"all tests passed ({checks} checks)")
