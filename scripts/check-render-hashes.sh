#!/usr/bin/env bash
# レンダリング回帰ハッシュの採取・比較（docs/plans/2026-08-16-1254-fx-batch1-foundation.md Phase 0）
#
# daw_tests が stdout へ出す "hash-*: <fnv1a>" 行（testMonoRenderRegressionHash /
# testTrackFxRegressionHash）を Debug / Release 両構成で採取し、ベースラインと比較する。
# ハッシュの値はコンパイラ・環境依存（浮動小数点の積和順序）なので、同一マシンでの
# 「リファクタ前後の比較」専用。CIや別マシンの基準としては使えない。
#
# 使い方:
#   scripts/check-render-hashes.sh capture [baseline-dir]   # 変更前に基準を採取
#   scripts/check-render-hashes.sh compare [baseline-dir]   # 変更後に比較（差分があれば非0終了）
# baseline-dir 省略時は .render-hash-baseline/（gitignore済み。build/ に置くと
# クリーンビルドで基準ごと消えるため別に置く）
set -euo pipefail
cd "$(dirname "$0")/.."

mode="${1:-}"
baseline_dir="${2:-.render-hash-baseline}"
if [ "$mode" != capture ] && [ "$mode" != compare ]; then
  echo "usage: $0 capture|compare [baseline-dir]" >&2
  exit 2
fi

# テスト行の名前と順序（TestsMain.cpp の実行順と対）。増減したらここも更新する
expected_names="hash-engine hash-bounce hash-fx-engine hash-fx-bounce hash-fx-project-bounce"

run_tests() { # $1 = debug|release。ハッシュ行を stdout へ、ビルドログ等は stderr へ
  local config="$1" build_dir cmake_type bin out
  if [ "$config" = debug ]; then
    build_dir=build cmake_type=Debug
  else
    build_dir=build-release cmake_type=Release
  fi
  bin="$build_dir/daw_tests_artefacts/$cmake_type/daw_tests"
  [ -f "$build_dir/CMakeCache.txt" ] || cmake -B "$build_dir" -DCMAKE_BUILD_TYPE="$cmake_type" >&2
  cmake --build "$build_dir" --target daw_tests >&2

  # テストの失敗を隠さない: 出力を一旦ファイルへ保存し、終了コードを確認してから
  # ハッシュ行を抽出する（`daw_tests | grep` の直結は grep の 0 が失敗を上書きする）
  out="$(mktemp)"
  if ! "$bin" >"$out" 2>&1; then
    echo "ERROR($config): daw_tests が失敗しました。出力末尾:" >&2
    tail -20 "$out" >&2
    rm -f "$out"
    return 1
  fi
  if ! grep '^hash-' "$out"; then
    echo "ERROR($config): ハッシュ行が出力にありません" >&2
    rm -f "$out"
    return 1
  fi
  rm -f "$out"
}

check_names() { # $1 = 採取ファイル。行数と名前の完全一致（経路の出力漏れを検知）
  local names
  names="$(sed -E 's/: .*//' "$1" | tr '\n' ' ' | sed 's/ $//')"
  if [ "$names" != "$expected_names" ]; then
    echo "ERROR: ハッシュ行の名前・順序が想定と一致しません" >&2
    echo "  expected: $expected_names" >&2
    echo "  actual:   $names" >&2
    return 1
  fi
}

mkdir -p "$baseline_dir"
status=0
for config in debug release; do
  current="$(mktemp)"
  run_tests "$config" >"$current"
  check_names "$current"
  baseline="$baseline_dir/$config.txt"
  if [ "$mode" = capture ]; then
    cp "$current" "$baseline"
    echo "==> $config: ベースラインを保存しました → $baseline"
    cat "$baseline"
  else
    if [ ! -f "$baseline" ]; then
      echo "ERROR: ベースラインがありません: $baseline（先に capture を実行してください）" >&2
      rm -f "$current"
      exit 2
    fi
    if diff -u "$baseline" "$current"; then
      echo "==> $config: ビット一致（全$(wc -l <"$baseline" | tr -d ' ')ハッシュ）"
    else
      echo "==> $config: ハッシュが変化しています（上の diff 参照）" >&2
      status=1
    fi
  fi
  rm -f "$current"
done
exit $status
