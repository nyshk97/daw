#!/usr/bin/env bash
# build.sh で作った dmg を配信 repo (nyshk97/daw-releases) の GitHub Release に上げ、
# appcast.xml を生成する。Sparkle は `latest/download/appcast.xml` を見て更新する。
# ~/ide (PolePole) の release.sh の移植・簡略版（ja のみ・統計プロキシなし）。
#
# 使い方: scripts/release.sh <version>
#   例: scripts/release.sh 0.2.0
#
# 注: このスクリプトは内部で build.sh を実行する（fresh build 込み）。
#     事前に build.sh 単体を叩く必要は無い。
# 注: notarytool が keychain を参照するため、ユーザーの Terminal で実行すること
#     （Claude Code の Bash からは動かない）。
#
# 前提:
#   - CMakeLists.txt の project(daw VERSION x.y.z) を <version> に bump してコミット済み
#   - Keychain に daw 用 Sparkle EdDSA 秘密鍵（アカウント名 "daw"）が登録済み
#     （バックアップ: ~/Library/CloudStorage/Dropbox/secrets/sparkle-ed25519-daw-private.key）
#   - `gh` で nyshk97/daw-releases に push 権限があること
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DMG_PATH="$PROJECT_ROOT/build-release/LaLa.dmg"
APPCAST_PATH="$PROJECT_ROOT/build-release/appcast.xml"
RELEASES_REPO="nyshk97/daw-releases"
FEED_URL="https://github.com/${RELEASES_REPO}/releases/latest/download/appcast.xml"
SIGN_UPDATE="$PROJECT_ROOT/.sparkle/bin/sign_update"
SPARKLE_ACCOUNT="daw"

