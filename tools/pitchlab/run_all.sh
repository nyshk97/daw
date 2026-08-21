#!/usr/bin/env bash
# Phase 0 の全工程を再現する（約 10 分。CREPE の推論が大半）。
#  1. 合成素材（既知 f0）→ 検出器3種の比較（GPE / voicing P-R）
#  2. 実声 2 区間（rap 4-12s / uta 7.5-15.5s）→ 検出器比較・目視ラベル用 PNG
#  3. ノート化 → ケース Z/A/B/C/D × 4 エンジンの再合成と数値検証
#  4. ブラインド試聴セット＋回答テンプレート
set -euo pipefail
cd "$(dirname "$0")"
PY=.venv/bin/python
SRC=~/Music/daw/2026-08-18-tundra
[ -x work/ssstretch ] || ./build_ssstretch.sh

$PY synth.py
$PY analyze.py work/synth/synth.wav synth --truth work/synth/synth.truth.json
$PY analyze.py "$SRC/clip-001.wav" rap-seg --crop 4.0 12.0
$PY analyze.py "$SRC/clip-008.wav" uta-seg --crop 7.5 15.5
for n in synth rap-seg uta-seg; do $PY notes.py "$n"; done
$PY resynth.py synth --scale chromatic
$PY resynth.py rap-seg --scale auto
$PY resynth.py uta-seg --scale auto
$PY blindprep.py
$PY answer_template.py
echo "pitchlab: 全工程完了。listen/ を聴いて docs/labs/reference-beat-human-answers/ の回答を埋める"
