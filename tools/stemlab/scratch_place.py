#!/usr/bin/env python3
"""Phase 3.5: reference回帰用のscratchコピーを作り、SWステムを分析器の枠へ配置する。

analyze.py は stems/htdemucs/track/ と stems/htdemucs_6s/track/ をハードコードしているため、
分析器は改修せず「枠へ中身を差し替えたコピー」を作って回す。

モード:
  uppers  … 6分割枠へSWの6ステムを配置。4分割枠は現行demucs出力のまま（上モノ評価用）
  lowend  … 4分割枠へ SW bass/drums/vocals＋other4(=piano+guitar+otherの生float加算) を配置。
            6分割枠は現行demucsのまま（ベース・ドラム評価用）

ステムは現行demucsの条件に合わせ44.1kHzへsoxrでresampleして配置する（分析器が普段見るSRと揃える）。
使い方: python scratch_place.py <song> <mode> <scratch出力dir>
"""
import shutil
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
import soxr

HERE = Path(__file__).parent
REFS = Path.home() / "Music/daw/references"
SW_RUN = "run1"  # 耳判定と同じrunを使う
TARGET_SR = 44100  # demucs出力と同条件


def load_sw(song: str, stem: str) -> np.ndarray:
    p = HERE / "runs" / song / "sw" / SW_RUN / f"track_{stem}.wav"
    x, sr = sf.read(str(p), always_2d=True, dtype="float64")
    if sr != TARGET_SR:
        x = soxr.resample(x, sr, TARGET_SR)
    return x


def write(path: Path, x: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), x.astype(np.float32), TARGET_SR, subtype="FLOAT")


def main() -> int:
    song, mode, out = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    src = REFS / song
    out.mkdir(parents=True, exist_ok=True)

    # track.wav と既存stemsをコピー（analysisは持ち込まない=全ステップ再計算させる）
    shutil.copy2(src / "track.wav", out / "track.wav")
    shutil.copytree(src / "stems", out / "stems", dirs_exist_ok=True)

    placed = {}
    if mode == "uppers":
        frame = out / "stems/htdemucs_6s/track"
        for stem in ("bass", "drums", "guitar", "other", "piano", "vocals"):
            write(frame / f"{stem}.wav", load_sw(song, stem))
            placed[f"htdemucs_6s/{stem}"] = "SW"
    elif mode == "lowend":
        frame = out / "stems/htdemucs/track"
        for stem in ("bass", "drums", "vocals"):
            write(frame / f"{stem}.wav", load_sw(song, stem))
            placed[f"htdemucs/{stem}"] = "SW"
        # other4 = piano + guitar + other の生float加算（SWのotherだけだと上モノ欠落と誤判定される）
        parts = [load_sw(song, s) for s in ("piano", "guitar", "other")]
        n = min(len(p) for p in parts)
        write(frame / "other.wav", sum(p[:n] for p in parts))
        placed["htdemucs/other"] = "SW piano+guitar+other 加算"
    else:
        print(f"不明なmode: {mode}")
        return 1

    # 「各指標が実際に読んだモデル」の記録（枠名≠中身のため必須）
    lines = [f"# scratch配置の記録 ({mode})", f"song: {song}", f"SW run: {SW_RUN}", ""]
    lines += [f"- {k}: {v}" for k, v in placed.items()]
    lines += ["- 上記以外の枠: 現行demucs出力のまま", f"- 配置SR: {TARGET_SR}Hz (soxr resample)"]
    (out / "PLACEMENT.md").write_text("\n".join(lines))
    print(f"scratch配置完了: {out}（PLACEMENT.md に実際の中身を記録）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
