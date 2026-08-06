#!/usr/bin/env python
"""ガチャ生成器（drums.py / bass.py）の共通部 — seed 導出・候補の同一性・原子的公開。

生成器ごとに違うのは「パターンの作り方」だけで、以下の契約は全生成器で共通:
- 決定性: (実効設定, レーン seed) が同じならバイト単位で同じ出力
- 同名なら同内容: ファイル名に レーン seed＋設定ハッシュ を埋め、期待と違う既存物はエラー
- 原子的公開: 一時ファイル→検算→リネーム。サイドカー .json は最後＝完成マーカー
"""
import hashlib
import json
import os
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf


class GachaError(Exception):
    """カードの壊れた値・候補ファイルの衝突など、黙って進んではいけない状態。"""


def is_num(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


# ---------------------------------------------------------------- seed 導出

def derive_lane_seed(generator: str, lane: str, global_seed: int) -> int:
    """全体 seed からレーン seed を決定的に導出する（32bit）。

    Python の hash() は PYTHONHASHSEED でプロセスごとに変わるので SHA-256 を使う。
    generator 名を混ぜるので、同じ全体 seed でも drums と bass のレーン seed は別になる。
    """
    digest = hashlib.sha256(f"{generator}:{lane}:{global_seed}".encode()).digest()
    return int.from_bytes(digest[:4], "big")


def derive_audio_seed(lane: str, lane_seed: int) -> int:
    """音声合成用 seed。パターン生成用と独立に導出する（wav の決定性のため）。"""
    digest = hashlib.sha256(f"audio:{lane}:{lane_seed:08x}".encode()).digest()
    return int.from_bytes(digest[:8], "big")


def resolve_lane_seeds(generator: str, lanes: tuple[str, ...], global_seed: int,
                       locks: dict[str, int]) -> dict[str, int]:
    return {lane: locks.get(lane, derive_lane_seed(generator, lane, global_seed)) for lane in lanes}


def parse_locks(text: str, lanes: tuple[str, ...]) -> dict[str, int]:
    """--lock kick=8f3a21bc,hat=00ff00ff（ファイル名と同じ8桁hex）を読む。"""
    locks: dict[str, int] = {}
    hint = ",".join(f"{lane}=HEX8" for lane in lanes)
    for part in text.split(","):
        lane, sep, value = part.partition("=")
        lane = lane.strip()
        if not sep or lane not in lanes:
            raise GachaError(f"--lock の形式が不正: {part!r}（{hint}）")
        try:
            seed = int(value.strip(), 16)
        except ValueError:
            raise GachaError(f"--lock {lane} の seed が16進数でない: {value!r}") from None
        if not (0 <= seed <= 0xFFFFFFFF):
            raise GachaError(f"--lock {lane} の seed が32bitの範囲外: {value}")
        locks[lane] = seed
    return locks


def resolve_card_path(arg: str) -> Path:
    p = Path(arg).expanduser()
    if p.is_dir():
        p = p / "card.json"
    if not p.exists():
        raise GachaError(f"card.json が見つからない: {p}")
    return p


# ---------------------------------------------------------------- 候補の同一性

def cfg_hash(payload: dict) -> str:
    return hashlib.sha256(json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def candidate_hash(payload: dict, lane_seeds_hex: dict[str, str]) -> str:
    """候補1件の完全な同一性（実効設定＋レーン seed）のハッシュ。

    cfg_hash は設定だけでレーン seed を含まないため、--from の照合を cfg_hash だけに
    頼ると seed の改変を検出できない（実測で hat seed 差し替えが素通りした）。
    """
    body = {"config": payload, "lane_seeds": lane_seeds_hex}
    return hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def candidate_status(outdir: Path, base: str, payload: dict, lane_seeds_hex: dict[str, str]) -> str:
    """'new' / 'skip'（完成済み）/ 'regen'（不完全）。期待との不一致は衝突・編集なのでエラー。

    完成判定は --from の照合と同じ基準で行う。基準がずれると「スキップは通るのに --from は
    失敗する」半端な候補が残り続ける。保存済みハッシュだけを比較すると本文（config）の編集を
    見逃すため、**本文そのものを期待値と突き合わせる**（本文が一致すれば本文からの再計算
    ハッシュは期待ハッシュと必然一致する）。判定順が肝心: 本文と cfg の検証を先に済ませてから
    candidate_sha256 の欠損（＝旧形式。再生成で現行形式に上がる）を regen 扱いにする。
    欠損チェックを先に返すと、欠損＋cfg 不一致の壊れたサイドカーが衝突エラーを迂回して
    黙って上書きされる。
    """
    cfg_sha = cfg_hash(payload)
    cand_sha = candidate_hash(payload, lane_seeds_hex)
    mid, wav, sc = (outdir / f"{base}{ext}" for ext in (".mid", ".wav", ".json"))
    if sc.exists():
        try:
            old = json.loads(sc.read_text())
        except (OSError, json.JSONDecodeError):
            return "regen"  # 壊れた書きかけ＝不完全候補として作り直す
        if (old.get("cfg_sha256") != cfg_sha or old.get("lane_seeds") != lane_seeds_hex
                or old.get("config") != payload):
            raise GachaError(
                f"{base}: 既存サイドカーと生成入力が一致しない（短縮ハッシュ衝突か編集の疑い）。"
                f"既存の候補を手で退避してから再実行すること"
            )
        if "candidate_sha256" not in old:
            return "regen"
        if old["candidate_sha256"] != cand_sha:
            raise GachaError(
                f"{base}: 既存サイドカーの candidate ハッシュが一致しない（編集の疑い）。"
                f"既存の候補を手で退避してから再実行すること"
            )
        return "skip" if mid.exists() and wav.exists() else "regen"
    if mid.exists() or wav.exists():
        return "regen"  # サイドカー（完成マーカー）が無い＝途中失敗の残骸
    return "new"


# ---------------------------------------------------------------- wav の検算・原子的公開

def verify_wav_data(data: np.ndarray, label: str) -> None:
    """書き込む前の float 配列を検算する。peak は PCM 化で黙ってクリップされ読み戻しでは検出できない。"""
    if not np.all(np.isfinite(data)):
        raise GachaError(f"{label}: wav に有限でないサンプルがある")
    peak = float(np.max(np.abs(data))) if len(data) else 0.0
    if peak > 0.999:
        raise GachaError(f"{label}: wav が 0dBFS を超えている（peak={peak:.3f}。同時発音のクリップ）")


def verify_wav_file(path: Path, label: str, sample_rate: int) -> None:
    """書き出したファイルを読み戻して検算する（「ファイルがある・長さが正しい」では確認しない）。"""
    data, sr = sf.read(path)
    if sr != sample_rate:
        raise GachaError(f"{label}: サンプルレートが不正: {sr}")
    if not np.all(np.isfinite(data)):
        raise GachaError(f"{label}: 読み戻した wav に有限でないサンプルがある")
    rms = float(np.sqrt(np.mean(np.square(data)))) if len(data) else 0.0
    if rms < 1e-3:
        raise GachaError(f"{label}: wav がほぼ無音（rms={rms:.2e}）")


def publish_candidate(outdir: Path, base: str, mid, wav_data: np.ndarray,
                      sidecar: dict, sample_rate: int) -> None:
    """一時ファイル→検算→リネームで原子的に公開する。サイドカーは最後＝完成マーカー。"""
    verify_wav_data(wav_data, base)
    tmp_paths = []
    try:
        for ext, write in ((".mid", lambda p: mid.save(p)),
                           (".wav", lambda p: sf.write(p, wav_data, sample_rate, subtype="PCM_16"))):
            fd, tmp = tempfile.mkstemp(dir=outdir, prefix=".gacha-", suffix=ext)
            os.close(fd)
            tmp_paths.append((tmp, outdir / f"{base}{ext}"))
            write(tmp)
        verify_wav_file(Path(tmp_paths[1][0]), base, sample_rate)
        for tmp, dest in tmp_paths:
            os.replace(tmp, dest)
        tmp_paths = []
        fd, tmp = tempfile.mkstemp(dir=outdir, prefix=".gacha-", suffix=".json")
        with os.fdopen(fd, "w") as f:
            f.write(json.dumps(sidecar, indent=2, ensure_ascii=False) + "\n")
        os.replace(tmp, outdir / f"{base}.json")
    finally:
        for tmp, _ in tmp_paths:
            Path(tmp).unlink(missing_ok=True)


def write_mix_wav(outdir: Path, name: str, data: np.ndarray, sample_rate: int) -> Path:
    """耳チェック用 mix wav（使い捨て成果物）の書き出し。候補の同一性・skip 判定に関与しない。

    正規候補と違い完成マーカーを持たない（壊れていたら再実行すれば上書きされるだけ）。
    書きかけを見せないための atomic 置換だけは共通。
    """
    verify_wav_data(data, name)
    fd, tmp = tempfile.mkstemp(dir=outdir, prefix=".gacha-", suffix=".wav")
    os.close(fd)
    try:
        sf.write(tmp, data, sample_rate, subtype="PCM_16")
        verify_wav_file(Path(tmp), name, sample_rate)
        dest = outdir / name
        os.replace(tmp, dest)
        return dest
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise
