#!/bin/bash
# 全FXエディタ（EQ/Comp/Sat/Lo-fi/Reverb A/B/Delay/Limiter）のスナップショットを一巡して保存する。
# 使い方: tools/fx-snapshots.sh <outdir> [projectDir]   （既定プロジェクト: ~/Music/daw/2026-08-09-river）
# LaLa-dev は多重起動不可なので 1 FX ごとに pkill → open -g → ログの debug.snapshot ok=1 待ち、を繰り返す。
# --comp-demo で dirty になるため quit でなく pkill で終了する（起動中の dev 版は巻き込んで終了するので注意）
set -u
OUT="$1"; mkdir -p "$OUT"
cd "$(dirname "$0")/.." || exit 1
APP="$PWD/build/daw_artefacts/Debug/LaLa-dev.app"
PROJ="${2:-$HOME/Music/daw/2026-08-09-river}"
LOGDIR=~/Library/Logs/daw
kill_app() { pkill -x LaLa-dev 2>/dev/null; for i in $(seq 1 30); do pgrep -x LaLa-dev >/dev/null || return 0; sleep 0.5; done; pkill -9 -x LaLa-dev; sleep 1; }
run_one() { # name args...
  local name="$1"; shift
  kill_app
  local before; before=$(ls -t "$LOGDIR" | head -1)
  open -g "$APP" --args --open "$PROJ" "$@" --play --snapshot "$OUT/$name.png"
  local ok=0
  for i in $(seq 1 40); do
    sleep 0.5
    local f; f="$LOGDIR/$(ls -t "$LOGDIR" | head -1)"
    [ "$(basename "$f")" = "$before" ] && continue
    if grep -q "debug.snapshot.*ok=1" "$f"; then ok=1; break; fi
  done
  echo "$name snapshot_ok=$ok $( [ -f "$OUT/$name.png" ] && sips -g pixelWidth -g pixelHeight "$OUT/$name.png" | awk '/pixel/{printf "%s ", $2}')"
}
run_one eq --eq-editor
run_one comp --comp-editor --comp-demo
run_one sat --sat-editor
run_one lofi --lofi-editor
run_one reverbA --reverb-editor 0
run_one reverbB --reverb-editor 1
run_one delay --delay-editor
run_one limiter --limiter-editor
kill_app