if [ $# -eq 0 ]; then
  echo "Usage: $0 <version>"
  echo "Example: $0 0.2.0"
  exit 1
fi

VERSION="$1"
TAG="v$VERSION"
CHANGELOG="$PROJECT_ROOT/docs/CHANGELOG.md"
RELEASE_NOTES_MD="$PROJECT_ROOT/build-release/release-notes-${VERSION}.md"
SPARKLE_DESC_HTML="$PROJECT_ROOT/build-release/sparkle-description-${VERSION}.html"
mkdir -p "$PROJECT_ROOT/build-release"
cd "$PROJECT_ROOT"

# === Step 0: preflight (リポジトリを書き換える前に環境と Git 状態を検証する) ===
if ! gh auth status >/dev/null 2>&1; then
  echo "ERROR: gh が未認証です。gh auth login を実行してください"
  exit 1
fi
# タグの検査はリモートの実状態に対して行う（別クローンからリリースされているとローカルの
# タグが古いまま preflight を通過し、Step 8 の gh release create で初めて衝突する）。
# fetch 失敗＝ネットワーク不通もここで止める
echo "==> preflight: fetching tags from origin..."
git fetch origin --tags --quiet
BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [ "$BRANCH" != "main" ]; then
  echo "ERROR: カレントブランチが main ではありません ($BRANCH)"
  exit 1
fi
# clean worktree（未コミット変更があると「DMG の中身とタグの commit が一致する」保証が崩れる）。
# 未追跡ファイルも対象にする（CMakeLists に追加済みの新規ソースが未追跡だと、DMG には
# 入るのにタグの commit には存在しない事故になる）。docs/plans/ だけはビルドに一切
# 関与しない作業メモ置き場なので除外する
DIRTY=$(git status --porcelain | grep -v '^?? docs/plans/' || true)
if [ -n "$DIRTY" ]; then
  echo "ERROR: 未コミットの変更または未追跡ファイルがあります。commit / stash してから再実行してください"
  echo "$DIRTY"
  exit 1
fi
# CMakeLists の VERSION bump し忘れをビルド前に検知する（ビルド後にも Info.plist で再検査）
CMAKE_VERSION=$(sed -nE 's/^project\(daw VERSION ([0-9.]+).*/\1/p' "$PROJECT_ROOT/CMakeLists.txt")
if [ "$CMAKE_VERSION" != "$VERSION" ]; then
  echo "ERROR: CMakeLists.txt の project VERSION ($CMAKE_VERSION) != 指定バージョン ($VERSION)"
  echo "       project(daw VERSION $VERSION ...) に bump してコミットしてから再実行してください"
  exit 1
fi
# 本体 repo のタグ再実行ガード: 同名タグが HEAD を指すなら続行（後半失敗からの再実行）、
# 別 commit を指すならバージョンの使い回しなのでエラー
if git rev-parse "$TAG" >/dev/null 2>&1; then
  if [ "$(git rev-parse "$TAG"^{commit})" != "$(git rev-parse HEAD)" ]; then
    echo "ERROR: タグ $TAG が既に存在し、HEAD と別の commit を指しています"
    echo "       バージョン番号を変えるか、タグを整理してから再実行してください"
    exit 1
  fi
  echo "==> preflight: タグ $TAG は既に HEAD を指しています（再実行とみなして続行）"
fi
# 配信 repo の存在確認（初回リリース前に gh repo create nyshk97/daw-releases --public が必要）
if ! gh repo view "$RELEASES_REPO" >/dev/null 2>&1; then
  echo "ERROR: 配信 repo $RELEASES_REPO が見つかりません"
  echo "       gh repo create $RELEASES_REPO --public で作成してください（README 1 commit が必要）"
  exit 1
fi
# 配信 repo 側の Release 再実行ガード。gh の失敗が「Release が無い」なのか「ネットワーク
# エラー」なのかを区別する（エラーを握りつぶすと既存 Release を見逃して続行してしまう）
if RELEASE_VIEW_ERR=$(gh release view "$TAG" --repo "$RELEASES_REPO" 2>&1 >/dev/null); then
  echo "ERROR: $RELEASES_REPO に Release $TAG が既に存在します（前回の中途失敗の可能性）"
  echo "       内容を確認して、作り直すなら:"
  echo "         gh release delete $TAG --repo $RELEASES_REPO --cleanup-tag"
  echo "       asset 差し替えだけなら:"
  echo "         gh release upload $TAG <files> --repo $RELEASES_REPO --clobber"
  exit 1
elif ! grep -qi "not found" <<< "$RELEASE_VIEW_ERR"; then
  echo "ERROR: Release $TAG の存在確認に失敗しました（ネットワーク?）: $RELEASE_VIEW_ERR"
  exit 1
fi
if [ ! -x "$SIGN_UPDATE" ]; then
  echo "ERROR: sign_update が見つかりません。scripts/fetch-sparkle.sh を実行してください"
  exit 1
fi

# === Step 1: changelog (claude headless で自動生成 → レビュー用ポーズ) ===
if [ ! -f "$CHANGELOG" ]; then
  echo "ERROR: $CHANGELOG が見つかりません"
  exit 1
fi
if ! grep -q "^## \[Unreleased\]" "$CHANGELOG"; then
  echo "ERROR: docs/CHANGELOG.md に '## [Unreleased]' セクションがありません"
  exit 1
fi

# 直前リリースの version を CHANGELOG.md から拾う ([Unreleased] の次の ## [X.Y.Z])
LAST_VERSION=$(awk '/^## \[Unreleased\]/{f=1; next} f && /^## \[([^]]+)\]/{match($0, /\[([^]]+)\]/); print substr($0, RSTART+1, RLENGTH-2); exit}' "$CHANGELOG")
RANGE=""
if [ -n "$LAST_VERSION" ] && git rev-parse "v${LAST_VERSION}" >/dev/null 2>&1; then
  RANGE="v${LAST_VERSION}..HEAD"
  echo ""
  echo "==> 前回リリース v${LAST_VERSION} 以降の commit:"
  # --max-count で git log 側で打ち切る (| head だと SIGPIPE × pipefail で全体が落ちる)
  git log "$RANGE" --max-count=100 --pretty=format:"  %h %s"
  echo ""
else
  echo ""
  echo "==> 直近 30 commit (CHANGELOG から前回リリースタグを特定できず):"
  git log -30 --pretty=format:"  %h %s"
  echo ""
fi

# [Unreleased] が空なら claude headless で自動生成する（手書き済みならそれを尊重）。
# 生成失敗・claude 不在・前回タグ不明（リリース途中失敗からの再開時など）は手動編集に倒す。
# 生成が CHANGELOG 以外に触った場合は直後の DIRTY_AFTER_PAUSE 検査が止める
UNRELEASED_BODY=$(awk '/^## \[Unreleased\]/{f=1; next} /^## \[/{f=0} f' "$CHANGELOG" | grep -v '^[[:space:]]*$' || true)
if [ -n "$UNRELEASED_BODY" ]; then
  echo ""
  echo "==> [Unreleased] に記入済みの内容があるため自動生成をスキップします"
elif [ -z "$RANGE" ] || ! command -v claude >/dev/null 2>&1; then
  echo ""
  echo "WARN: 自動生成をスキップします (前回リリースタグ不明 or claude CLI なし)。手動で編集してください"
else
  echo ""
  echo "==> claude で [Unreleased] を自動生成しています…"
  if claude -p "docs/CHANGELOG.md の [Unreleased] セクションを、git の ${RANGE} のコミット内容から埋めてください。ファイル冒頭の『書き方』セクション（フォーマット・カテゴリ・文体）に厳密に従うこと。ユーザー目視で気づく変更だけ書き、内部リファクタ・ドキュメント・ビルド設定の変更は書かないこと。docs/CHANGELOG.md 以外のファイルを変更してはいけない。" \
      --allowedTools "Read,Grep,Glob,Edit,Bash(git log:*),Bash(git show:*),Bash(git diff:*)"; then
    echo ""
    echo "==> 生成結果 (git diff):"
    git --no-pager diff -- "$CHANGELOG"
  else
    echo "WARN: claude の自動生成に失敗しました。手動で編集してください"
  fi
fi

echo ""
echo "↑ $CHANGELOG の [Unreleased] を確認し、必要なら手直ししてください:"
echo "    - ユーザー目視で気づく変更だけ書く (内部リファクタ・ドキュメント変更は除く)"
echo "    - カテゴリは ✨ Added / 📝 Changed / 🐛 Fixed / 🗑️ Removed"
echo ""
read -r -p "  問題なければ Enter で続行 (Ctrl+C で中断): " _

# ポーズ中に CHANGELOG 以外を編集していないか再検査する。ここで混入したソース変更は
# ビルド（DMG）には入るのに Step 2 の commit（= タグの commit）には入らず乖離するため
DIRTY_AFTER_PAUSE=$(git status --porcelain | grep -v '^?? docs/plans/' | grep -v 'docs/CHANGELOG\.md$' || true)
if [ -n "$DIRTY_AFTER_PAUSE" ]; then
  echo "ERROR: ポーズ中に CHANGELOG 以外が変更されています。commit してから再実行してください"
  echo "$DIRTY_AFTER_PAUSE"
  exit 1
fi

# === Step 2: [Unreleased] → [<version>] - <today> 書き換え + commit ===
TODAY=$(date +%Y-%m-%d)
python3 - "$CHANGELOG" "$VERSION" "$TODAY" <<'PY'
import sys, re, pathlib
path = pathlib.Path(sys.argv[1])
version, date = sys.argv[2], sys.argv[3]
text = path.read_text()

# 再実行ガード: 途中失敗後の再実行では [Unreleased] が空で [version] が既に存在する。
# その場合はリネーム済みとみなしてスキップする
unreleased = re.search(r"^## \[Unreleased\]\s*$(.*?)(?=^## \[|\Z)", text, flags=re.M | re.S)
if not unreleased:
    sys.exit("ERROR: [Unreleased] セクションが見つかりません")
has_content = bool(unreleased.group(1).strip())
already_renamed = re.search(rf"^## \[{re.escape(version)}\]", text, flags=re.M)

if already_renamed and not has_content:
    print(f"  CHANGELOG.md: [{version}] は既に存在し [Unreleased] は空 — リネーム済みとみなしてスキップ (再実行)")
    sys.exit(0)
if already_renamed and has_content:
    sys.exit(f"ERROR: [{version}] が既に存在するのに [Unreleased] にも内容があります。CHANGELOG を手で整理してください")
if not has_content:
    sys.exit("ERROR: [Unreleased] が空です。リリースノートを書いてから再実行してください")

new = re.sub(
    r"^## \[Unreleased\]\s*$",
    f"## [Unreleased]\n\n## [{version}] - {date}",
    text, count=1, flags=re.M,
)
path.write_text(new)
print(f"  CHANGELOG.md: [Unreleased] の下に [{version}] - {date} を挿入")
PY

if ! git diff --quiet "$CHANGELOG"; then
  git add "$CHANGELOG"
  git commit -m "docs(changelog): release ${VERSION}"
  echo "  CHANGELOG.md を commit (release ${VERSION})"
fi

# === Step 3: 該当 section から release notes (md) と Sparkle description (HTML) を生成 ===
python3 - "$CHANGELOG" "$VERSION" "$RELEASE_NOTES_MD" "$SPARKLE_DESC_HTML" <<'PY'
import sys, re, pathlib
md_path = pathlib.Path(sys.argv[1])
version = sys.argv[2]
notes_path = pathlib.Path(sys.argv[3])
desc_path = pathlib.Path(sys.argv[4])
md = md_path.read_text()
pattern = re.compile(rf"^## \[{re.escape(version)}\][^\n]*\n(.*?)(?=^## \[|\Z)", re.S | re.M)
m = pattern.search(md)
if not m:
    sys.exit(f"ERROR: CHANGELOG から [{version}] section を抽出できません")
body = m.group(1).strip()

# --- release notes (markdown, そのまま) ---
notes = f"# LaLa {version}\n\n{body}\n"
notes_path.write_text(notes)
print(f"  Wrote {notes_path}")

# --- Sparkle description (HTML) ---
def inline(text):
    text = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", r'<a href="\2">\1</a>', text)
    return text

# Sparkle の Update Notes は WKWebView で描画される。`color-scheme: light dark` で
# 配色をシステムに任せる（固定色はダーク背景で本文が読めなくなる事故のもと）
html = ['<style>:root{color-scheme:light dark}body{font:-apple-system-body;line-height:1.5}h3{font-size:14px;margin:16px 0 6px}ul{margin:0;padding-left:20px}li{margin:3px 0}code{background:rgba(127,127,127,0.18);padding:1px 5px;border-radius:3px;font-size:90%}</style>']
in_ul = False
for line in body.split("\n"):
    line = line.rstrip()
    if line.startswith("### "):
        if in_ul:
            html.append("</ul>"); in_ul = False
        html.append(f"<h3>{inline(line[4:])}</h3>")
    elif line.startswith("- "):
        if not in_ul:
            html.append("<ul>"); in_ul = True
        html.append(f"<li>{inline(line[2:].strip())}</li>")
if in_ul:
    html.append("</ul>")
desc_path.write_text("\n".join(html))
print(f"  Wrote {desc_path}")
PY

echo "==> Running fresh build (always rebuild to avoid uploading stale dmg)..."
"$SCRIPT_DIR/build.sh"

# === Step 4: ビルド成果物の整合チェック ===
BUILT_APP="/tmp/daw-export/LaLa.app"
BUNDLE_VERSION=$(plutil -extract CFBundleVersion raw "${BUILT_APP}/Contents/Info.plist")
SHORT_VERSION=$(plutil -extract CFBundleShortVersionString raw "${BUILT_APP}/Contents/Info.plist")
if [ "$SHORT_VERSION" != "$VERSION" ]; then
  echo "ERROR: built CFBundleShortVersionString ($SHORT_VERSION) != requested <version> ($VERSION)"
  exit 1
fi
# Sparkle が新旧比較に使うのは CFBundleVersion。JUCE の plist 生成が project(VERSION) と
# 乖離した場合（JUCE 更新等）に、タグと不一致な sparkle:version を配信しないよう検査する
if [ "$BUNDLE_VERSION" != "$VERSION" ]; then
  echo "ERROR: built CFBundleVersion ($BUNDLE_VERSION) != requested <version> ($VERSION)"
  exit 1
fi
# ビルド成果物の SUFeedURL が本スクリプトの配信先と一致するか検査する
# （CMakeLists.txt と release.sh の 2 箇所にある配信 repo 定義の乖離を検知する）
APP_FEED=$(plutil -extract SUFeedURL raw "${BUILT_APP}/Contents/Info.plist")
if [ "$APP_FEED" != "$FEED_URL" ]; then
  echo "ERROR: built SUFeedURL ($APP_FEED) != release.sh FEED_URL ($FEED_URL)"
  echo "       CMakeLists.txt の SUFeedURL と release.sh の RELEASES_REPO がずれています"
  exit 1
fi

# === Step 5: push（タグは Release 作成成功後に打つ） ===
echo "==> Pushing main to origin..."
git push origin main

# === Step 6: EdDSA 署名 ===
echo "==> Signing dmg with EdDSA (keychain account: $SPARKLE_ACCOUNT)..."
SIG_OUTPUT=$("$SIGN_UPDATE" --account "$SPARKLE_ACCOUNT" "$DMG_PATH")
echo "$SIG_OUTPUT"
ED_SIG=$(echo "$SIG_OUTPUT" | sed -nE 's/.*sparkle:edSignature="([^"]+)".*/\1/p')
LENGTH=$(echo "$SIG_OUTPUT" | sed -nE 's/.*length="([^"]+)".*/\1/p')
if [ -z "$ED_SIG" ] || [ -z "$LENGTH" ]; then
  echo "ERROR: failed to parse sign_update output"
  exit 1
fi

# === Step 7: appcast.xml 生成 ===
echo "==> Generating appcast.xml..."
# pubDate は RFC 822。LC_ALL=C で曜日/月名を英語に固定（ja_JP のままだと Sparkle がパースできない）
PUB_DATE=$(LC_ALL=C date -u "+%a, %d %b %Y %H:%M:%S +0000")
DOWNLOAD_URL="https://github.com/${RELEASES_REPO}/releases/download/${TAG}/LaLa.dmg"
# 最小 OS はビルド済みバイナリの minos から取る（Info.plist に LSMinimumSystemVersion は無い）。
# 変数に受けてから awk する（pipefail 下での SIGPIPE 対策 + 取得失敗を silent fallback で
# 誤った既定値にせずエラー停止する）
OTOOL_OUT=$(otool -l "${BUILT_APP}/Contents/MacOS/LaLa")
MIN_OS=$(echo "$OTOOL_OUT" | awk '/minos/{print $2; exit}')
if [ -z "$MIN_OS" ]; then
  echo "ERROR: バイナリの minos を otool から取得できません（出力形式が変わった可能性）"
  exit 1
fi

# 既存 appcast を取得（初回は空の RSS テンプレを用意）。累積方式（過去バージョンも残す）。
# 既に Release が存在するのに取得に失敗した場合はエラー停止する（一時的なネットワーク障害を
# 「初回リリース」と誤認して履歴のない appcast を公開し、過去 item を黙って消す事故を防ぐ）
EXISTING_RELEASES=$(gh release list --repo "$RELEASES_REPO" --limit 1)
TMP_APPCAST="$(mktemp)"
trap 'rm -f "$TMP_APPCAST"' EXIT
if curl -fsSL "${FEED_URL}" -o "$TMP_APPCAST" && grep -q "<rss" "$TMP_APPCAST"; then
  echo "    Fetched existing appcast.xml from ${FEED_URL}"
elif [ -n "$EXISTING_RELEASES" ]; then
  echo "ERROR: ${RELEASES_REPO} に既存 Release があるのに appcast.xml を取得できませんでした"
  echo "       （一時的な障害の可能性。ここで fresh を作ると過去バージョンの item が feed から消えます）"
  echo "       ネットワークを確認して再実行してください"
  exit 1
else
  echo "    No existing appcast.xml; creating fresh (初回リリース)"
  cat > "$TMP_APPCAST" <<EOF
<?xml version="1.0" standalone="yes"?>
<rss xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" version="2.0">
  <channel>
    <title>LaLa</title>
    <link>${FEED_URL}</link>
    <description>Most recent LaLa updates</description>
    <language>ja</language>
  </channel>
</rss>
EOF
fi

DESC_BODY=$(cat "$SPARKLE_DESC_HTML")
NEW_ITEM="    <item>
      <title>${SHORT_VERSION}</title>
      <pubDate>${PUB_DATE}</pubDate>
      <sparkle:version>${BUNDLE_VERSION}</sparkle:version>
      <sparkle:shortVersionString>${SHORT_VERSION}</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>${MIN_OS}</sparkle:minimumSystemVersion>
      <description><![CDATA[
${DESC_BODY}
]]></description>
      <enclosure
        url=\"${DOWNLOAD_URL}\"
        sparkle:edSignature=\"${ED_SIG}\"
        length=\"${LENGTH}\"
        type=\"application/x-apple-diskimage\" />
    </item>"

# </channel> の直前に挿入。クオートの二重エスケープを避けるため新 item は env で渡す
NEW_ITEM_ENV="$NEW_ITEM" python3 - "$TMP_APPCAST" "$APPCAST_PATH" <<'PY'
import os, sys
inp, out = sys.argv[1], sys.argv[2]
new_item = os.environ['NEW_ITEM_ENV']
with open(inp) as f:
    body = f.read()
needle = '  </channel>'
if needle not in body:
    raise SystemExit("ERROR: no '  </channel>' found in appcast.xml")
body = body.replace(needle, new_item + '\n' + needle, 1)
with open(out, 'w') as f:
    f.write(body)
PY

echo "    Generated $APPCAST_PATH"

# === Step 8: GitHub Release 作成（配信 repo） ===
echo "==> Creating release on ${RELEASES_REPO} (Sparkle feed + dmg)..."
gh release create "$TAG" \
  "$DMG_PATH" \
  "$APPCAST_PATH" \
  --repo "${RELEASES_REPO}" \
  --title "$TAG" \
  --notes-file "$RELEASE_NOTES_MD"

# === Step 9: 本体 repo にタグ（DMG を作った commit を記録） ===
# Release 作成の成功後に打つ。先に push すると、後半失敗時に「リリース物のないタグ」が
# 公開され、修正コミットが必要なケースで preflight の同名タグガードが再リリースを塞ぐ
if ! git rev-parse "$TAG" >/dev/null 2>&1; then
  git tag "$TAG"
fi
git push origin "$TAG"

SHA256=$(shasum -a 256 "$DMG_PATH" | awk '{print $1}')
echo ""
echo "==> Release created: $TAG"
echo "==> Feed URL:        ${FEED_URL}"
echo "==> Download URL:    ${DOWNLOAD_URL}"
echo "==> SHA256:          $SHA256"
echo "==> EdDSA signed:    ${ED_SIG:0:24}..."
