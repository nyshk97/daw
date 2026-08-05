#!/usr/bin/env bash
# report.sh のトランザクション・排他の回帰テスト（fake claude を CLAUDE_BIN で差し込む）。
# 実 claude は呼ばない（トークンを使わない・数秒で終わる）。
#
# 使い方: bash tests/test_report_sh.sh
set -uo pipefail
TESTS="$(cd "$(dirname "$0")" && pwd)"
TOOLS="$(dirname "$TESTS")"
CHECKS=0

ok() { # ok <条件の真偽(0/1)> <説明>
  if [ "$1" -ne 0 ]; then echo "FAIL: $2" >&2; exit 1; fi
  CHECKS=$((CHECKS + 1))
}

make_ref() { # 分析済みを装った最小フォルダ。旧 report.md / report.html 入り
  local ref="$1"
  mkdir -p "$ref/analysis"
  echo '{}' > "$ref/analysis/gates.json"
  echo '# old report' > "$ref/report.md"
  echo '<html>old</html>' > "$ref/report.html"
}

make_fake_claude() { # $1=パス $2=中身（cwd はリファレンスフォルダで呼ばれる）
  printf '#!/bin/bash\n%s\n' "$2" > "$1"
  chmod +x "$1"
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ---- 1. 成功: .next が report.md に置き換わり、html が消え、.next も残らない ----
REF="$TMP/ok"; make_ref "$REF"
make_fake_claude "$TMP/claude-ok" 'echo "# new report" > report.md.next
for i in $(seq 1 100); do echo "検証用の本文行 $i — サイズ下限(2000B)を満たすための実質的な内容" >> report.md.next; done'
CLAUDE_BIN="$TMP/claude-ok" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
ok $? "成功ケースは exit 0"
grep -q 'new report' "$REF/report.md"; ok $? "report.md が新しい内容に置き換わる"
[ ! -f "$REF/report.html" ]; ok $? "成功時に report.html（キャッシュ）が消える"
[ ! -f "$REF/report.md.next" ]; ok $? "成功時に .next が残らない"

# ---- 2. exit 非0: 旧 report.md / html を維持し .next も残らない ----
REF="$TMP/fail"; make_ref "$REF"
make_fake_claude "$TMP/claude-fail" 'echo "# partial" > report.md.next; exit 1'
CLAUDE_BIN="$TMP/claude-fail" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
[ $? -ne 0 ]; ok $? "claude 失敗は exit 非0"
grep -q 'old report' "$REF/report.md"; ok $? "失敗時は旧 report.md を維持"
[ -f "$REF/report.html" ]; ok $? "失敗時は旧 report.html を維持"
[ ! -f "$REF/report.md.next" ]; ok $? "失敗時に .next が残らない"

# ---- 3. 妥当性検査落ち（見出し無し）: 旧を維持 ----
REF="$TMP/invalid"; make_ref "$REF"
make_fake_claude "$TMP/claude-invalid" 'echo "not a report" > report.md.next'
CLAUDE_BIN="$TMP/claude-invalid" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
[ $? -eq 65 ]; ok $? "見出し無しは exit 65"
grep -q 'old report' "$REF/report.md"; ok $? "検査落ちでは旧 report.md を維持"
[ ! -f "$REF/report.md.next" ]; ok $? "検査落ちで .next が残らない"

# ---- 4. 書かれなかった（ゲート落ち相当）: exit 65 ----
REF="$TMP/nowrite"; make_ref "$REF"
make_fake_claude "$TMP/claude-nowrite" 'true'
CLAUDE_BIN="$TMP/claude-nowrite" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
[ $? -eq 65 ]; ok $? "未出力は exit 65"
grep -q 'old report' "$REF/report.md"; ok $? "未出力では旧 report.md を維持"

# ---- 4b. 部分出力（見出しだけ書いて exit 0）: サイズ下限で弾いて旧を維持 ----
REF="$TMP/partial"; make_ref "$REF"
make_fake_claude "$TMP/claude-partial" 'echo "# x" > report.md.next'
CLAUDE_BIN="$TMP/claude-partial" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
[ $? -eq 65 ]; ok $? "短すぎる部分出力は exit 65"
grep -q 'old report' "$REF/report.md"; ok $? "部分出力では旧 report.md を維持"
[ ! -f "$REF/report.md.next" ]; ok $? "部分出力で .next が残らない"

# ---- 5. SIGTERM（アプリのキャンセル相当）: 旧を維持し .next を回収 ----
REF="$TMP/term"; make_ref "$REF"
make_fake_claude "$TMP/claude-slow" 'echo "# slow" > report.md.next; sleep 30'
CLAUDE_BIN="$TMP/claude-slow" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1 &
SH_PID=$!
sleep 1
kill -TERM "$SH_PID" 2>/dev/null
wait "$SH_PID" 2>/dev/null
STATUS=$?
[ "$STATUS" -eq 143 ]; ok $? "SIGTERM は exit 143（実測: ${STATUS}）"
grep -q 'old report' "$REF/report.md"; ok $? "中断では旧 report.md を維持"
[ ! -f "$REF/report.md.next" ]; ok $? "中断で .next が残らない"
pkill -f "$TMP/claude-slow" 2>/dev/null # 孤児の sleep を回収

# ---- 6. 起動時に古い .next があっても影響しない（ロック下で掃除される）----
REF="$TMP/stale"; make_ref "$REF"
echo '# stale' > "$REF/report.md.next"
make_fake_claude "$TMP/claude-ok2" 'echo "# fresh" > report.md.next
for i in $(seq 1 100); do echo "検証用の本文行 $i — サイズ下限(2000B)を満たすための実質的な内容" >> report.md.next; done'
CLAUDE_BIN="$TMP/claude-ok2" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
ok $? "古い .next があっても成功する"
grep -q 'fresh' "$REF/report.md"; ok $? "古い .next でなく新しい出力が採用される"

# ---- 7. 排他: 保持中の2本目は exit 75 で、1本目の .next に触れない ----
REF="$TMP/lock"; make_ref "$REF"
echo '# first run output' > "$REF/report.md.next" # 1本目の途中出力に見立てる
(
  exec 9>"$REF/.report.lock"
  /usr/bin/lockf -s -t 0 9 || exit 1
  sleep 5
) &
HOLDER=$!
sleep 0.5
make_fake_claude "$TMP/claude-never" 'echo should-not-run > /dev/null'
CLAUDE_BIN="$TMP/claude-never" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
[ $? -eq 75 ]; ok $? "ロック保持中の2本目は exit 75"
grep -q 'first run output' "$REF/report.md.next"; ok $? "2本目は1本目の .next に触れない"
kill "$HOLDER" 2>/dev/null; wait "$HOLDER" 2>/dev/null

echo "ok ($CHECKS checks)"
