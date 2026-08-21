#!/usr/bin/env bash
# build-salva.sh で作った dmg を配信 repo (nyshk97/salva-releases) の GitHub Release に上げ、
# appcast.xml を生成する。scripts/release.sh（LaLa）のSalva版。
# LaLaの配信（daw-releases）とは完全分離: タグは salva-vX.Y.Z、feedはsalva-releases固定。
#
# 使い方: scripts/release-salva.sh <version>
#   例: scripts/release-salva.sh 0.1.0
#
# 注: Claude Code のセッションから叩いてよい。notarytool の資格情報は画面ロック中だけ読めないので、
#     preflight でロック中なら止める。CHANGELOG の [Unreleased] は叩く前にセッションが埋めて commit する
#     （Claude Code の Bash からは動かない）。
#
# 前提:
#   - apps/salva/CMakeLists.txt の set(SALVA_VERSION x.y.z) を <version> に bump してコミット済み
#   - Keychain に Sparkle EdDSA 秘密鍵（アカウント名 "daw"。LaLaと共用）が登録済み
#   - `gh` で nyshk97/salva-releases に push 権限があること
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DMG_PATH="$PROJECT_ROOT/build-release/Salva.dmg"
APPCAST_PATH="$PROJECT_ROOT/build-release/appcast-salva.xml"
RELEASES_REPO="nyshk97/salva-releases"
FEED_URL="https://github.com/${RELEASES_REPO}/releases/latest/download/appcast.xml"
SIGN_UPDATE="$PROJECT_ROOT/.sparkle/bin/sign_update"
SPARKLE_ACCOUNT="daw"

