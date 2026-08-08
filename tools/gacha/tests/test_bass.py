#!/usr/bin/env python
"""bass.py の回帰テスト（プレーン assert・pytest 不要。fixture のみで数秒〜数十秒）。

drums.py のテストと同じ方針: 決定性はプロセスをまたいでバイト比較、契約（候補の同一性・
--from 再現・改変検出）は CLI 経由、生成ロジックの性質（ルート列・音域・モノフォニック・
同時打ちスナップ）はモジュール関数で固定する。

使い方: tools/reference/.venv/bin/python tools/gacha/tests/test_bass.py
"""
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import mido
import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import bass  # noqa: E402
import drums  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "fixtures"
BASS_PY = Path(__file__).resolve().parent.parent / "bass.py"
DRUMS_PY = Path(__file__).resolve().parent.parent / "drums.py"
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
    """(絶対tick, note, velocity) の note_on 列。"""
    mid = mido.MidiFile(path)
    notes, now = [], 0
    for m in mid.tracks[0]:
        now += m.time
        if m.type == "note_on" and m.velocity > 0:
            notes.append((now, m.note, m.velocity))
    return notes


def circular_root_changes(seq: list[int]) -> int:
    return sum(1 for i in range(len(seq)) if seq[i] != seq[(i + 1) % len(seq)])


CHILL = FIXTURES / "bass-chill.json"
TRAP = FIXTURES / "bass-trap.json"

# --- キーのパース ---
ok(bass.parse_key("F#:minor") == {"root": 6, "mode": "minor"}, "F#:minor")
ok(bass.parse_key("Db:major") == {"root": 1, "mode": "major"}, "♭の別名も同じピッチクラス")
ok(bass.parse_key("c:major") == {"root": 0, "mode": "major"}, "小文字も受ける")
for bad in ("F#", "H:minor", "F#:dorian", "F##:minor"):
    try:
        bass.parse_key(bad)
        ok(False, f"{bad} でエラーになるはず")
    except bass.GachaError:
        ok(True, f"{bad} を拒否")

# --- ルート列: 変化数が性格（root_changes_per_loop）に追従・トニック開始・スケール内 ---
# my-way 実カード相当（8小節・16スロット・変化数14）— chill 系の主要経路を退化させない
chill_chords = {"loop_bars": 8, "slots_per_bar": 2, "root_changes_per_loop": 14,
                "root_motion": "mixed", "has_7th_or_more": True}
key_fs_major = {"root": 6, "mode": "major"}
scale_pcs = {(6 + d) % 12 for d in bass.MAJOR_DEGREES}
for seed in (1, 2, 42, 1234):
    seq = bass.generate_root_sequence(chill_chords, key_fs_major, np.random.default_rng(seed))
    ok(len(seq) == 16, f"seed {seed}: 16スロット（loop_bars×slots_per_bar）")
    ok(seq[0] == 6, f"seed {seed}: スロット0はトニック")
    ok(circular_root_changes(seq) == 14, f"seed {seed}: 循環の変化数が14に一致: {seq}")
    ok(set(seq) <= scale_pcs, f"seed {seed}: 全ルートがスケール内: {seq}")

# 変化数0 → 全スロット・トニック / 変化数1 → 循環で構成できないため2に繰り上げ
seq0 = bass.generate_root_sequence({**chill_chords, "root_changes_per_loop": 0},
                                   key_fs_major, np.random.default_rng(1))
ok(seq0 == [6] * 16, "変化数0はトニック連打")
seq1 = bass.generate_root_sequence({**chill_chords, "root_changes_per_loop": 1},
                                   key_fs_major, np.random.default_rng(1))
ok(circular_root_changes(seq1) == 2, f"変化数1は2に繰り上げ: {seq1}")
# chords 無し → 1スロット退化
ok(bass.generate_root_sequence(None, key_fs_major, np.random.default_rng(1)) == [6],
   "chords 無しはルート固定へ退化")

# root_motion=up: 変化ごとのスケール度数ステップが上行に寄る（厳密な単調でなく方向性の検証）
seq_up = bass.generate_root_sequence({**chill_chords, "root_motion": "up", "root_changes_per_loop": 6},
                                     key_fs_major, np.random.default_rng(3))
