#!/usr/bin/env python
"""gates.py の判定ロジックの回帰テスト（プレーン assert・pytest 不要）。

固定する性質: tempo_stable が実測済みのライブ実揺れ（後半減衰も local_bpm_std の増大も
出さず、中盤の対比 <1.0 だけがサイン。docs/labs/reference-beat.md 2026-08-04）を
落とし続けること。「揺れ＝後半だけ落ちる/std大」の直感で min(gc) を緩めると
この曲を通してしまう（一度やりかけたレビュー指摘済みの誤り）。

使い方: .venv/bin/python tests/test_gates.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from gates import tempo_stable_ok  # noqa: E402

checks = 0


def ok(cond: bool, msg: str) -> None:
    global checks
    assert cond, msg
    checks += 1


# 実測回帰: ライブ演奏の実テンポ揺れ（Jinmenusagi - GOAT ライブ映像）。
# ドラムは均一に鳴っているのに中盤の対比が 1.0 を割る＝実揺れ、と切り分け済み。
# std は 0.64 と小さく後半減衰も無い — min(gc) だけがこれを捕まえる
LIVE_WOBBLE_LAB = [2.00, 1.08, 0.89, 0.95, 1.13, 1.01, 1.33, 1.16]   # 2026-08-04 の実測
LIVE_WOBBLE_RETRY = [2.022, 1.024, 0.932, 0.978, 1.08, 0.988, 1.425, 1.142]  # 同曲を再取得した実測
ok(not tempo_stable_ok(LIVE_WOBBLE_LAB, 0.64), "ライブ実揺れ（実験記録の実測値）は落ちる")
ok(not tempo_stable_ok(LIVE_WOBBLE_RETRY, 0.563), "ライブ実揺れ（再取得の実測値）も落ちる")

# クオンタイズ済みの安定曲（fixture normal 相当）: 全区間で対比 >1.0
ok(tempo_stable_ok([1.3, 1.4, 1.35, 1.5, 1.2, 1.45, 1.4, 1.6], 0.3), "全区間>1.0 の安定曲は通る")

# 後半減衰（グリッドが後半でずれていく）は min が僅かに 1.0 を超えていても落ちる
ok(not tempo_stable_ok([2.0, 1.9, 1.8, 1.7, 1.05, 1.04, 1.03, 1.02], 0.5), "後半減衰は落ちる")

# 局所BPMの揺れが大きい（大きく揺れる生演奏）は対比が良くても落ちる
ok(not tempo_stable_ok([1.3, 1.4, 1.35, 1.5, 1.2, 1.45, 1.4, 1.6], 4.0), "local_bpm_std が大きいと落ちる")

# 入力欠損は落ちる（クラッシュしない）
ok(not tempo_stable_ok([], 0.5), "gc 空は落ちる")
ok(not tempo_stable_ok([1.3, 1.4], None), "local_bpm_std 欠損は落ちる")
ok(tempo_stable_ok([1.5], 0.5), "区間1つ（短い曲）は減衰チェックを飛ばして通る")

print(f"OK ({checks} checks)")
