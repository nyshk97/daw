#!/usr/bin/env python
"""gates.py の判定ロジックの回帰テスト（プレーン assert・pytest 不要）。

固定する性質:
- tempo_stable が実測済みのライブ実揺れ（後半減衰も local_bpm_std の増大も出さず、
  中盤の対比 <1.0 だけがサイン。docs/labs/reference-beat.md 2026-08-04）を落とし続けること。
  「揺れ＝後半だけ落ちる/std大」の直感で min(gc) を緩めるとこの曲を通してしまう
  （一度やりかけたレビュー指摘済みの誤り）
- bpm の octave_ok がオクターブ乗り換え時の精密化ずれ（梯子の値と採用値が最大0.3BPM
  ずれる）を「取り違え」と誤判定しないこと（labs 2026-08-09 うそさ）

使い方: .venv/bin/python tests/test_gates.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from gates import bpm_octave_ok, tempo_stable_ok  # noqa: E402

checks = 0


def ok(cond: bool, msg: str) -> None:
    global checks
    assert cond, msg
    checks += 1


# --- tempo_stable ---

# 実測回帰: ライブ演奏の実テンポ揺れ（Jinmenusagi - GOAT ライブ映像）。
# ドラムは均一に鳴っているのに中盤の対比が 1.0 を割る＝実揺れ、と切り分け済み。
# std は 0.64 と小さく後半減衰も無い — min(gc) だけがこれを捕まえる
LIVE_WOBBLE_LAB = [2.00, 1.08, 0.89, 0.95, 1.13, 1.01, 1.33, 1.16]   # 2026-08-04 の実測
LIVE_WOBBLE_RETRY = [2.022, 1.024, 0.932, 0.978, 1.08, 0.988, 1.425, 1.142]  # 同曲を再取得した実測
ok(not tempo_stable_ok(LIVE_WOBBLE_LAB), "ライブ実揺れ（実験記録の実測値）は落ちる")
ok(not tempo_stable_ok(LIVE_WOBBLE_RETRY), "ライブ実揺れ（再取得の実測値）も落ちる")

# 実測回帰: 打ち込みのスタジオMV（うそさ）。全区間 >1.0 で通る。
# この曲はビートトラッカーがグリッドの×1.5にロックして local_bpm_std=2.13 と膨らんだ —
# std をゲート条件にすると健全な曲を落とす（一度足して撤回した）ため、判定は gc のみ
USOSA_STUDIO = [1.29, 1.299, 1.044, 1.193, 1.269, 1.308, 1.074, 1.378]
ok(tempo_stable_ok(USOSA_STUDIO), "打ち込みスタジオ曲（実測値）は通る")

ok(tempo_stable_ok([1.3, 1.4, 1.35, 1.5, 1.2, 1.45, 1.4, 1.6]), "全区間>1.0 の安定曲は通る")
ok(not tempo_stable_ok([2.0, 1.9, 1.8, 1.7, 1.05, 1.04, 1.03, 1.02]), "後半減衰は落ちる")
ok(not tempo_stable_ok([]), "gc 空は落ちる")
ok(tempo_stable_ok([1.5]), "区間1つ（短い曲）は減衰チェックを飛ばして通る")

# --- bpm octave ---

# 実測回帰: うそさ。梯子は乗り換え前の基準 68.99 から作られ double=137.98、
# 乗り換え後の精密化で採用値は 138.0 → 同一オクターブと判定すべき（旧 <0.01 は誤って落とした）
USOSA_OC = {
    "half": {"bpm": 34.495, "weighted": 0.021},
    "x1": {"bpm": 68.99, "weighted": 0.1111},
    "x1.5": {"bpm": 103.485, "weighted": 0.1093},
    "double": {"bpm": 137.98, "weighted": 0.1506},
}
ok(bpm_octave_ok(USOSA_OC, 138.0), "オクターブ乗り換え＋精密化ずれ（実測値）は同一オクターブ扱い")

# GOAT: 差 0.002 で従来も通っていたケース
GOAT_OC = {
    "half": {"bpm": 35.5, "weighted": 0.0362},
    "x1": {"bpm": 71.0, "weighted": 0.2326},
    "x1.5": {"bpm": 106.5, "weighted": 0.158},
    "double": {"bpm": 142.0, "weighted": 0.2332},
}
ok(bpm_octave_ok(GOAT_OC, 141.998), "精密化ずれが小さいケースも通る")

# 本物の取り違え: 重み最大が half なのに採用値が double のまま → 落ちる
MISMATCH_OC = {
    "half": {"bpm": 69.0, "weighted": 0.3},
    "x1": {"bpm": 138.0, "weighted": 0.1},
}
ok(not bpm_octave_ok(MISMATCH_OC, 138.0), "重み最大と別オクターブの採用値は落ちる")

ok(not bpm_octave_ok({}, 120.0), "octave_check 空は落ちる")

print(f"OK ({checks} checks)")
