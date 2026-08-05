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
#
# トランザクション（LaLa からの起動・途中キャンセルに耐える。docs/plans/2026-08-05-1804）:
#   - 出力は report.md.next へ書かせ、exit 0＋妥当性検査を通ったときだけ report.md へ
#     atomic rename する。失敗・中断では旧 report.md / report.html に触れない
#   - フォルダ単位の排他は lockf の fd 形式（このシェル自身がロックを保持し、trap 中も維持。
#     command 形式だと killpg で lockf が先に死に、cleanup 完了前にロックが解放される）
#   - SIGKILL では trap が走らないため、残骸 .next は「次回実行がロック取得後に掃除」
#     「LaLa 側が join 後にロックを取れたときだけ掃除」の2経路で回収する
set -euo pipefail
TOOLS="$(cd "$(dirname "$0")" && pwd)"
REF="${1:?使い方: ./report.sh <リファレンスフォルダ>}"
REF="$(cd "${REF%/}" && pwd)"

[ -f "$REF/analysis/gates.json" ] || { echo "先に ./analyze.sh '$REF' を回してください（gates.json が要る）" >&2; exit 1; }

# claude の解決: env 上書き（テストの fake 用）→ PATH → mise shims（GUI 起動の LaLa は
# PATH に mise が乗らないため。shims は node のバージョンに依存しない安定パス）
if [ -z "${CLAUDE_BIN:-}" ]; then
  CLAUDE_BIN="$(command -v claude || true)"
  [ -n "$CLAUDE_BIN" ] || CLAUDE_BIN="$HOME/.local/share/mise/shims/claude"
fi
[ -x "$CLAUDE_BIN" ] || { echo "claude が見つかりません（PATH にも ~/.local/share/mise/shims にも無い）" >&2; exit 1; }

# フォルダ単位のロック。取れなければ即あきらめる（先発の .next には一切触れない）
LOCK_FILE="$REF/.report.lock"
exec 9>"$LOCK_FILE"
/usr/bin/lockf -s -t 0 9 || { echo "別のレポート生成が走行中です" >&2; exit 75; }

NEXT="$REF/report.md.next"
cleanup() { rm -f "$NEXT"; }
on_signal() { cleanup; exit "$1"; }
trap cleanup EXIT
trap 'on_signal 129' HUP
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

# 前回クラッシュ（SIGKILL 等）の残骸。ロック取得後なので生きている実行の出力ではない
rm -f "$NEXT"

# 雛形と見本はリポジトリ側にあるので、プロンプト内のパスを実パスに差し替えて渡す。
# 出力先も report.md.next に差し替える（report-template.md は別名なので誤置換しない）
PROMPT=$(sed -e "s#TOOLS/#${TOOLS}/#g" -e "s#report\.md#report.md.next#g" "$TOOLS/report-prompt.md")

echo "==> レポートを書いています（10〜15分）。何をしているかを流します"
cd "$REF"

# 既定の出力は「全部書き終わるまで無言」なので、10分以上だんまりになって
# 止まったように見える。stream-json で経過を流す。
"$CLAUDE_BIN" -p "$PROMPT" \
  --add-dir "$TOOLS" \
  --allowedTools Read Write Glob Grep Bash \
  --output-format stream-json --verbose \
  | "$TOOLS/.venv/bin/python" -u "$TOOLS/stream_progress.py"

# 妥当性検査 → atomic rename（trap は有効なまま。mv 失敗時も EXIT cleanup が .next を回収する）。
# サイズ下限は「途中まで書いて exit 0」の部分出力から旧 report.md を守る粗い網
# （実レポートは17KB前後・ゲート落ちだらけでも数KBは下回らない。節ごとの完成検査は後続 plan
#   2026-08-05-1858 で入る予定）
MIN_BYTES=2000
[ -s "$NEXT" ] || { echo "レポートが書かれませんでした（分析のゲート落ち等。上の出力を確認）" >&2; exit 65; }
grep -q '^# ' "$NEXT" || { echo "出力に見出しが無く、レポートの体裁になっていません" >&2; exit 65; }
NEXT_BYTES=$(wc -c < "$NEXT" | tr -d ' ')
[ "$NEXT_BYTES" -ge "$MIN_BYTES" ] || {
  echo "出力が短すぎます（${NEXT_BYTES}B < ${MIN_BYTES}B）。部分出力とみなし旧レポートを維持します" >&2
  exit 65
}
mv -f "$NEXT" "$REF/report.md"
rm -f "$REF/report.html"  # HTML キャッシュを無効化（次に開くときに再変換される）

echo
echo "書き出し: $REF/report.md"