ok(circular_root_changes(seq_up) == 6, f"up でも変化数は一致: {seq_up}")

# --- 全12キー: ルートは窓 28..39 に置かれ、装飾込みでもハード範囲 28..51 に収まる ---
eff_trap, bpm_trap, _ = bass.parse_card(json.loads(TRAP.read_text()), "trap")
eff_chill, bpm_chill, _ = bass.parse_card(json.loads(CHILL.read_text()), "chill")
for pc in range(12):
    key = {"root": pc, "mode": "minor"}
    seeds = bass.resolve_lane_seeds("bass", bass.LANES, 42, {})
    notes_trap = bass.generate_notes(eff_trap, key, [], seeds, bars=1, bpm=bpm_trap)
    root_midi = 28 + (pc - 4) % 12
    ok(all(n == root_midi for _, n, _ in notes_trap),
       f"pc {pc}: chords 無しは全ノートがルート（窓 28..39 内 = {root_midi}）")
    ok(28 <= root_midi <= 39, f"pc {pc}: ルートが窓に収まる")
    notes_chill = bass.generate_notes(eff_chill, key, [], seeds, bars=8, bpm=bpm_chill)
    ok(all(28 <= n <= 51 for _, n, _ in notes_chill), f"pc {pc}: 装飾込みハード範囲 28..51")

# --- ジッター無し（v4）: カードに quantize_dev_ms があっても読まず、キック無しなら
# 全ノートが16分の正位置に居る（±20ms超の無相関な揺れが「ヨレ」に聞こえた実聴の結果）---
ok("quantize_dev_ms" in json.loads(CHILL.read_text())["bass"], "前提: fixture には qdev がある")
ok("quantize_dev_ms" not in eff_chill["bass"], "実効設定に quantize_dev_ms を取り込まない")
for gseed in (0, 7, 42):
    seeds = bass.resolve_lane_seeds("bass", bass.LANES, gseed, {})
    ticks_ng = [t for t, _, _ in bass.generate_notes(eff_chill, key_fs_major, [], seeds,
                                                     bars=8, bpm=bpm_chill) if t % 120 != 0]
    ok(ticks_ng == [], f"seed {gseed}: キック無しなら全ノートが16分正位置: {ticks_ng[:5]}")

