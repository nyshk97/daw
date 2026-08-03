#!/usr/bin/env python
"""上モノ（コード楽器・メロディ）の深掘り。実験の重心。

やること:
  1. コード進行の推定（ベースのルートを効かせたテンプレートマッチ）とループ長の検出
  2. basic-pitch で音声→MIDI し、精度を評価できる形で出す（原音と重ねた確認用wavも書く）
  3. 音色・質感の数値化（アタック・サステイン・明るさ・ゆらぎ）

出力:
  analysis/topline.json      コード進行・ループ長・MIDI統計・音色特徴
  analysis/chords.png        小節ごとのコードとクロマ
  analysis/topline.mid       basic-pitch の出力
  analysis/midi_check.wav    原音(L)とMIDIをサイン波で鳴らしたもの(R)。ズレを耳で確認する用

使い方: topline.py <stem.wav> <bass.wav> <basics.json> <outdir> [--label other]
"""
import argparse
import json
import warnings
from pathlib import Path

warnings.filterwarnings("ignore")
import logging

logging.disable(logging.WARNING)

import librosa
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import soundfile as sf

SR = 22050
HOP = 512
PITCHES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# コードのテンプレート。度数の集合で書き、クロマとの内積で当てる。
# HIPHOP/R&B の上モノは 7th・9th が効いた「柔らかい」和音が多いので、
# 3和音だけで当てると全部 maj/min に潰れて曲の性格が消える。
CHORD_TEMPLATES = {
    "": [0, 4, 7],
    "m": [0, 3, 7],
    "7": [0, 4, 7, 10],
    "maj7": [0, 4, 7, 11],
    "m7": [0, 3, 7, 10],
    "m9": [0, 3, 7, 10, 2],
    "maj9": [0, 4, 7, 11, 2],
    "6": [0, 4, 7, 9],
    "m6": [0, 3, 7, 9],
    "add9": [0, 2, 4, 7],
    "sus4": [0, 5, 7],
    "7sus4": [0, 5, 7, 10],
    # マイナーキーの ii は m7b5（ハーフディミニッシュ）。これを入れないと dim に化けて
    # 「なぜか減和音だらけ」の誤読になる（実測でそうなった）
    "m7b5": [0, 3, 6, 10],
    "dim": [0, 3, 6],
    "5": [0, 7],
}


def chord_templates() -> tuple[list[str], np.ndarray]:
    names, mats = [], []
    for root in range(12):
        for suffix, degrees in CHORD_TEMPLATES.items():
            v = np.zeros(12)
            for d in degrees:
                v[(root + d) % 12] = 1.0
            v[root] += 0.6  # ルートを少し重くする（転回形より基本形を優先）
            names.append(f"{PITCHES[root]}{suffix}")
            mats.append(v / np.linalg.norm(v))
    return names, np.array(mats)


def _root_index(name: str) -> int:
    return PITCHES.index(name[:2]) if len(name) > 1 and name[1] == "#" else PITCHES.index(name[0])


def match_chord(c: np.ndarray, cb: np.ndarray, names: list[str], T: np.ndarray) -> tuple[str | None, float]:
    if c.sum() < 1e-6:
        return None, 0.0
    c = c / (np.linalg.norm(c) + 1e-9)
    # ベースの最強音をルート候補として加点する。上モノだけだと転回形やテンションで
    # ルートを取り違えやすく、実際「どのコードに聴こえるか」はベースが決めているため。
    bonus = np.zeros(len(names))
    if cb is not None and cb.sum() > 1e-6:
        broot = int(np.argmax(cb))
        for j, nm in enumerate(names):
            if _root_index(nm) == broot:
                bonus[j] = 0.18
    score = T @ c + bonus
    j = int(np.argmax(score))
    return names[j], float(score[j])