if [ $# -eq 0 ]; then
  echo "Usage: $0 <version>"
  echo "Example: $0 0.1.0"
  exit 1
fi

VERSION="$1"
TAG="salva-v$VERSION"
CHANGELOG="$PROJECT_ROOT/docs/CHANGELOG-salva.md"
RELEASE_NOTES_MD="$PROJECT_ROOT/build-release/release-notes-salva-${VERSION}.md"
SPARKLE_DESC_HTML="$PROJECT_ROOT/build-release/sparkle-description-salva-${VERSION}.html"
mkdir -p "$PROJECT_ROOT/build-release"
cd "$PROJECT_ROOT"

# === Step 0: preflight (リポジトリを書き換える前に環境と Git 状態を検証する) ===
if ! gh auth status >/dev/null 2>&1; then
  echo "ERROR: gh が未認証です。gh auth login を実行してください"
  exit 1
fi
echo "==> preflight: fetching tags from origin..."
git fetch origin --tags --quiet
BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [ "$BRANCH" != "main" ]; then
  echo "ERROR: カレントブランチが main ではありません ($BRANCH)"
  exit 1
fi
DIRTY=$(git status --porcelain | grep -v '^?? docs/plans/' || true)
if [ -n "$DIRTY" ]; then
  echo "ERROR: 未コミットの変更または未追跡ファイルがあります。commit / stash してから再実行してください"
  echo "$DIRTY"
  exit 1
fi
# origin/main より遅れていないこと（別クローンからリリース済みの古い main で二重リリースしない）。
# mise の bump commit で HEAD が 1 つ先行するのは正常なので「origin/main が HEAD の祖先」で判定する
if ! git merge-base --is-ancestor origin/main HEAD; then
  echo "ERROR: HEAD が origin/main を含んでいません（pull 忘れ）。origin: $(git rev-parse --short origin/main) / local: $(git rev-parse --short HEAD)"
  exit 1
fi
# 画面ロック中は notarytool の資格情報（data-protection keychain）が読めない。ビルドに数分かけてから落ちないよう先に見る
CONSOLE_LOCKED=$(ioreg -n Root -d1 -a 2>/dev/null | plutil -extract IOConsoleLocked raw -o - - 2>/dev/null || true)
if [ "$CONSOLE_LOCKED" = "true" ]; then
  echo "ERROR: 画面がロックされています。解除してから再実行してください"
  exit 1
fi
NOTARY_PROFILE="${NOTARY_PROFILE:-nyshk97-notary}"
if ! notary_out=$(xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" 2>&1); then
  echo "ERROR: notarize の keychain プロファイル '${NOTARY_PROFILE}' が使えません:"
  echo "$notary_out" | head -3
  exit 1
fi
# SALVA_VERSION の bump し忘れをビルド前に検知する（ビルド後にも Info.plist で再検査）
CMAKE_VERSION=$(sed -nE 's/^set\(SALVA_VERSION ([0-9.]+)\)/\1/p' "$PROJECT_ROOT/apps/salva/CMakeLists.txt")
if [ "$CMAKE_VERSION" != "$VERSION" ]; then
  echo "ERROR: apps/salva/CMakeLists.txt の SALVA_VERSION ($CMAKE_VERSION) != 指定バージョン ($VERSION)"
  echo "       set(SALVA_VERSION $VERSION) に bump してコミットしてから再実行してください"
  exit 1
fi
if git rev-parse "$TAG" >/dev/null 2>&1; then
  if [ "$(git rev-parse "$TAG"^{commit})" != "$(git rev-parse HEAD)" ]; then
    echo "ERROR: タグ $TAG が既に存在し、HEAD と別の commit を指しています"
    echo "       バージョン番号を変えるか、タグを整理してから再実行してください"
    exit 1
  fi
  echo "==> preflight: タグ $TAG は既に HEAD を指しています（再実行とみなして続行）"
fi
if ! gh repo view "$RELEASES_REPO" >/dev/null 2>&1; then
  echo "ERROR: 配信 repo $RELEASES_REPO が見つかりません"
  echo "       gh repo create $RELEASES_REPO --public --add-readme で作成してください"
  exit 1
fi
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

# === Step 1: changelog の確認 ===
if [ ! -f "$CHANGELOG" ]; then
  echo "ERROR: $CHANGELOG が見つかりません"
  exit 1
fi
if ! grep -q "^## \[Unreleased\]" "$CHANGELOG"; then
  echo "ERROR: docs/CHANGELOG-salva.md に '## [Unreleased]' セクションがありません"
  exit 1
fi

LAST_VERSION=$(awk '/^## \[Unreleased\]/{f=1; next} f && /^## \[([^]]+)\]/{match($0, /\[([^]]+)\]/); print substr($0, RSTART+1, RLENGTH-2); exit}' "$CHANGELOG")
RANGE=""
if [ -n "$LAST_VERSION" ] && git rev-parse "salva-v${LAST_VERSION}" >/dev/null 2>&1; then
  RANGE="salva-v${LAST_VERSION}..HEAD"
  echo ""
  echo "==> 前回リリース salva-v${LAST_VERSION} 以降の commit:"
  git log "$RANGE" --max-count=100 --pretty=format:"  %h %s"
  echo ""
else
  echo ""
  echo "==> 直近 30 commit (CHANGELOG から前回リリースタグを特定できず):"
  git log -30 --pretty=format:"  %h %s"
  echo ""
fi

# [Unreleased] は叩く前に Claude Code のセッションが埋めて commit しておく（対話も claude -p の
# 入れ子も無い）。空なら Step 2 で止まる。上の commit 一覧は参考表示。
UNRELEASED_BODY=$(awk '/^## \[Unreleased\]/{f=1; next} /^## \[/{f=0} f' "$CHANGELOG" | grep -v '^[[:space:]]*$' || true)
if [ -z "$UNRELEASED_BODY" ]; then
  echo ""
  echo "ERROR: $CHANGELOG の [Unreleased] が空です。上の commit を読んで埋め、commit してから再実行してください"
  echo "       （書き方は CHANGELOG 冒頭。Salva（apps/salva/）に関するユーザー目視の変更だけ／無ければ '- 内部的な変更のみ'）"
  exit 1
fi

# === Step 2: [Unreleased] → [<version>] - <today> 書き換え + commit ===
TODAY=$(date +%Y-%m-%d)
python3 - "$CHANGELOG" "$VERSION" "$TODAY" <<'PY'
import sys, re, pathlib
path = pathlib.Path(sys.argv[1])
version, date = sys.argv[2], sys.argv[3]
text = path.read_text()

unreleased = re.search(r"^## \[Unreleased\]\s*$(.*?)(?=^## \[|\Z)", text, flags=re.M | re.S)
if not unreleased:
    sys.exit("ERROR: [Unreleased] セクションが見つかりません")
has_content = bool(unreleased.group(1).strip())
already_renamed = re.search(rf"^## \[{re.escape(version)}\]", text, flags=re.M)

if already_renamed and not has_content:
    print(f"  CHANGELOG-salva.md: [{version}] は既に存在し [Unreleased] は空 — リネーム済みとみなしてスキップ (再実行)")
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
print(f"  CHANGELOG-salva.md: [Unreleased] の下に [{version}] - {date} を挿入")
PY

CHANGELOG_COMMITTED=0
if ! git diff --quiet "$CHANGELOG"; then
  git add "$CHANGELOG"
  git commit -m "docs(changelog-salva): release ${VERSION}"
  echo "  CHANGELOG-salva.md を commit (release ${VERSION})"
  CHANGELOG_COMMITTED=1
fi
rollback_changelog_commit() {
  if [ "$CHANGELOG_COMMITTED" -eq 1 ]; then
    echo "↩️  失敗したので CHANGELOG の commit を巻き戻します（remote は未変更）"
    git reset -q --hard HEAD~1
  fi
}
trap 'rollback_changelog_commit' ERR

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

notes = f"# Salva {version}\n\n{body}\n"
notes_path.write_text(notes)
print(f"  Wrote {notes_path}")

def inline(text):
    text = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", r'<a href="\2">\1</a>', text)
    return text

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
"$SCRIPT_DIR/build-salva.sh"

# === Step 4: ビルド成果物の整合チェック ===
BUILT_APP="/tmp/salva-export/Salva.app"
BUNDLE_VERSION=$(plutil -extract CFBundleVersion raw "${BUILT_APP}/Contents/Info.plist")
SHORT_VERSION=$(plutil -extract CFBundleShortVersionString raw "${BUILT_APP}/Contents/Info.plist")
if [ "$SHORT_VERSION" != "$VERSION" ]; then
  echo "ERROR: built CFBundleShortVersionString ($SHORT_VERSION) != requested <version> ($VERSION)"
  exit 1
fi
if [ "$BUNDLE_VERSION" != "$VERSION" ]; then
  echo "ERROR: built CFBundleVersion ($BUNDLE_VERSION) != requested <version> ($VERSION)"
  exit 1
fi
APP_FEED=$(plutil -extract SUFeedURL raw "${BUILT_APP}/Contents/Info.plist")
if [ "$APP_FEED" != "$FEED_URL" ]; then
  echo "ERROR: built SUFeedURL ($APP_FEED) != release-salva.sh FEED_URL ($FEED_URL)"
  echo "       apps/salva/CMakeLists.txt の SUFeedURL と release-salva.sh の RELEASES_REPO がずれています"
  exit 1
fi

# === Step 5: push（タグは Release 作成成功後に打つ） ===
echo "==> Pushing main to origin..."
git push origin main
CHANGELOG_COMMITTED=0
trap - ERR

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
PUB_DATE=$(LC_ALL=C date -u "+%a, %d %b %Y %H:%M:%S +0000")
DOWNLOAD_URL="https://github.com/${RELEASES_REPO}/releases/download/${TAG}/Salva.dmg"
OTOOL_OUT=$(otool -l "${BUILT_APP}/Contents/MacOS/Salva")
MIN_OS=$(echo "$OTOOL_OUT" | awk '/minos/{print $2; exit}')
if [ -z "$MIN_OS" ]; then
  echo "ERROR: バイナリの minos を otool から取得できません（出力形式が変わった可能性）"
  exit 1
fi

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
    <title>Salva</title>
    <link>${FEED_URL}</link>
    <description>Most recent Salva updates</description>
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
# asset名はappcast.xml固定にする必要がある（feedの latest/download/appcast.xml は
# asset名=実ファイル名で解決される。gh の `#ラベル` 記法は表示名だけでasset名を変えない）。
# ローカルの appcast-salva.xml はLaLaの build-release/appcast.xml と衝突しないための名前
# なので、一時ディレクトリに appcast.xml として置いてからアップロードする
echo "==> Creating release on ${RELEASES_REPO} (Sparkle feed + dmg)..."
APPCAST_UPLOAD_DIR="$(mktemp -d)"
cp "$APPCAST_PATH" "$APPCAST_UPLOAD_DIR/appcast.xml"
gh release create "$TAG" \
  "$DMG_PATH" \
  "$APPCAST_UPLOAD_DIR/appcast.xml" \
  --repo "${RELEASES_REPO}" \
  --title "$TAG" \
  --notes-file "$RELEASE_NOTES_MD"
rm -rf "$APPCAST_UPLOAD_DIR"

# === Step 9: 本体 repo にタグ（DMG を作った commit を記録） ===
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
