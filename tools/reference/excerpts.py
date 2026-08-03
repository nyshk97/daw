#!/usr/bin/env python
"""耳で確認するための短いクリップを `listen/` に切り出す。

分析結果を人間が検算するとき、秒数を計算して長い音源をシークするのが一番の障害になる。
確認したい箇所だけを、確認する順に番号を振ったファイルにしておけば
Finder でスペースキー（Quick Look）を連打するだけで一周できる。

出力: <ref>/listen/*.wav と listen/README.md

使い方: excerpts.py <リファレンスフォルダ>
"""
import argparse
import json
import warnings
from pathlib import Path

warnings.filterwarnings("ignore")

import librosa
import numpy as np
import soundfile as sf

SR = 44100
PITCHES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
CHORD_DEGREES = {
    "": [0, 4, 7], "m": [0, 3, 7], "7": [0, 4, 7, 10], "maj7": [0, 4, 7, 11],
    "m7": [0, 3, 7, 10], "m9": [0, 3, 7, 10, 14], "maj9": [0, 4, 7, 11, 14],
    "6": [0, 4, 7, 9], "m6": [0, 3, 7, 9], "add9": [0, 2, 4, 7],
    "sus4": [0, 5, 7], "7sus4": [0, 5, 7, 10], "m7b5": [0, 3, 6, 10],
    "dim": [0, 3, 6], "5": [0, 7],
}


def cut(src: Path, dst: Path, t0: float, t1: float, lead: float = 0.3) -> None:
    """切り出して両端にごく短いフェードを掛ける。頭に lead 秒ぶん余白を足して1拍目を切らない。

    ffmpeg は使わない。`-ss`/`-to` を `-i` の後ろに置くと出力オプション扱いになる一方で
    `-af` のフィルタは入力側のタイムスタンプを見るため、`afade=t=out:st=<クリップ内の秒>`
    が「とっくに過ぎた時刻」と解釈されてクリップ全体が無音になる（実際にこれで14本が無音になった）。
    サンプル単位で切って掛ける方が短く、取り違えようがない。
    """
    info = sf.info(str(src))
    start = max(0, int((t0 - lead) * info.samplerate))
    stop = min(info.frames, int(t1 * info.samplerate))
    y, sr = sf.read(str(src), start=start, stop=stop, always_2d=True, dtype="float32")
    n_in, n_out = int(sr * 0.02), int(sr * 0.05)
    if len(y) > n_in + n_out:
        y[:n_in] *= np.linspace(0, 1, n_in)[:, None]
        y[-n_out:] *= np.linspace(1, 0, n_out)[:, None]
    sf.write(str(dst), y, sr)


