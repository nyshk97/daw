#!/usr/bin/env bash
# ノイズ除去ラボの venv を作り直す。Python は .mise.toml の [tools] の 3.12 を使う
set -euo pipefail
cd "$(dirname "$0")"

mise install python
mise exec -- python -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/pip install -r requirements.txt

.venv/bin/python - <<'PY'
import numpy as np, soundfile as sf, tempfile, os, subprocess, sys
print(f"numpy {np.__version__} / soundfile {sf.__version__}")
# 合成WAV（-60dBFSの白色ノイズ＋後半に-6dBFSのサイン）で measure.py の煙試験
rng = np.random.default_rng(0)
sr = 48000
noise = rng.standard_normal(sr * 8) * (10 ** (-60 / 20))
sig = noise.copy()
t = np.arange(sr * 2) / sr
sig[sr * 6 : sr * 8] += 0.5 * np.sin(2 * np.pi * 220 * t)
wav = os.path.join(tempfile.mkdtemp(), "smoke.wav")
sf.write(wav, sig, sr, subtype="PCM_24")
out = subprocess.run([sys.executable, "measure.py", wav], capture_output=True, text=True, check=True).stdout
assert "ノイズフロア" in out, out
print("measure.py: 煙試験 ok")
PY
