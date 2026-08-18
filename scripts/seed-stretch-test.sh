#!/usr/bin/env bash
# 移調・タイムストレッチ確認用テストプロジェクトのseeder（冪等・再実行可）。
# plan: docs/plans/2026-08-18-1028-audio-transpose-stretch.md の動作確認用
#
#   scripts/seed-stretch-test.sh     → ~/Music/daw/0-0-stretch-test を生成（既存なら上書き）
#
# 中身: 440Hz 2秒のトーン1本を +2半音 / 1.25倍（v20の要求値）で敷いたプロジェクト。
#   - 開いた直後は原音素通しで始まり、読込時の一括再生成が完了すると見かけ長が
#     0.75小節 → 0.9375小節（90BPM）へ一斉に伸びる（+2バッジも出る）
#   - 右クリック →「移調・伸縮…」の吹き出し・分割・保存→再読込の同一音（固定シード）の
#     実機確認の起点に使う。音量は通常値（0.8）— 音を出さずに確認したいときはフェーダーを
#     -60dB へ下げる（メーターには信号が出続けるので疎通は見える）
#
# 検証フックでの開き方（VERIFY.md「移調・タイムストレッチ」参照）:
#   open -g build/daw_artefacts/Debug/LaLa-dev.app --args \
#     --open ~/Music/daw/0-0-stretch-test --snapshot /tmp/stretch-check.png
set -euo pipefail
cd "$(dirname "$0")/.."

DEST="$HOME/Music/daw/0-0-stretch-test"
mkdir -p "$DEST"

python3 - "$DEST" <<'PYEOF'
import json
import math
import struct
import sys

dest = sys.argv[1]
SR = 48000
N = SR * 2  # 2秒 440Hz（24bit モノ。録音クリップと同じ形式）

frames = bytearray()
for i in range(N):
    v = int(0.4 * math.sin(2 * math.pi * 440 * i / SR) * 8388607)
    frames += struct.pack("<i", v)[0:3]
header = (b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVEfmt "
          + struct.pack("<IHHIIHH", 16, 1, 1, SR, SR * 3, 3, 24)
          + b"data" + struct.pack("<I", len(frames)))
with open(f"{dest}/clip-001.wav", "wb") as f:
    f.write(header + bytes(frames))

project = {
    "version": 20, "nextId": 10, "bpm": 90.0, "sampleRate": float(SR), "memo": "",
    "tracks": [{
        "id": 1, "type": "audio", "name": "Loop",
        "mute": False, "solo": False, "volume": 0.8, "pan": 0.0,
        "sends": [0, 0, 0],
        "clips": [{
            "file": "clip-001.wav", "name": "stretch-test",
            "startSample": 0, "offsetSamples": 0, "lengthSamples": N,
            "muted": False, "loopCount": 1,
            "transposeSemitones": 2, "stretchRatio": 1.25,
        }],
    }],
    "markers": [],
    "cycle": {"start": 0, "end": 0, "enabled": False},
    "fadeOut": {"start": 0, "end": 0},
}
with open(f"{dest}/project.json", "w") as f:
    json.dump(project, f, ensure_ascii=False)
print(f"seeded: {dest}")
PYEOF
