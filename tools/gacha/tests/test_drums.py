#!/usr/bin/env python
"""drums.py の回帰テスト（プレーン assert・pytest 不要。fixture のみで数秒）。

実カードはリポジトリ外で再分析のたびに値が動くので、回帰の固定はここの fixture で行う。
決定性はプロセスをまたいで確認する（CLI を subprocess で2回走らせてバイト比較）—
seed 再現性がこのツールの生命線なので、導出値そのものも既知値で固定する。

使い方: tools/reference/.venv/bin/python tools/gacha/tests/test_drums.py
"""
import copy
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import mido
import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import drums  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "fixtures"
DRUMS_PY = Path(__file__).resolve().parent.parent / "drums.py"
checks = 0


def ok(cond: bool, msg: str) -> None:
    global checks
    assert cond, msg
    checks += 1


def run_cli(*args: str, expect_rc: int = 0) -> subprocess.CompletedProcess:
    r = subprocess.run([sys.executable, str(DRUMS_PY), *args], capture_output=True, text=True)
    assert r.returncode == expect_rc, f"exit {r.returncode} (期待 {expect_rc}): {r.stderr}\n{r.stdout}"
    return r


def load_fixture(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text())


# --- seed 導出の既知値固定（導出式が変わると過去候補の seed 再現が全滅する） ---
ok(drums.derive_lane_seed(42, "kick") == 0xCF50D9FE, "kick の lane seed 導出値")
ok(drums.derive_lane_seed(42, "snare") == 0x27068BBD, "snare の lane seed 導出値")
ok(drums.derive_lane_seed(42, "hat") == 0xCEFB6B8A, "hat の lane seed 導出値")
ok(drums.derive_audio_seed("kick", 0xDEADBEEF) == 0x80E3052070AF92A6, "audio seed 導出値")

# --- 過去 seed 互換の固定: 固定 fixture・固定 seed の出力を既知 SHA-256 で焼き込む ---
# 現行コード同士の2回比較（下の①）は「決定的であること」しか見ておらず、生成ロジックや
# wav 合成を変えても両方が同じ新出力になって素通りする。ここが落ちたら「同じファイル名で
# 以前と違う音が出る」変更をしたということ。意図的な変更なら GENERATOR_VERSION を上げて
# （ファイル名の cfg ハッシュ部が変わり旧候補と共存できる）、この既知値を採り直す。
# full.json は quantize_dev_ms 0 でスウィング位置の厳密テスト用。ジッター経路（σ換算・
# 乱数消費・クリップ・tick 丸め）は素通りするので、実カード相当（dev 11.3ms）の jitter.json
# でもゴールデンを焼き、両経路の変更を検出する
GOLDENS = (
    ("full.json", "42", "drums-kcf50d9fe-s27068bbd-hcefb6b8a-e7b845",
     "d772cc3bc0425c2a4861872a85bf40016baf5091a2cf8e8975760a4afdbda5b6",
     "13f791414863873a04c80898ac167225626cc263c01cec4259e23128650d96aa"),
    ("jitter.json", "7", "drums-k5d935960-sbf6a240a-h7b19edc8-69a67a",
     "7e4bf02e479e6a5fbbdb41c381692ff6570ad3fe3c44d702d0c43b6a5b6342aa",
     "7ad9b82929a2a2910c2e398338ad9ddb2aa8f43c28b7c14e4f9e84bd1ddf422f"),
)
with tempfile.TemporaryDirectory() as tmp:
    for fixture, seed, base, mid_sha, wav_sha in GOLDENS:
        out = Path(tmp) / f"known-{fixture}"
        run_cli(str(FIXTURES / fixture), "--seed", seed, "--count", "1", "--bars", "2", "--out", str(out))
        mid_files = list(out.glob("*.mid"))
        ok(len(mid_files) == 1 and mid_files[0].stem == base, f"{fixture}: 既知のファイル名: {mid_files}")
        ok(hashlib.sha256((out / f"{base}.mid").read_bytes()).hexdigest() == mid_sha,
           f"{fixture}: 既知 seed の .mid が過去バージョンとバイト同一（違うなら GENERATOR_VERSION を上げること）")
        ok(hashlib.sha256((out / f"{base}.wav").read_bytes()).hexdigest() == wav_sha,
           f"{fixture}: 既知 seed の .wav が過去バージョンとバイト同一（違うなら GENERATOR_VERSION を上げること）")

