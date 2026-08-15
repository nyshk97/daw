#!/bin/zsh
# Phase 2 比較実行: 3候補 × 3曲 × 2run を同条件で回す。
# 使い方: ./run_all.sh <song-dir-name> <runN>   例: ./run_all.sh rau-def-freeze run1
# 出力: runs/<song>/<candidate>/<runN>/
# 時間・ピークメモリは /usr/bin/time -l で logs/ に記録する。
set -u
cd "$(dirname "$0")"

SONG="$1"
RUN="$2"
REF=~/Music/daw/references/"$SONG"
TRACK="$REF/track.wav"
REFPY=~/daw/tools/reference/.venv/bin/python
LABPY=.venv/bin/python
OUT=runs/"$SONG"
LOGS=logs/"$SONG"
mkdir -p "$OUT" "$LOGS"

[[ -f "$TRACK" ]] || { echo "track.wav なし: $TRACK"; exit 1; }

echo "=== $SONG $RUN ==="

# --- 候補1: demucs baseline (htdemucs + htdemucs_6s, Salvaのseparate.shと同条件: -d mps --float32)
D="$OUT/demucs/$RUN"
if [[ ! -d "$D/htdemucs_6s/track" ]]; then
  mkdir -p "$D"
  /usr/bin/time -l "$REFPY" -m demucs.separate -n htdemucs    -d mps --float32 -o "$D" "$TRACK" > "$LOGS/demucs4-$RUN.log" 2>&1
  /usr/bin/time -l "$REFPY" -m demucs.separate -n htdemucs_6s -d mps --float32 -o "$D" "$TRACK" >> "$LOGS/demucs4-$RUN.log" 2>&1
  echo "demucs done"
else
  echo "demucs skip (既存)"
fi

# --- 候補2: BS-RoFormer SW 単発 (--device mps 明示。autoはMPS未対応でCPUに落ちる)
# 出力は store_dir 直下に track_<stem>.wav のフラット形式（入力と同SR・同長・float）
S="$OUT/sw/$RUN"
if [[ ! -f "$S/track_vocals.wav" ]]; then
  mkdir -p "$S/in"
  ln -sf "$TRACK" "$S/in/track.wav"
  /usr/bin/time -l .venv/bin/bs-roformer-infer --device mps \
    --input_folder "$S/in" --store_dir "$S" > "$LOGS/sw-$RUN.log" 2>&1
  rm -rf "$S/in"
  echo "sw done"
else
  echo "sw skip (既存)"
fi

# --- 候補3: 多段 (vocal特化MelBand RoFormer → instrumentalをdemucs 2本)
M="$OUT/multi/$RUN"
if [[ "${SKIP_MULTI:-0}" == "1" ]]; then
  echo "multi skip (SKIP_MULTI=1)"
elif [[ ! -d "$M/htdemucs_6s/instrumental" ]]; then
  mkdir -p "$M"
  if [[ ! -f "$M/vocals_rf.wav" ]]; then
    /usr/bin/time -l .venv/bin/audio-separator "$TRACK" \
      --model_filename vocals_mel_band_roformer.ckpt \
      --model_file_dir ~/.cache/audio-separator-models \
      --output_dir "$M" --output_format WAV > "$LOGS/multi-vocal-$RUN.log" 2>&1
    # 出力名は "track_(vocals)_<モデル名>.wav" 形式（小文字・モデル名にもvocalsを含むので
    # "(vocals)" のタグ部分で判別する）→ 固定名に揃える
    for f in "$M"/*'(vocals)'*.wav(N); do mv "$f" "$M/vocals_rf.wav"; done
    for f in "$M"/*'(instrumental)'*.wav(N) "$M"/*'(other)'*.wav(N); do mv "$f" "$M/instrumental.wav"; done
  fi
  [[ -f "$M/instrumental.wav" ]] || { echo "多段: instrumental出力が見つからない"; ls "$M"; exit 1; }
  /usr/bin/time -l "$REFPY" -m demucs.separate -n htdemucs    -d mps --float32 -o "$M" "$M/instrumental.wav" > "$LOGS/multi-demucs-$RUN.log" 2>&1
  /usr/bin/time -l "$REFPY" -m demucs.separate -n htdemucs_6s -d mps --float32 -o "$M" "$M/instrumental.wav" >> "$LOGS/multi-demucs-$RUN.log" 2>&1
  echo "multi done"
else
  echo "multi skip (既存)"
fi

echo "=== $SONG $RUN 完了 ==="
