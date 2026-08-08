#!/usr/bin/env python
"""おすすめ5 — リファレンスに雰囲気が近い上モノループをライブラリから引く。

2段構え（docs/design/reference-beat.md「音色の調達」）:
  1. ハードフィルタ = 「使えるか」。キー±2半音（平行調は同一視）・BPM±10%
  2. ランキング = 「似ているか」。リファレンスの上モノ（6分割 piano/guitar/other の合算）と
     ループの特徴量距離。計算は index.compute_features の1関数に揃える（距離の左右で
     計算方法が違うと比較にならないため）

出力は決定的（同じ入力 → 同じ並び）。「振り直し」は無く、--page で6位以降を見る。
理由の文言は内部用語を使わない（明るさ・帯域の配分・音数・アタック感）。

使い方: recommend.py <リファレンスフォルダ> [--library DIR] [--page 1] [--json]
                     [--include-contrast]（evaluate.py 用。通常は対照群を候補に出さない）
"""
import argparse
import json
import math
import sys
import warnings
from pathlib import Path

warnings.filterwarnings("ignore")

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from index import DEFAULT_LIBRARY, SCHEMA_VERSION, compute_features  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "reference"))
from basics import PITCHES  # noqa: E402

FEATURES_VERSION = 1  # upper-features キャッシュの版。compute_features を変えたら上げる
UPPER_STEMS = ("piano", "guitar", "other")  # 6分割の上モノ。合算して「上モノの雰囲気」とする
PAGE_SIZE = 5
BPM_TOLERANCE = 0.10  # ±10%（比率で 0.90〜1.10。1/1.1=0.909 を下限にすると−10%ちょうどを弾く）
KEY_TOLERANCE = 2     # 移調±2半音まで（それ以上は音色が変わりすぎる）

NOTE_TO_PC = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5,
              "F#": 6, "Gb": 6, "G": 7, "G#": 8, "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11}


# --- リファレンス側の入力 ---------------------------------------------------

def load_ref_meta(refdir: Path) -> dict:
    """カードから BPM とキーを読む。カードはゲート通過済みの値だけを持つ（既存原則）ので
    第一情報源にし、キーだけゲート落ちしている場合は basics の推定1位を低信頼として使う。"""
    card_path = refdir / "card.json"
    if not card_path.exists():
        raise SystemExit(f"ERROR: card.json がありません（分析未完かゲート落ち）: {refdir}")
    card = json.loads(card_path.read_text())
    g = card.get("global", {})
    bpm = g.get("bpm")
    if not bpm:
        raise SystemExit(f"ERROR: カードに BPM がありません: {card_path}")

    key = g.get("key")
    key_trusted = True
    if not key:
        basics = json.loads((refdir / "analysis" / "basics.json").read_text())
        top = basics["key"]["top5"][0]["key"].split()  # 例: "C# major"
        key = {"root": top[0], "mode": top[1]}
        key_trusted = False
    return {
        "bpm": float(bpm),
        "key_root": NOTE_TO_PC[key["root"]],
        "key_mode": key["mode"],
        "key_trusted": key_trusted,
    }


def upper_features(refdir: Path) -> dict:
    """6分割の上モノステムを合算した特徴量。重いので analysis/upper-features.json に
    キャッシュする（ステムの size+mtime が変わったら再計算）。"""
    # demucs は stems/htdemucs_6s/<入力名>/<stem>.wav に出す（1階層深い）。直下も許す
    stems_root = refdir / "stems" / "htdemucs_6s"
    candidates = [stems_root] + (sorted(d for d in stems_root.iterdir() if d.is_dir())
                                 if stems_root.is_dir() else [])
    stems_dir = next((d for d in candidates
                      if all((d / f"{n}.wav").exists() for n in UPPER_STEMS)), None)
    if stems_dir is None:
        raise SystemExit(f"ERROR: 6分割ステム（{'/'.join(UPPER_STEMS)}）が {stems_root} 配下に"
                         "見つかりません（analyze.sh を先に）")
    paths = [stems_dir / f"{name}.wav" for name in UPPER_STEMS]

    sig = [[p.name, p.stat().st_size, p.stat().st_mtime_ns] for p in paths]
    cache_path = refdir / "analysis" / "upper-features.json"
    if cache_path.exists():
        try:
            cached = json.loads(cache_path.read_text())
            if cached.get("version") == FEATURES_VERSION and cached.get("stems_sig") == sig:
                return cached["features"]
        except (json.JSONDecodeError, KeyError):
            pass

    import librosa  # 遅延 import（キャッシュヒット時は librosa を読まない → 起動が速い）
    ys = []
    for p in paths:
        y, sr = librosa.load(p, sr=22050, mono=True)
        ys.append(y)
    n = max(len(y) for y in ys)
    mix = np.zeros(n, dtype=np.float64)
    for y in ys:
        mix[: len(y)] += y
    features = compute_features(mix, 22050)["features"]

    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(json.dumps(
        {"version": FEATURES_VERSION, "stems_sig": sig, "features": features},
        ensure_ascii=False, indent=1) + "\n")
    return features


