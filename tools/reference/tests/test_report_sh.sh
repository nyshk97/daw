#!/usr/bin/env bash
# report.sh のトランザクション・排他・完成検査の回帰テスト（fake claude を CLAUDE_BIN で差し込む）。
# 実 claude は呼ばない（トークンを使わない・数秒で終わる）。
#
# 新フロー: report.py fill がドラフト(report.md.next)を書き、claude は【判】を埋め、
# report.py check が完成検査。fake claude は「実際の report.py 出力」に対して振る舞う
# （空の簡易ドラフトで代用しない — 【機】の表・単独強調ラベルがある状態で検出できることの検証）。
#
# 使い方: bash tests/test_report_sh.sh
set -uo pipefail
TESTS="$(cd "$(dirname "$0")" && pwd)"
TOOLS="$(dirname "$TESTS")"
PY="$TOOLS/.venv/bin/python"
CHECKS=0

ok() { # ok <条件の真偽(0/1)> <説明>
  if [ "$1" -ne 0 ]; then echo "FAIL: $2" >&2; exit 1; fi
  CHECKS=$((CHECKS + 1))
}

make_ref() { # 分析済みを装ったフォルダ（実 fixture の analysis 一式）。旧 report.md / report.html 入り
  local ref="$1"
  mkdir -p "$ref"
  cp -R "$TESTS/fixtures/normal/analysis" "$ref/analysis"
  echo '# old report' > "$ref/report.md"
  echo '<html>old</html>' > "$ref/report.html"
}

make_fake_claude() { # $1=パス $2=中身（cwd はリファレンスフォルダで呼ばれる）
  printf '#!/bin/bash\n%s\n' "$2" > "$1"
  chmod +x "$1"
}

# 〈…〉を文章に置き換える＝【判】を全部書いた状態にする fake claude の中身
COMPLETE_CMD="\"$PY\" -c \"
import re, pathlib
p = pathlib.Path('report.md.next')
t = p.read_text()
parts = []
def stash(m):
    parts.append(m.group(0)); return chr(0) + str(len(parts)-1) + chr(0)
t = re.sub(r'<!--.*?-->', stash, t, flags=re.DOTALL)
t = re.sub(r'〈[^〉\\n]*〉', '疑似実行の文章段落。数値が聴こえ方に効く理由を書いた体のダミー。', t)
t = re.sub(chr(0) + r'(\d+)' + chr(0), lambda m: parts[int(m.group(1))], t)
p.write_text(t)
\""

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ---- 1. 成功: 【判】が書かれ、report.md に置き換わり、html が消え、.next も残らない ----
REF="$TMP/ok"; make_ref "$REF"
make_fake_claude "$TMP/claude-ok" "$COMPLETE_CMD"
CLAUDE_BIN="$TMP/claude-ok" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
ok $? "成功ケースは exit 0"
grep -q '疑似実行の文章段落' "$REF/report.md"; ok $? "report.md が新しい内容に置き換わる"
grep -q '判:groove:start' "$REF/report.md"; ok $? "【判】マーカーは最終出力にも残る"
[ ! -f "$REF/report.html" ]; ok $? "成功時に report.html（キャッシュ）が消える"
[ ! -f "$REF/report.md.next" ]; ok $? "成功時に .next が残らない"
[ -f "$REF/logs/report-digest.json" ]; ok $? "ダイジェストが logs/ に書かれる"

# ---- 2. claude が exit 非0: 旧 report.md / html を維持し .next も残らない ----
REF="$TMP/fail"; make_ref "$REF"
make_fake_claude "$TMP/claude-fail" 'exit 1'
CLAUDE_BIN="$TMP/claude-fail" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
[ $? -ne 0 ]; ok $? "claude 失敗は exit 非0"
grep -q 'old report' "$REF/report.md"; ok $? "失敗時は旧 report.md を維持"
[ -f "$REF/report.html" ]; ok $? "失敗時は旧 report.html を維持"
[ ! -f "$REF/report.md.next" ]; ok $? "失敗時に .next が残らない"

# ---- 2b. claude の API エラー: 理由は stream-json（stdout）にしか出ない → RUN_LOG から
#          抽出して stderr に出す（LaLa のトーストが stderr 末尾行を表示するため）----
REF="$TMP/apierr"; make_ref "$REF"
make_fake_claude "$TMP/claude-apierr" 'cat <<JSONEOF
{"type":"assistant","message":{"content":[{"type":"text","text":"OAuth token has expired. Please run /login."}]},"error":"authentication_error","is_api_error_message":true}
{"type":"result","is_error":true,"terminal_reason":"api_error","result":"OAuth token has expired. Please run /login."}
JSONEOF
exit 1'
ERR=$(CLAUDE_BIN="$TMP/claude-apierr" bash "$TOOLS/report.sh" "$REF" 2>&1 >/dev/null)
RC=$?
[ "$RC" -eq 1 ]; ok $? "API エラーは claude の exit code で失敗（実測: ${RC}）"
LAST_ERR=$(printf '%s\n' "$ERR" | grep -v '^$' | tail -1)
[[ "$LAST_ERR" == *"OAuth token has expired"* ]]; ok $? "stderr の末尾行にエラー理由が出る（実測: ${LAST_ERR}）"
grep -q 'old report' "$REF/report.md"; ok $? "API エラーでは旧 report.md を維持"
[ ! -f "$REF/report.md.next" ]; ok $? "API エラーで .next が残らない"

