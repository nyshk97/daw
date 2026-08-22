#!/usr/bin/env python3
"""レーン候補の実音A/B — 基準パターンに1レーンずつ足して「あったほうが好きか」を測る。

「好きな曲に何が入っているか」を数える列挙は、①パーカの機械検出が機能しない
②候補の打点だけ切り出しても他のレーンが一緒に鳴っていて判定できない、の2点で
行き詰まったので、**欲しいかを直接聞く**形に替えた（2026-08-22）。

1ファイル = 前半4小節が基準パターンのみ・後半4小節が基準＋候補。同じ位置で切り替わるので
「足した音以外は完全に同じ」が保証される。ハット密度の境界を出した abtest.py と同じ流儀だが、
今回は**音色そのものが問い**（クラップかスネアか等）なので合成音でなく市販キットの実音を使う。

サンプルはカテゴリごとに**重心が中央値のもの**を選ぶ（典型を選ぶ・決定的）。
音量はレーンごとの固定ゲインで、実測した目標のレーン間バランス
（キック基準で snare -7.0dB / hat -22.7dB）に寄せてある。レンダリング後に同じ帯域で測って
表示するので、素材が意図どおりかを聴く前に確かめられる。

  python3 tools/drumlab/lanetest.py [--bpm 90] [--out DIR]
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
from kittarget import LANES, _bandpass, _centroid  # noqa: E402

LIB = Path.home() / "Music/daw/library/oneshots"
SR = 44100
BARS_HALF = 4          # 前半（基準のみ）/ 後半（基準＋候補）の小節数
PEAK_DBFS = -3.0

# 基準パターン（16分位置）。docs/design/drum-pattern.md の「基準となるパターン」
BASE = {
    "kick":  [0, 8],                          # 1拍・3拍
    "snare": [4, 12],                         # 2拍・4拍
    "hat":   [0, 2, 4, 6, 8, 10, 12, 14],     # 8分
}
EIGHTHS = BASE["hat"]
OFF16 = [1, 3, 5, 7, 9, 11, 13, 15]

# カテゴリ名 → ファイル名の正規表現
CATEGORY = {
    "kick": r"\bkick\b", "snare": r"\bsnare\b(?!.*rimshot)",
    "hat": r"closed[ _-]*hihat", "clap": r"\bclap\b", "open_hat": r"open[ _-]*hihat",
    "ride": r"\bride\b", "rimshot": r"rimshot", "perc": r"percussion",
    "crash": r"\bcrash\b", "snap": r"\bsnap\b",
}
# 基準3レーンのゲイン。実測した目標のレーン間バランス（キック基準で snare -7.0dB /
# hat -22.7dB）に寄せてある
GAIN = {"kick": 1.00, "snare": 0.62, "hat": 0.30}

# 追加レーンは固定ゲインにしない。**ピーク正規化では聴こえ方が揃わない**（2026-08-22 実測:
# 同じゲインでもスナップとパーカは基準に足しても帯域RMSが +0.0dB しか動かず、実質無音だった。
# 短く軽い音はピークが同じでもエネルギーが桁違いに小さい）。
# そのレーンだけを鳴らした実効RMSが、基準パターン全体に対して下の値になるようゲインを解く。
ADDED_LEVEL_DB = {"clap": -10.0, "snap": -10.0, "open_hat": -14.0, "perc": -14.0,
                  "perc_hi": -18.0, "crash": -6.0}
# 置き換え系は「置き換えられる側と同じ実効RMS」にする（音色だけが変わる比較になる）
REPLACES = {"ride": "hat", "rimshot": "snare"}

# 候補: (表示名, 追加/置換するレーン, 16分位置, 説明)
CASES = [
    ("clap",     {"add": {"clap": [4, 12]}},
     "クラップをスネアに重ねる（2拍4拍）"),
    ("snap",     {"add": {"snap": [4, 12]}},
     "スナップ（指パッチン系）をスネアに重ねる。クラップとの比較"),
    ("open_hat", {"replace_hat": [14], "add": {"open_hat": [14]}},
     "4拍目の裏だけオープンハットに置き換える"),
    ("ride",     {"replace_hat": EIGHTHS, "add": {"ride": EIGHTHS}},
     "8分のハットを全部ライドに置き換える"),
    ("rimshot",  {"drop_snare": True, "add": {"rimshot": [4, 12]}},
     "スネアをリムショットに置き換える"),
    ("perc",     {"add": {"perc": [6, 14]}},
     "パーカ（太鼓系）を2拍裏・4拍裏に足す"),
    ("perc_hi",  {"add": {"perc_hi": OFF16}},
     "パーカ（シャカシャカ系）を16分の裏に小音量で足す。シェイカーの代用"),
    ("crash",    {"add": {"crash": [0]}, "first_bar_only": True},
     "クラッシュを後半の頭に1回だけ"),
]


def load_samples() -> dict[str, tuple[str, np.ndarray]]:
    """カテゴリごとに「重心が中央値」のサンプルを1本選ぶ。

    perc は重心の中央値で2群に割り、太鼓系(perc)とシャカシャカ系(perc_hi)にする
    （手持ちのパックにシェイカー・タンバリンのカテゴリが無いため）。
    """
    files: dict[str, list] = {}
    for f in sorted(LIB.rglob("*.wav")):
        for cat, rx in CATEGORY.items():
            if re.search(rx, f.name, re.I):
                files.setdefault(cat, []).append(f)
                break
    picked: dict[str, tuple[str, np.ndarray]] = {}
    for cat, fs in files.items():
        rows = []
        for f in fs:
            y, sr = sf.read(str(f), dtype="float32")
            if y.ndim > 1:
                y = y.mean(axis=1)
            if sr != SR:
                continue
            rows.append((_centroid(y, sr, 20, 16000), f, y))
        rows = [r for r in rows if not np.isnan(r[0])]
        if not rows:
            continue
        rows.sort(key=lambda r: r[0])
        if cat == "perc":
            lo, hi = rows[: len(rows) // 2], rows[len(rows) // 2:]
            for name, grp in (("perc", lo), ("perc_hi", hi)):
                c, f, y = grp[len(grp) // 2]
                picked[name] = (f.name, y / (np.max(np.abs(y)) + 1e-12))
        else:
            c, f, y = rows[len(rows) // 2]
            picked[cat] = (f.name, y / (np.max(np.abs(y)) + 1e-12))
    return picked


def solve_gains(samples: dict, bpm: float) -> dict[str, float]:
    """追加レーンのゲインを、そのレーン単独の実効RMSから逆算する。"""
    base = render({}, samples, bpm, {})
    base_rms = float(np.sqrt(np.mean(base ** 2)))
    gains: dict[str, float] = {}

    def solo_rms(cat: str, positions: list[int], first_bar_only: bool = False) -> float:
        y = render({"add": {cat: positions}, "first_bar_only": first_bar_only},
                   samples, bpm, {cat: 1.0}, solo=cat)
        return float(np.sqrt(np.mean(y ** 2)))

    for name, case, _ in CASES:
        for cat, positions in case.get("add", {}).items():
            r = solo_rms(cat, positions, bool(case.get("first_bar_only")))
            if r <= 0:
                gains[cat] = 1.0
                continue
            if cat in REPLACES:
                tgt_cat = REPLACES[cat]
                tgt = solo_rms(tgt_cat, BASE[tgt_cat] if tgt_cat != "hat" else BASE["hat"]) \
                    * GAIN[tgt_cat]
            else:
                tgt = base_rms * 10 ** (ADDED_LEVEL_DB[cat] / 20)
            gains[cat] = tgt / r
    return gains


def render(case: dict, samples: dict, bpm: float, extra_gain: dict[str, float],
           solo: str | None = None) -> np.ndarray:
    bar_s = 4 * 60.0 / bpm
    step = bar_s / 16
    total = int((2 * BARS_HALF * bar_s + 1.0) * SR)
    buf = np.zeros(total)

    def place(cat: str, bar: int, pos: int) -> None:
        if solo is not None and cat != solo:
            return
        _, y = samples[cat]
        g = GAIN.get(cat) if cat in GAIN else extra_gain.get(cat, 1.0)
        a = int((bar * bar_s + pos * step) * SR)
        b = min(a + len(y), total)
        buf[a:b] += y[: b - a] * g

    hat_drop = set(case.get("replace_hat", []))
    for bar in range(2 * BARS_HALF):
        second = bar >= BARS_HALF
        for p in BASE["kick"]:
            place("kick", bar, p)
        if not (second and case.get("drop_snare")):
            for p in BASE["snare"]:
                place("snare", bar, p)
        for p in BASE["hat"]:
            if second and p in hat_drop:
                continue
            place("hat", bar, p)
        if not second:
            continue
        for cat, positions in case.get("add", {}).items():
            if case.get("first_bar_only") and bar != BARS_HALF:
                continue
            for p in positions:
                place(cat, bar, p)
    return buf


def band_balance(y: np.ndarray) -> dict[str, float]:
    tot = float(np.sqrt(np.mean(y ** 2))) + 1e-12
    return {ln: 20 * np.log10(max(float(np.sqrt(np.mean(_bandpass(y, SR, *b["detect"]) ** 2))), 1e-12) / tot)
            for ln, b in LANES.items()}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bpm", type=float, default=90.0)
    ap.add_argument("--out", type=str,
                    default=str(Path.home() / "Music/daw/reference-beat-corpus/review/drum-lanes"))
    args = ap.parse_args()

    samples = load_samples()
    missing = [c for _, case, _ in CASES for c in case.get("add", {}) if c not in samples]
    if missing:
        raise SystemExit(f"素材が無いカテゴリ: {sorted(set(missing))}")

    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.wav"):
        old.unlink()

    print("使うサンプル（カテゴリごとに重心が中央値のもの）:")
    for cat in ("kick", "snare", "hat") + tuple(c for _, case, _ in CASES for c in case.get("add", {})):
        if cat in samples:
            print(f"  {cat:9s} {samples[cat][0]}")

    gains = solve_gains(samples, args.bpm)
    print("\n追加レーンのゲイン（単独の実効RMSから逆算）:")
    for cat, g in sorted(gains.items()):
        tgt = f"{ADDED_LEVEL_DB[cat]:+.0f}dB" if cat in ADDED_LEVEL_DB else f"{REPLACES[cat]}と同じ"
        print(f"  {cat:9s} ×{g:6.3f}  （目標 {tgt}）")

    base_only = render({}, samples, args.bpm, gains)
    rendered = {name: render(case, samples, args.bpm, gains) for name, case, _ in CASES}
    loudest = max([float(np.max(np.abs(y))) for y in rendered.values()] + [float(np.max(np.abs(base_only)))])
    gain = 10 ** (PEAK_DBFS / 20) / loudest

    bal = band_balance(base_only * gain)
    kb = bal["kick"]
    print(f"\n基準パターンのレーン間バランス（キック基準）: "
          + " / ".join(f"{ln} {bal[ln] - kb:+.1f}dB" for ln in LANES))
    print("  実曲7曲の目標: snare -7.0dB / hat -22.7dB")

    key, lines = [], []
    for i, (name, case, desc) in enumerate(CASES, 1):
        y = rendered[name] * gain
        fn = f"{i:02d}-{name}.wav"
        sf.write(str(out / fn), y.astype(np.float32), SR)
        key.append({"file": fn, "case": name, "desc": desc,
                    "sample": samples[list(case.get("add", {}))[0]][0]})
        lines.append(f'| {fn} | {desc} |  |')
        print(f"  {fn:18s} {desc}")

    bar_s = 4 * 60.0 / args.bpm
    md = ["# レーン候補の実音A/B", "",
          f"素材: `{out}`（{len(CASES)}本・各 {2*BARS_HALF}小節 {2*BARS_HALF*bar_s:.0f}秒・BPM {args.bpm:g}）", "",
          "## 聴き方", "",
          f"**各ファイルは前半{BARS_HALF}小節が基準パターンだけ、後半{BARS_HALF}小節が基準＋候補**。",
          "同じ位置で切り替わるので、変わったのは足した音だけ。", "",
          "答えるのは3択:", "",
          "- **足したほうが好き** — 常設レーンに入れる",
          "- **変わらない / どちらでもいい** — 常設しない（必要な曲だけ足す）",
          "- **無いほうがいい** — 入れない", "",
          "迷ったら「変わらない」でいい。**常設は8レーンまで**なので、"
          "はっきり good が付いたものだけ入れれば足りる。", "",
          "## 回答", "",
          "| ファイル | 内容 | 3択 |", "|---|---|---|"] + lines + [
          "", "回答後、このファイルを `docs/labs/reference-beat-human-answers/` へコピーする。"]
    (out / "answers.md").write_text("\n".join(md) + "\n")
    (out.parent / "drum-lanes-key.json").write_text(
        json.dumps({"bpm": args.bpm, "bars_half": BARS_HALF, "items": key},
                   ensure_ascii=False, indent=2))
    print(f"\n完了: {len(CASES)}本 → {out}\n  回答シート: {out / 'answers.md'}")


if __name__ == "__main__":
    main()
