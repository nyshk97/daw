#!/bin/bash
# Salva ステム分離スクリプト（appバンドルのResourcesに同梱。契約はplan「ステム分離の契約」）
#
#   separate.sh <入力ファイル> <出力identityディレクトリ> <python絶対パス> <sampleRate> <lengthSamples>
#
# sampleRate / lengthSamples はアプリ（JUCE=CoreAudio）が読んだ元音源の値。
# スクリプト側で元音源をデコードしない（libsndfileはm4a/AACを読めないため、
# ここでsf.infoすると2モデルの分離を待たせた最後に失敗する）
#
# 前提: 呼び出し側（アプリ）が <出力dir>/lock/ をmkdirで取得済み。
# 責務: ①起動直後に自PID/PGIDをlockへatomic記録 ②不変な runs/<uuid>/ に成果物を生成
#       ③各ステムを元音源と同SR・同長・ステレオ・24bit WAVへ揃える
#       ④一時マニフェスト→manifest.jsonへの1回のrenameで公開
# 中身は暫定demucs（htdemucs＋htdemucs_6s）。モデル選定はstem-model-labの別planで行い、
# 勝者が決まったらこのスクリプトを差し替える（アプリはこの契約だけを知る）
set -euo pipefail

input="$1"
outdir="$2"
python="$3"
sample_rate="$4"
length_samples="$5"

# ① workerのPID/PGIDをatomic記録（一時ファイル→rename。アプリが死んでもworkerツリーの
#    生存判定ができるように、appのspawn後追記でなくスクリプト自身が最初に行う）
pgid="$(ps -o pgid= -p $$ | tr -d ' ')"
worker_tmp="$outdir/lock/worker.json.tmp.$$"
printf '{"pid": %s, "pgid": %s}\n' "$$" "$pgid" > "$worker_tmp"
mv "$worker_tmp" "$outdir/lock/worker.json"

# ② 不変なrunディレクトリ
runid="$("$python" -c 'import uuid; print(uuid.uuid4())')"
rundir="$outdir/runs/$runid"
work="$rundir/work"
mkdir -p "$work"

# 段階マーカー（アプリの進捗表示用・契約の一部）: 各段階の直前に stderr へ
# "salva-stage: <4stems|6stems|export>" を出す。demucs の tqdm 進捗も stderr なので、
# 同じストリームに流すことで到着順が崩れない（Source/shared/SeparationProgress.h が読む）
stage() { echo "salva-stage: $1" >&2; }

# 分離（--float32: 中間を16bitに落とさない。24bit化は正規化ステップで行う）
stage 4stems
"$python" -m demucs.separate -n htdemucs    --float32 -o "$work" "$input"
stage 6stems
"$python" -m demucs.separate -n htdemucs_6s --float32 -o "$work" "$input"

# ③④ 正規化＋マニフェスト公開
stage export
"$python" - "$input" "$outdir" "$rundir" "$runid" "$sample_rate" "$length_samples" <<'PYEOF'
import json
import os
import shutil
import sys

import numpy as np
import soundfile as sf

inp, outdir, rundir, runid = sys.argv[1:5]
sr = int(sys.argv[5])
length = int(sys.argv[6])

st = os.stat(inp)
track = os.path.splitext(os.path.basename(inp))[0]


def resample(data, from_sr, to_sr):
    if from_sr == to_sr:
        return data
    try:
        import soxr
        return soxr.resample(data, from_sr, to_sr)
    except ImportError:
        import librosa
        return librosa.resample(data.T, orig_sr=from_sr, target_sr=to_sr).T


groups = [
    ("htdemucs", "4 STEMS", ["drums", "bass", "other", "vocals"]),
    ("htdemucs_6s", "6 STEMS", ["drums", "bass", "other", "vocals", "guitar", "piano"]),
]
manifest_groups = []
for model, display, stems in groups:
    entries = []
    for stem in stems:
        src = os.path.join(rundir, "work", model, track, stem + ".wav")
        data, model_sr = sf.read(src, always_2d=True)
        data = resample(data, model_sr, sr)
        if data.ndim == 1:
            data = data[:, None]
        if data.shape[1] == 1:
            data = np.repeat(data, 2, axis=1)
        # 端数の切り揃え: 元音源と同じサンプル数へ（短ければ無音でパディング）
        if len(data) < length:
            data = np.vstack([data, np.zeros((length - len(data), data.shape[1]))])
        data = data[:length, :2]
        out_rel = os.path.join("runs", runid, model, stem + ".wav")
        out_abs = os.path.join(outdir, out_rel)
        os.makedirs(os.path.dirname(out_abs), exist_ok=True)
        sf.write(out_abs, data, sr, subtype="PCM_24")
        entries.append({"name": stem, "file": out_rel})
    manifest_groups.append({"id": model, "name": display, "stems": entries})

shutil.rmtree(os.path.join(rundir, "work"))

manifest = {
    "contractVersion": 1,
    "model": "demucs htdemucs + htdemucs_6s",
    "source": {
        "path": inp,  # アプリが絶対パスで渡す契約（JUCE側の表記と一致させるためrealpathしない）
        "size": st.st_size,
        "mtimeMs": st.st_mtime_ns // 1_000_000,  # JUCEの tv_sec*1000 + tv_nsec/1e6 と同じ切り捨て
    },
    "sampleRate": sr,
    "lengthSamples": length,
    "status": "complete",
    "run": "runs/" + runid,
    "groups": manifest_groups,
}
tmp = os.path.join(outdir, "manifest.json.tmp")
with open(tmp, "w") as f:
    json.dump(manifest, f, ensure_ascii=False, indent=1)
os.replace(tmp, os.path.join(outdir, "manifest.json"))
print("published runs/" + runid)
PYEOF
