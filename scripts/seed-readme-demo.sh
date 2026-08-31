#!/usr/bin/env bash
# READMEスクリーンショット用デモプロジェクトのseeder（冪等・再実行可）。
#
#   scripts/seed-readme-demo.sh          → ~/Music/daw/0-0-readme-demo を生成（既存なら上書き）
#
# 撮影手順（dev版の検証フックで背面・無音のまま撮れる）:
#   open -g build/daw_artefacts/Debug/LaLa-dev.app --args --open ~/Music/daw/0-0-readme-demo
#   → AppleScriptで set size of window 1 to {1600, 1000} → screencapture -x -o -l <windowID>
#
# 見た目の狙い（README向けに「使い込まれたアレンジ」に見せる）:
#   - セクションマーカー: intro(1) / verse(3) / hook(11) / verse(19)
#     （イントロ2小節の前倒し構成 = デフォルトズームでもhookとサイクル帯が画面に入る）
#   - サイクル帯: verse区間（bar 3-11）を黄色で表示（再生がverse頭から始まり、
#     追従スクロール前に「曲頭の構成＋再生中＋メーター点灯」を1枚に収められる）
#   - ループリージョン（Drums/808: 4小節クリップ×リピート）・フェード・クリップ名・
#     ミュートリージョン・MIDIリージョン（コード+メロディのミニチュア）を混ぜる
#   - 波形は chill hiphop 想定（BPM 90）で、フレーズ状の抑揚を持たせて録り音らしく見せる
#
# 素材は決定的に合成（固定seed）。BPM 90 / 48kHz / 22小節（約59秒）
set -euo pipefail
cd "$(dirname "$0")/.."

DEST="$HOME/Music/daw/0-0-readme-demo"
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
BAR = BEAT * 4              # 128000
PPQ = 960                   # MIDIの1拍
rng = random.Random(20260831)  # 決定的（再実行で同じ素材）


def bar_start(bar):
    """1始まりの小節番号 → サンプル位置"""
    return (bar - 1) * BAR


def write_wav(name, channels, peak_target=0.6):
    peak = max(max(abs(v) for v in ch) for ch in channels) or 1.0
    scale = peak_target / peak
    with wave.open(f"{dest}/{name}", "wb") as w:
        w.setnchannels(len(channels))
        w.setsampwidth(2)
        w.setframerate(SR)
        frames = bytearray()
        for i in range(len(channels[0])):
            for ch in channels:
                v = max(-1.0, min(1.0, ch[i] * scale))
                frames += struct.pack("<h", int(v * 32767))
        w.writeframes(bytes(frames))


def env_exp(i, decay_samples):
    return math.exp(-i / decay_samples)


def saw_partials(phase_base, harmonics=8):
    v = 0.0
    for h in range(1, harmonics + 1):
        v += math.sin(phase_base * h) / h
    return v


# コード進行（Am7 - Fmaj7 - Cmaj7 - G。chill定番の6-4-1-5）
CHORD_ROOTS = [57, 53, 48, 55]                    # MIDIノート（A2 F2 C2 G2）
CHORD_TONES = [[0, 3, 7, 10], [0, 4, 7, 11], [0, 4, 7, 11], [0, 4, 7, 10]]


def midi_hz(note):
    return 440.0 * 2 ** ((note - 69) / 12)


# ---- clip-001: Drums 4小節ループ（ブーンバップ。2小節パターン×2） ----
n = BAR * 4
drums = [0.0] * n

def add_kick(buf, start):
    for i in range(int(0.3 * SR)):
        if start + i >= len(buf):
            break
        f = 90.0 * math.exp(-i / (0.03 * SR)) + 45.0
        buf[start + i] += math.sin(2 * math.pi * f * i / SR) * env_exp(i, 0.1 * SR)

def add_snare(buf, start):
    for i in range(int(0.18 * SR)):
        if start + i >= len(buf):
            break
        tone = math.sin(2 * math.pi * 190.0 * i / SR) * 0.4
        buf[start + i] += (tone + rng.uniform(-1, 1)) * env_exp(i, 0.05 * SR) * 0.75

def add_hat(buf, start, open_hat=False, level=0.3):
    dec = (0.1 if open_hat else 0.028) * SR
    prev = 0.0
    for i in range(int(0.13 * SR)):
        if start + i >= len(buf):
            break
        v = rng.uniform(-1, 1)
        buf[start + i] += (v - prev) * env_exp(i, dec) * level
        prev = v

