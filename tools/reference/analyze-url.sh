#!/usr/bin/env bash
# URL から丸ごと。取得 → トリム → analyze.sh、をひと続きで走らせる入口。
# 分析本体は analyze.sh にあり（LaLa から呼ぶのも Phase 3 ではそちら）、ここは URL 版の皮。
#
#   ./analyze-url.sh '<URL>'                        取得 → トリム自動判定 → 分析まで通し
#   ./analyze-url.sh '<URL>' <名前>                 フォルダ名を指定する（省略時は曲名から自動）
#   ./analyze-url.sh '<URL>' --trim 12.5-201.0      自動判定が外れたとき手で指定
#   ./analyze-url.sh '<URL>' --dir <親フォルダ>      置き場を変える（既定 ~/Music/daw/references）
#
# source.wav が既にあればダウンロードを飛ばすので、--trim を変えて撃ち直すのは安い。
set -euo pipefail
cd "$(dirname "$0")"
PY=.venv/bin/python

URL=""; NAME=""; TRIM=""; DIR="$HOME/Music/daw/references"
while [ $# -gt 0 ]; do
  case "$1" in
    --trim) TRIM="$2"; shift 2 ;;
    --dir)  DIR="$2";  shift 2 ;;
    -*)     echo "不明なオプション: $1" >&2; exit 1 ;;
    *)      if [ -z "$URL" ]; then URL="$1"; else NAME="$1"; fi; shift ;;
  esac
done
[ -n "$URL" ] || { sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

# --- フォルダ名: 指定が無ければ曲名から作る ---
if [ -z "$NAME" ]; then
  TITLE=$(yt-dlp --skip-download --no-playlist --print "%(title)s" "$URL")
  NAME=$($PY - "$TITLE" <<'PY'
import re, sys, unicodedata
t = sys.argv[1]
t = re.sub(r"[(（\[【].*?[)）\]】]", " ", t)       # (Official Music Video) 等を落とす
t = unicodedata.normalize("NFKC", t).lower()
t = re.sub(r"[^0-9a-zぁ-んァ-ヶ一-龥ー]+", "-", t)  # 記号・空白は - に潰す
print(re.sub(r"-{2,}", "-", t).strip("-")[:48] or "reference")
PY
)
  # 全角文字の直前は必ず ${} で括る。macOS の bash 3.2 はマルチバイトを変数名の一部として
  # 食うので、`$TITLE）` が `TITLE<0xef>` という名前になり unbound variable で落ちる
  echo "==> フォルダ名: ${NAME}  （元のタイトル: ${TITLE}）"
fi

REF="$DIR/$NAME"
mkdir -p "$REF/analysis"

# --- 音源 ---
if [ -f "$REF/source.wav" ]; then
  echo "==> source.wav は取得済み。ダウンロードを飛ばす"
else
  echo "==> 音源を取得"
  # 一部のYouTube動画はbestaudioが選ぶWebM/251だけCDNで403になる一方、AAC/140は
  # 同じ動画から正常取得できる。動画削除と誤認せず、1回だけformatを限定してfallbackする。
  if ! yt-dlp -f bestaudio --extract-audio --audio-format wav --audio-quality 0 --no-playlist \
              -o "$REF/source.%(ext)s" --write-info-json "$URL"; then
    echo "==> bestaudio取得失敗。AAC format 140で1回だけ再試行" >&2
    yt-dlp -f 140 --extract-audio --audio-format wav --audio-quality 0 --no-playlist \
           -o "$REF/source.%(ext)s" --write-info-json "$URL"
  fi
fi

# --- 全体像（無音区間の検出も兼ねる） ---
echo "==> 全体像とトリム候補"
$PY overview.py "$REF/source.wav" "$REF/analysis/source-overview.png" --title "$NAME (source)" >/dev/null

# --- トリム位置 ---
# 曲頭側の最後のギャップの終わり 〜 曲尾側の最初のギャップの始まり、を本編とみなす。
# MVエディット（頭のSE・尻の切断）はここに無音の切れ目を作るため。
# 曲中のブレイクを誤って掴まないよう、前後30秒の範囲だけ見る。
if [ -z "$TRIM" ]; then
  TRIM=$($PY - "$REF/analysis/source-overview.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
dur = d["duration_sec"]
gaps = d["near_silence"]
head = [g["end"] for g in gaps if g["start"] < 30]
tail = [g["start"] for g in gaps if g["end"] > dur - 30]
print(f"{max(head) if head else 0:.3f}-{min(tail) if tail else dur:.3f}")
PY
)
  echo "    無音区間: $(jq -c '[.near_silence[] | "\(.start)-\(.end)"]' "$REF/analysis/source-overview.json")"
  echo "    自動判定 → $TRIM  （外れていたら --trim a-b で撃ち直す。ダウンロードは走らない）"
else
  echo "    手動指定 → $TRIM"
fi

echo "==> 本編を切り出し"
ffmpeg -hide_banner -loglevel error -y -i "$REF/source.wav" \
       -ss "${TRIM%-*}" -to "${TRIM#*-}" -c:a pcm_s16le "$REF/track.wav"

./analyze.sh "$REF"
