#!/usr/bin/env bash
# リファレンス1曲ぶんの分析を通しで走らせる入口。
# 本体は analyze.py（依存グラフに沿った並列オーケストレータ）。LaLa もここを呼ぶ。
#
#   ./analyze.sh <リファレンスフォルダ>
#
# 前提: フォルダに track.wav（MVエディットをトリム済みの本編）が置いてあること。
#   yt-dlp -f bestaudio -x --audio-format wav -o 'source.%(ext)s' --write-info-json '<URL>'
#   ./overview.py source.wav analysis/overview.png     # ← ここで頭と尻を目視
#   ffmpeg -i source.wav -ss <開始> -to <終了> track.wav
# トリム位置の判断だけは自動化しない（MVエディットの入り方は曲ごとに違う）。
set -euo pipefail
cd "$(dirname "$0")"
exec .venv/bin/python analyze.py "${1:?使い方: ./analyze.sh <リファレンスフォルダ>}"