for bar in range(4):
    base = bar * BAR
    add_kick(drums, base)
    add_kick(drums, base + int((2.5 if bar % 2 == 0 else 2.75) * BEAT))
    add_snare(drums, base + BEAT)
    add_snare(drums, base + 3 * BEAT)
    if bar % 2 == 1:  # ゴーストスネア
        add_snare(drums, base + int(3.75 * BEAT))
    for eighth in range(8):
        add_hat(drums, base + eighth * BEAT // 2,
                open_hat=(bar % 2 == 1 and eighth == 7),
                level=0.32 if eighth % 2 == 0 else 0.22)

write_wav("clip-001.wav", [drums])

# ---- clip-002: 808 Sub 4小節ループ（コードのルートを追う） ----
n = BAR * 4
sub = [0.0] * n
SUB_HITS = [(0.0, 1.4), (2.5, 1.0)]  # (拍, 長さ拍)
for bar in range(4):
    root = midi_hz(CHORD_ROOTS[bar] - 12)  # 1オクターブ下
    for beat_pos, length in SUB_HITS:
        start = bar * BAR + int(beat_pos * BEAT)
        hit = int(length * BEAT)
        for i in range(hit):
            if start + i >= n:
                break
            glide = 1.0 + 0.06 * math.exp(-i / (0.02 * SR))  # 打点のピッチグライド
            attack = min(1.0, i / (0.006 * SR))
            release = min(1.0, (hit - i) / (0.05 * SR))
            v = math.sin(2 * math.pi * root * glide * i / SR)
            v += 0.15 * math.sin(2 * math.pi * root * 2 * glide * i / SR)
            sub[start + i] += v * attack * release

write_wav("clip-002.wav", [sub])

# ---- clip-003/004: Rhodes風コードチョップ 2小節×2種（ステレオ） ----
def render_chop(chord_index_pair, fname):
    n = BAR * 2
    l = [0.0] * n
    r = [0.0] * n
    STABS = [0.0, 1.5, 3.0]  # 表拍+スウィング裏
    for bar, ci in enumerate(chord_index_pair):
        notes = [CHORD_ROOTS[ci] + 12 + t for t in CHORD_TONES[ci]]
        for stab in STABS:
            start = bar * BAR + int(stab * BEAT)
            hit = int(1.1 * BEAT)
            for i in range(hit):
                if start + i >= n:
                    break
                e = env_exp(i, 0.22 * SR) * min(1.0, i / (0.004 * SR))
                trem = 1.0 + 0.12 * math.sin(2 * math.pi * 5.5 * i / SR)  # Rhodesのトレモロ
                for note in notes:
                    f = midi_hz(note)
                    ph = 2 * math.pi * f * i / SR
                    v = (math.sin(ph) + 0.35 * math.sin(2 * ph) + 0.1 * math.sin(4 * ph)) * e * trem
                    l[start + i] += v * 0.25 * (1.0 + 0.15 * math.sin(2 * math.pi * 0.6 * i / SR))
                    r[start + i] += v * 0.25 * (1.0 - 0.15 * math.sin(2 * math.pi * 0.6 * i / SR))
    write_wav(fname, [l, r])

render_chop([0, 1], "clip-003.wav")  # Am7 → Fmaj7
render_chop([2, 3], "clip-004.wav")  # Cmaj7 → G

# ---- clip-005: Pad 8小節（ゆっくり膨らむ持続和音・ステレオ） ----
n = BAR * 8
pad_l = [0.0] * n
pad_r = [0.0] * n
for bar in range(8):
    ci = bar % 4 if bar % 4 < 2 else bar % 4  # 進行そのまま
    notes = [CHORD_ROOTS[bar % 4] + t for t in CHORD_TONES[bar % 4]]
    start = bar * BAR
    for i in range(BAR):
        attack = min(1.0, i / (0.5 * SR))
        release = min(1.0, (BAR - i) / (0.2 * SR))
        e = attack * release
        for note in notes:
            base = 2 * math.pi * midi_hz(note - 12) * i / SR
            pad_l[start + i] += (math.sin(base * 1.004) + 0.3 * math.sin(2 * base * 1.004)) * e * 0.2
            pad_r[start + i] += (math.sin(base * 0.996) + 0.3 * math.sin(2 * base * 0.996)) * e * 0.2