# ---- 3. 何も書かず exit 0: 完成検査で弾いて旧を維持 ----
REF="$TMP/noop"; make_ref "$REF"
make_fake_claude "$TMP/claude-noop" 'true'
CLAUDE_BIN="$TMP/claude-noop" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
[ $? -eq 65 ]; ok $? "未執筆のまま終わると exit 65"
grep -q 'old report' "$REF/report.md"; ok $? "未執筆では旧 report.md を維持"
[ ! -f "$REF/report.md.next" ]; ok $? "未執筆で .next が残らない"

# ---- 3b. 見出しだけ直して【判】の文章を書かない: 完成検査で弾く ----
#（実際の report.py 出力に対して。【機】の表・強調ラベルが充填済みでも検出できることの検証）
REF="$TMP/lazy"; make_ref "$REF"
make_fake_claude "$TMP/claude-lazy" "\"$PY\" -c \"
import re, pathlib
p = pathlib.Path('report.md.next')
t = p.read_text()
parts = []
def stash(m):
    parts.append(m.group(0)); return chr(0) + str(len(parts)-1) + chr(0)
t = re.sub(r'<!--.*?-->', stash, t, flags=re.DOTALL)
t = re.sub(r'〈[^〉\\n]*をここに〉', '', t)   # 本文型【判】は空のまま
t = re.sub(r'〈[^〉\\n]*〉', 'ダミー', t)      # 見出し・セルは埋める
t = re.sub(chr(0) + r'(\d+)' + chr(0), lambda m: parts[int(m.group(1))], t)
p.write_text(t)
\""
CLAUDE_BIN="$TMP/claude-lazy" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
[ $? -eq 65 ]; ok $? "見出しだけ直して本文が空だと exit 65"
grep -q 'old report' "$REF/report.md"; ok $? "本文空では旧 report.md を維持"

# ---- 3c. ドラフトを壊した（体裁が無い）: 妥当性検査で弾く ----
REF="$TMP/junk"; make_ref "$REF"
make_fake_claude "$TMP/claude-junk" 'echo "not a report" > report.md.next'
CLAUDE_BIN="$TMP/claude-junk" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
[ $? -eq 65 ]; ok $? "見出し無しは exit 65"
grep -q 'old report' "$REF/report.md"; ok $? "検査落ちでは旧 report.md を維持"

# ---- 4. BPM 系ゲート落ち: claude を起動せず非ゼロ終了・旧を維持 ----
REF="$TMP/gate"; make_ref "$REF"
"$PY" -c "
import json, pathlib
p = pathlib.Path('$REF/analysis/gates.json')
g = json.loads(p.read_text())
g['bpm']['octave_ok'] = False
p.write_text(json.dumps(g))
"
make_fake_claude "$TMP/claude-sentinel" 'touch claude-was-invoked'
CLAUDE_BIN="$TMP/claude-sentinel" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
[ $? -eq 66 ]; ok $? "ゲート落ちは exit 66"
[ ! -f "$REF/claude-was-invoked" ]; ok $? "ゲート落ちでは claude が起動されない"
grep -q 'old report' "$REF/report.md"; ok $? "ゲート落ちでは旧 report.md を維持"
[ -f "$REF/report.html" ]; ok $? "ゲート落ちでは旧 report.html を維持"
[ ! -f "$REF/report.md.next" ]; ok $? "ゲート落ちで .next が残らない"

# ---- 5. SIGTERM（アプリのキャンセル相当）: 旧を維持し .next を回収 ----
REF="$TMP/term"; make_ref "$REF"
make_fake_claude "$TMP/claude-slow" 'sleep 30'
CLAUDE_BIN="$TMP/claude-slow" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1 &
SH_PID=$!
sleep 2
kill -TERM "$SH_PID" 2>/dev/null
wait "$SH_PID" 2>/dev/null
STATUS=$?
[ "$STATUS" -eq 143 ]; ok $? "SIGTERM は exit 143（実測: ${STATUS}）"
grep -q 'old report' "$REF/report.md"; ok $? "中断では旧 report.md を維持"
[ ! -f "$REF/report.md.next" ]; ok $? "中断で .next が残らない"
pkill -f "$TMP/claude-slow" 2>/dev/null # 孤児の sleep を回収

# ---- 6. 起動時に古い .next があっても影響しない（ロック下で掃除→fill が書き直す）----
REF="$TMP/stale"; make_ref "$REF"
echo '# stale' > "$REF/report.md.next"
make_fake_claude "$TMP/claude-ok2" "$COMPLETE_CMD"
CLAUDE_BIN="$TMP/claude-ok2" bash "$TOOLS/report.sh" "$REF" > /dev/null 2>&1
ok $? "古い .next があっても成功する"
grep -q '疑似実行の文章段落' "$REF/report.md"; ok $? "古い .next でなく新しい出力が採用される"

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