# --- リズムモチーフの反復（v5）: ハーモニー周期が長くても（16小節ループのアンカー等）
# リズムは偶数=2小節・奇数=1小節のモチーフの繰り返しになる。全小節を独立サンプリング
# していた旧実装は「毎小節リズムが違う＝型の反復が無い」ベースになりヨレて聞こえた（実聴）。
# 音高は進行を追従して変わってよいので、検査は小節内スロットパターンのみ
T16, TB = bass.TICKS_16TH, bass.TICKS_BAR
roots16 = {"loop_bars": 16, "slots_per_bar": 2, "roots": (list(range(12)) * 3)[:32]}
for gseed in (0, 5, 42):
    seeds = bass.resolve_lane_seeds("bass", bass.LANES, gseed, {})
    notes16 = bass.generate_notes(eff_chill, key_fs_major, [], seeds, bars=16,
                                  bpm=bpm_chill, roots_cfg=roots16)
    per_bar = [sorted((t % TB) // T16 for t, _, _ in notes16 if b * TB <= t < (b + 1) * TB)
               for b in range(16)]
    ok(all(per_bar[b] == per_bar[b % 2] for b in range(16)),
       f"seed {gseed}: リズムは2小節モチーフの反復: {per_bar[:4]}")
roots3 = {"loop_bars": 3, "slots_per_bar": 1, "roots": [0, 5, 7]}
seeds = bass.resolve_lane_seeds("bass", bass.LANES, 5, {})
notes3 = bass.generate_notes(eff_chill, key_fs_major, [], seeds, bars=3,
                             bpm=bpm_chill, roots_cfg=roots3)
per_bar3 = [sorted((t % TB) // T16 for t, _, _ in notes3 if b * TB <= t < (b + 1) * TB)
            for b in range(3)]
ok(per_bar3[0] == per_bar3[1] == per_bar3[2], f"奇数ループは1小節モチーフ: {per_bar3}")

# 装飾のスケール検査は seed 1つでは装飾自体を引かないことがある — 全12キー×両モード×
# 複数 seed の全ノートをまとめて検査する（v1 では C major seed 4 が F♯ を出した実バグ）
for mode, degrees in (("major", bass.MAJOR_DEGREES), ("minor", bass.MINOR_DEGREES)):
    violations = []
    for pc in range(12):
        scale = {(pc + d) % 12 for d in degrees}
        for gseed in range(8):
            seeds = bass.resolve_lane_seeds("bass", bass.LANES, gseed, {})
            for _, n, _ in bass.generate_notes(eff_chill, {"root": pc, "mode": mode}, [],
                                               seeds, bars=8, bpm=bpm_chill):
                if n % 12 not in scale:
                    violations.append((pc, gseed, n))
    ok(violations == [], f"{mode}: 装飾もスケール構成音のみ（クロマチック禁止）: {violations[:5]}")

# --- 疎なカード（notes_per_bar < 1）を小節単位の最低1音で増量しない ---
eff_sparse = {"bass": {"notes_per_bar": 0.25, "note_length_16ths": 4.0,
                       "onset_rate_by_16th": [1.0] + [0.1] * 15},
              "chords": {"loop_bars": 8, "slots_per_bar": 2, "root_changes_per_loop": 0,
                         "root_motion": "static", "has_7th_or_more": False}}
sparse_counts = []
for gseed in range(16):
    seeds = bass.resolve_lane_seeds("bass", bass.LANES, gseed, {})
    n = len(bass.generate_notes(eff_sparse, {"root": 0, "mode": "minor"}, [], seeds,
                                bars=8, bpm=100.0))
    ok(1 <= n <= 6, f"seed {gseed}: 8小節×0.25 は数音（毎小節1音=8音に増量されない）: {n}")
    sparse_counts.append(n)
ok(sum(sparse_counts) / len(sparse_counts) < 4.0,
   f"疎カードの平均本数が目標（約2音）の近傍: {sparse_counts}")
ok(min(sparse_counts) >= 1, "全滅 seed でもパターン全体で最低1音は保証される")

# 最低1音フォールバックも同時打ちスナップを通す（v2 では正位置直置きでキックとフラムになった）。
# notes_per_bar=0.01 でほぼ全 seed がフォールバック経路に入る。プロファイル最大スロット1に
# キック（実 tick 145）を置き、スロット1のノートが必ず 145 に居ることを検査する（v4はジッター無しなので
# 非キックスロットのノートは必ず正位置に居る）
eff_fallback = {"bass": {"notes_per_bar": 0.01, "note_length_16ths": 4.0,
                         "onset_rate_by_16th": [0.1, 1.0] + [0.1] * 14}, "chords": None}
fallback_hits = 0
for gseed in range(16):
    seeds = bass.resolve_lane_seeds("bass", bass.LANES, gseed, {})
    notes_fb = bass.generate_notes(eff_fallback, {"root": 0, "mode": "minor"}, [145], seeds,
                                   bars=1, bpm=120.0)
    ok(len(notes_fb) >= 1, f"seed {gseed}: 最低1音")
    for t, _, _ in notes_fb:
        slot = (t + 60) // 120
        if slot == 1:
            ok(t == 145, f"seed {gseed}: スロット1のノートはキック実 tick 145 へスナップ: {t}")
            fallback_hits += 1
ok(fallback_hits >= 8, f"フォールバック経路が十分な回数踏まれている: {fallback_hits}")

# --- 同時打ちスナップ: キックの実 tick（オフグリッド）へノートが吸着する ---
eff_dense = {"bass": {"notes_per_bar": 16.0, "note_length_16ths": 1.0,
                      "onset_rate_by_16th": [0.5] * 16}, "chords": None}
seeds = bass.resolve_lane_seeds("bass", bass.LANES, 5, {})
notes_snap = bass.generate_notes(eff_dense, {"root": 0, "mode": "minor"}, [145], seeds, bars=1, bpm=120.0)
ok(145 in [t for t, _, _ in notes_snap],
   f"スロット1のノートがキック実 tick 145 へスナップ: {[t for t, _, _ in notes_snap]}")
ticks_snap = [t for t, _, _ in notes_snap]
ok(all(a < b for a, b in zip(ticks_snap, ticks_snap[1:])), "スナップ後も tick 列は狭義単調増加")

# --- パターン×繰り返し: 同一 seed で --bars だけ変えても各繰り返しが同一パターン ---
notes_1 = bass.generate_notes(eff_trap, {"root": 5, "mode": "minor"}, [], seeds, bars=1, bpm=bpm_trap)
notes_4 = bass.generate_notes(eff_trap, {"root": 5, "mode": "minor"}, [], seeds, bars=4, bpm=bpm_trap)
ok(len(notes_4) == 4 * len(notes_1), "1小節パターン×4繰り返し")
for r in range(4):
    seg = notes_4[r * len(notes_1): (r + 1) * len(notes_1)]
    ok(seg == [(t + r * bass.TICKS_BAR, n, v) for t, n, v in notes_1], f"繰り返し{r}が同一パターン")

# --- 2軸ロック: prog を固定するとルート列（音高の並び）が一致し、rhythm は変わる ---
lock_prog = {"prog": 0xABCD1234}
sa = bass.resolve_lane_seeds("bass", bass.LANES, 1, lock_prog)
sb = bass.resolve_lane_seeds("bass", bass.LANES, 2, lock_prog)
ok(sa["prog"] == sb["prog"] == 0xABCD1234 and sa["rhythm"] != sb["rhythm"], "prog ロックの seed 解決")
rng_seq_a = bass.generate_root_sequence(chill_chords, key_fs_major, np.random.default_rng(sa["prog"]))
rng_seq_b = bass.generate_root_sequence(chill_chords, key_fs_major, np.random.default_rng(sb["prog"]))
ok(rng_seq_a == rng_seq_b, "prog ロックでルート列が完全一致")
na = bass.generate_notes(eff_chill, key_fs_major, [], sa, bars=8, bpm=bpm_chill)
nb = bass.generate_notes(eff_chill, key_fs_major, [], sb, bars=8, bpm=bpm_chill)
ok([t for t, _, _ in na] != [t for t, _, _ in nb], "rhythm は未ロックなので発音位置が変わる")
# rhythm を固定すると発音位置・装飾が一致し、音高（ルート列）は変わる
lock_rhythm = {"rhythm": 0x11112222}
sc_ = bass.resolve_lane_seeds("bass", bass.LANES, 1, lock_rhythm)
sd = bass.resolve_lane_seeds("bass", bass.LANES, 2, lock_rhythm)
nc = bass.generate_notes(eff_chill, key_fs_major, [], sc_, bars=8, bpm=bpm_chill)
nd = bass.generate_notes(eff_chill, key_fs_major, [], sd, bars=8, bpm=bpm_chill)
ok([t for t, _, _ in nc] == [t for t, _, _ in nd], "rhythm ロックで発音位置が完全一致")
ok([n for _, n, _ in nc] != [n for _, n, _ in nd], "prog は未ロックなので音高が変わる")

# --- 壊れたカード値はエラー停止（デフォルト扱いしない） ---
base_card = json.loads(CHILL.read_text())
for label, mutate in (
    ("card_version != 1", lambda c: c.update(card_version=2)),
    ("notes_per_bar が範囲外", lambda c: c["bass"].update(notes_per_bar=17)),
    ("onset 長違い", lambda c: c["bass"].update(onset_rate_by_16th=[0.5] * 15)),
    ("onset 値が範囲外", lambda c: c["bass"].update(onset_rate_by_16th=[1.5] + [0.0] * 15)),
    ("loop_bars が不正", lambda c: c["chords"].update(loop_bars=0)),
    ("slots_per_bar が不正", lambda c: c["chords"].update(slots_per_bar=0)),
    ("root_motion が不正", lambda c: c["chords"].update(root_motion="sideways")),
):
    broken = copy.deepcopy(base_card)
    mutate(broken)
    try:
        bass.parse_card(broken, "broken")
        ok(False, f"{label} でエラーになるはず")
    except bass.GachaError:
        ok(True, f"{label} を検出")

# --- 旧形式カード互換: slots_per_bar / root_changes_per_loop 欠損の補完 ---
old_card = copy.deepcopy(base_card)
del old_card["chords"]["slots_per_bar"]
del old_card["chords"]["root_changes_per_loop"]
eff_old, _, defaulted_old = bass.parse_card(old_card, "old")
ok(eff_old["chords"]["slots_per_bar"] == 2, "slots_per_bar 欠損は 2 を補完（旧カード互換）")
ok(eff_old["chords"]["root_changes_per_loop"] == old_card["chords"]["changes_per_loop"],
   "root_changes_per_loop 欠損は changes_per_loop で代用")
ok({"chords.slots_per_bar", "chords.root_changes_per_loop"} <= set(defaulted_old),
   f"補完が申告される: {defaulted_old}")

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)

    # --- 決定性: プロセスをまたいで .mid / .wav / .json がバイト同一 ---
    out1, out2 = tmp / "a", tmp / "b"
    for out in (out1, out2):
        run_cli(str(CHILL), "--key", "F#:major", "--seed", "42", "--count", "2", "--out", str(out))
    names1 = sorted(p.name for p in out1.iterdir())
    ok(names1 == sorted(p.name for p in out2.iterdir()) and len(names1) == 6,
       f"2候補×3ファイルが同名で揃う: {names1}")
    for name in names1:
        ok((out1 / name).read_bytes() == (out2 / name).read_bytes(), f"{name} がバイト同一")

    # --- MIDI の中身: ch1（mido 0）・program 33・テンポ・モノフォニック ---
    mid_path = next(out1.glob("*.mid"))
    mid = mido.MidiFile(mid_path)
    msgs = [m for t in mid.tracks for m in t]
    note_msgs = [m for m in msgs if m.type in ("note_on", "note_off")]
    ok(all(m.channel == 0 for m in note_msgs), "全ノートが ch1（ドラム ch10 でない）")
    ok([m.program for m in msgs if m.type == "program_change"] == [33], "program 33 = Finger Bass")
    ok([m.tempo for m in msgs if m.type == "set_tempo"] == [mido.bpm2tempo(100.0)],
       "テンポがカード BPM（--bpm 省略時のフォールバック）")
    active, mono = set(), True
    for m in mid.tracks[0]:
        if m.type == "note_on" and m.velocity > 0:
            mono = mono and not active
            active.add(m.note)
        elif m.type in ("note_off",) or (m.type == "note_on" and m.velocity == 0):
            active.discard(m.note)
    ok(mono, "モノフォニック（同時に鳴るノートが常に1つ以下）")

    # --- wav の検算 ---
    data, sr = sf.read(next(out1.glob("*.wav")))
    ok(sr == 44100 and np.all(np.isfinite(data)), "wav が 44.1kHz で有限値のみ")
    ok(float(np.max(np.abs(data))) <= 1.0, "peak が 0dBFS 以下")
    ok(float(np.sqrt(np.mean(np.square(data)))) > 1e-3, "無音でない")

    # --- サイドカーと候補の同一性 ---
    sc_path = next(out1.glob("*.json"))
    sc = json.loads(sc_path.read_text())
    ok(set(sc) == {"cfg_sha256", "candidate_sha256", "global_seed", "lane_seeds", "config",
                   "defaulted_fields", "reference"}, f"サイドカーのキー: {set(sc)}")
    ok(set(sc["lane_seeds"]) == {"prog", "rhythm"}, "レーンは prog / rhythm の2軸")
    ok(sc["config"]["key"] == {"root": 6, "mode": "major"}, "正規化した key が config に入る")
    ok(sc["config"]["kick_ticks"] == [], "キック無しは空リスト")
    ok(sc["config"]["chords"]["slots_per_bar"] == 2, "正規化済み chords が config に入る")
    ok(sc["reference"] == "fixture-bass-chill", "meta.reference を持つ（置き場非依存）")

    # key が違えば別候補名（同じ seed でも別ファイル）
    out_key = tmp / "key2"
    r = run_cli(str(CHILL), "--key", "A:minor", "--seed", "42", "--count", "2", "--out", str(out_key))
    ok(sorted(p.name for p in out_key.iterdir()) != names1, "キーが違えば候補名が変わる")
    # 実効 BPM が違えば別候補名
    out_bpm = tmp / "bpm2"
    run_cli(str(CHILL), "--key", "F#:major", "--bpm", "92.5", "--seed", "42", "--count", "2",
            "--out", str(out_bpm))
    ok(sorted(p.name for p in out_bpm.iterdir()) != names1, "実効 BPM が違えば候補名が変わる")
    mid_bpm = next(out_bpm.glob("*.mid"))
    ok([m.tempo for m in mido.MidiFile(mid_bpm).tracks[0] if m.type == "set_tempo"]
       == [mido.bpm2tempo(92.5)], "--bpm がテンポメタに反映される")
    # kick ticks が違えば別候補名
    out_kick = tmp / "kick"
    run_cli(str(CHILL), "--key", "F#:major", "--seed", "42", "--count", "2",
            "--kick-ticks", "0,960,1920,2880", "--out", str(out_kick))
    ok(sorted(p.name for p in out_kick.iterdir()) != names1, "kick ticks が違えば候補名が変わる")
    sc_kick = json.loads(next(out_kick.glob("*.json")).read_text())
    ok(sc_kick["config"]["kick_ticks"] == [0, 960, 1920, 2880],
       "折り畳み済み kick ticks が config に入る")

    # --- --from: サイドカーだけから同一候補を完全再現（同時打ちスナップ位置も） ---
    out_snap = tmp / "snap"
    run_cli(str(CHILL), "--key", "F#:major", "--seed", "9", "--count", "1",
            "--kick-ticks", "145,960,1931", "--out", str(out_snap))
    snap_sc = next(out_snap.glob("*.json"))
    out_from = tmp / "from"
    run_cli("--from", str(snap_sc), "--out", str(out_from))
    for ext in (".mid", ".wav", ".json"):
        ok((out_from / (snap_sc.stem + ext)).read_bytes() == (out_snap / (snap_sc.stem + ext)).read_bytes(),
           f"--from の {ext} が元とバイト同一（スナップ位置含む）")
    run_cli("--from", str(snap_sc), str(CHILL), expect_rc=1)          # card と併用は拒否
    run_cli("--from", str(snap_sc), "--key", "A:minor", expect_rc=1)  # 黙って無視せず拒否
    run_cli("--from", str(snap_sc), "--bpm", "90", expect_rc=1)
    run_cli("--from", str(snap_sc), "--kick-ticks", "0", expect_rc=1)

    # sidecar の key / kick_ticks / bpm の改変が検出される
    intact = snap_sc.read_text()
    for label, mutate in (
        ("key の改変", lambda s: s["config"]["key"].update(root=7)),
        ("kick_ticks の改変", lambda s: s["config"].update(kick_ticks=[0])),
        ("bpm の改変", lambda s: s["config"].update(bpm=101.0)),
        ("lane seed の改変", lambda s: s["lane_seeds"].update(rhythm="00000000")),
    ):
        tampered = json.loads(intact)
        mutate(tampered)
        bad = tmp / "tampered.json"
        bad.write_text(json.dumps(tampered))
        r = run_cli("--from", str(bad), "--out", str(tmp / "x"), expect_rc=1)
        ok("一致しない" in r.stderr, f"{label} を拒否: {r.stderr}")

    # 改変サイドカーが既存候補の隣にあるとき（rerun 時の candidate_status）もエラー
    tampered = json.loads(intact)
    tampered["config"]["key"]["root"] = 7
    snap_sc.write_text(json.dumps(tampered))
    r = run_cli(str(CHILL), "--key", "F#:major", "--seed", "9", "--count", "1",
                "--kick-ticks", "145,960,1931", "--out", str(out_snap), expect_rc=1)
    ok("一致しない" in r.stderr, f"既存サイドカーの key 改変は再実行で検出: {r.stderr}")
    snap_sc.write_text(intact)

    # --- --bars の契約: 省略時 = loop_bars / 倍数のみ受理 ---
    ok(json.loads(next(out1.glob("*.json")).read_text())["config"]["bars"] == 8,
       "chill（loop_bars=8）の省略時 bars は 8")
    run_cli(str(CHILL), "--key", "F#:major", "--seed", "1", "--bars", "4",
            "--out", str(tmp / "z"), expect_rc=1)  # loop_bars より短い
    run_cli(str(CHILL), "--key", "F#:major", "--seed", "1", "--bars", "12",
            "--out", str(tmp / "z"), expect_rc=1)  # 倍数でない
    # 1小節パターン × --bars 4（トラップ系: ベースが短い側）
    out_rep = tmp / "rep"
    run_cli(str(TRAP), "--key", "F:minor", "--seed", "3", "--count", "1", "--bars", "4",
            "--out", str(out_rep))
    rep_notes = midi_notes(next(out_rep.glob("*.mid")))
    per_bar = [[(t - b * 1920, n) for t, n, _ in rep_notes if b * 1920 <= t < (b + 1) * 1920]
               for b in range(4)]
    ok(all(bar == per_bar[0] for bar in per_bar), "4小節とも同一の1小節パターン")

    # --- --porcelain: JSON Lines・prog/rhythm レーン・2回目は skipped ---
    out_p = tmp / "porcelain"
    args_p = (str(CHILL), "--key", "F#:major", "--seed", "5", "--count", "3", "--out", str(out_p),
              "--porcelain")
    r = run_cli(*args_p)
    lines = [ln for ln in r.stdout.splitlines() if ln.strip()]
    ok(len(lines) == 3, f"--count 3 で3行: {lines}")
    for rec in map(json.loads, lines):
        ok(set(rec) == {"base", "lane_seeds", "status"} and set(rec["lane_seeds"]) == {"prog", "rhythm"},
           f"porcelain 行の形式: {rec}")
        ok(rec["status"] == "generated", f"初回は generated: {rec}")
    ok("完了" in r.stderr and "完了" not in r.stdout, "人間向け出力は stderr のみ")
    r2 = run_cli(*args_p)
    ok(all(json.loads(ln)["status"] == "skipped" for ln in r2.stdout.splitlines() if ln.strip()),
       "2回目は skipped")

    # --- --key 必須 / --kick-ticks と --drums の併用拒否 ---
    r = run_cli(str(CHILL), "--seed", "1", "--out", str(tmp / "z"), expect_rc=1)
    ok("--key" in r.stderr, "--key 必須")
    run_cli(str(CHILL), "--key", "A:minor", "--kick-ticks", "0", "--drums", "x.json",
            "--out", str(tmp / "z"), expect_rc=1)

    # ---- --drums: キック導出・mix 合成・BPM の契約 ----
    # ドラム候補（BPM 120 の fixture・4小節）を用意する
    drums_out = tmp / "drums4"
    subprocess.run([sys.executable, str(DRUMS_PY), str(FIXTURES / "full.json"), "--seed", "42",
                    "--count", "1", "--bars", "4", "--out", str(drums_out)],
                   capture_output=True, text=True, check=True)
    drums_sc = next(drums_out.glob("*.json"))

    # bass 8小節 + drums 4小節 sidecar → mix は8小節あり後半にもドラムが鳴る（8対4:
    # bass に --bars 8 を渡し、mix 合成時に drums 側を 4→8 へ延長するケース）。
    # 実効 BPM は bass カードの 100（drums カードの 120 は使わない）
    out_mix = tmp / "mix"
    r = run_cli(str(CHILL), "--key", "F#:major", "--bpm", "100", "--seed", "11", "--count", "1",
                "--bars", "8", "--drums", str(drums_sc), "--out", str(out_mix))
    mix_files = list(out_mix.glob("*.mix-*.wav"))
    ok(len(mix_files) == 1, f"mix wav が1つ出る: {[p.name for p in out_mix.iterdir()]}")
    mix_data, _ = sf.read(mix_files[0])
    bar_sec = 240.0 / 100.0  # 実効 BPM 100 → 1小節 2.4 秒
    ok(len(mix_data) >= int(8 * bar_sec * 44100), "mix が8小節ぶんある（実効 BPM 100 の小節境界）")
    bass_only, _ = sf.read(next(p for p in out_mix.glob("*.wav") if ".mix-" not in p.name))
    # 後半（5〜8小節）にドラムが乗っている: ベースの音域（基音+倍音でも 1kHz 未満）に無い
    # 高域成分（ハット/スネア）の有無で判定する（rms 比較はベースが支配的で誤差に埋もれる）
    from scipy.signal import butter, sosfilt  # noqa: E402
    hp = butter(4, 3000, btype="highpass", fs=44100, output="sos")
    half = int(4 * bar_sec * 44100)
    end = int(8 * bar_sec * 44100)
    rms = lambda x: float(np.sqrt(np.mean(np.square(x))))  # noqa: E731
    ok(rms(sosfilt(hp, mix_data[half:end])) > 10 * max(rms(sosfilt(hp, bass_only[half:end])), 1e-6),
       "mix 後半にドラムの高域が鳴る（4→8 小節へ延長合成）")

    # 参照キックの同一性: config.kick_ticks == drums sidecar から導出した kick 列の折り畳み
    mix_sc = json.loads(next(out_mix.glob("bass-*.json")).read_text())
    d_eff, d_bpm, _b, _d, d_seeds, _g, _r = drums.load_sidecar(drums_sc)
    d_notes = drums.generate_pattern(d_eff, d_seeds, bars=8, bpm=d_bpm)
    expected_kicks = sorted({t % (8 * 1920) for t, _ in d_notes["kick"]})
    ok(mix_sc["config"]["kick_ticks"] == expected_kicks,
       "mix のドラムとベースが参照した kick が同一（sidecar から同じ導出）")

    # mix 用反復は正規候補の同一性に影響しない: --drums の有無でなく kick ticks だけが効く
    out_kt = tmp / "kt"
    run_cli(str(CHILL), "--key", "F#:major", "--bpm", "100", "--seed", "11", "--count", "1",
            "--bars", "8", "--kick-ticks", ",".join(map(str, expected_kicks)), "--out", str(out_kt))
    canonical = lambda d: sorted(p.name for p in d.glob("bass-*") if ".mix-" not in p.name)  # noqa: E731
    ok(canonical(out_kt) == canonical(out_mix),
       "--drums と等価な --kick-ticks は同じ候補名（mix 反復はハッシュに関与しない）")
    for name in canonical(out_kt):
        ok((out_kt / name).read_bytes() == (out_mix / name).read_bytes(),
           f"{name}: 正規候補3点がバイト同一")

    # --drums の候補を変えると bass の候補名も MIDI も変わる（キック制約が効いている）
    drums_out2 = tmp / "drums4b"
    subprocess.run([sys.executable, str(DRUMS_PY), str(FIXTURES / "jitter.json"), "--seed", "7",
                    "--count", "1", "--bars", "4", "--out", str(drums_out2)],
                   capture_output=True, text=True, check=True)
    out_mix2 = tmp / "mix2"
    run_cli(str(CHILL), "--key", "F#:major", "--bpm", "100", "--seed", "11", "--count", "1",
            "--bars", "8", "--drums", str(next(drums_out2.glob("*.json"))), "--out", str(out_mix2))
    ok(sorted(p.name for p in out_mix2.glob("bass-*.mid")) != sorted(p.name for p in out_mix.glob("bass-*.mid")),
       "--drums の候補を変えると bass の候補名が変わる")

    # 2小節パターン × 4小節書き出し（2対4）: 旧形式 chords（欠損補完）経路で確認
    old_path = tmp / "old-card.json"
    old_path.write_text(json.dumps({**old_card, "chords": {**old_card["chords"], "loop_bars": 2}}))
    out_24 = tmp / "b24"
    run_cli(str(old_path), "--key", "F#:major", "--seed", "2", "--count", "1", "--bars", "4",
            "--out", str(out_24))
    n24 = midi_notes(next(out_24.glob("*.mid")))
    first_half = [(t, n) for t, n, _ in n24 if t < 2 * 1920]
    second_half = [(t - 2 * 1920, n) for t, n, _ in n24 if t >= 2 * 1920]
    ok(first_half == second_half, "2小節パターンが 4小節で2回繰り返される（旧形式カードでも生成できる）")

print(f"OK: {checks} 件のチェックが通った")
