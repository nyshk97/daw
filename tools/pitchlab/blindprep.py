#!/usr/bin/env python3
"""ブラインド試聴セットの生成（Phase 0 の耳判定用）。

- 素材 × ケース（Z/A/B/C/D）ごとに 4 エンジンの出力をシャッフルして W/X/Y/Z と伏せる
  （割当は sha256(素材/ケース) で決まる再現可能なランダム化）
- ソロ: 素材ごとに「元音声を -20LUFS にするゲイン」を全エンジンへ共通適用（相対音量を保存）
- オケ込み: 同じ声ゲイン＋ oke（clip-002 の同じ時間範囲）を -22LUFS にして加算
- 各ケースのフォルダに _ref_original.wav（無加工の元音声。⑤透明性と「何が変わったか」の基準）を置く
- 対応表は docs/labs/reference-beat-human-answers/<日付>-pitchlab-blind-map.json（回答完了まで開かない）

使い方: blindprep.py   （resynth.py を rap-seg / uta-seg で実行済みであること）
出力: listen/<素材>/<ケース>/{solo,mix}_<W|X|Y|Z>.wav
"""
from __future__ import annotations

import hashlib
import json
import random
from pathlib import Path

import numpy as np
import pyloudnorm
import soundfile as sf

from common import WORK, LISTEN, ANSWERS, load_mono

DATE = "2026-08-21"
MATERIALS = {
    # name: (oke wav, crop start, crop end)  ※ crop は analyze.py に渡したものと同じ
    "rap-seg": (Path.home() / "Music/daw/2026-08-18-tundra/clip-002.wav", 4.0, 12.0),
    "uta-seg": (Path.home() / "Music/daw/2026-08-18-tundra/clip-002.wav", 7.5, 15.5),
}
CASES = ["Z", "A", "B", "C", "D"]
ENGINES = ["psola", "world", "signalsmith", "rubberband"]
LABELS = ["W", "X", "Y", "Z"]
VOICE_LUFS, OKE_LUFS = -20.0, -22.0


def lufs(x: np.ndarray, sr: int) -> float:
    return pyloudnorm.Meter(sr).integrated_loudness(x.astype(np.float64))


def main() -> int:
    entries = []
    for mat, (oke_path, c0, c1) in MATERIALS.items():
        src, sr = load_mono(WORK / mat / "source.wav")
        oke, osr = load_mono(oke_path)
        assert osr == sr
        oke = oke[int(c0 * sr): int(c1 * sr)]
        oke = np.pad(oke, (0, max(0, len(src) - len(oke))))[: len(src)]
        gv = 10 ** ((VOICE_LUFS - lufs(src, sr)) / 20)
        go = 10 ** ((OKE_LUFS - lufs(oke, sr)) / 20)
        for case in CASES:
            seed = int.from_bytes(hashlib.sha256(f"{mat}/{case}".encode()).digest()[:8], "big")
            order = ENGINES[:]
            random.Random(seed).shuffle(order)
            d = LISTEN / mat / case; d.mkdir(parents=True, exist_ok=True)
            sf.write(str(d / "_ref_original.wav"), np.clip(src * gv, -1, 1), sr, subtype="PCM_24")
            sf.write(str(d / "_ref_original_mix.wav"), np.clip(src * gv + oke * go, -1, 1), sr, subtype="PCM_24")
            assign = {}
            for label, eng in zip(LABELS, order):
                wav = WORK / mat / "resynth" / f"{case}_{eng}.wav"
                if not wav.exists():
                    continue
                y, _ = load_mono(wav)
                y = np.pad(y, (0, max(0, len(src) - len(y))))[: len(src)]
                sf.write(str(d / f"solo_{label}.wav"), np.clip(y * gv, -1, 1), sr, subtype="PCM_24")
                sf.write(str(d / f"mix_{label}.wav"), np.clip(y * gv + oke * go, -1, 1), sr, subtype="PCM_24")
                assign[label] = eng
            entries.append({"material": mat, "case": case, "assign": assign,
                            "voice_gain_db": round(20 * np.log10(gv), 2), "oke_gain_db": round(20 * np.log10(go), 2)})
            print(f"{mat}/{case}: {len(assign)} files → {d}")
    out = ANSWERS / f"{DATE}-pitchlab-blind-map.json"
    out.write_text(json.dumps({"date": DATE, "entries": entries,
                               "note": "回答完了まで開かない。W/X/Y/Z → エンジン名の対応表"}, indent=1, ensure_ascii=False))
    print(f"map: {out.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
