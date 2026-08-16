#!/usr/bin/env bash
# FXバッチ3（Sat / Lo-fi）確認用テストプロジェクトのseeder（冪等・再実行可）。
# plan: docs/plans/2026-08-16-2058-fx-batch3-saturation-lofi.md の動作確認用
#
#   scripts/seed-fx-test.sh          → ~/Music/daw/0-0-fx-test を生成（既存なら上書き）
#
# トラック構成（確認項目との対応）:
#   1 Drums (モノ)   : キック明確なブーンバップ・Lo-fi軽めプリセット
#                      → Lo-fiの質感・Wow最大時の打点遅れ・ノイズ追従の耳確認
#   2 Bass  (モノ)   : サブベース・FX中立 → Wowの打点遅れとキックの噛み合わせ確認
#   3 Keys  (ステレオ): 明るい倍音のコードスタブ・Satプリセット(Drive 35%)
#                      → Satの質感・倍音バー表示・エイリアス聴感
#   4 Pad   (ステレオ): 持続コード・FX中立 → Driveを上げて「音量ほぼ一定」のA/B確認
#
# 素材は決定的に合成（固定seed）。BPM 90 / 48kHz / 8小節（約21秒）
set -euo pipefail
cd "$(dirname "$0")/.."

DEST="$HOME/Music/daw/0-0-fx-test"
mkdir -p "$DEST"

python3 - "$DEST" <<'PYEOF'
import json
import math
import random
import struct
import sys
import wave

dest = sys.argv[1]
SR = 48000
BPM = 90.0
BEAT = int(SR * 60 / BPM)   # 32000
BAR = BEAT * 4
BARS = 8
TOTAL = BAR * BARS
rng = random.Random(20260816)  # 決定的（再実行で同じ素材）


def write_wav(name, channels):
    """channels: [list[float]] 1ch=モノ/2ch=ステレオ。ピークを-4.4dBFS(0.6)へ正規化して16bitで書く"""
    peak = max(max(abs(v) for v in ch) for ch in channels) or 1.0
    scale = 0.6 / peak
    with wave.open(f"{dest}/{name}", "wb") as w:
        w.setnchannels(len(channels))
        w.setsampwidth(2)
        w.setframerate(SR)
        frames = bytearray()
        for i in range(TOTAL):
            for ch in channels:
                v = max(-1.0, min(1.0, ch[i] * scale))
                frames += struct.pack("<h", int(v * 32767))
        w.writeframes(bytes(frames))


def env_exp(i, decay_samples):
    return math.exp(-i / decay_samples)


# ---- Drums: キック(1拍目・2.5拍目)・スネア(2・4拍目)・ハット(8分) の2小節パターン ----
drums = [0.0] * TOTAL

def add_kick(start):
    for i in range(int(0.35 * SR)):
        if start + i >= TOTAL:
            break
        f = 95.0 * math.exp(-i / (0.028 * SR)) + 48.0  # ピッチスイープ 143→48Hz
        phase = 2 * math.pi * f * i / SR
        drums[start + i] += math.sin(phase) * env_exp(i, 0.11 * SR)

def add_snare(start):
    for i in range(int(0.16 * SR)):
        if start + i >= TOTAL:
            break
        tone = math.sin(2 * math.pi * 185.0 * i / SR) * 0.5
        noise = rng.uniform(-1, 1) * 0.9
        drums[start + i] += (tone + noise) * env_exp(i, 0.045 * SR) * 0.8

def add_hat(start, open_hat=False):
    dec = (0.09 if open_hat else 0.03) * SR
    prev = 0.0
    for i in range(int(0.12 * SR)):
        if start + i >= TOTAL:
            break
        n = rng.uniform(-1, 1)
        hp = n - prev  # 1次差分で高域寄りに（ハットらしく）
        prev = n
        drums[start + i] += hp * env_exp(i, dec) * 0.35

