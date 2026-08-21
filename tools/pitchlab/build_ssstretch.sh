#!/usr/bin/env bash
# signalsmith-stretch CLI（ssstretch）をビルドする。ソースは LaLa 本体が FetchContent で pin した
# build/_deps/ のものを使う（本体と同じバージョンで聴き比べるため）。未取得なら mise run build を先に
set -euo pipefail
cd "$(dirname "$0")"
DEPS="../../build/_deps"
[ -f "$DEPS/signalsmith-stretch-src/signalsmith-stretch.h" ] || { echo "build/_deps に signalsmith が無い。先に mise run build"; exit 1; }
mkdir -p work
c++ -std=c++17 -O3 -DNDEBUG \
  -framework Accelerate -DSIGNALSMITH_USE_ACCELERATE \
  -I "$DEPS/signalsmith-stretch-src/include" -I "$DEPS/signalsmith-linear-src/include" -I "$DEPS/signalsmith-stretch-src/cmd" \
  ssstretch.cpp -o work/ssstretch
echo "built: $PWD/work/ssstretch"
