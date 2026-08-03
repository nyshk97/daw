#!/usr/bin/env bash
# 分析済みのフォルダから report.md を書く（Claude Code のヘッドレスモードを呼ぶ）。
#
#   ./report.sh <リファレンスフォルダ>
#
# analyze.sh が出した analysis/*.json を読み、report-template.md の書式で report.md を書く。
# 判断が要る節（定石と癖の切り分け・因果の説明）はテンプレート化できないので、ここは AI に投げる。
#
# 注意:
#   - Claude Code の認証をそのまま使う（API キーは不要）。トークンは通常のセッションと同じ扱い
#   - 書き込みは対象フォルダの report.md のみ。ツールは Read/Write/Glob/Grep に限定している
#   - 下書きが出るだけ。数値の裏取りは listen/ のクリップで耳でやる
set -euo pipefail
TOOLS="$(cd "$(dirname "$0")" && pwd)"
REF="${1:?使い方: ./report.sh <リファレンスフォルダ>}"
REF="$(cd "${REF%/}" && pwd)"

[ -f "$REF/analysis/gates.json" ] || { echo "先に ./analyze.sh '$REF' を回してください（gates.json が要る）" >&2; exit 1; }

# 雛形と見本はリポジトリ側にあるので、プロンプト内のパスを実パスに差し替えて渡す
PROMPT=$(sed "s#TOOLS/#${TOOLS}/#g" "$TOOLS/report-prompt.md")

cd "$REF"
claude -p "$PROMPT" \
  --add-dir "$TOOLS" \
  --allowedTools Read Write Glob Grep Bash

echo
echo "書き出し: $REF/report.md"