for bar in range(BARS):
    base = bar * BAR
    add_kick(base)
    add_kick(base + int(2.5 * BEAT))
    add_snare(base + BEAT)
    add_snare(base + 3 * BEAT)
    for eighth in range(8):
        add_hat(base + eighth * BEAT // 2, open_hat=(eighth == 7))

write_wav("clip-001.wav", [drums])

# ---- Bass: A1中心の2小節パターン（キックを避けた配置。基音+2倍音で存在感）----
bass = [0.0] * TOTAL
BASS_PATTERN = [(0.0, 55.0, 0.9), (1.5, 55.0, 0.4), (2.5, 65.41, 0.9), (3.5, 49.0, 0.4)]  # (拍, Hz, 長さ拍)

for bar in range(BARS):
    root = [55.0, 55.0, 43.65, 49.0][bar % 4]  # A1 A1 F1 G1 進行
    for beat_pos, freq, length in BASS_PATTERN:
        f = freq * root / 55.0
        start = bar * BAR + int(beat_pos * BEAT)
        n = int(length * BEAT)
        for i in range(n):
            if start + i >= TOTAL:
                break
            attack = min(1.0, i / (0.008 * SR))
            release = min(1.0, (n - i) / (0.03 * SR))
            phase = 2 * math.pi * f * i / SR
            v = math.sin(phase) + 0.25 * math.sin(2 * phase)
            bass[start + i] += v * attack * release * 0.8

write_wav("clip-002.wav", [bass])

# ---- Keys: Am7系スタブ（倍音豊富なノコギリ風・L/R微デチューンのステレオ）----
def saw_partials(phase_base, harmonics=10):
    v = 0.0
    for h in range(1, harmonics + 1):
        v += math.sin(phase_base * h) / h
    return v

CHORDS = [
    [220.0, 261.63, 329.63, 392.0],   # Am7
    [220.0, 261.63, 329.63, 392.0],
    [174.61, 220.0, 261.63, 349.23],  # Fmaj7
    [196.0, 246.94, 293.66, 392.0],   # G
]
keys_l = [0.0] * TOTAL
keys_r = [0.0] * TOTAL
STABS = [1.5, 3.5]  # 拍位置（裏打ち）

for bar in range(BARS):
    chord = CHORDS[bar % 4]
    for stab in STABS:
        start = bar * BAR + int(stab * BEAT)
        n = int(0.45 * BEAT)
        for i in range(n):
            if start + i >= TOTAL:
                break
            e = env_exp(i, 0.09 * SR) * min(1.0, i / (0.003 * SR))
            for f in chord:
                keys_l[start + i] += saw_partials(2 * math.pi * f * 1.002 * i / SR) * e * 0.25
                keys_r[start + i] += saw_partials(2 * math.pi * f * 0.998 * i / SR) * e * 0.25

write_wav("clip-003.wav", [keys_l, keys_r])

# ---- Pad: 持続コード（ゆっくりしたアタック・小節単位で鳴りっぱなし）----
pad_l = [0.0] * TOTAL
pad_r = [0.0] * TOTAL
for bar in range(BARS):
    chord = CHORDS[bar % 4]
    start = bar * BAR
    for i in range(BAR):
        if start + i >= TOTAL:
            break
        attack = min(1.0, i / (0.4 * SR))
        release = min(1.0, (BAR - i) / (0.15 * SR))
        e = attack * release
        for f in chord:
            base = 2 * math.pi * (f / 2) * i / SR  # 1オクターブ下で厚みを出す
            pad_l[start + i] += (math.sin(base * 1.003) + 0.4 * math.sin(2 * base * 1.003)
                                 + 0.2 * math.sin(3 * base * 1.003)) * e * 0.22
            pad_r[start + i] += (math.sin(base * 0.997) + 0.4 * math.sin(2 * base * 0.997)
                                 + 0.2 * math.sin(3 * base * 0.997)) * e * 0.22

write_wav("clip-004.wav", [pad_l, pad_r])

# ---- project.json (v18) ----
def track(tid, name, clip, volume, pan, sat=None, lofi=None):
    fx = {"eq": {"enabled": True}}
    if sat is not None:
        fx["sat"] = sat
    if lofi is not None:
        fx["lofi"] = lofi
    return {
        "id": tid, "type": "audio", "name": name,
        "mute": False, "solo": False, "volume": volume, "pan": pan,
        "sends": [0.0, 0.0, 0.0], "fx": fx,
        "clips": [{"file": clip, "startSample": 0, "offsetSamples": 0,
                   "lengthSamples": TOTAL, "muted": False}],
    }

project = {
    "version": 18,
    "nextId": 5,
    "bpm": BPM,
    "sampleRate": float(SR),
    "memo": "FXバッチ3確認用（seeder: scripts/seed-fx-test.sh）\n"
            "Drums=Lo-fi軽めプリセット / Keys=Sat 35%プリセット / Bass・Padは中立",
    "tracks": [
        # Lo-fi軽め: Wow浅め・ノイズ・Crush少し（「レコードに寄るか」の出発点）
        track(1, "Drums", "clip-001.wav", 0.8, 0.0,
              lofi={"enabled": True, "wow": 0.25, "tone": 0.2, "noise": 0.35, "crush": 0.3}),
        track(2, "Bass", "clip-002.wav", 0.75, 0.0),
        # Sat軽め: 倍音バー・質感の出発点（Driveを上げ下げして音量一定を確認）
        track(3, "Keys", "clip-003.wav", 0.7, 0.15,
              sat={"enabled": True, "drive": 0.35, "mix": 1.0}),
        track(4, "Pad", "clip-004.wav", 0.6, -0.1),
    ],
    "markers": [],
    "cycle": {"start": 0, "end": BAR * 4, "enabled": False},
    "fadeOut": {"start": 0, "end": 0},
    "buses": [{"gain": 1.0, "mute": False}] * 3,
    "master": {"gain": 0.9},
}

with open(f"{dest}/project.json", "w") as f:
    json.dump(project, f, ensure_ascii=False, indent=2)

print(f"seeded: {dest} (BPM {BPM} / {SR}Hz / {BARS}小節 / 4トラック)")
PYEOF
