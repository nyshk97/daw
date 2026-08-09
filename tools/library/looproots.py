#!/usr/bin/env python
"""採用ループからベースが従うルート列を抽出する（ループ追従モードの入力を作る）。

ループが和声のアンカーになった帰結として、ベースの進行は「カードの性格から生成」でなく
「採用ループの実進行に従う」が主経路になる（docs/design/reference-beat.md「音色の調達」）。
このスクリプトはループ1本からスロット単位のルート列を1回だけ抽出し、契約 JSON に書く。
LaLa はこれを project.json に永続化し、ベースガチャ（bass.py --roots）は保存済みの値を読む。

## ルート列の JSON 契約（この docstring が真実の源。bass.py と project.json 永続化が従う）

    {
      "version": 1,
      "slots_per_bar": 2,          1小節あたりのハーモニースロット数。1/2/4/8/16 のみ
      "loop_bars": 4,              ループ小節数（1以上の整数）
      "roots": [9, 9, 2, 4, ...],  長さ = loop_bars × slots_per_bar。各値 0..11 の整数
      "confidence": [0.83, ...],   roots と同じ長さ。スロット別の照合スコア 0..1
      "degraded": false,           低信頼で「トニックのみ」に退化したか
      "key_root": 9,               退化時のトニックの由来（0..11）
      "key_mode": "minor",         "major" | "minor"
      "source": "loops/P/x.wav"    抽出元（表示用。同一性には関与しない）
    }

- 長さ不一致・範囲外・型違いは受け取り側（bass.py）が**即エラー**にする。黙った退化はしない —
  退化は「低信頼 → トニックのみ」の1経路だけで、それは degraded=true として出力に現れる
- --bars がループ長を超える場合の反復は bass.py 側（chords カードと同じ規則 = パターンの繰り返し）
- bass.py が読むのは slots_per_bar / loop_bars / roots のみ（confidence 等は生成結果を変えないので
  候補の同一性にも入らない）

コード検出は topline.py の**関数だけ**を借りる（CLI はステム・basics・2反復以上の畳み込みを
前提にしていて、1周しか無いループには使えない）。グリッドは BPM とループ小節数から自前で作る。

使い方: looproots.py <loop.wav> [--bpm N] [--bars N] [--slots-per-bar 2]
                     [--key ROOT:MODE] [--out FILE]
        bpm / bars / key は 引数 → ファイル名 →（keyのみ）音声推定 の順で解決する。
        --out 省略時は契約 JSON を stdout に出す。
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

# パスだけ先に通す（import はしない — bass.py が検証関数だけ使うときの起動を軽く保つ）
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "reference"))

CONTRACT_VERSION = 1
VALID_SLOTS_PER_BAR = (1, 2, 4, 8, 16)  # TICKS_BAR を割り切れる音楽的な分割だけ
# 照合スコアの中央値がこれ未満なら進行を信用せずトニックへ退化する。
# きれいな三和音は0.8前後・無音は0.0が出る実測に対する暫定値（動作確認1で較正する）
DEGRADE_THRESHOLD = 0.55

NOTE_TO_PC = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5,
              "F#": 6, "Gb": 6, "G": 7, "G#": 8, "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11}


# --- 契約の検証（bass.py・永続化・このスクリプトの3者が同じ関数を通る） -------------

def validate_roots_core(slots_per_bar, loop_bars, roots) -> None:
    """bass.py が読む最小セットの検証。不正は ValueError（受け取り側で即エラーにする契約）。"""
    if slots_per_bar not in VALID_SLOTS_PER_BAR:
        raise ValueError(f"slots_per_bar は {VALID_SLOTS_PER_BAR} のみ: {slots_per_bar!r}")
    if not isinstance(loop_bars, int) or isinstance(loop_bars, bool) or loop_bars < 1:
        raise ValueError(f"loop_bars は 1 以上の整数: {loop_bars!r}")
    n = loop_bars * slots_per_bar
    if not isinstance(roots, list) or len(roots) != n:
        raise ValueError(f"roots の長さは loop_bars×slots_per_bar={n} 必須: {len(roots) if isinstance(roots, list) else roots!r}")
    for r in roots:
        if not isinstance(r, int) or isinstance(r, bool) or not (0 <= r <= 11):
            raise ValueError(f"roots の値は 0..11 の整数: {r!r}")


def validate_contract(d: dict) -> dict:
    """契約 JSON 全体の検証。通れば d をそのまま返す。"""
    if not isinstance(d, dict):
        raise ValueError(f"契約はオブジェクト: {type(d).__name__}")
    if d.get("version") != CONTRACT_VERSION:
        raise ValueError(f"version は {CONTRACT_VERSION} のみ: {d.get('version')!r}")
    validate_roots_core(d.get("slots_per_bar"), d.get("loop_bars"), d.get("roots"))
    conf = d.get("confidence")
    if not isinstance(conf, list) or len(conf) != len(d["roots"]):
        raise ValueError("confidence は roots と同じ長さのリスト")
    for c in conf:
        if not isinstance(c, (int, float)) or isinstance(c, bool) or not (0.0 <= c <= 1.0):
            raise ValueError(f"confidence の値は 0..1: {c!r}")
    if not isinstance(d.get("degraded"), bool):
        raise ValueError(f"degraded は bool: {d.get('degraded')!r}")
    kr = d.get("key_root")
    if not isinstance(kr, int) or isinstance(kr, bool) or not (0 <= kr <= 11):
        raise ValueError(f"key_root は 0..11 の整数: {kr!r}")
    if d.get("key_mode") not in ("major", "minor"):
        raise ValueError(f"key_mode は major|minor: {d.get('key_mode')!r}")
    return d


def load_contract(path: Path) -> dict:
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise ValueError(f"{path}: 契約 JSON を読めない: {e}") from e
    try:
        return validate_contract(data)
    except ValueError as e:
        raise ValueError(f"{path}: {e}") from e


# --- 抽出 ---------------------------------------------------------------------

def extract(y, sr: int, bpm: float, bars: int, slots_per_bar: int,
            key_root: int, key_mode: str, source: str = "") -> dict:
    """ループ音声からルート列を抽出して契約 dict を返す（純関数寄り・テストの本体）。"""
    # topline は import 時に librosa/matplotlib を引き込むので関数内に閉じる
    # （bass.py が検証関数だけ使うときに重い import を払わないため）
    from topline import chord_templates, match_chord, sub_chroma, _root_index

    import numpy as np

    bar_len = 240.0 / bpm
    names, T = chord_templates()
    # グリッドは先頭=小節頭とみなして自前で構築（ループ素材は頭出し済みが前提。
    # ズレた素材は動作確認1で発覚したら first_down 推定を足す）
    slots, chroma, t = sub_chroma(y, sr, first_down=0.0, bar_len=bar_len,
                                  n_bars=bars, per_bar=slots_per_bar)

    def polyphony_info(i: int) -> float:
        """スロット i の「和音としての情報量」係数（同時発音1音=0・2音=半減・3音以上=満額）。

        テンプレート一致だけだと単音でも高スコアが出る（A単音は "A5" に0.85で一致）。
        さらにスロット**平均**のクロマで数えると、スロット内を動く単音メロディが
        3音以上に見えて和音扱いになる（レビューで実測）。だから**時間フレームごと**の
        同時発音数を数え、鳴っているフレームの中央値で判定する。"""
        seg = bar_len / slots_per_bar
        i0, i1 = np.searchsorted(t, [i * seg, (i + 1) * seg])
        frames = chroma[:, i0:i1]
        if frames.size == 0:
            return 0.0
        loud = frames.max(axis=0) > 0.1 * float(chroma.max() or 1.0)  # 無音フレームを除く
        if not loud.any():
            return 0.0
        active = frames[:, loud]
        poly = (active >= 0.25 * active.max(axis=0)).sum(axis=0)
        med = float(np.median(poly))
        return 0.0 if med <= 1 else (0.5 if med <= 2 else 1.0)

    roots, conf = [], []
    prev = key_root
    for i, c in enumerate(slots):
        name, score = match_chord(c, None, names, T)
        if name is None:  # 無音スロットは直前のルートを引き継ぐ（和声は鳴り続いている扱い）
            roots.append(prev)
            conf.append(0.0)
            continue
        # 単音（持続もメロディも）はここが0になり、中央値判定で plan どおりトニックへ退化する
        prev = _root_index(name)
        roots.append(prev)
        conf.append(round(min(float(score), 1.0) * polyphony_info(i), 3))

    degraded = statistics.median(conf) < DEGRADE_THRESHOLD
    if degraded:
        roots = [key_root] * len(roots)

    contract = {
        "version": CONTRACT_VERSION,
        "slots_per_bar": slots_per_bar,
        "loop_bars": bars,
        "roots": roots,
        "confidence": conf,
        "degraded": degraded,
        "key_root": key_root,
        "key_mode": key_mode,
        "source": source,
    }
    return validate_contract(contract)


def parse_key_arg(s: str) -> tuple[int, str]:
    try:
        root_s, mode = s.split(":")
    except ValueError:
        raise SystemExit(f"ERROR: --key は ROOT:MODE 形式（例 A:minor）: {s!r}")
    if root_s not in NOTE_TO_PC or mode not in ("major", "minor"):
        raise SystemExit(f"ERROR: --key が不正: {s!r}")
    return NOTE_TO_PC[root_s], mode


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("loop", type=Path)
    ap.add_argument("--bpm", type=float, default=None)
    ap.add_argument("--bars", type=int, default=None)
    ap.add_argument("--slots-per-bar", type=int, default=2)
    ap.add_argument("--key", type=str, default=None, help="退化時トニックの由来 ROOT:MODE（例 A:minor）")
    ap.add_argument("--out", type=Path, default=None, help="契約 JSON の書き出し先（省略時 stdout）")
    args = ap.parse_args()

    from index import estimate_bars, parse_filename_meta  # noqa: E402

    import librosa  # 重い import はここから（--help を速く保つ）

    meta = parse_filename_meta(args.loop.stem)
    bpm = args.bpm if args.bpm is not None else meta["bpm"]
    if bpm is None:
        raise SystemExit("ERROR: BPM が引数にもファイル名にも無い（--bpm で指定する）")

    y, sr = librosa.load(args.loop, sr=22050, mono=True)
    bars = args.bars if args.bars is not None else estimate_bars(y, sr, bpm)
    if bars is None:
        raise SystemExit("ERROR: ループ小節数を尺から確定できない（余韻付き書き出し?）。--bars で指定する")

    if args.key is not None:
        key_root, key_mode = parse_key_arg(args.key)
    elif meta["key_root"] is not None:
        key_root, key_mode = meta["key_root"], meta["key_mode"]
    else:
        from basics import PITCHES, estimate_key  # noqa: E402
        top = estimate_key(librosa.effects.harmonic(y), sr)[0]["key"].split()
        key_root, key_mode = PITCHES.index(top[0]), top[1]

    contract = extract(y, sr, bpm, bars, args.slots_per_bar, key_root, key_mode,
                       source=args.loop.name)
    text = json.dumps(contract, ensure_ascii=False, indent=1) + "\n"
    if args.out:
        args.out.write_text(text)
        note = "（トニック連打に退化）" if contract["degraded"] else ""
        print(f"{args.out}: {bars}小節×{args.slots_per_bar}スロット{note}")
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
