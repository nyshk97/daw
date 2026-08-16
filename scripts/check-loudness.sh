#!/usr/bin/env bash
# LUFS/トゥルーピーク計測の ffmpeg ebur128 照合（docs/plans/2026-08-16-1523-fx-batch2-meters-limiter.md Phase 3）
#
# LaLa の計測（Loudness::measureFile = バウンス完了表示・RTメーターと同じ計算部品）と
# ffmpeg の ebur128 フィルタで同じWAVを測り、integrated LUFS / TP を突き合わせる。
#
# 使い方:
#   scripts/check-loudness.sh [wavファイル...]
# 引数なしなら合成テスト信号（ピンクノイズ＋正弦の2本）を生成して照合する。
#
# 許容差（実測から決定・推測しない）:
#   integrated: ±0.15 LU（サブブロック境界の丸め・実装差）
#   TP:         ±0.5 dB（オーバーサンプリングのフィルタ実装差。ffmpegは別カーネル）
#
# 注意: クリック列のような**単発インパルス系の素材はTPがこれ以上割れることがある**
# （帯域制限補間のオーバーシュートがフィルタ長に依存し、厳密な正解が存在しない領域。
#  ffmpegはswresampleの長いカーネルでBS.1770のリファレンス48タップより高めに読む。
#  LaLaは96タップ＝リファレンスより長め。実測: クリック列でΔTP=0.56dB・音楽的素材
#  （正弦・ピンクノイズ）ではΔTP≤0.04dB）。NGが出たら素材の性質を先に疑う
set -euo pipefail
cd "$(dirname "$0")/.."

command -v ffmpeg >/dev/null || { echo "ffmpeg が必要です（brew の Brewfile 経由で導入）" >&2; exit 2; }

cmake --build build --target daw_tests >/dev/null
bin="build/daw_tests_artefacts/Debug/daw_tests"
[ -x "$bin" ] || { echo "daw_tests が見つかりません: $bin" >&2; exit 2; }

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

files=("$@")
if [ ${#files[@]} -eq 0 ]; then
  # 合成テスト信号（決定的）: ①997Hz正弦 -20dBFS ②ピンクノイズ（seed固定）
  ffmpeg -v error -f lavfi -i "sine=frequency=997:duration=15:sample_rate=48000" \
    -af "volume=0.1,pan=stereo|c0=c0|c1=c0" -c:a pcm_s24le "$tmpdir/sine.wav"
  ffmpeg -v error -f lavfi -i "anoisesrc=colour=pink:duration=15:seed=7:amplitude=0.25:sample_rate=48000" \
    -af "pan=stereo|c0=c0|c1=c0" -c:a pcm_s24le "$tmpdir/pink.wav"
  files=("$tmpdir/sine.wav" "$tmpdir/pink.wav")
fi

fail=0
for wav in "${files[@]}"; do
  ours="$("$bin" --measure-loudness "$wav")"
  our_i="$(echo "$ours" | grep -oE 'integrated=-?[0-9.]+' | cut -d= -f2)"
  our_tp="$(echo "$ours" | grep -oE 'truePeakDb=-?[0-9.]+' | cut -d= -f2)"

  # ffmpeg ebur128 のサマリー（stderrに出る）から Integrated と True peak を抽出
  ff_out="$(ffmpeg -nostats -i "$wav" -filter_complex "ebur128=peak=true" -f null - 2>&1 | tail -30)"
  ff_i="$(echo "$ff_out" | grep -A1 'Integrated loudness' | grep -oE 'I:\s+-?[0-9.]+' | grep -oE '\-?[0-9.]+' | head -1)"
  ff_tp="$(echo "$ff_out" | grep -A2 'True peak' | grep -oE 'Peak:\s+-?[0-9.]+' | grep -oE '\-?[0-9.]+' | head -1)"

  di="$(python3 -c "print(abs($our_i - ($ff_i)))")"
  dtp="$(python3 -c "print(abs($our_tp - ($ff_tp)))")"
  ok_i="$(python3 -c "print(1 if $di <= 0.15 else 0)")"
  ok_tp="$(python3 -c "print(1 if $dtp <= 0.5 else 0)")"

  echo "$(basename "$wav"): LaLa I=$our_i TP=$our_tp / ffmpeg I=$ff_i TP=$ff_tp (ΔI=$di ΔTP=$dtp)"
  if [ "$ok_i" != 1 ] || [ "$ok_tp" != 1 ]; then
    echo "  ==> NG: 許容差（I±0.15 / TP±0.5）を超えています" >&2
    fail=1
  fi
done

[ "$fail" = 0 ] && echo "==> 照合OK（全ファイル許容差内）"
exit "$fail"