# --- ① 決定性: プロセスをまたいで .mid / .wav / .json がバイト同一 ---
with tempfile.TemporaryDirectory() as tmp:
    out1, out2 = Path(tmp) / "a", Path(tmp) / "b"
    for out in (out1, out2):
        run_cli(str(FIXTURES / "full.json"), "--seed", "42", "--count", "2", "--bars", "2", "--out", str(out))
    names1 = sorted(p.name for p in out1.iterdir())
    names2 = sorted(p.name for p in out2.iterdir())
    ok(names1 == names2 and len(names1) == 6, f"2候補×3ファイルが同名で揃う: {names1}")
    for name in names1:
        ok((out1 / name).read_bytes() == (out2 / name).read_bytes(), f"{name} がバイト同一")

    # --- ② MIDI の中身: GM ノート・mido 上で channel==9・テンポ ---
    mid_path = next(out1.glob("*.mid"))
    mid = mido.MidiFile(mid_path)
    note_msgs = [m for t in mid.tracks for m in t if m.type in ("note_on", "note_off")]
    ok(len(note_msgs) > 0, "ノートがある")
    ok(all(m.channel == 9 for m in note_msgs), "全ノートが mido 上で channel==9（人間表記 ch10）")
    ok({m.note for m in note_msgs} <= {36, 38, 42}, "ノート番号は GM の 36/38/42 のみ")
    tempos = [m.tempo for t in mid.tracks for m in t if m.type == "set_tempo"]
    ok(tempos == [mido.bpm2tempo(120.0)], f"テンポがカードの BPM: {tempos}")

    # --- ⑫ wav の検算: 有限・クリップ無し・無音でない ---
    data, sr = sf.read(next(out1.glob("*.wav")))
    ok(sr == 44100 and np.all(np.isfinite(data)), "wav が 44.1kHz で有限値のみ")
    ok(float(np.max(np.abs(data))) <= 1.0, "peak が 0dBFS 以下")
    ok(float(np.sqrt(np.mean(np.square(data)))) > 1e-3, "無音でない")

    # --- サイドカー: 再現に必要な全情報を持つ完成マーカー ---
    sc_path = next(out1.glob("*.json"))
    sc = json.loads(sc_path.read_text())
    ok(
        set(sc) == {"cfg_sha256", "candidate_sha256", "global_seed", "lane_seeds", "config",
                    "defaulted_fields", "reference"},
        f"サイドカーのキー: {set(sc)}",
    )
    ok(sc["config"]["drums"]["swing_ratio"] == 0.6, "サイドカーに補完済み実効設定が入る")
    ok(sc["defaulted_fields"] == [], "full fixture に補完は無い")
    ok(sc["reference"] == "fixture-full", "カードのパスでなく meta.reference を持つ（置き場非依存）")

    # --- --from: サイドカーだけから同一候補を完全再現できる（カード不要） ---
    out3 = Path(tmp) / "from"
    r = run_cli("--from", str(sc_path), "--out", str(out3))
    ok("再現" in r.stdout, f"--from の申告: {r.stdout}")
    base = sc_path.stem
    for ext in (".mid", ".wav", ".json"):
        ok(
            (out3 / f"{base}{ext}").read_bytes() == (out1 / f"{base}{ext}").read_bytes(),
            f"--from の {ext} が元とバイト同一",
        )
    run_cli("--from", str(sc_path), str(FIXTURES / "full.json"), expect_rc=1)  # card と併用は拒否
    run_cli("--from", str(sc_path), "--count", "2", expect_rc=1)  # 黙って無視せず拒否
    run_cli("--from", str(sc_path), "--bars", "8", expect_rc=1)
    for label, mutate in (
        ("config.bpm の改変", lambda s: s["config"].update(bpm=121.0)),
        ("lane seed の改変", lambda s: s["lane_seeds"].update(hat="00000000")),
        ("wav_sample_rate の改変", lambda s: s["config"].update(wav_sample_rate=48000)),
        ("generator_version の改変", lambda s: s["config"].update(generator_version=99)),
    ):
        tampered_sc = json.loads(sc_path.read_text())
        mutate(tampered_sc)
        bad = Path(tmp) / "tampered.json"
        bad.write_text(json.dumps(tampered_sc))
        r = run_cli("--from", str(bad), "--out", str(Path(tmp) / "x"), expect_rc=1)
        ok("一致しない" in r.stderr, f"{label} を拒否: {r.stderr}")

