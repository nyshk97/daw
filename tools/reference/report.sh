#!/usr/bin/env bash
# 分析済みのフォルダから report.md を書く（機械充填 report.py ＋ Claude Code ヘッドレス）。
#
#   ./report.sh <リファレンスフォルダ>
#
# 分業（docs/plans/2026-08-05-1858）:
#   1. report.py fill が report-template.md の【機】を analysis/*.json から充填した
#      ドラフト（report.md.next）と、【判】用ダイジェストを書く。数値の転記と判定語の
#      線引きを AI に任せない（転記ミスをゼロにし、曲をまたいで目盛りを揃える）
#   2. Claude Code はドラフトの【判】マーカーの内側だけを Edit で書く（Read/Edit のみ許可。
#      見本とダイジェストはプロンプトに同梱するので、読みのターンがほぼ発生しない）
#   3. report.py check が完成検査（プレースホルダ残存・必須見出し・各【判】の実質執筆）
#
# 注意:
#   - Claude Code の認証をそのまま使う（API キーは不要）。トークンは通常のセッションと同じ扱い
#   - モデルは claude-opus-5 固定。effort は env REPORT_EFFORT で上書き可（既定 high）
#   - 生の stream-json を <ref>/logs/ に保存する（時間・ターン・トークン・コストの実測用）
#   - 下書きが出るだけ。数値の裏取りは listen/ のクリップで耳でやる
#
# トランザクション（LaLa からの起動・途中キャンセルに耐える。docs/plans/2026-08-05-1804）:
#   - 出力は report.md.next へ書かせ、exit 0＋完成検査を通ったときだけ report.md へ
#     atomic rename する。失敗・中断では旧 report.md / report.html に触れない
#   - フォルダ単位の排他は lockf の fd 形式（このシェル自身がロックを保持し、trap 中も維持。
#     command 形式だと killpg で lockf が先に死に、cleanup 完了前にロックが解放される）
#   - SIGKILL では trap が走らないため、残骸 .next は「次回実行がロック取得後に掃除」
#     「LaLa 側が join 後にロックを取れたときだけ掃除」の2経路で回収する
#
# exit code: 0=成功 / 65=完成検査・妥当性検査落ち / 66=BPM系ゲート落ち（claude 未起動） /
#            75=ロック競合 / 129,130,143=シグナル
set -euo pipefail
TOOLS="$(cd "$(dirname "$0")" && pwd)"
REF="${1:?使い方: ./report.sh <リファレンスフォルダ>}"
REF="$(cd "${REF%/}" && pwd)"
PY="$TOOLS/.venv/bin/python"

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

# モデルは固定（曲をまたいで書きぶりの目盛りを揃える）。REPORT_MODEL / REPORT_EFFORT は
# A/B 実験用の上書き口（通常運用では設定しない）
MODEL="${REPORT_MODEL:-claude-opus-5}"
EFFORT="${REPORT_EFFORT:-high}"

LOG_DIR="$REF/logs"
mkdir -p "$LOG_DIR"
RUN_LOG="$LOG_DIR/report-${MODEL#claude-}-${EFFORT}-$(date +%Y%m%d-%H%M%S).jsonl"
DIGEST="$LOG_DIR/report-digest.json"

# 【機】の充填。BPM 系ゲート落ち（exit 3）はレポート自体を書かない — claude を起動しない
set +e
"$PY" "$TOOLS/report.py" fill "$REF" --out "$NEXT" --digest "$DIGEST"
FILL_RC=$?
set -e
if [ "$FILL_RC" -eq 3 ]; then
  echo "レポートを書かずに終了します（グリッド系ゲート落ち。理由は上の行）" >&2
  exit 66
fi
[ "$FILL_RC" -eq 0 ] || { echo "report.py fill が失敗しました（exit ${FILL_RC}）" >&2; exit 1; }

# プロンプト = 指示 + 見本（別の曲の完成品）+ ダイジェスト。ファイル参照でなく同梱して
# 読みのターンを削る（ベースライン実測で読みが時間の過半だったため）
PROMPT="$(cat "$TOOLS/report-prompt.md")

=====
以下は見本 — 別の曲で実際に書いたレポートの完成品。書きぶり・粒度・因果の書き方はこれに合わせる。
（この見本の数値・内容をいま書く曲に持ち込まないこと）

$(cat "$TOOLS/report-example.md")

=====
以下はこの曲のダイジェスト — 【判】の執筆に必要な分析値の抜粋。

$(cat "$DIGEST")"

echo "==> レポートを書いています。何をしているかを流します"
cd "$REF"

# 既定の出力は「全部書き終わるまで無言」なので、だんまりに見える。stream-json で経過を流す。
"$CLAUDE_BIN" -p "$PROMPT" \
  --model "$MODEL" --effort "$EFFORT" \
  --allowedTools Read Edit \
  --output-format stream-json --verbose \
  | tee "$RUN_LOG" \
  | "$PY" -u "$TOOLS/stream_progress.py"

# 妥当性検査 → 完成検査 → atomic rename（trap は有効なまま。mv 失敗時も EXIT cleanup が回収）
[ -s "$NEXT" ] || { echo "ドラフトが消えています（claude が削除した可能性。旧レポートを維持）" >&2; exit 65; }
grep -q '^# ' "$NEXT" || { echo "出力に見出しが無く、レポートの体裁になっていません" >&2; exit 65; }
"$PY" "$TOOLS/report.py" check "$NEXT" --digest "$DIGEST" || {
  echo "完成検査に落ちました（【判】の書き残しあり。旧レポートを維持します）" >&2
  exit 65
}
mv -f "$NEXT" "$REF/report.md"
rm -f "$REF/report.html"  # HTML キャッシュを無効化（次に開くときに再変換される）

echo
echo "書き出し: $REF/report.md"
