#!/usr/bin/env bash
# 配布用 Salva.app をビルド → Developer ID で inside-out 再署名 → notarize → staple →
# build-release/Salva.dmg に出力する。scripts/build.sh（LaLa）のSalva版。
#
# 前提は build.sh と同じ（Developer ID 証明書・notarytool profile "nyshk97-notary"・create-dmg）。
# 注: 環境によっては notarytool が Claude Code の Bash から keychain に届かないことがある。
#     リリースはユーザーの Terminal で実行するのが確実（通常は release-salva.sh 経由）。
#
# --skip-notarize: 署名検証まで実行して終了する（notarize / DMG 生成をスキップ）
set -euo pipefail

SKIP_NOTARIZE=0
for arg in "$@"; do
  case "$arg" in
    --skip-notarize) SKIP_NOTARIZE=1 ;;
    *) echo "不明な引数: ${arg}（使えるのは --skip-notarize のみ）" >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build-release"
EXPORT_DIR="/tmp/salva-export"
APP="$EXPORT_DIR/Salva.app"
DMG_PATH="$BUILD_DIR/Salva.dmg"
ENTITLEMENTS="$PROJECT_ROOT/Resources/daw.entitlements"  # audio-input のみ。LaLaと共用でよい
NOTARY_PROFILE="${NOTARY_PROFILE:-nyshk97-notary}"

cd "$PROJECT_ROOT"

echo "==> Building (Release, clean)..."
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target salva --clean-first

BUILT_APP="$BUILD_DIR/apps/salva/salva_artefacts/Release/Salva.app"
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

SIGNING_IDENTITY=$(security find-identity -v -p codesigning | awk -F'"' '/Developer ID Application.*VYDUR99LAM/ {print $2; exit}')
if [ -z "$SIGNING_IDENTITY" ]; then
  echo "ERROR: Developer ID Application (Team VYDUR99LAM) 証明書がキーチェーンに見つかりません"
  exit 1
fi

echo "==> Re-signing inside-out with Developer ID (hardened runtime)..."
SPARKLE_FW="$APP/Contents/Frameworks/Sparkle.framework"
SPARKLE_B="$SPARKLE_FW/Versions/$(readlink "$SPARKLE_FW/Versions/Current")"
if [ ! -d "$SPARKLE_B" ]; then
  echo "ERROR: Sparkle.framework の Versions/Current を解決できません ($SPARKLE_B)"
  exit 1
fi
codesign --force --options runtime --timestamp --preserve-metadata=entitlements \
  --sign "$SIGNING_IDENTITY" "$SPARKLE_B/XPCServices/Downloader.xpc"
codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$SPARKLE_B/XPCServices/Installer.xpc"
codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$SPARKLE_B/Autoupdate"
codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$SPARKLE_B/Updater.app"
codesign --force --options runtime --timestamp --sign "$SIGNING_IDENTITY" "$APP/Contents/Frameworks/Sparkle.framework"
codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" \
  --sign "$SIGNING_IDENTITY" "$APP"

echo "==> Verifying signature..."
codesign --verify --strict --deep "$APP"
codesign -dvv "$APP" 2>&1 | grep -E 'Authority|TeamIdentifier|flags'
ENT=$(codesign -d --entitlements - "$APP" 2>/dev/null || true)
if echo "$ENT" | grep -q "get-task-allow"; then
  echo "ERROR: entitlements に get-task-allow が残っています（配布物には禁止）"
  exit 1
fi

if [ "$SKIP_NOTARIZE" = 1 ]; then
  echo "==> --skip-notarize: 署名検証まで完了（notarize / DMG はスキップ）"
  echo "    app: $APP"
  exit 0
fi

echo "==> Notarizing app (profile: $NOTARY_PROFILE)..."
NOTARIZE_ZIP="/tmp/salva-notarize.zip"
rm -f "$NOTARIZE_ZIP"
ditto -c -k --keepParent "$APP" "$NOTARIZE_ZIP"
xcrun notarytool submit "$NOTARIZE_ZIP" --keychain-profile "$NOTARY_PROFILE" --wait

echo "==> Stapling app..."
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"

echo "==> Packaging into DMG..."
rm -f "$DMG_PATH"
DMG_SRC_DIR="/tmp/salva-dmg-src"
rm -rf "$DMG_SRC_DIR"
mkdir -p "$DMG_SRC_DIR"
ditto "$APP" "$DMG_SRC_DIR/Salva.app"

create-dmg \
  --volname "Salva" \
  --window-size 600 400 \
  --icon-size 100 \
  --icon "Salva.app" 150 200 \
  --app-drop-link 450 200 \
  --hide-extension "Salva.app" \
  --codesign "$SIGNING_IDENTITY" \
  --notarize "$NOTARY_PROFILE" \
  "$DMG_PATH" \
  "$DMG_SRC_DIR/"

echo "==> Done: $DMG_PATH"
shasum -a 256 "$DMG_PATH"
