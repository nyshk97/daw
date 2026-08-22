#!/usr/bin/env python3
"""自分のキットとリファレンスのドラムを並べて、何が違うかを言葉にするための素材。

reference-beat.md の原則「音色は単体試聴で選ばない」に沿って、**組んだ状態で比べる**。
1ファイル = 前半4小節が自分のドラム（基準パターン）・後半4小節がリファレンスのドラムステム。
同じBPM・同じRMSに揃えてあるので、残る差が音色と処理になる。

キットはディレクトリを渡すと、レーンごとに**目標の重心に一番近いサンプル**を自動で選ぶ
（`mise run drum:target` が出した kick 88Hz / snare 2880Hz / hat 6624Hz）。決定的。

比較相手は、ドラム判定 +2 の7曲のうち**本命テンポ帯（80〜100BPM）でハットが疎な順**に選ぶ。
基準パターン（8分ハット・1/3キック・2/4スネア）に近いものと比べるほうが、
パターンの違いが差として混ざりにくい。

**差の一部は音色でなく処理**（リバーブ・コンプ・マスタリング）。こちらは素のワンショットを
並べただけなので、そこは分けて聴く必要がある。処理の差ならサンプルを買い換えても埋まらない。

  python3 tools/drumlab/kitcompare.py --kit "<キットのディレクトリ>" [--songs 3]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kittarget import LANES, _bandpass, _centroid, loudness_lufs, top_songs  # noqa: E402

INVENTORY = Path.home() / "Music/daw/reference-beat-corpus/review/drum-inventory"
SR = 44100
BARS = 4
TARGET_LUFS = -20.0
FADE_MS = 20.0
# レーン間バランスの目標（`mise run drum:target` の実測。キック基準の相対dB）。
# **固定ゲインは使わない** — サンプルを差し替えるとピーク正規化では実効音量が揃わず、
# 低域偏重になって「RMSは同じなのに小さく聞こえる」が起きる（実測で2回踏んだ）。
# レーンを単独でレンダリングして帯域RMSを測り、この比になるゲインを毎回解く。
TARGET_BALANCE = {"kick": 0.0, "snare": -7.0, "hat": -22.7}
BASE = {"kick": [0, 8], "snare": [4, 12], "hat": [0, 2, 4, 6, 8, 10, 12, 14]}
# 目標の重心（mise run drum:target の実測。ここを直したら labs も直す）
TARGET_CENTROID = {"kick": 88.0, "snare": 2880.0, "hat": 6624.0}
TEMPO_RANGE = (80.0, 100.0)


def _w(word: str) -> str:
    # `_` は \b の境界にならない（`Kick_1_...`）。英字だけを除外する
    return rf"(?<![A-Za-z]){word}(?![A-Za-z])"


LANE_MATCH = {
    "kick":  lambda n: re.search(_w("kick"), n, re.I),
    "snare": lambda n: re.search(_w("snare"), n, re.I) and not re.search("rim", n, re.I),
    "hat":   lambda n: (re.search(_w("hi[ _-]?hat") + "|" + _w("hats?"), n, re.I)
                        and not re.search("open", n, re.I)),
}


def pick_kit(root: Path) -> dict[str, tuple[Path, float]]:
    """レーンごとに、目標の重心へ一番近いサンプルを選ぶ。"""
    audio = [f for ext in ("*.wav", "*.aif", "*.aiff") for f in sorted(root.rglob(ext))]
    kit: dict[str, tuple[Path, float]] = {}
    for lane, match in LANE_MATCH.items():
        best = None
        for f in (x for x in audio if match(x.name)):
            y, sr = sf.read(str(f), dtype="float32")
            if y.ndim > 1:
                y = y.mean(axis=1)
            if sr != SR:
                continue
            c = _centroid(y, sr, *LANES[lane]["measure"])
            if np.isnan(c):
                continue
            d = abs(np.log2(c / TARGET_CENTROID[lane]))   # 比で見る（Hzの差でなく）
            if best is None or d < best[0]:
                best = (d, f, c, y / (np.max(np.abs(y)) + 1e-12))
        if best is None:
            raise SystemExit(f"{root}: {lane} のサンプルが見つからない")
        kit[lane] = (best[1], best[2], best[3])
    return kit


def render(kit: dict, bpm: float, gains: dict[str, float] | None = None,
           only: str | None = None) -> np.ndarray:
    gains = gains or {ln: 1.0 for ln in BASE}
    bar_s = 4 * 60.0 / bpm
    step = bar_s / 16
    buf = np.zeros(int((BARS * bar_s + 1.0) * SR))
    for bar in range(BARS):
        for lane, positions in BASE.items():
            if only is not None and lane != only:
                continue
            _, _, y = kit[lane]
            for p in positions:
                a = int((bar * bar_s + p * step) * SR)
                b = min(a + len(y), len(buf))
                buf[a:b] += y[: b - a] * gains[lane]
    return buf[: int(BARS * bar_s * SR)]


def solve_gains(kit: dict, bpm: float) -> dict[str, float]:
    """レーンを単独でレンダリングし、帯域RMSが TARGET_BALANCE の比になるゲインを解く。

    キットを差し替えるたびに解き直す。ピーク正規化＋固定ゲインでは、サンプルの
    エネルギーが違うぶんバランスが崩れる（切り出したキットで低域偏重になり、
    RMSを揃えたのに聴感で小さくなった実例がある）。
    """
    solo = {}
    for lane in BASE:
        y = render(kit, bpm, only=lane)
        solo[lane] = float(np.sqrt(np.mean(_bandpass(y, SR, *LANES[lane]["detect"]) ** 2))) + 1e-12
    return {lane: (solo["kick"] * 10 ** (TARGET_BALANCE[lane] / 20)) / solo[lane] for lane in BASE}


def normalize(x: np.ndarray) -> np.ndarray:
    """聴感（K特性）で揃える。RMSで揃えると低域偏重の素材が小さく聞こえる。"""
    x = x * 10 ** ((TARGET_LUFS - loudness_lufs(x, SR)) / 20)
    n = int(FADE_MS / 1000 * SR)
    if len(x) > 2 * n:
        x[:n] *= np.linspace(0, 1, n)
        x[-n:] *= np.linspace(1, 0, n)
    return np.clip(x, -1.0, 1.0)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--kit", type=str, required=True, help="キットのディレクトリ（再帰）")
    ap.add_argument("--songs", type=int, default=3)
    ap.add_argument("--out", type=str,
                    default=str(Path.home() / "Music/daw/reference-beat-corpus/review/kit-vs-reference"))
    args = ap.parse_args()

    kit = pick_kit(Path(args.kit).expanduser())
    print(f"キット: {args.kit}")
    for lane, (f, c, _) in kit.items():
        t = TARGET_CENTROID[lane]
        print(f"  {lane:6s} {f.name:38s} 重心 {c:7.0f}Hz  目標 {t:6.0f}Hz  ×{c/t:.2f}")

    # 比較相手: 本命テンポ帯の曲。抜粋は inventory.py が top_songs() の順に
    # `NN-<名前>-A.wav` と採番しているので、**名前でなく番号で対応させる**
    # （名前の正規化で突き合わせると全角・記号で取り違え、BPMがずれて比較自体が壊れる）
    picked = []
    for n, s in enumerate(top_songs(), 1):
        if not (TEMPO_RANGE[0] <= s["bpm"] <= TEMPO_RANGE[1]):
            continue
        matches = sorted(INVENTORY.glob(f"{n:02d}-*-A.wav"))
        if len(matches) != 1:
            raise SystemExit(f"{INVENTORY}: {n:02d}-*-A.wav が1件でない（{len(matches)}件）。"
                             f"`mise run drum:inventory` を先に走らせる")
        picked.append((n, s, matches[0]))
    picked = picked[: args.songs]
    if not picked:
        raise SystemExit(f"比較用の抜粋が見つからない（{INVENTORY} に *-A.wav が要る）")

    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.wav"):
        old.unlink()

    rows = []
    print(f"\n比較相手（本命テンポ帯・{len(picked)}曲）:")
    for i, (n, s, f) in enumerate(picked, 1):
        mine = normalize(render(kit, s["bpm"], solve_gains(kit, s["bpm"])))
        ref, sr = sf.read(str(f), dtype="float32")
        if ref.ndim > 1:
            ref = ref.mean(axis=1)
        bar_s = 4 * 60.0 / s["bpm"]
        ref = normalize(ref[: int(BARS * bar_s * sr)].copy())
        gap = np.zeros(int(0.25 * SR))
        # 抜粋のファイル名をそのまま流用する（曲名の正規化で潰れないよう）
        name = f"{i:02d}-vs-{f.stem[3:-2]}.wav"
        sf.write(str(out / name), np.concatenate([mine, gap, ref]).astype(np.float32), SR)
        rows.append({"file": name, "song": s["name"], "bpm": s["bpm"]})
        print(f"  {name:28s} {s['name']} / BPM {s['bpm']:.0f}")

    md = ["# 自分のキット vs リファレンスのドラム", "",
          f"素材: `{out}`", "", "## 何を聴くか", "",
          f"**前半{BARS}小節が自分のドラム（基準パターン）、後半{BARS}小節がリファレンス。**",
          "同じBPM・同じRMSに揃えてあるので、残る差は音色と処理だけ。", "",
          "**まず「音色の差」と「処理の差」を分けてください。**", "",
          "- 音色の差 = キックの重さ/長さ・スネアの胴鳴り・ハットの明るさや粒。"
          "→ **サンプルを差し替えれば埋まる**",
          "- 処理の差 = 全体の乾き・奥行きの無さ・馴染みの悪さ。"
          "→ **サンプルを買い換えても埋まらない**（EQ・サチュ・リバーブの仕事）", "",
          "「なんか違う」で止めず、**どっちなのかを言葉にする**のがこの比較の目的です。",
          "それが決まらないと、パックを買うべきかFXを掛けるべきかが決まりません。", "",
          "## 回答", "",
          "| ファイル | リファレンス | 音色の差（どこが） | 処理の差（どこが） |",
          "|---|---|---|---|"]
    for r in rows:
        md.append(f'| {r["file"]} | {r["song"]} (BPM {r["bpm"]:.0f}) |  |  |')
    md += ["", "使ったキット:", ""]
    for lane, (f, c, _) in kit.items():
        md.append(f"- **{lane}** — `{f.name}`（重心 {c:.0f}Hz / 目標 {TARGET_CENTROID[lane]:.0f}Hz）")
    md += ["", "回答後、このファイルを `docs/labs/reference-beat-human-answers/` へコピーする。"]
    (out / "answers.md").write_text("\n".join(md) + "\n")
    (out / "kit.json").write_text(json.dumps(
        {"kit_dir": args.kit, "lanes": {l: {"file": str(f), "centroid": round(c, 1)}
                                        for l, (f, c, _) in kit.items()},
         "songs": rows}, ensure_ascii=False, indent=2))
    print(f"\n完了: {len(rows)}本 → {out}\n  回答シート: {out / 'answers.md'}")


if __name__ == "__main__":
    main()