def sub_chroma(y: np.ndarray, sr: int, first_down: float, bar_len: float, n_bars: int, per_bar: int, bass: bool = False):
    """1小節を per_bar 分割した各区間の平均クロマ（n_slot × 12）。"""
    kw = dict(fmin=librosa.note_to_hz("C1"), n_octaves=3) if bass else {}
    src = y if bass else librosa.effects.harmonic(y, margin=3.0)
    chroma = librosa.feature.chroma_cqt(y=src, sr=sr, hop_length=HOP, **kw)
    t = librosa.frames_to_time(np.arange(chroma.shape[1]), sr=sr, hop_length=HOP)
    seg = bar_len / per_bar
    out = np.zeros((n_bars * per_bar, 12))
    for i in range(n_bars * per_bar):
        t0 = first_down + i * seg
        i0, i1 = np.searchsorted(t, [t0, t0 + seg])
        if i1 > i0:
            out[i] = chroma[:, i0:i1].mean(axis=1)
    return out, chroma, t


def loop_length(sub: np.ndarray, per_bar: int) -> dict:
    """何小節で1周しているか。コード名でなくクロマの類似度で測る。

    コード名は1区間ごとの誤検出が乗るので周期が見えにくい。クロマの余弦類似度なら
    「だいたい同じ響き」を連続量で拾えるので、ループ長の判定はこちらが安定する。
    """
    norm = sub / (np.linalg.norm(sub, axis=1, keepdims=True) + 1e-9)
    res = {}
    for lag_bars in (1, 2, 4, 8, 16):
        lag = lag_bars * per_bar
        if lag >= len(norm):
            continue
        sim = (norm[:-lag] * norm[lag:]).sum(axis=1)
        res[f"{lag_bars}bar"] = round(float(sim.mean()), 3)
    if not res:
        return {"similarity_by_lag": {}, "most_likely": None}
    best_v = max(res.values())
    # 同点なら短い方を採る（8小節ループは4小節ループ2回でも同じ値になるため）
    best = next(k for k in ("1bar", "2bar", "4bar", "8bar", "16bar") if k in res and res[k] >= best_v - 0.01)
    return {"similarity_by_lag": res, "most_likely": best}


def folded_progression(sub: np.ndarray, sub_b: np.ndarray, per_bar: int, loop_bars: int, names, T) -> dict:
    """ループ長でクロマを畳んでからコードを当てる。

    同じ進行が20回以上繰り返される曲なので、繰り返しを重ねると1回ぶんの誤検出が消えて
    進行の骨格だけが残る。1周ぶんの「これがこの曲のループ」を出すのが目的。
    """
    L = loop_bars * per_bar
    n_rep = len(sub) // L
    if n_rep < 2:
        return {}
    stack = sub[: n_rep * L].reshape(n_rep, L, 12)
    stack_b = sub_b[: n_rep * L].reshape(n_rep, L, 12)
    med = np.median(stack, axis=0)
    med_b = np.median(stack_b, axis=0)
    norm = med / (np.linalg.norm(med, axis=1, keepdims=True) + 1e-9)
    sn = stack / (np.linalg.norm(stack, axis=2, keepdims=True) + 1e-9)
    stability = (sn * norm[None]).sum(axis=2).mean(axis=0)  # 各スロットが何回ぶん一致しているか

    prog = []
    for i in range(L):
        name, conf = match_chord(med[i], med_b[i], names, T)
        prog.append(
            {
                "bar": i // per_bar + 1,
                "sub": i % per_bar,
                "chord": name,
                "conf": round(conf, 3),
                "stability": round(float(stability[i]), 3),
            }
        )
    return {"loop_bars": loop_bars, "repetitions_folded": n_rep, "progression": prog}


