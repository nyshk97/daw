#!/usr/bin/env python3
"""列挙用の素材を作る — 好きな曲のドラムに「何が鳴っているか」を数えるための抜粋。

対象は round2 で score=+2 だった正例（ドラム判定が最高の曲）。1曲につき2区間を切る。
1区間だけだと、フックにしか出ないパーカやセクション頭のクラッシュを取りこぼすため。

好き嫌いの判定ではなく在庫の確認なので**ラベルは伏せない**（曲が分かったほうが思い出せる）。
ラウドネスバイアスを避けるため全抜粋を同一RMSへ揃えるのは、判定と同じ理由で必要
（大きい抜粋のほうが「たくさん鳴っている」と錯覚する）。

区間の選び方: 1つ目は drum-listen-2 で使った位置（ドラムが鳴っているのが確認済み）。
2つ目は、そこから曲の30%以上離れた中で最もドラムの音量が大きい16小節。
小節線は drum-listen-2-key.json の start_bar / start_sec から復元したグリッドに合わせる。

  python3 tools/drumlab/inventory.py [--bars 16] [--out DIR]
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

import numpy as np
import soundfile as sf

CORPUS = Path.home() / "Music/daw/reference-beat-corpus"
ANSWERS = Path(__file__).resolve().parents[2] / "docs/labs/reference-beat-human-answers/2026-08-22-drum-pattern-answers.json"
TARGET_RMS_DB = -20.0
FADE_MS = 20.0
MIN_GAP_RATIO = 0.30   # 2区間をどれだけ離すか（曲の尺に対する割合）

CHECK_ITEMS = [
    ("クラップ", "スネアと同時に鳴る、手を叩いたような音。スネアより軽く広がる"),
    ("オープンハット", "閉じずに開いた音。シャーンと伸びて次の打点まで残る"),
    ("ライド", "金物で、チーンと長く伸びる。ハットより粒が大きい"),
    ("リムショット", "カッという硬く軽い音。スネアの胴鳴りが無い"),
    ("シェイカー・タンバリン", "シャカシャカした細かい音。ハットより柔らかく粒が細かい"),
    ("パーカッション（太鼓系）", "コンガ・ボンゴなど、音程感のある手打ちの太鼓"),
    ("クラッシュ", "ジャーンという大きい金物。セクションの頭に1回だけ入ることが多い"),
]


def top_songs() -> list[dict]:
    ans = json.loads(ANSWERS.read_text())["round2"]
    mixed = set(ans.get("mixed_in", []))
    want = {a["video_id"]: a["name"] for a in ans["answers"]
            if a["score"] == 2 and a["video_id"] not in mixed}
    key = json.loads((CORPUS / "drum-listen-2-key.json").read_text()) \
        if (CORPUS / "drum-listen-2-key.json").exists() \
        else json.loads((CORPUS / "review/drum-listen-2-key.json").read_text())
    return [{"video_id": i["video_id"], "name": want[i["video_id"]], "bpm": float(i["bpm"]),
             "start_sec": float(i["start_sec"]), "start_bar": int(i["start_bar"])}
            for i in key["items"] if i["video_id"] in want]


def slug(name: str) -> str:
    s = re.sub(r"[^\w\-]+", "-", name, flags=re.UNICODE).strip("-")
    return s or "song"


def normalize(x: np.ndarray, sr: int) -> np.ndarray:
    rms = float(np.sqrt(np.mean(x ** 2)))
    if rms > 0:
        x = x * (10 ** (TARGET_RMS_DB / 20) / rms)
    n = int(FADE_MS / 1000 * sr)
    if len(x) > 2 * n:
        x[:n] *= np.linspace(0, 1, n)
        x[-n:] *= np.linspace(1, 0, n)
    return np.clip(x, -1.0, 1.0)


def pick_second_window(y: np.ndarray, sr: int, bar_s: float, bars: int,
                       origin: float, first_start: float) -> float:
    """1つ目から十分離れた中で、最もドラムの音量が大きい小節頭を返す。"""
    win = bars * bar_s
    dur = len(y) / sr
    gap = dur * MIN_GAP_RATIO
    best, best_rms = None, -1.0
    b = 0
    while True:
        t = origin + b * bar_s
        b += 1
        if t < 0:
            continue
        if t + win > dur:
            break
        if abs(t - first_start) < gap:
            continue
        seg = y[int(t * sr):int((t + win) * sr)]
        r = float(np.sqrt(np.mean(seg ** 2)))
        if r > best_rms:
            best, best_rms = t, r
    return best if best is not None else first_start


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bars", type=int, default=16)
    ap.add_argument("--out", type=str, default=str(CORPUS / "review/drum-inventory"))
    args = ap.parse_args()

    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    songs = top_songs()
    made = []
    for n, s in enumerate(songs, 1):
        stem = CORPUS / f'tracks/{s["video_id"]}/stems/htdemucs_6s/track/drums.wav'
        if not stem.exists():
            print(f'  [スキップ] {s["name"]}: ステムが無い ({stem})')
            continue
        y, sr = sf.read(str(stem), dtype="float32")
        if y.ndim > 1:
            y = y.mean(axis=1)
        bar_s = 4 * 60.0 / s["bpm"]
        origin = s["start_sec"] - s["start_bar"] * bar_s   # 第0小節の時刻（負でもよい）
        t1 = s["start_sec"]
        t2 = pick_second_window(y, sr, bar_s, args.bars, origin, t1)
        win = args.bars * bar_s
        base = f'{n:02d}-{slug(s["name"])}'
        for tag, t in (("A", t1), ("B", t2)):
            seg = y[int(t * sr):int((t + win) * sr)].copy()
            if len(seg) < sr:
                continue
            sf.write(str(out / f"{base}-{tag}.wav"), normalize(seg, sr), sr, subtype="PCM_16")
        made.append({"name": s["name"], "bpm": s["bpm"], "base": base,
                     "A_sec": round(t1, 1), "B_sec": round(t2, 1)})
        print(f'  {s["name"][:18]:18s} A={t1:6.1f}s  B={t2:6.1f}s  ({args.bars}小節 = {win:.1f}s)')

    lines = ["# ドラムの列挙 — 好きな曲に何が鳴っているか", "",
             f"素材: `{out}`（{len(made)}曲 × 2区間・各{args.bars}小節・全抜粋を同一RMSへ正規化）", "",
             "## 聴き方", "",
             "1. 曲順（01→07）に、A → B の順で聴く", "2. 各区間で、下の表の項目に○（はっきり鳴っている）"
             "／△（小さく鳴っている・自信なし）／空欄（無い）を入れる",
             "3. 表に無い音が聞こえたら「その他」に書く（それが一番価値がある）", "",
             "**○と△を分けるのが肝**。常設レーンにするかは「はっきり鳴っているか」で決まり、"
             "△止まりのものは常設せず必要な曲だけ足せばいい。", "",
             "## 何を探すか", ""]
    for label, hint in CHECK_ITEMS:
        lines.append(f"- **{label}** — {hint}")
    lines += ["", "キック・スネア・クローズハットは在る前提なので表に入れていない。"
              "**無い曲があったらそれこそ書き留める**（基準4レーンの前提が崩れる）。", "",
              "## 回答表", "",
              "| 曲 | 区間 | クラップ | オープンハット | ライド | リムショット | シェイカー等 | パーカ | クラッシュ | その他 |",
              "|---|---|---|---|---|---|---|---|---|---|"]
    for m in made:
        for tag in ("A", "B"):
            lines.append(f'| {m["name"]} | {tag} |  |  |  |  |  |  |  |  |')
    lines += ["", "## 素材の対応", "",
              "| # | 曲 | BPM | A（秒） | B（秒） |", "|---|---|---:|---:|---:|"]
    for m in made:
        lines.append(f'| {m["base"][:2]} | {m["name"]} | {m["bpm"]:.0f} | {m["A_sec"]} | {m["B_sec"]} |')
    lines += ["", "回答後、このファイルを `docs/labs/reference-beat-human-answers/` へコピーする"
              "（人間の判断はやり直しが要るため）。"]
    (out / "checklist.md").write_text("\n".join(lines) + "\n")
    print(f"\n完了: {len(made)}曲 × 2区間 → {out}\n  チェックシート: {out / 'checklist.md'}")


if __name__ == "__main__":
    main()