# --- ③ スウィング: 8分裏が (ratio-0.5)×4分音符 だけ符号付きで動く ---
effective, bpm, defaulted = drums.parse_card(load_fixture("full.json"), "full")
seeds = drums.resolve_lane_seeds(42, {})
notes = drums.generate_pattern(effective, seeds, bars=1, bpm=bpm)
hat_ticks = {t for t, _ in notes["hat"]}
# swing 0.6 → +0.1×480 = +48 tick。骨格の8分（表 0,480,960,1440 / 裏 288,768,1248,1728）
ok({0, 480, 960, 1440} <= hat_ticks, f"8分表はスウィングで動かない: {sorted(hat_ticks)}")
ok({288, 768, 1248, 1728} <= hat_ticks, f"8分裏が +48 tick 遅れる: {sorted(hat_ticks)}")
fast = copy.deepcopy(effective)
fast["swing_ratio"] = 0.45  # 実カード 0.482 と同じ「裏拍が突っ込む」側
hat_fast = {t for t, _ in drums.generate_pattern(fast, seeds, bars=1, bpm=bpm)["hat"]}
ok({216, 696, 1176, 1656} <= hat_fast, f"swing<0.5 は 8分裏が -24 tick 前進する: {sorted(hat_fast)}")

# --- ④ 省略フィールドのデフォルト解釈 ---
eff_min, bpm_min, def_min = drums.parse_card(load_fixture("minimal.json"), "minimal")
ok(set(def_min) == set(drums.DEFAULTS), f"全フィールドが補完扱い: {def_min}")
ok(bpm_min == 90.0 and eff_min["swing_ratio"] == 0.5, "既定値: ストレート")
notes_min = drums.generate_pattern(eff_min, seeds, bars=1, bpm=bpm_min)
ok([t for t, _ in notes_min["snare"]] == [480, 1440], f"snare 既定は2・4拍バックビート: {notes_min['snare']}")
ok([t for t, _ in notes_min["kick"]] == [0, 960], f"kick 既定は拍1・3（0.25 の装飾は seed 次第）: {notes_min['kick']}")

# --- ⑤ 壊れた値はデフォルト扱いせずエラー停止 ---
base = load_fixture("full.json")
for label, mutate in (
    ("card_version != 1", lambda c: c.update(card_version=2)),
    ("BPM が範囲外", lambda c: c["global"].update(bpm=500)),
    ("BPM が数値でない", lambda c: c["global"].update(bpm="fast")),
    ("プロファイル長違い", lambda c: c["drums"].update(kick_profile_by_beat=[1.0, 0.5, 0.5])),
    ("プロファイル値が 0〜1 の外", lambda c: c["drums"].update(hat_profile_by_16th=[1.5] + [0.0] * 15)),
    ("swing が範囲外（上）", lambda c: c["drums"].update(swing_ratio=1.2)),
    ("swing が plausible 上限超え", lambda c: c["drums"].update(swing_ratio=0.801)),
    ("swing が 1 近傍（単調化で小節末に積み重なる）", lambda c: c["drums"].update(swing_ratio=0.999)),
    ("swing が plausible 下限未満", lambda c: c["drums"].update(swing_ratio=0.44)),
    ("quantize_dev が負", lambda c: c["drums"].update(quantize_dev_ms=-3)),
):
    broken = copy.deepcopy(base)
    mutate(broken)
    try:
        drums.parse_card(broken, "broken")
        ok(False, f"{label} でエラーになるはず")
    except drums.GachaError:
        ok(True, f"{label} を検出")

# --- ⑥ レーンロック: 全体 seed を変えてもロックしたレーンは完全一致、未ロックは変化 ---
locks = {"kick": 0xABCD1234}
n_a = drums.generate_pattern(effective, drums.resolve_lane_seeds(1, locks), bars=1, bpm=bpm)
n_b = drums.generate_pattern(effective, drums.resolve_lane_seeds(2, locks), bars=1, bpm=bpm)
ok(n_a["kick"] == n_b["kick"], "ロックした kick はノート位置・velocity とも完全一致")
ok(n_a["snare"] != n_b["snare"] or n_a["hat"] != n_b["hat"], "未ロックのレーンは変化する")

