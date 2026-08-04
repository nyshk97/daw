#!/usr/bin/env python
"""card.py / gates.py の回帰テスト（プレーン assert・pytest 不要）。

実曲3本はリポジトリ外にあり、再分析のたびに値が動く（demucs は同じ入力でも
同じステムを出さない）ので、回帰の固定はここの最小 fixture で行う。
ゲート分岐は fixture を増やさず、正常系の gates.json をテスト内で書き換えて網羅する。

使い方: .venv/bin/python tests/test_card.py
"""
import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import card  # noqa: E402
import gates  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "fixtures"
checks = 0


def ok(cond: bool, msg: str) -> None:
    global checks
    assert cond, msg
    checks += 1


def run_fixture(name: str, mutate=None, pre_card: bool = False):
    """fixture を一時フォルダにコピーして card.py を走らせる。fixture 自体は汚さない。"""
    with tempfile.TemporaryDirectory() as tmp:
        ref = Path(tmp) / name
        shutil.copytree(FIXTURES / name, ref)
        if mutate:
            gp = ref / "analysis" / "gates.json"
            g = json.loads(gp.read_text())
            mutate(g)
            gp.write_text(json.dumps(g))
        if pre_card:
            (ref / "card.json").write_text('{"stale": true}')
        rc = card.generate(ref)
        out = ref / "card.json"
        return rc, json.loads(out.read_text()) if out.exists() else None


def excluded_paths(c: dict) -> set[str]:
    return {e["path"] for e in c["meta"]["excluded"]}


# --- gates.pick_harmony_source: 4分割が usable でも 6s 全 false なら None（kzm で踏んだ実バグ） ---
PROG = ["Am7", "F", "G"]
tops = [
    {"stem": "other", "usable": True, "rms_below_mix_db": -20.0, "progression": PROG},
    {"stem": "6s-piano", "usable": False, "rms_below_mix_db": -60.0, "progression": PROG},
    {"stem": "6s-guitar", "usable": False, "rms_below_mix_db": -40.0, "progression": PROG},
    {"stem": "6s-other", "usable": None, "rms_below_mix_db": None, "progression": []},
]
ok(gates.pick_harmony_source(tops) is None, "4s other だけ usable なら harmony は不成立のはず")
tops2 = [
    {"stem": "other", "usable": True, "rms_below_mix_db": -10.0, "progression": PROG},
    {"stem": "6s-piano", "usable": True, "rms_below_mix_db": -18.0, "progression": PROG},
    {"stem": "6s-guitar", "usable": True, "rms_below_mix_db": -15.0, "progression": PROG},
]
ok(gates.pick_harmony_source(tops2) == "6s-guitar", "usable な 6s のうち最大音量を選ぶ（4s が最大でも無視）")
# 曲が短くループ2周ぶん取れないと topline は loop_progression={} を出す。
# usable でも進行が空のステムを選ぶと下流が KeyError で落ちるので候補から外す
tops3 = [
    {"stem": "6s-piano", "usable": True, "rms_below_mix_db": -12.0, "progression": []},
    {"stem": "6s-guitar", "usable": True, "rms_below_mix_db": -18.0, "progression": PROG},
]
ok(gates.pick_harmony_source(tops3) == "6s-guitar", "進行が空のステムは音量が最大でも選ばない")
ok(gates.pick_harmony_source(tops3[:1]) is None, "進行が取れたステムが無ければ不成立")
tops4 = [{"stem": "6s-piano", "usable": True, "rms_below_mix_db": -12.0, "progression": [None, None]}]
ok(gates.pick_harmony_source(tops4) is None, "全スロット None の進行（コードを1つも読めていない）は不成立")

# --- 正常系: 全スライスが期待値どおり ---
rc, c = run_fixture("normal")
ok(rc == 0 and c is not None, "正常系でカードが生成されるはず")
ok(c["global"] == {"bpm": 120.0, "key": {"root": "A", "mode": "minor"}}, f"global: {c['global']}")
ok(c["meta"]["assumptions"] == {"time_signature": "4/4"}, "拍子は決め打ちとして meta.assumptions に置く")
ok(c["meta"]["excluded"] == [], "正常系に省略は無いはず")
# Am7 Am7 F G（循環）: 変化3回・移動 [-4,+2,+2] → 上行 2/3 → up
ok(
    c["chords"] == {"loop_bars": 2, "has_7th_or_more": True, "changes_per_loop": 3, "root_motion": "up"},
    f"chords: {c['chords']}",
)
ok(c["drums"]["kick_profile_by_beat"] == [1.0, 0.5, 0.75, 0.25], f"kick畳み: {c['drums']['kick_profile_by_beat']}")
ok(c["drums"]["quantize_dev_ms"] == 4.0, f"qdev（|2|,|-4|,|6| の平均）: {c['drums']['quantize_dev_ms']}")
ok(c["drums"]["swing_ratio"] == 0.55, "swing")
ok(len(c["drums"]["snare_profile_by_16th"]) == 16 and len(c["drums"]["hat_profile_by_16th"]) == 16, "16分プロファイル")
ok(c["bass"]["pitch_center"] == "C2" and c["bass"]["notes_per_bar"] == 3.0, "bass")
ok(c["arrangement"]["instrumentation"] == ["bass", "drums", "piano"], "編成＝セクション active の和集合")
ok(c["arrangement"]["sections"][0] == {"bars": [1, 8], "active": ["bass", "drums"], "rms_db": -12.3}, "sections")