# --- ハードフィルタ -----------------------------------------------------------

def relative_major_root(root: int, mode: str) -> int:
    """平行調を同一視するため、マイナーは平行メジャーのルートに正規化する（Am → C）。
    平行調は構成音が同じ = ループを重ねたときにぶつからない、が同一視の根拠。"""
    return root % 12 if mode == "major" else (root + 3) % 12


def transpose_semitones(loop_root: int, loop_mode: str, ref_root: int, ref_mode: str) -> int | None:
    """ループをリファレンスのキー圏に合わせるための移調量（±KEY_TOLERANCE 以内）。圏外は None。"""
    diff = (relative_major_root(ref_root, ref_mode) - relative_major_root(loop_root, loop_mode)) % 12
    if diff <= KEY_TOLERANCE:
        return diff
    if diff >= 12 - KEY_TOLERANCE:
        return diff - 12
    return None


def tempo_relation(loop_bpm: float, ref_bpm: float) -> tuple[str, float] | None:
    """BPM が使える範囲か（±10%のみ。真実の源 reference-beat.md「音色の調達」）。圏外は None。

    倍/半分テンポ表記（hiphop の BPM 表記慣習）の許容は一度実装して撤回した —
    「160bpm 表記のループを 80bpm 相当として薦めたとき、逆コピーでどちらをプロジェクトへ
    入れるか」が未定義になるため（プランレビュー指摘）。実地で表記倍のループが弾かれて
    困ったら、逆コピーの意味論とセットで再検討する。"""
    r = loop_bpm / ref_bpm
    if 1.0 - BPM_TOLERANCE - 1e-9 <= r <= 1.0 + BPM_TOLERANCE + 1e-9:  # 境界±10%ちょうどを含む
        return "same", r
    return None


# --- ランキング ---------------------------------------------------------------

# 重み: 帯域の配分と明るさが「雰囲気」の主成分（レポートの音色と帯域節が効いた経験則）。
# 音数とアタック感は補助。効かなければ動作確認1で作り直す（plan の中断点）。
WEIGHTS = {"brightness": 1.0, "band": 1.2, "density": 0.6, "attack": 0.4}


def per_beat_rate(features: dict, bpm: float) -> float:
    """音数はテンポで割って拍あたりに直す（BPM が違う2者の密度を比べるため）。"""
    return features["onset_rate_per_s"] * 60.0 / bpm


def distance(loop_f: dict, loop_bpm: float, ref_f: dict, ref_bpm: float) -> tuple[float, dict]:
    b_l, b_r = loop_f["spectral_centroid_hz_median"], ref_f["spectral_centroid_hz_median"]
    brightness = abs(math.log2(max(b_l, 1.0) / max(b_r, 1.0)))

    bb_l, bb_r = loop_f["band_balance"], ref_f["band_balance"]
    band = 0.5 * sum(abs(bb_l[k] - bb_r[k]) for k in bb_r)

    d_l, d_r = per_beat_rate(loop_f, loop_bpm), per_beat_rate(ref_f, ref_bpm)
    density = min(abs(math.log2(max(d_l, 0.01) / max(d_r, 0.01))), 3.0)

    hp_l, hp_r = loop_f["harmonic_percussive_ratio"], ref_f["harmonic_percussive_ratio"]
    attack = min(abs(math.log2(max(hp_l, 0.01) / max(hp_r, 0.01))), 3.0)

    parts = {"brightness": brightness, "band": band, "density": density, "attack": attack}
    score = sum(WEIGHTS[k] * v for k, v in parts.items())
    return round(score, 4), parts


def reason_text(parts: dict, loop_f: dict, loop_bpm: float, ref_f: dict, ref_bpm: float) -> str:
    """平易語の一行。内部用語（重心・HPSS等）は出さない。"""
    words = []
    if parts["brightness"] < 0.15:
        words.append("明るさがほぼ同じ")
    elif parts["brightness"] < 0.5:
        hi = loop_f["spectral_centroid_hz_median"] > ref_f["spectral_centroid_hz_median"]
        words.append("少し明るめ" if hi else "少し暗め")
    if parts["band"] < 0.12:
        words.append("帯域の配分が近い")
    ratio = per_beat_rate(loop_f, loop_bpm) / max(per_beat_rate(ref_f, ref_bpm), 0.01)
    if 0.8 <= ratio <= 1.25:
        words.append("音数が同じくらい")
    elif ratio > 1.25:
        words.append("音数は多め")
    else:
        words.append("音数は少なめ")
    if parts["attack"] < 0.4:
        words.append("鳴りの質感も近い")
    return "・".join(words)


