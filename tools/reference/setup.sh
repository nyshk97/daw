#!/usr/bin/env bash
# リファレンス分析パイプラインの venv を作り直す。
# Python は .mise.toml の [tools] で 3.12 に固定（homebrew の 3.14 では torch の wheel が無い）。
set -euo pipefail
cd "$(dirname "$0")"

mise install python
mise exec -- python -m venv .venv
.venv/bin/python -m pip install --upgrade pip
# resampy は basic-pitch の pin を意図的に上書きするので後段で入れ直す（requirements.txt のコメント参照）
.venv/bin/pip install -r requirements.txt

.venv/bin/python - <<'PY'
import warnings, logging
warnings.filterwarnings("ignore"); logging.disable(logging.WARNING)
import torch, torchaudio, librosa, demucs.separate
from basic_pitch.inference import predict
print(f"torch {torch.__version__} (mps={torch.backends.mps.is_available()})")
print(f"torchaudio {torchaudio.__version__} / librosa {librosa.__version__}")
print("demucs / basic-pitch: import ok")
PY