# --- 上モノ無し: chords スライスごと省略 ---
rc, c = run_fixture("no-uppers")
ok(rc == 0 and "chords" not in c, "harmony 落ちで chords が省略されるはず")
e = c["meta"]["excluded"]
ok(len(e) == 1 and e[0]["path"] == "chords" and e[0]["gate"] == "harmony.ok", f"excluded: {e}")

# --- BPM/テンポのゲート落ち: カード自体を作らず、旧カードを消す ---
for field, mut in (
    ("bpm.octave_ok", lambda g: g["bpm"].update(octave_ok=False)),
    ("tempo_stable.ok", lambda g: g["tempo_stable"].update(ok=False)),
):
    rc, c = run_fixture("normal", mutate=mut, pre_card=True)
    ok(rc == 0 and c is None, f"{field} 落ちでカードを生成せず旧カードも消すはず")

# --- downbeat 落ち: 小節内の位置情報だけ省略、拍相対の値は残る ---
rc, c = run_fixture("normal", mutate=lambda g: g["downbeat"].update(ok=False))
ok(rc == 0, "downbeat 落ちでも生成はする")
ok(
    excluded_paths(c)
    >= {
        "drums.kick_profile_by_beat",
        "drums.snare_profile_by_16th",
        "drums.hat_profile_by_16th",
        "bass.onset_rate_by_16th",
        "chords",
        "arrangement.sections",
    },
    f"downbeat 落ちの省略: {excluded_paths(c)}",
)
ok("swing_ratio" in c["drums"] and "notes_per_bar" in c["bass"], "拍相対・位置非依存の値は残る")
ok("instrumentation" in c["arrangement"] and "sections" not in c["arrangement"], "編成は残り sections は消える")

# --- swing 落ち: swing_ratio とハットだけ省略、kick/snare は残る ---
rc, c = run_fixture("normal", mutate=lambda g: g["swing"].update(ok=False))
ok(excluded_paths(c) == {"drums.swing_ratio", "drums.hat_profile_by_16th"}, f"swing 落ち: {excluded_paths(c)}")
ok("kick_profile_by_beat" in c["drums"] and "snare_profile_by_16th" in c["drums"], "kick/snare は残る")

# --- key 低: global.key だけ省略 ---
rc, c = run_fixture("normal", mutate=lambda g: g["key"].update(confidence="低"))
ok("key" not in c["global"] and excluded_paths(c) == {"global.key"}, f"key 低: {excluded_paths(c)}")

# --- 自己検算の型検査: 数値のはずのフィールドが文字列なら非ゼロ終了し、旧カードは保持する ---
rc, c = run_fixture("normal", mutate=lambda g: g["bpm"].update(value="not-a-number"), pre_card=True)
ok(rc == 1, "型が不正なら検算に落ちて非ゼロのはず")
ok(c == {"stale": True}, "検算失敗では既存の card.json に触らない")
ok(card.validate({"meta": {"excluded": []}, "chords": {"loop_bars": "two"}}) != [], "loop_bars の文字列を検出")
ok(card.validate({"meta": {"excluded": []}, "chords": {"root_motion": "sideways"}}) != [], "root_motion の不正値を検出")
ok(card.validate({"meta": {"excluded": []}, "arrangement": {"n_bars": "many"}}) != [], "n_bars の文字列を検出")
ok(
    card.validate({"meta": {"excluded": []}, "drums": {"kick_profile_by_beat": [1.0, "x", 0.5, 0.2]}}) != [],
    "プロファイル配列の非数値要素を検出",
)
# 「フィールド欠損」（ゲート落ちの省略＝正当）と「値が None」（バグ）を区別する
ok(card.validate({"meta": {"excluded": []}, "global": {"bpm": None}}) != [], "bpm=None を検出（欠損とは別物）")
ok(card.validate({"meta": {"excluded": []}, "global": {}}) == [], "フィールド欠損そのものはエラーでない")
ok(card.validate({"card_version": "one", "meta": {"excluded": []}}) != [], "card_version の文字列を検出")
BASE_SEC = {"bars": [1, 8], "active": ["drums"], "rms_db": -12.0}
ok(card.validate({"meta": {"excluded": []}, "arrangement": {"sections": [BASE_SEC]}}) == [], "正常な section は通る")
ok(
    card.validate({"meta": {"excluded": []}, "arrangement": {"sections": [{**BASE_SEC, "rms_db": "loud"}]}}) != [],
    "sections[].rms_db の文字列を検出",
)
ok(
    card.validate({"meta": {"excluded": []}, "arrangement": {"sections": [{**BASE_SEC, "bars": ["one", 8]}]}}) != [],
    "sections[].bars の非 int 要素を検出",
)

print(f"OK: {checks} 件のチェックが通った")