def rank(entries: list[dict], ref_meta: dict, ref_features: dict,
         include_contrast: bool = False, skip_filters: bool = False) -> list[dict]:
    """フィルタ→距離→決定的な並び。テストの本体はこの純関数。
    skip_filters は evaluate.py 用 — 検証したいのは類似度（ランキング）の質であって
    キー/BPM の足切りではないので、対照群を土俵に残したまま並べるために使う。"""
    out = []
    for e in entries:
        if e.get("kind") != "loop":
            continue
        if e.get("is_contrast") and not include_contrast:
            continue
        if e.get("key_root") is None or e.get("bpm") is None:
            continue
        semis = transpose_semitones(e["key_root"], e["key_mode"], ref_meta["key_root"], ref_meta["key_mode"])
        rel = tempo_relation(e["bpm"], ref_meta["bpm"])
        if skip_filters:
            semis = semis if semis is not None else 0
            rel = rel if rel is not None else ("same", e["bpm"] / ref_meta["bpm"])
        if semis is None or rel is None:
            continue
        score, parts = distance(e["features"], e["bpm"], ref_features, ref_meta["bpm"])
        out.append({
            "path": e["path"],
            "bpm": e["bpm"],
            "key_root": e["key_root"],
            "key_mode": e["key_mode"],
            "transpose_semitones": semis,
            "tempo_relation": rel[0],
            "bpm_ratio": round(rel[1], 3),
            "score": score,
            "reason": reason_text(parts, e["features"], e["bpm"], ref_features, ref_meta["bpm"]),
            "is_contrast": bool(e.get("is_contrast")),
        })
    out.sort(key=lambda c: (c["score"], c["path"]))  # 同点はパスで決定的に
    return out


# --- CLI ----------------------------------------------------------------------

def key_name(root: int, mode: str) -> str:
    return f"{PITCHES[root]} {mode}"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("refdir", type=Path, help="分析済みリファレンスフォルダ（card.json 必須）")
    ap.add_argument("--library", type=Path, default=DEFAULT_LIBRARY)
    ap.add_argument("--page", type=int, default=1)
    ap.add_argument("--json", action="store_true", dest="as_json")
    ap.add_argument("--include-contrast", action="store_true")
    args = ap.parse_args()

    library = args.library.expanduser()
    index_path = library / "index.json"
    if not index_path.exists():
        raise SystemExit(f"ERROR: index.json がありません（lib:index を先に）: {library}")
    index = json.loads(index_path.read_text())
    if index.get("schema_version") != SCHEMA_VERSION:
        raise SystemExit(f"ERROR: index の版が合いません（lib:index --force で作り直し）")

    refdir = args.refdir.expanduser()
    ref_meta = load_ref_meta(refdir)
    ref_features = upper_features(refdir)

    ranked = rank(index["entries"], ref_meta, ref_features, include_contrast=args.include_contrast)
    page = ranked[PAGE_SIZE * (args.page - 1): PAGE_SIZE * args.page]

    if args.as_json:
        print(json.dumps({
            "reference": str(refdir),
            "ref_bpm": ref_meta["bpm"],
            "ref_key": key_name(ref_meta["key_root"], ref_meta["key_mode"]),
            "key_trusted": ref_meta["key_trusted"],
            "total": len(ranked),
            "page": args.page,
            "candidates": page,
        }, ensure_ascii=False, indent=1))
        return

    key_note = "" if ref_meta["key_trusted"] else "（キーはゲート落ちのため推定値）"
    print(f"リファレンス: {refdir.name}  {key_name(ref_meta['key_root'], ref_meta['key_mode'])}"
          f" / {ref_meta['bpm']}bpm {key_note}")
    if not ranked:
        print("キーと BPM の条件に合うループがありません（ライブラリを増やすか、別リファレンスで）")
        return
    print(f"候補 {len(ranked)}本中 {PAGE_SIZE * (args.page - 1) + 1}〜{PAGE_SIZE * (args.page - 1) + len(page)}位:")
    for i, c in enumerate(page, start=PAGE_SIZE * (args.page - 1) + 1):
        semis = c["transpose_semitones"]
        key_part = "キーそのまま" if semis == 0 else f"移調 {semis:+d} 半音"
        pct = round((c["bpm_ratio"] - 1) * 100)
        print(f"{i}. {c['path']}")
        print(f"   {key_name(c['key_root'], c['key_mode'])} {c['bpm']}bpm"
              f"（{key_part}・BPM {pct:+d}%） — {c['reason']}")
    if len(ranked) > PAGE_SIZE * args.page:
        print(f"→ 次の5本: --page {args.page + 1}")


if __name__ == "__main__":
    main()