# --- ⑧ スウィング適用後の最終 tick 列が単調（順序逆転しない） ---
extreme = copy.deepcopy(effective)
extreme["swing_ratio"] = 0.8  # plausible 上限。裏拍が16分1.2個ぶん動き次スロットを追い越す
extreme["hat_profile_by_16th"] = [1.0] * 16
hat_ticks_ex = [t for t, _ in drums.generate_pattern(extreme, seeds, bars=1, bpm=bpm)["hat"]]
ok(all(a < b for a, b in zip(hat_ticks_ex, hat_ticks_ex[1:])), f"tick 列が狭義単調増加: {hat_ticks_ex}")
ok(len(hat_ticks_ex) == 16 and hat_ticks_ex[-1] < drums.TICKS_BAR, "全ノートが小節内に収まる")

# --- ⑦ 全レーンロック＋--count>1 は1件だけ生成して残りをスキップ申告 ---
with tempfile.TemporaryDirectory() as tmp:
    out = Path(tmp) / "locked"
    r = run_cli(
        str(FIXTURES / "full.json"), "--seed", "1", "--count", "3", "--bars", "1",
        "--lock", "kick=00000001,snare=00000002,hat=00000003", "--out", str(out),
    )
    ok("全レーンがロックされている" in r.stdout, f"スキップ理由を申告: {r.stdout}")
    ok(len(list(out.iterdir())) == 3, "候補は1件（3ファイル）だけ")

# --- スキップ3分岐: 完成済み=スキップ / サイドカー欠損=再生成 / ハッシュ不一致=エラー ---
with tempfile.TemporaryDirectory() as tmp:
    out = Path(tmp) / "skip"
    args = (str(FIXTURES / "full.json"), "--seed", "42", "--count", "1", "--bars", "1", "--out", str(out))
    run_cli(*args)
    files = sorted(out.iterdir())
    mtimes = {p.name: p.stat().st_mtime_ns for p in files}
    r = run_cli(*args)
    ok("完成済み" in r.stdout and "生成 0 件" in r.stdout, f"2回目は全スキップ: {r.stdout}")
    ok(all(p.stat().st_mtime_ns == mtimes[p.name] for p in sorted(out.iterdir())), "スキップ時はファイルに触らない")

    sidecar = next(out.glob("*.json"))
    sidecar.unlink()  # 完成マーカーだけ消す＝途中失敗の残骸を再現
    r = run_cli(*args)
    ok("再生成" in r.stdout and sidecar.exists(), f"サイドカー欠損は再生成: {r.stdout}")

    intact = sidecar.read_text()
    partial = json.loads(intact)
    del partial["candidate_sha256"]  # 旧形式（candidate 導入前）のサイドカー相当
    sidecar.write_text(json.dumps(partial))
    r = run_cli(*args)
    ok("再生成" in r.stdout, f"candidate_sha256 欠損は完成扱いにせず再生成: {r.stdout}")
    ok("candidate_sha256" in json.loads(sidecar.read_text()), "再生成で現行形式に上がる")

    def tamper_config_bpm(s):
        s["config"]["bpm"] = 121.0  # 記録済みハッシュは元のまま＝本文だけの編集

    def tamper_del_config(s):
        del s["config"]

    def tamper_bypass(s):
        # candidate_sha256 欠損（旧形式のふり）＋ cfg 不一致。欠損チェックを先に返す実装だと
        # 衝突エラーを迂回して黙って上書き再生成される（レビュー実測）
        del s["candidate_sha256"]
        s["cfg_sha256"] = "0" * 64

    for label, mutate in (
        ("cfg_sha256", lambda s: s.update(cfg_sha256="0" * 64)),
        ("candidate_sha256", lambda s: s.update(candidate_sha256="0" * 64)),
        ("config 本文（bpm）", tamper_config_bpm),
        ("config 丸ごと削除", tamper_del_config),
        ("candidate 欠損＋cfg 不一致", tamper_bypass),
    ):
        tampered = json.loads(intact)
        mutate(tampered)
        sidecar.write_text(json.dumps(tampered))
        r = run_cli(*args, expect_rc=1)
        ok("一致しない" in r.stderr, f"{label} の改変は完成扱いにせずエラー: {r.stderr}")
    sidecar.write_text(intact)  # 原状復帰
    r = run_cli(*args)
    ok("完成済み" in r.stdout, "復帰後は再びスキップされる")

print(f"OK: {checks} 件のチェックが通った")