def chord_tone_agreement(midi, chords: list[dict], seg: float) -> float | None:
    """MIDIノートの発音時間のうち、その時点のコードの構成音だった割合。

    「スケール内か」より厳しく、音高そのものの正しさに効く指標。基準側のコード推定にも
    誤差があるので絶対値でなく比較用（ステム間・設定間の相対）として読む。
    """
    lut = {}
    for c in chords:
        if not c["chord"]:
            continue
        root = _root_index(c["chord"])
        suffix = c["chord"][len(PITCHES[root]) :]
        lut[round(c["t"], 3)] = {(root + d) % 12 for d in CHORD_TEMPLATES.get(suffix, [0, 4, 7])}
    if not lut:
        return None
    times = np.array(sorted(lut))
    hit = tot = 0.0
    for inst in midi.instruments:
        for n in inst.notes:
            i = int(np.searchsorted(times, n.start, side="right")) - 1
            if i < 0:
                continue
            d = n.end - n.start
            tot += d
            if n.pitch % 12 in lut[times[i]]:
                hit += d
    return round(hit / tot, 3) if tot else None


def midi_stats(midi, first_down: float, bar_len: float, key_pcs: set[int]) -> dict:
    notes = [n for inst in midi.instruments for n in inst.notes]
    if not notes:
        return {"count": 0}
    pitches = np.array([n.pitch for n in notes])
    starts = np.array([n.start for n in notes])
    durs = np.array([n.end - n.start for n in notes])
    step = bar_len / 16
    rel = (starts - first_down) / step
    dev = (rel - np.round(rel)) * step * 1000
    in_key = float(np.mean([p % 12 in key_pcs for p in pitches]))
    # 同時発音数: 各ノートの開始時に鳴っている音の数の中央値
    poly = [int(((starts <= s) & (starts + durs > s)).sum()) for s in starts[:: max(1, len(starts) // 500)]]
    return {
        "count": len(notes),
        "notes_per_bar": round(len(notes) / max(1, (starts[-1] - first_down) / bar_len), 2),
        "pitch_min": librosa.midi_to_note(int(pitches.min())),
        "pitch_max": librosa.midi_to_note(int(pitches.max())),
        "pitch_median": librosa.midi_to_note(int(np.median(pitches))),
        "dur_median_in_16ths": round(float(np.median(durs) / step), 2),
        "in_scale_ratio": round(in_key, 3),
        "grid_dev_ms_abs_mean": round(float(np.abs(dev).mean()), 1),
        "grid_dev_ms_within_30ms": round(float((np.abs(dev) < 30).mean()), 3),
        "polyphony_median": int(np.median(poly)) if poly else 0,
        "velocity_median": int(np.median([n.velocity for n in notes])),
    }


def timbre(y: np.ndarray, sr: int) -> dict:
    """音色の質感を数値にする。「暗い/明るい」「刺す/包む」を後で言葉にするための材料。"""
    S = np.abs(librosa.stft(y, n_fft=2048, hop_length=HOP))

    # アタック/サステインはサンプル解像度の振幅包絡で測る。
    # STFT の hop（23ms）だと「叩く音」と「立ち上がる音」の差（数ms〜100ms）が丸まって
    # どのステムでも同じ値になってしまう（実測で全ステム 325ms に張り付いた）。
    n = max(1, int(sr * 0.003))
    amp = np.convolve(np.abs(y), np.ones(n) / n, mode="same")
    rise = np.diff(amp, prepend=amp[0])
    rise[rise < 0] = 0
    step = int(sr * 0.05)
    # 閾値はループの外で1回だけ計算する（条件式の中に書くと 50ms ごとに
    # 500万要素の percentile を引き直して、この関数だけで2分半かかる）
    thresh = float(np.percentile(rise, 99.5))
    peak_per_step = np.maximum.reduceat(rise[: (len(rise) // step) * step], np.arange(0, (len(rise) // step) * step, step))
    cand = (np.flatnonzero(peak_per_step > thresh) * step).tolist()

    atk, sus = [], []
    win = int(sr * 0.25)
    for c in cand:
        seg = amp[c : c + win]
        if len(seg) < win // 2:
            continue
        pk = int(np.argmax(seg))
        if pk == 0:
            continue
        # 立ち上がり: ピークの 10% を超えた点からピークまでの時間
        lo = np.where(seg[:pk] <= seg[pk] * 0.1)[0]
        atk.append((pk - (lo[-1] if len(lo) else 0)) / sr)
        tail = amp[c + pk + win : c + pk + 2 * win]
        if len(tail):
            sus.append(float(tail.mean() / (seg[pk] + 1e-12)))

    return {
        "attack_ms_median": round(float(np.median(atk) * 1000), 1) if atk else None,
        "sustain_ratio_250ms": round(float(np.median(sus)), 3) if sus else None,
        "centroid_hz_median": round(float(np.median(librosa.feature.spectral_centroid(S=S, sr=sr))), 1),
        "centroid_hz_iqr": round(
            float(np.subtract(*np.percentile(librosa.feature.spectral_centroid(S=S, sr=sr), [75, 25]))), 1
        ),
        "flatness_median": round(float(np.median(librosa.feature.spectral_flatness(S=S))), 5),
        "bandwidth_hz_median": round(float(np.median(librosa.feature.spectral_bandwidth(S=S, sr=sr))), 1),
        "onsets_per_sec": round(len(cand) / (len(y) / sr), 2),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("stem")
    ap.add_argument("bass")
    ap.add_argument("basics")
    ap.add_argument("outdir")
    ap.add_argument("--label", default="other")
    ap.add_argument("--per-bar", type=int, default=2, help="1小節を何分割してコードを当てるか")
    args = ap.parse_args()

    basics = json.loads(Path(args.basics).read_text())
    bpm = basics["tempo"]["bpm"]
    bar_len = 4 * 60.0 / bpm
    first_down = basics["grid"]["first_downbeat_sec"]
    outdir = Path(args.outdir)

    harm, sr = librosa.load(args.stem, sr=SR, mono=True)
    bass, _ = librosa.load(args.bass, sr=SR, mono=True)
    n_bars = int((len(harm) / sr - first_down) / bar_len)

    names, T = chord_templates()
    sub, chroma, ct = sub_chroma(harm, sr, first_down, bar_len, n_bars, args.per_bar)
    sub_b, _, _ = sub_chroma(bass, sr, first_down, bar_len, n_bars, args.per_bar, bass=True)

    chords = []
    for i in range(len(sub)):
        name, conf = match_chord(sub[i], sub_b[i], names, T)
        chords.append(
            {
                "bar": i // args.per_bar + 1,
                "sub": i % args.per_bar,
                "t": round(float(first_down + i * bar_len / args.per_bar), 3),
                "chord": name,
                "conf": round(conf, 3),
            }
        )
    loops = loop_length(sub, args.per_bar)
    loop_bars = int(loops["most_likely"].replace("bar", "")) if loops["most_likely"] else 4

    result = {
        "stem": args.label,
        "bpm": bpm,
        "n_bars": n_bars,
        "loop": loops,
        "loop_progression": folded_progression(sub, sub_b, args.per_bar, loop_bars, names, T),
        "chords": chords,
        "timbre": timbre(harm, sr),
    }

    # --- basic-pitch で MIDI 化 ---
    # 既定は TF SavedModel だが、TF 2.16(Keras 3) では読めない（basic-pitch 0.3.0 は
    # py3.12 では TF 2.16 しか入らない＝この組み合わせでは既定が必ず落ちる）。ONNX を明示する。
    import scipy.signal

    # basic-pitch 0.3.0 は scipy 1.13 で windows 配下に移動した scipy.signal.gaussian を
    # ピッチベンド推定で直接呼ぶ。librosa 側が新しい scipy を要求するので、こちらを戻す。
    if not hasattr(scipy.signal, "gaussian"):
        scipy.signal.gaussian = scipy.signal.windows.gaussian

    from basic_pitch import FilenameSuffix, build_icassp_2022_model_path
    from basic_pitch.inference import predict

    _, midi, _ = predict(args.stem, build_icassp_2022_model_path(FilenameSuffix.onnx))
    midi_path = outdir / f"topline-{args.label}.mid"
    midi.write(str(midi_path))

    key_name = basics["key"]["top5"][0]["key"]
    root = PITCHES.index(key_name.split()[0])
    scale = [0, 2, 4, 5, 7, 9, 11] if "major" in key_name else [0, 2, 3, 5, 7, 8, 10]
    key_pcs = {(root + s) % 12 for s in scale}
    result["midi"] = midi_stats(midi, first_down, bar_len, key_pcs)
    result["midi"]["chord_tone_ratio"] = chord_tone_agreement(midi, chords, bar_len / args.per_bar)
    result["midi"]["scale_used_for_check"] = key_name
    result["midi"]["file"] = midi_path.name

    # 原音(L) と MIDI のサイン波(R) を左右に振った確認用wav。ズレ・幻の音が一発で分かる。
    syn = midi.synthesize(fs=sr)
    n = max(len(harm), len(syn))
    stereo = np.zeros((n, 2))
    stereo[: len(harm), 0] = harm / (np.abs(harm).max() + 1e-9) * 0.8
    stereo[: len(syn), 1] = syn / (np.abs(syn).max() + 1e-9) * 0.5
    sf.write(outdir / f"midi_check-{args.label}.wav", stereo, sr)

    (outdir / f"topline-{args.label}.json").write_text(json.dumps(result, indent=2, ensure_ascii=False))

    # --- 図: 上=畳み込んだループ1周（読む用）/ 下=先頭16小節の実クロマ（生データ確認用） ---
    prog = result["loop_progression"].get("progression", [])
    fig, ax = plt.subplots(2, 1, figsize=(18, 9))

    if prog:
        L = len(prog)
        folded = np.array(
            [np.median(sub[: (len(sub) // L) * L].reshape(-1, L, 12), axis=0)[i] for i in range(L)]
        ).T
        ax[0].imshow(folded, aspect="auto", origin="lower", cmap="magma", interpolation="nearest")
        ax[0].set_yticks(range(12))
        ax[0].set_yticklabels(PITCHES)
        ax[0].set_xticks(range(L))
        ax[0].set_xticklabels([f"{p['bar']}.{p['sub'] + 1}" for p in prog])
        for i, p in enumerate(prog):
            if p["chord"]:
                ax[0].text(i, 11.6, p["chord"], color="white", fontsize=9, ha="center", va="top")
            if p["sub"] == 0:
                ax[0].axvline(i - 0.5, color="cyan", lw=1.4)
        n_rep = result["loop_progression"]["repetitions_folded"]
        # 図中は ASCII のみ（matplotlib に日本語フォントが無く豆腐になる）
        ax[0].set_title(f"folded loop ({args.label}) — {result['loop_progression']['loop_bars']} bars, median of {n_rep} repetitions")

    show = 16
    sel = (ct >= first_down) & (ct < first_down + show * bar_len)
    librosa.display.specshow(
        chroma[:, sel], y_axis="chroma", x_axis="time", sr=sr, hop_length=HOP, ax=ax[1], cmap="magma"
    )
    for c in chords[: show * args.per_bar]:
        x = c["t"] - first_down
        ax[1].axvline(x, color="white", alpha=0.5 if c["sub"] else 0.9, lw=0.8 if c["sub"] else 1.6)
        if c["chord"]:
            ax[1].text(x + 0.05, 11.4, c["chord"], color="white", fontsize=8, va="top")
    ax[1].set_title(f"raw chroma & per-segment guess (first {show} bars) — this is how noisy it is before folding")
    fig.tight_layout()
    fig.savefig(outdir / f"chords-{args.label}.png", dpi=110)

    slim = json.loads(json.dumps(result))
    slim["chords"] = slim["chords"][: 8 * args.per_bar]
    print(json.dumps(slim, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
