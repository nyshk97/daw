#!/usr/bin/env bash
# pitchlab の venv を作り直す。Python は .mise.toml の [tools] の 3.12 を使う。
# signalsmith-stretch の CLI（ssstretch）も同時にビルドする（LaLa 本体と同じ pin 済みソースを使う）
set -euo pipefail
cd "$(dirname "$0")"

mise install python
mise exec -- python -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/pip install -r requirements.txt

./build_ssstretch.sh

.venv/bin/python - <<'PY'
import numpy, soundfile, librosa, torchcrepe, pyworld, matplotlib
print("numpy", numpy.__version__, "/ librosa", librosa.__version__, "/ pyworld", pyworld.__version__)
print("pitchlab: 依存 ok")
PY
echo "rubberband CLI: $(command -v rubberband || echo '無し（brew 経由で Brewfile に rubberband を追加）')"