write_wav("clip-005.wav", [pad_l, pad_r])

# ---- clip-006: Vocal 8小節（フレーズ状の抑揚を持つ帯域ノイズ+基音。録り音らしい波形） ----
n = BAR * 8
voc = [0.0] * n
phrase_t = 0
while phrase_t < n - BEAT:
    length = int(rng.uniform(1.2, 3.2) * BEAT)          # フレーズ長 1.2〜3.2拍
    gap = int(rng.uniform(0.3, 1.4) * BEAT)             # ブレスの間
    f0 = rng.uniform(150, 240)
    syllables = max(2, int(length / (0.35 * BEAT)))
    for s in range(syllables):
        s_start = phrase_t + s * length // syllables
        s_len = int(length / syllables * rng.uniform(0.7, 0.95))
        s_amp = rng.uniform(0.5, 1.0)
        prev = 0.0
        for i in range(s_len):
            if s_start + i >= n:
                break
            attack = min(1.0, i / (0.01 * SR))
            release = min(1.0, (s_len - i) / (0.06 * SR))
            vib = 1.0 + 0.015 * math.sin(2 * math.pi * 5.0 * i / SR)
            tone = math.sin(2 * math.pi * f0 * vib * i / SR) * 0.6
            tone += math.sin(2 * math.pi * f0 * 2 * vib * i / SR) * 0.25
            nz = rng.uniform(-1, 1)
            breath = (nz - prev) * 0.35  # 高域寄りの息成分
            prev = nz
            voc[s_start + i] += (tone + breath) * attack * release * s_amp
    phrase_t += length + gap

write_wav("clip-006.wav", [voc], peak_target=0.7)

# ---- clip-007: Lead 8小節（Aマイナーペンタの単音・hook用） ----
n = BAR * 8
lead = [0.0] * n
PENTA = [69, 72, 74, 76, 79, 81]  # A4 C5 D5 E5 G5 A5
for bar in range(8):
    pos = 0.0
    while pos < 3.5:
        note = PENTA[rng.randrange(len(PENTA))]
        length = rng.choice([0.5, 0.5, 1.0, 1.5])
        start = bar * BAR + int(pos * BEAT)
        hit = int(length * 0.9 * BEAT)
        for i in range(hit):
            if start + i >= n:
                break
            e = env_exp(i, 0.25 * SR) * min(1.0, i / (0.005 * SR))
            lead[start + i] += saw_partials(2 * math.pi * midi_hz(note) * i / SR, 6) * e * 0.5
        pos += length

write_wav("clip-007.wav", [lead])


# ---- project.json ----
def audio_clip(file, start_bar, name="", loop=0, fade_in=0, fade_out=0, muted=False, bars=None):
    length = (bars if bars is not None else {"clip-001.wav": 4, "clip-002.wav": 4,
                                             "clip-003.wav": 2, "clip-004.wav": 2,
                                             "clip-005.wav": 8, "clip-006.wav": 8,
                                             "clip-007.wav": 8}[file]) * BAR
    c = {"file": file, "startSample": bar_start(start_bar), "offsetSamples": 0,
         "lengthSamples": length, "muted": muted}
    if name:
        c["name"] = name
    if loop:
        c["loopCount"] = loop
    if fade_in:
        c["fadeInSamples"] = fade_in
    if fade_out:
        c["fadeOutSamples"] = fade_out
    return c


def track(tid, ttype, name, volume, pan, clips=None, regions=None, sends=None, gm=0):
    t = {"id": tid, "type": ttype, "name": name,
         "mute": False, "solo": False, "volume": volume, "pan": pan,
         "sends": sends or [0.0, 0.0, 0.0], "fx": {"eq": {"enabled": True}}}
    if ttype == "audio":
        t["clips"] = clips or []
    else:
        t["gmProgram"] = gm
        t["drums"] = False
        t["regions"] = regions or []
    return t


