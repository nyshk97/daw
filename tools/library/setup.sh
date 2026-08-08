#!/usr/bin/env bash
# サンプルライブラリの置き場を作る冪等セットアップ（複数マシン用）。
#
# 実体は iCloud Drive（マシン間同期）、LaLa と CLI からは ~/Music/daw/library の
# symlink 経由で見る。設計は docs/plans/2026-08-07-2319-loop-track.md と
# docs/design/reference-beat.md「音色の調達」節。
set -euo pipefail

ICLOUD="$HOME/Library/Mobile Documents/com~apple~CloudDocs/daw-library"
LINK="$HOME/Music/daw/library"

mkdir -p "$ICLOUD/loops" "$ICLOUD/oneshots" "$ICLOUD/_contrast/loops" "$ICLOUD/_contrast/oneshots"
mkdir -p "$HOME/Music/daw"

if [ -L "$LINK" ]; then
  current=$(readlink "$LINK")
  if [ "$current" != "$ICLOUD" ]; then
    ln -sfn "$ICLOUD" "$LINK"
    echo "symlink を張り替えました: $current -> $ICLOUD"
  fi
elif [ -e "$LINK" ]; then
  # 実ディレクトリを勝手に消して symlink にするのは破壊的なので手に委ねる
  echo "ERROR: $LINK が symlink 以外で存在します。中身を確認して退避してから再実行してください" >&2
  exit 1
else
  ln -s "$ICLOUD" "$LINK"
fi

echo "OK: $LINK -> $ICLOUD"
echo "残り手作業: Finder で daw-library を右クリック →「ダウンロードを保持」を指定する"
echo "（iCloud の「Macストレージを最適化」がローカル実体を退避すると、試聴が DL 待ちで止まるため）"
