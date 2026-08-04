#!/usr/bin/env bash
# リファレンス分析パイプラインの venv を作り直す。
# Python は .mise.toml の [tools] で 3.12 に固定（homebrew の 3.14 では torch の wheel が無い）。
set -euo pipefail
cd "$(dirname "$0")"

mise install python
mise exec -- python -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/pip install -r requirements.txt
# basic-pitch は resampy pin の衝突で requirements.txt に同居できない（詳細はそちらのコメント参照）。
# 依存は requirements.txt に列挙済みなので --no-deps で本体だけ入れる
.venv/bin/pip install --no-deps basic-pitch==0.3.0

.venv/bin/python - <<'PY'
import warnings, logging
warnings.filterwarnings("ignore"); logging.disable(logging.WARNING)
import torch, torchaudio, librosa, demucs.separate
from basic_pitch.inference import predict
print(f"torch {torch.__version__} (mps={torch.backends.mps.is_available()})")
print(f"torchaudio {torchaudio.__version__} / librosa {librosa.__version__}")
print("demucs / basic-pitch: import ok")

# basic-pitch は --no-deps 導入なので、import だけでなく実推論でバックエンド欠落を検出する
# （topline.py と同じ ONNX モデル・scipy shim で呼ぶ）
import numpy as np, soundfile as sf, tempfile, os
import scipy.signal
if not hasattr(scipy.signal, "gaussian"):
    scipy.signal.gaussian = scipy.signal.windows.gaussian
from basic_pitch import FilenameSuffix, build_icassp_2022_model_path
t = np.linspace(0, 1, 22050, endpoint=False)
wav = os.path.join(tempfile.mkdtemp(), "a440.wav")
sf.write(wav, 0.5 * np.sin(2 * np.pi * 440 * t), 22050)
_, midi, _ = predict(wav, build_icassp_2022_model_path(FilenameSuffix.onnx))
notes = midi.instruments[0].notes if midi.instruments else []
assert any(abs(n.pitch - 69) <= 1 for n in notes), f"A4 が検出されない: {notes}"
print(f"basic-pitch: 実推論 ok（A440 → {len(notes)} notes）")
PY
