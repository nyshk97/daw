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
#   - 10〜15分かかる。文章は1トークンずつ順番にしか出せないので、これは短縮できない
#   - 下書きが出るだけ。数値の裏取りは listen/ のクリップで耳でやる
set -euo pipefail
TOOLS="$(cd "$(dirname "$0")" && pwd)"
REF="${1:?使い方: ./report.sh <リファレンスフォルダ>}"
REF="$(cd "${REF%/}" && pwd)"

[ -f "$REF/analysis/gates.json" ] || { echo "先に ./analyze.sh '$REF' を回してください（gates.json が要る）" >&2; exit 1; }

# 雛形と見本はリポジトリ側にあるので、プロンプト内のパスを実パスに差し替えて渡す
PROMPT=$(sed "s#TOOLS/#${TOOLS}/#g" "$TOOLS/report-prompt.md")

echo "==> レポートを書いています（10〜15分）。何をしているかを流します"
cd "$REF"

# 既定の出力は「全部書き終わるまで無言」なので、10分以上だんまりになって
# 止まったように見える。stream-json で経過を流す。
claude -p "$PROMPT" \
  --add-dir "$TOOLS" \
  --allowedTools Read Write Glob Grep Bash \
  --output-format stream-json --verbose \
  | "$TOOLS/.venv/bin/python" -u "$TOOLS/stream_progress.py"

echo
echo "書き出し: $REF/report.md"
