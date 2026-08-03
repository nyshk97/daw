#!/usr/bin/env bash
# リファレンス1曲ぶんの分析を通しで走らせる。
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
PY=.venv/bin/python
REF="${1:?使い方: ./analyze.sh <リファレンスフォルダ>}"
REF="${REF%/}"
A="$REF/analysis"
mkdir -p "$A"

echo "==> 全体像"
$PY overview.py "$REF/track.wav" "$A/overview.png" --title "$(basename "$REF")"

echo "==> ステム分離"
for m in htdemucs htdemucs_6s; do
  [ -d "$REF/stems/$m/track" ] || $PY -m demucs -d mps -n "$m" -o "$REF/stems" "$REF/track.wav"
done

echo "==> BPM・キー・構成"
$PY basics.py "$REF/track.wav" "$A"

echo "==> ステムのインベントリ"
$PY stems.py "$REF/stems/htdemucs/track"    "$REF/track.wav" "$A/basics.json" "$A" --label 4s
$PY stems.py "$REF/stems/htdemucs_6s/track" "$REF/track.wav" "$A/basics.json" "$A" --label 6s

echo "==> 構成マップ"
$PY arrange.py "$A/stems-6s.json" "$A"

echo "==> グルーヴ"
$PY groove.py "$REF/stems/htdemucs/track" "$A/basics.json" "$A"

echo "==> 上モノ"
BASS="$REF/stems/htdemucs/track/bass.wav"
$PY topline.py "$REF/stems/htdemucs/track/other.wav"     "$BASS" "$A/basics.json" "$A" --label other
for s in piano guitar other; do
  $PY topline.py "$REF/stems/htdemucs_6s/track/$s.wav"   "$BASS" "$A/basics.json" "$A" --label "6s-$s"
done

echo "==> 耳で確認するクリップ"
$PY excerpts.py "$REF"

echo
echo "完了: $A"
echo "次: open '$REF/listen' を開いて、番号順にスペースキーで再生（$REF/listen/README.md に聴きどころ）"
