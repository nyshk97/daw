#!/usr/bin/env bash
# 配布用 LaLa.app をビルド → Developer ID で inside-out 再署名 → notarize → staple →
# build-release/LaLa.dmg に出力する。~/ide (PolePole) の build.sh の CMake 移植版。
#
# CMake の POST_BUILD 署名は TCC 維持用の Apple Development なので、配布用に
# Developer ID + hardened runtime でここで再署名する。順序は Sparkle 公式の
# 手動再署名手順 (https://sparkle-project.org/documentation/sandboxing/) に従い、
# --deep や全体への --entitlements 一括適用はしない（XPC の entitlement を壊すため）。
#
# 前提（一度だけ手作業で用意する。PolePole と共通のものを流用）:
#   1. キーチェーンに "Developer ID Application: ... (VYDUR99LAM)" 証明書がある
#   2. notarytool の認証情報が keychain profile "ide-notary" に保存済み
#   3. create-dmg (Brewfile 経由でインストール済み)
#
# 注: notarytool は Claude Code の Bash からは keychain に届かない。
#     このスクリプトはユーザーの Terminal で実行すること（通常は release.sh 経由）。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build-release"
EXPORT_DIR="/tmp/daw-export"
APP="$EXPORT_DIR/LaLa.app"
DMG_PATH="$BUILD_DIR/LaLa.dmg"
ENTITLEMENTS="$PROJECT_ROOT/Resources/daw.entitlements"
NOTARY_PROFILE="${NOTARY_PROFILE:-ide-notary}"

cd "$PROJECT_ROOT"

echo "==> Building (Release, clean)..."
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target daw --clean-first

BUILT_APP="$BUILD_DIR/daw_artefacts/Release/LaLa.app"
if [ ! -d "$BUILT_APP" ]; then
  echo "ERROR: $BUILT_APP が見つかりません"
  exit 1
fi

# ビルドツリーを汚さないよう /tmp にコピーしてから再署名する。
# ditto は framework 内 symlink を保持する（cp -R や zip は flatten して codesign を壊す）
echo "==> Exporting to $EXPORT_DIR..."
rm -rf "$EXPORT_DIR"
mkdir -p "$EXPORT_DIR"
ditto "$BUILT_APP" "$APP"

# Developer ID Application 証明書を Team ID で絞り込む（keychain に複数あっても誤爆しない）
SIGNING_IDENTITY=$(security find-identity -v -p codesigning | awk -F'"' '/Developer ID Application.*VYDUR99LAM/ {print $2; exit}')
if [ -z "$SIGNING_IDENTITY" ]; then
  echo "ERROR: Developer ID Application (Team VYDUR99LAM) 証明書がキーチェーンに見つかりません"
  exit 1
fi

echo "==> Re-signing inside-out with Developer ID (hardened runtime)..."
# Versions/B を直書きすると将来の Sparkle レイアウト変更（過去に A→B の実績あり）で
# リリース途中に死ぬため、Current symlink から実バージョンを解決する
SPARKLE_FW="$APP/Contents/Frameworks/Sparkle.framework"
SPARKLE_B="$SPARKLE_FW/Versions/$(readlink "$SPARKLE_FW/Versions/Current")"
if [ ! -d "$SPARKLE_B" ]; then
  echo "ERROR: Sparkle.framework の Versions/Current を解決できません ($SPARKLE_B)"
  exit 1
fi
# 1. Downloader.xpc は既存 entitlement（sandbox）を保持して署名する
codesign --force --options runtime --timestamp --preserve-metadata=entitlements \
  --sign "$SIGNING_IDENTITY" "$SPARKLE_B/XPCServices/Downloader.xpc"
# 2. 残りの Sparkle 内部 → framework 本体の順
codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$SPARKLE_B/XPCServices/Installer.xpc"
codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$SPARKLE_B/Autoupdate"
codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$SPARKLE_B/Updater.app"
codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$APP/Contents/Frameworks/Sparkle.framework"
# 3. 最後に本体 .app（entitlements はここだけ。マイク入力に必要）
codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" \
  --sign "$SIGNING_IDENTITY" "$APP"

echo "==> Verifying signature..."
codesign --verify --strict --deep "$APP"
codesign -dvv "$APP" 2>&1 | grep -E 'Authority|TeamIdentifier|flags'
# get-task-allow が残っていると notarize で Invalid になる
ENT=$(codesign -d --entitlements - "$APP" 2>/dev/null || true)
if echo "$ENT" | grep -q "get-task-allow"; then
  echo "ERROR: entitlements に get-task-allow が残っています（配布物には禁止）"
  exit 1
fi

echo "==> Notarizing app (profile: $NOTARY_PROFILE)..."
NOTARIZE_ZIP="/tmp/daw-notarize.zip"
rm -f "$NOTARIZE_ZIP"
ditto -c -k --keepParent "$APP" "$NOTARIZE_ZIP"
xcrun notarytool submit "$NOTARIZE_ZIP" --keychain-profile "$NOTARY_PROFILE" --wait

echo "==> Stapling app..."
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"

echo "==> Packaging into DMG..."
rm -f "$DMG_PATH"
DMG_SRC_DIR="/tmp/daw-dmg-src"
rm -rf "$DMG_SRC_DIR"
mkdir -p "$DMG_SRC_DIR"
ditto "$APP" "$DMG_SRC_DIR/LaLa.app"

# --codesign + --notarize で dmg の codesign → notarytool submit --wait → staple を自動実行
create-dmg \
  --volname "LaLa" \
  --window-size 600 400 \
  --icon-size 100 \
  --icon "LaLa.app" 150 200 \
  --app-drop-link 450 200 \
  --hide-extension "LaLa.app" \
  --codesign "$SIGNING_IDENTITY" \
  --notarize "$NOTARY_PROFILE" \
  "$DMG_PATH" \
  "$DMG_SRC_DIR/"

echo "==> Done: $DMG_PATH"
shasum -a 256 "$DMG_PATH"