# MIDI Keys: intro(1-4)とverse2(21-24)で鳴るコード+上声メロ
def keys_region(rid, start_bar):
    notes = []
    nid = rid * 100
    for bar in range(4):
        base_ppq = bar * 4 * PPQ
        ci = bar % 4
        for t in CHORD_TONES[ci]:
            notes.append({"id": nid, "pitch": CHORD_ROOTS[ci] + 12 + t,
                          "startPpq": base_ppq, "lengthPpq": int(3.5 * PPQ), "velocity": 78})
            nid += 1
        melody = [76, 79, 81, 79]
        notes.append({"id": nid, "pitch": melody[ci],
                      "startPpq": base_ppq + 2 * PPQ, "lengthPpq": int(1.5 * PPQ), "velocity": 92})
        nid += 1
    return {"id": rid, "startPpq": (start_bar - 1) * 4 * PPQ, "lengthPpq": 16 * PPQ, "notes": notes}


project = {
    "version": 21,
    "nextId": 400,
    "bpm": BPM,
    "sampleRate": float(SR),
    "memo": "READMEスクリーンショット用（seeder: scripts/seed-readme-demo.sh）",
    "tracks": [
        track(1, "audio", "Drums", 0.82, 0.0, clips=[
            audio_clip("clip-001.wav", 3, loop=1),                       # verse: 4小節×2
            audio_clip("clip-001.wav", 11, loop=1),                      # hook
            audio_clip("clip-001.wav", 19, muted=True),                  # verse2: ミュートで抜く
        ], sends=[0.0, 0.0, 0.0]),
        track(2, "audio", "808", 0.78, 0.0, clips=[
            audio_clip("clip-002.wav", 3, loop=1),
            audio_clip("clip-002.wav", 11, loop=1),
            audio_clip("clip-002.wav", 19),
        ]),
        track(3, "audio", "Rhodes Chop", 0.7, -0.15, clips=[
            audio_clip("clip-003.wav", 3, name="chop_Am7"),
            audio_clip("clip-004.wav", 5, name="chop_Cmaj7"),
            audio_clip("clip-003.wav", 7, name="chop_Am7"),
            audio_clip("clip-004.wav", 9, name="chop_Cmaj7"),
            audio_clip("clip-003.wav", 11, name="chop_Am7", loop=1),
            audio_clip("clip-004.wav", 15, name="chop_Cmaj7", loop=1),
        ], sends=[0.25, 0.0, 0.0]),
        track(4, "midi", "Keys", 0.68, 0.2, regions=[
            keys_region(300, 1),
            keys_region(310, 19),
        ]),
        track(5, "audio", "Pad", 0.58, -0.1, clips=[
            audio_clip("clip-005.wav", 1, fade_in=BAR),
            audio_clip("clip-005.wav", 11),
            audio_clip("clip-005.wav", 19, bars=4, fade_out=BAR // 2),
        ], sends=[0.0, 0.35, 0.0]),
        track(6, "audio", "Vocal", 0.85, 0.0, clips=[
            audio_clip("clip-006.wav", 3, name="take3", fade_in=2400),
            audio_clip("clip-006.wav", 11, name="hook_comp", fade_out=9600),
        ], sends=[0.18, 0.0, 0.12]),
        track(7, "audio", "Lead", 0.62, 0.25, clips=[
            audio_clip("clip-007.wav", 11, fade_out=BAR // 2),
        ], sends=[0.0, 0.2, 0.3]),
    ],
    "markers": [
        {"bar": 1, "type": "intro"},
        {"bar": 3, "type": "verse"},
        {"bar": 11, "type": "hook"},
        {"bar": 19, "type": "verse"},
    ],
    "cycle": {"start": 2 * 16, "end": 10 * 16, "enabled": True},  # verse区間（16分音符単位）
    "fadeOut": {"start": 0, "end": 0},
    "buses": [
        {"gain": 1.0, "mute": False,
         "reverb": {"size": 0.4, "damp": 0.5, "width": 1.0, "predelay": 20.0, "lowcut": 120.0}},
        {"gain": 1.0, "mute": False,
         "reverb": {"size": 0.8, "damp": 0.35, "width": 1.0, "predelay": 45.0, "lowcut": 100.0}},
        {"gain": 1.0, "mute": False,
         "delay": {"time": 2, "feedback": 0.4, "tone": 0.6, "pingpong": True}},
    ],
    "master": {"gain": 0.9},
}

with open(f"{dest}/project.json", "w") as f:
    json.dump(project, f, ensure_ascii=False, indent=2)

print(f"seeded: {dest} (BPM {BPM} / {SR}Hz / 22小節 / 7トラック)")
PYEOF
