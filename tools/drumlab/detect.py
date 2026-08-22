#!/usr/bin/env python3
"""列挙の下書き — 抜粋の中から「キック/スネア/クローズハット以外かもしれない打点」を拾う。

40秒の抜粋を通しで聴いて表を埋めるのは重すぎたので、**機械が候補の打点だけを切り出して
並べ、人間は0.7秒のクリップに一言答える**形にした。

2種類を拾う。

1. **伸びる高域打点** — オープンハット / ライド / クラッシュ。打点の 80〜200ms 後の残り
   （ピーク比dB）で測る。手持ちのCobraで較正済み: クローズハット21本が −149〜−33.6dB、
   オープンハット21本が −23.3〜−11.4dB で**完全分離**する（ライド −15.7 / クラッシュ −14.5 も
   伸びる側）。閾値は隙間の中点 −28dB。
   → **機械にはオープンハットとスネアの残響の区別がつかない**ので、そこは耳で切る
2. **3レーンのどれとも一致しない打点** — パーカ / リムショット候補。全帯域で検出した打点の
   うち、キック帯・スネア帯・ハット帯のどの打点とも ±40ms 以内に無いもの。
   → **この検出はほぼ機能しない**（2026-08-22 実測。16抜粋中14で0件）。パーカの打点も
   スネア帯かハット帯で検出されるため「一致しない」にならないため。**0件は「無い」の
   証拠にならない**。パーカ・クラップ・シェイカーは耳か実音A/Bで決めるしかない
   （クラップはスネアと同時に鳴るので、原理的にここには出ない）

出力は曲ごとの「候補ダイジェスト」wav（候補を0.3秒の無音で区切って連結）と、
1行1クリップの回答シート。

  python3 tools/drumlab/detect.py [--top 6] [--dir DIR]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kittarget import DELTA, LANES, _bandpass  # noqa: E402

INVENTORY = Path.home() / "Music/daw/reference-beat-corpus/review/drum-inventory"
HAT_BAND = LANES["hat"]["detect"]
SUSTAIN_FROM, SUSTAIN_TO = 0.080, 0.200   # 打点の何秒後を「残り」とみなすか
SUSTAIN_THRESH_DB = -28.0                 # Cobra の closed/open の隙間の中点（較正済み）
MATCH_TOL = 0.040                         # 「同じ打点」とみなす時間差
CLIP_PRE, CLIP_LEN = 0.05, 0.70           # 切り出すクリップ（打点の少し前から）
GAP_S = 0.30                              # ダイジェストの区切り


def onsets(y: np.ndarray, sr: int, band: tuple[float, float] | None) -> np.ndarray:
    x = _bandpass(y, sr, *band) if band else y
    env = librosa.onset.onset_strength(y=x, sr=sr, hop_length=256)
    fr = librosa.onset.onset_detect(onset_envelope=env, sr=sr, hop_length=256,
                                    backtrack=False, delta=DELTA, wait=2)
    return librosa.frames_to_time(fr, sr=sr, hop_length=256)


def sustain_db(x: np.ndarray, sr: int, t: float, t_next: float | None) -> float:
    """打点 t の「残り」をピーク比dBで返す。次の打点の 20ms 前で窓を打ち切る。"""
    a = int(t * sr)
    peak_win = x[a:min(a + int(0.05 * sr), len(x))]
    pk = float(np.abs(peak_win).max()) if len(peak_win) else 0.0
    if pk <= 0:
        return float("nan")
    s0 = a + int(SUSTAIN_FROM * sr)
    s1 = min(a + int(SUSTAIN_TO * sr), len(x))
    if t_next is not None:
        s1 = min(s1, int(t_next * sr) - int(0.02 * sr))
    if s1 - s0 < 64:
        return float("nan")
    return 20 * np.log10(max(float(np.sqrt(np.mean(x[s0:s1] ** 2))), 1e-12) / pk)


def analyze(path: Path, top: int) -> dict:
    y, sr = sf.read(str(path), dtype="float32")
    if y.ndim > 1:
        y = y.mean(axis=1)
    lane_t = {ln: onsets(y, sr, b["detect"]) for ln, b in LANES.items()}
    hat = _bandpass(y, sr, *HAT_BAND)

    ring = []
    for i, t in enumerate(lane_t["hat"]):
        nxt = lane_t["hat"][i + 1] if i + 1 < len(lane_t["hat"]) else None
        s = sustain_db(hat, sr, t, nxt)
        if not np.isnan(s) and s > SUSTAIN_THRESH_DB:
            ring.append((t, s))
    ring.sort(key=lambda r: -r[1])

    known = np.concatenate([v for v in lane_t.values()]) if lane_t else np.array([])
    other = []
    for t in onsets(y, sr, None):
        if len(known) == 0 or np.min(np.abs(known - t)) > MATCH_TOL:
            other.append(t)

    return {"sr": sr, "y": y, "n_hat": len(lane_t["hat"]),
            "ring": ring[:top], "n_ring": len(ring),
            "other": other[:top], "n_other": len(other)}


def digest(y: np.ndarray, sr: int, times: list[float]) -> np.ndarray:
    gap = np.zeros(int(GAP_S * sr))
    parts = []
    for t in times:
        a = max(0, int((t - CLIP_PRE) * sr))
        b = min(len(y), a + int(CLIP_LEN * sr))
        seg = y[a:b].copy()
        n = int(0.005 * sr)
        if len(seg) > 2 * n:
            seg[:n] *= np.linspace(0, 1, n)
            seg[-n:] *= np.linspace(1, 0, n)
        parts += [seg, gap]
    return np.concatenate(parts) if parts else np.zeros(0)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=6, help="1曲あたり各カテゴリで切り出す上限")
    ap.add_argument("--dir", type=str, default=str(INVENTORY))
    ap.add_argument("--sections", type=str, default="A",
                    help="対象の区間（既定 A。両方なら AB）")
    args = ap.parse_args()

    d = Path(args.dir).expanduser()
    out = d / "candidates"
    out.mkdir(parents=True, exist_ok=True)
    files = [f for s in args.sections for f in sorted(d.glob(f"*-{s}.wav"))]
    rows = []
    print(f"{'抜粋':26s}{'ハット打点':>10}{'伸びる':>8}{'3レーン外':>10}")
    for f in files:
        r = analyze(f, args.top)
        print(f"{f.stem:26s}{r['n_hat']:10d}{r['n_ring']:8d}{r['n_other']:10d}")
        for tag, times in (("ring", [t for t, _ in r["ring"]]), ("other", r["other"])):
            if not times:
                continue
            wav = digest(r["y"], r["sr"], times)
            name = f"{f.stem}-{tag}.wav"
            sf.write(str(out / name), wav, r["sr"], subtype="PCM_16")
            rows.append({"file": name, "stem": f.stem, "tag": tag, "n": len(times),
                         "total": r["n_ring"] if tag == "ring" else r["n_other"]})

    lines = ["# 列挙の下書き — 候補の打点だけを聴く", "",
             f"素材: `{out}`", "",
             "40秒の抜粋を通しで聴く代わりに、**機械が拾った候補の打点だけ**を0.7秒ずつ"
             "切り出して連結してある。1ファイルにつき最大"
             f"{args.top}個、0.3秒の無音で区切ってある。", "",
             "## 何を答えるか", "",
             "- `-ring` = **伸びる高域打点**。オープンハット / ライド / クラッシュ / "
             "スネアやリバーブの残響 のどれか。**機械はここを区別できない**",
             "- `-other` = **キック・スネア・ハットのどれとも位置が一致しない打点**。"
             "パーカ / リムショット / 検出のブレ のどれか。"
             "**この検出はほぼ機能していない**（ほとんどの抜粋で0件）ので、"
             "無いことを「パーカが無い」と読まないこと",
             "",
             "クリップごとに1語でいい（「オープンハット」「残響」「パーカ」「わからない」）。"
             "同じファイル内で全部同じなら1行にまとめて書いてよい。", "",
             "**クラップ・シェイカー・パーカはこの下書きでは決まらない**"
             "（クラップはスネアと同時に鳴るので位置が一致し、パーカはスネア帯・ハット帯で"
             "検出されてしまう）。この4つは元の抜粋を聴くか、実音A/Bで決める。", "",
             "## 回答", "",
             "| ファイル | 切り出し数 / 検出総数 | 何だったか |", "|---|---:|---|"]
    for r in rows:
        lines.append(f'| {r["file"]} | {r["n"]} / {r["total"]} |  |')
    lines += ["", "回答後、このファイルを `docs/labs/reference-beat-human-answers/` へコピーする。"]
    (out / "answers.md").write_text("\n".join(lines) + "\n")
    print(f"\n完了: {len(rows)}ファイル → {out}\n  回答シート: {out / 'answers.md'}")


if __name__ == "__main__":
    main()