def chord_pad(prog: list[dict], bar_len: float, per_bar: int, n_loops: int) -> np.ndarray:
    """推定したコードを柔らかいサイン波の和音として鳴らす。

    絶対音感が無くても「合っているか」は判定できる — 合っていれば曲に溶け、
    違っていれば濁って聴こえる。コード名を読むより速くて確実な検算方法。
    """
    seg = bar_len / per_bar
    out = np.zeros(int(SR * seg * len(prog) * n_loops))
    for rep in range(n_loops):
        for i, p in enumerate(prog):
            if not p["chord"]:
                continue
            name = p["chord"]
            root = PITCHES.index(name[:2]) if len(name) > 1 and name[1] == "#" else PITCHES.index(name[0])
            suffix = name[len(PITCHES[root]) :]
            start = int(SR * seg * (i + rep * len(prog)))
            n = int(SR * seg)
            t = np.arange(n) / SR
            # 和音ごと平行移動してルートを G3-G4 に置く（度数ごとに折り返すと転回形になり、
            # ルートが3度より上に来て響きが変わってしまう）。低すぎると曲の低域に埋もれる
            base = 48 + root
            while base < 55:
                base += 12
            for d in CHORD_DEGREES.get(suffix, [0, 4, 7]):
                out[start : start + n] += np.sin(2 * np.pi * librosa.midi_to_hz(base + d) * t) / 6
            env = np.minimum(1.0, np.minimum(t / 0.03, (seg - t) / 0.08))
            out[start : start + n] *= np.clip(env, 0, 1)
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    args = ap.parse_args()

    ref = Path(args.ref).expanduser()
    a = ref / "analysis"
    out = ref / "listen"
    out.mkdir(exist_ok=True)
    for old in out.glob("*"):
        old.unlink()

    basics = json.loads((a / "basics.json").read_text())
    bpm = basics["tempo"]["bpm"]
    bar_len = 4 * 60.0 / bpm
    fd = basics["grid"]["first_downbeat_sec"]

    def bar_t(n: int) -> float:
        return fd + (n - 1) * bar_len

    notes: list[tuple[str, str]] = []

    # 1) BPM と小節頭 — 曲頭と曲尾でクリックがズレていないか
    for tag, b0, b1 in (("head", 1, 17), ("tail", 77, 93)):
        f = f"01-click-{tag}-bar{b0}-{b1 - 1}.wav"
        cut(a / "click.wav", out / f, bar_t(b0), bar_t(b1))
        notes.append((f, f"高いクリック＝小節頭、低い＝拍。{b0}-{b1 - 1}小節目。曲頭と曲尾の両方でズレていなければ BPM {bpm} は正しい"))

    # 2) コード — 推定した和音を重ねて、濁らないか
    prog = json.loads((a / "topline-6s-piano.json").read_text())["loop_progression"]
    per_bar = 2
    loop_bars = prog["loop_bars"]
    b0 = 13  # 全パートが揃っていて、かつループの頭に当たる小節
    mix, _ = librosa.load(str(ref / "track.wav"), sr=SR, mono=True, offset=bar_t(b0), duration=bar_len * loop_bars * 2)
    pad = chord_pad(prog["progression"], bar_len, per_bar, 2)
    n = min(len(mix), len(pad))
    sf.write(out / "02-chords-check-bar13-20.wav", np.clip(mix[:n] * 0.85 + pad[:n] * 0.5, -1, 1), SR)
    names = " → ".join(p["chord"] or "-" for p in prog["progression"])
    notes.append(("02-chords-check-bar13-20.wav", f"推定コードをサイン波で重ねたもの（{names}）。**合っていれば曲に溶け、違えば濁る**。2小節目後半と4小節目後半だけ推定が揺れているので、そこが濁らないか"))

    # 02 と並べて聴くので、こちらは頭の余白を入れない（0.3秒ずれると比較しづらい）
    cut(ref / "track.wav", out / "03-loop-only-bar13-20.wav", bar_t(b0), bar_t(b0 + loop_bars * 2), lead=0.0)
    notes.append(("03-loop-only-bar13-20.wav", "同じ区間のコード無し版。02 と聴き比べる用"))

    # 3) ステムの分離品質 — 一番大きい小節と、鳴っているが小さい小節
    stems = json.loads((a / "stems-6s.json").read_text())
    for name, d in stems["stems"].items():
        for kind, key in (("loud", "listen_bars_loudest"), ("quiet", "listen_bars_quiet")):
            bars = d[key][:1]
            if not bars:
                continue
            b = bars[0]
            f = f"04-stem-{name}-{kind}-bar{b}.wav"
            cut(ref / "stems" / "htdemucs_6s" / "track" / f"{name}.wav", out / f, bar_t(b), bar_t(b + 4))
            notes.append((f, f"{name} の{'一番大きい' if kind == 'loud' else '鳴っているが小さい'}区間（{b}-{b + 3}小節）。"
                             + ("素性が出る" if kind == "loud" else "**分離の破綻・幽霊音はここで一番聴こえる**")))

    # 4) 音声→MIDI の精度
    for label in ("6s-piano", "other"):
        src = a / f"midi_check-{label}.wav"
        if src.exists():
            f = f"05-midi-{label}-bar13-20.wav"
            cut(src, out / f, bar_t(b0), bar_t(b0 + loop_bars * 2))
            notes.append((f, f"左=原音（{label}）/ 右=basic-pitch の結果をサイン波で。音高が合っているか、刻まれ方がどれだけ実態とズレているか"))

    (out / "README.md").write_text(
        "# 耳で確認するクリップ\n\n"
        "番号順に聴けば一周できる。Finder でこのフォルダを開き、ファイルを選んで**スペースキー**で再生するのが速い。\n"
        "（ターミナルなら `afplay <ファイル>`、フォルダごとなら `for f in *.wav; do echo $f; afplay $f; done`）\n\n"
        f"1小節 = {bar_len:.3f}秒 / {bpm} BPM / 1小節目の頭 = {fd:.3f}秒\n\n"
        + "".join(f"### {f}\n{n}\n\n" for f, n in notes)
        + "\n## 聴きながらメモしてほしいこと\n\n"
        "- 各クリップが OK か NG か（NG ならどう聴こえたか一言）\n"
        "- **「特にここが好き」な瞬間を2〜3個**（秒でも小節番号でもよい）。次の曲の分析でそこを重点的に見る\n"
    )

    # 書き出したクリップに実際に音が入っているかを自分で検算する。
    # 「ファイルがある・長さが正しい」だけ見ていたせいで、中身が全部無音のまま
    # レビューを依頼してしまったことがある。長さでなく RMS を見る。
    print(f"{len(notes)} 本を {out} に書き出した\n")
    silent = []
    for f, n in notes:
        y, sr = sf.read(str(out / f), always_2d=True, dtype="float32")
        m = y.mean(axis=1)
        rms = 20 * np.log10(max(float(np.sqrt((m**2).mean())), 1e-9))
        peak = 20 * np.log10(max(float(np.abs(m).max()), 1e-9))
        mark = "無音" if peak < -60 else ("小さい" if rms < -45 else "ok  ")
        if peak < -60:
            silent.append(f)
        print(f"  [{mark}] {f}  RMS {rms:.1f}dB / peak {peak:.1f}dB\n         {n}")
    if silent:
        raise SystemExit(f"\nERROR: 無音のクリップがある: {silent}")


if __name__ == "__main__":
    main()
