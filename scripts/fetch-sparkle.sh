#!/usr/bin/env bash
# Sparkle の prebuilt 配布物を .sparkle/ に取得する。
# - Sparkle.framework: CMake がアプリバンドルに埋め込む（CMakeLists.txt が configure 時に本スクリプトを自動実行）
# - bin/generate_keys, bin/sign_update: EdDSA 鍵生成・リリース時の dmg 署名に使う
#
# バージョンは pin する。上げるときは PolePole (~/ide) 側の実績バージョンも参考にすること。
set -euo pipefail

SPARKLE_VERSION="2.9.1"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DEST_DIR="$PROJECT_ROOT/.sparkle"
STAMP_FILE="$DEST_DIR/.version"
URL="https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"

if [ -f "$STAMP_FILE" ] && [ "$(cat "$STAMP_FILE")" = "$SPARKLE_VERSION" ] \
   && [ -d "$DEST_DIR/Sparkle.framework" ]; then
  echo "Sparkle ${SPARKLE_VERSION} は取得済み ($DEST_DIR)"
  exit 0
fi

echo "==> Sparkle ${SPARKLE_VERSION} をダウンロード中..."
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
curl -fsSL "$URL" -o "$TMP_DIR/sparkle.tar.xz"
tar -xJf "$TMP_DIR/sparkle.tar.xz" -C "$TMP_DIR"

if [ ! -d "$TMP_DIR/Sparkle.framework" ]; then
  echo "ERROR: 展開結果に Sparkle.framework が見つかりません" >&2
  exit 1
fi

rm -rf "$DEST_DIR" 2>/dev/null || true
mkdir -p "$DEST_DIR"
# ditto で symlink 構造 (Versions/A → Current 等) を保持してコピーする
ditto "$TMP_DIR/Sparkle.framework" "$DEST_DIR/Sparkle.framework"
ditto "$TMP_DIR/bin" "$DEST_DIR/bin"
echo "$SPARKLE_VERSION" > "$STAMP_FILE"
echo "==> 完了: $DEST_DIR (framework + bin/generate_keys, sign_update)"
