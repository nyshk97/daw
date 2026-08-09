#!/usr/bin/env python
"""サンプルライブラリのインデックス作成 — loops をスキャンして index.json を作る。

おすすめ5（recommend.py）が読む土台。ライブラリの各ループについて、
ハードフィルタ用のメタデータ（キー・BPM・ループ小節数）と、ランキング用の特徴量
（明るさ＝帯域重心・帯域バランス・音の密度・叩く音と伸びる音の比）を1回だけ計算して貯める。
パック追加時に手で回す低頻度処理（feature-scope の「低頻度は別プロセスへ」）。

設計の要点:
- 対象は loops/ と _contrast/loops/ のみ（oneshots はワンショットサンプラー実装時に +1）
- パスは**ライブラリ相対**で持つ（index.json は iCloud でマシン間同期されるため、絶対パス禁止）
- キー/BPM はファイル名の慣習（"82 BPM G Min" / "Am_140bpm" 等）を第一情報源にし、
  無いときだけ音声から推定する（推定は filename より信頼度が落ちるので出所を必ず記録する）
- 差分更新: size+mtime が一致する既存エントリは再分析しない。消えたファイルは index から落ちる
- 書き込みは一時ファイル → os.replace の原子的置き換え（同期中の壊れた index を作らない）

使い方: index.py [--library ~/Music/daw/library] [--force]
"""
import argparse
import json
import os
import re
import sys
import warnings
from pathlib import Path

warnings.filterwarnings("ignore")

import librosa
import numpy as np

# キー推定（Krumhansl-Kessler 相関）は分析パイプラインの実装を単一の真実の源にする
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "reference"))
from basics import PITCHES, estimate_key  # noqa: E402

SCHEMA_VERSION = 2  # v2: loop_bars_estimate がテール検知＋30ms丸めに（旧版エントリは全再分析）
SR = 22050
AUDIO_EXTS = {".wav", ".aif", ".aiff", ".flac", ".mp3"}
BPM_MIN, BPM_MAX = 40, 220
DEFAULT_LIBRARY = Path.home() / "Music" / "daw" / "library"

NOTE_TO_PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}

# ファイル名のキー表記: 区切り（先頭/空白/_-.()[]）に続く 音名＋変化記号＋モード。
# モード表記が無い裸の音名（"Ab" 単独等）は誤検出が多いので拾わない → 音声推定に回す。
_SEP = r"[\s_\-\.\(\)\[\]]"
KEY_RE = re.compile(
    rf"(?i)(?:^|{_SEP})([A-G])([#♯b♭]?)[\s_\-]*(maj(?:or)?|min(?:or)?|m)(?=$|{_SEP}|\d)"
)
# BPM 表記: "82bpm" / "82 BPM" / "bpm82"。数字だけのトークンは年号や連番と区別できないので拾わない。
# 例外はキー表記の直前の数字（"126 A Min" — Cymatics 旧世代の命名）で、これは BPM とみなせる
BPM_RE_A = re.compile(rf"(?i)(?:^|{_SEP})(\d{{2,3}})[\s_\-]*bpm(?=$|{_SEP})")
BPM_RE_B = re.compile(rf"(?i)(?:^|{_SEP})bpm[\s_\-]*(\d{{2,3}})(?=$|{_SEP})")
BPM_RE_C = re.compile(rf"(?i)(?:^|{_SEP})(\d{{2,3}})[\s_\-]+[A-G][#♯b♭]?[\s_\-]*(?:maj(?:or)?|min(?:or)?|m)(?=$|{_SEP}|\d)")


def parse_filename_meta(stem: str) -> dict:
    """ファイル名（拡張子抜き）からキーと BPM を読む。無いものは None。"""
    out = {"bpm": None, "key_root": None, "key_mode": None}

    m = BPM_RE_A.search(stem) or BPM_RE_B.search(stem) or BPM_RE_C.search(stem)
    if m:
        bpm = int(m.group(1))
        if BPM_MIN <= bpm <= BPM_MAX:
            out["bpm"] = float(bpm)

    m = KEY_RE.search(stem)
    if m:
        letter, accidental, mode = m.group(1).upper(), m.group(2), m.group(3).lower()
        pc = NOTE_TO_PC[letter]
        if accidental in ("#", "♯"):
            pc += 1
        elif accidental in ("b", "♭"):
            pc -= 1
        out["key_root"] = pc % 12
        # "m" 単独と "min..." は minor、"maj..." は major
        out["key_mode"] = "major" if mode.startswith("maj") else "minor"
    return out


# テール検知の閾値（いずれもファイル全体ピーク比dB）。実測の根拠: Cymatics Piano Wet 6 で
# 実音小節 -10〜-14dB / 余韻小節 -36dB / 無音小節 -91dB（docs/plans/2026-08-09-0024）。
# RMS 単独だと「最後に短い音が1つだけの疎な小節」（無音時間が平均を下げる）をテール扱いする
# ので、ピーク条件が「意図した音が1つでもあるか」を守る — 両方満たすときだけ切る
BAR_TAIL_RMS_DB = -30.0
BAR_TAIL_PEAK_DB = -12.0  # 余韻小節の頭は減衰の始まりでピーク -15dB 前後になる（実測）。
                          # 意図した音（鳴らすために置かれた音）はほぼ確実にこれより大きい
# 切り上げ丸めの絶対上限。±8%は比率だと 40bpm・4小節で約1.9秒にもなり、採用時の無音埋めで
# 毎周その長さの穴が開く。30ms は Phase 2（クリップ刻み）の無音埋め上限と同値
ROUND_UP_MAX_SHORTFALL_S = 0.030


def estimate_bars(y: np.ndarray, sr: int, bpm: float) -> int | None:
    """ループ小節数（4/4）の推定。末尾の「テール（余韻・無音）として説明できる小節」を
    落としてから整数化する。整数化に使ってよい不足はテールと 30ms 以内の書き出し誤差だけで、
    それ以外は None — 曖昧なら切らない（間違った小節グリッドでアンカーにする方が、進行検出・
    ベース追従・クリップ刻みの全部に効いて害が大きい）。"""
    if bpm <= 0 or sr <= 0 or len(y) == 0:
        return None
    bar_s = 240.0 / bpm  # 240 = 60秒 × 4拍
    duration = len(y) / sr
    # 30ms 以内のはみ出しは端数に数えない（切り上げ許容と対称）。リサンプルの丸めで
    # 「1サンプルだけの部分小節」ができ、無意味な小節メトリクスで判定が壊れる実例があった
    slop = ROUND_UP_MAX_SHORTFALL_S
    total = int(np.ceil((duration - slop) / bar_s))
    if total < 1:
        return None
    keep = total
    peak = float(np.abs(y).max())
    if peak > 0.0:
        for b in range(total - 1, 0, -1):  # 先頭小節は必ず残す（1小節未満にしない）
            seg = y[int(b * bar_s * sr): int((b + 1) * bar_s * sr)]
            if len(seg) == 0:
                keep = b
                continue
            rms_db = 20 * np.log10(max(float(np.sqrt((seg ** 2).mean())), 1e-12) / peak)
            peak_db = 20 * np.log10(max(float(np.abs(seg).max()), 1e-12) / peak)
            if rms_db <= BAR_TAIL_RMS_DB and peak_db <= BAR_TAIL_PEAK_DB:
                keep = b
            else:
                break
    if keep < total:
        return keep  # テールを落とした残りはグリッドの整数小節（説明できる不足だけで整数化できた）
    # テール無し: 最終小節が部分小節なら切り上げになる。許すのは不足が 30ms 以内
    # （かつ従来の±8%比率）のときだけ — それ以外は「説明できない不足」なので不明
    shortfall = keep * bar_s - duration
    if shortfall <= min(ROUND_UP_MAX_SHORTFALL_S, 0.08 * keep * bar_s) + 1e-9:
        return keep
    return None


def compute_features(y: np.ndarray, sr: int) -> dict:
    """ランキング用の特徴量。ループ（index.py）とリファレンスの上モノ合算（recommend.py）の
    両方がこの1関数を通る — 距離を取る2者の計算方法を揃えるため、ここ以外に実装を作らない。"""
    duration = float(len(y) / sr)
    S = np.abs(librosa.stft(y, n_fft=2048, hop_length=512))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=2048)
    total = S.sum() + 1e-9
    # 帯域の切り方は stems.py と同じ（リファレンスのステム統計と距離を取るため揃える）
    bands = {
        "sub_20_80": (20, 80),
        "low_80_250": (80, 250),
        "mid_250_2k": (250, 2000),
        "hi_2k_6k": (2000, 6000),
        "air_6k_11k": (6000, 11000),
    }
    balance = {k: round(float(S[(freqs >= lo) & (freqs < hi)].sum() / total), 3) for k, (lo, hi) in bands.items()}

    y_harm, y_perc = librosa.effects.hpss(y, margin=2.0)
    hp = float(np.sqrt((y_harm**2).mean()) / (np.sqrt((y_perc**2).mean()) + 1e-9))

    onset_env = librosa.onset.onset_strength(y=y, sr=sr)
    onsets = librosa.onset.onset_detect(onset_envelope=onset_env, sr=sr)
    onset_rate = float(len(onsets) / duration)

    features = {
        "duration_s": round(duration, 3),
        "rms_db": round(float(20 * np.log10(max(np.sqrt((y**2).mean()), 1e-9))), 2),
        "spectral_centroid_hz_median": round(float(np.median(librosa.feature.spectral_centroid(S=S, sr=sr))), 1),
        "spectral_rolloff95_hz": round(float(librosa.feature.spectral_rolloff(S=S, sr=sr, roll_percent=0.95).mean()), 1),
        "band_balance": balance,
        "harmonic_percussive_ratio": round(hp, 2),
        "onset_rate_per_s": round(onset_rate, 3),
    }
    return {"features": features, "_y": y, "_y_harm": y_harm, "_onset_env": onset_env, "_sr": sr}


def analyze_audio(path: Path) -> dict:
    """1ファイルを読み、特徴量と（ファイル名にメタが無いときの）キー/BPM 推定材料を返す。"""
    y, sr = librosa.load(path, sr=SR, mono=True)
    if len(y) < sr // 10:
        raise ValueError("音声が短すぎる（0.1秒未満）")
    return compute_features(y, sr)


def estimate_bpm(onset_env: np.ndarray, sr: int) -> float | None:
    # librosa の版差吸収: beat.tempo は新しい版で feature.rhythm.tempo に移った
    try:
        tempo_fn = librosa.feature.rhythm.tempo  # type: ignore[attr-defined]
    except AttributeError:
        tempo_fn = librosa.beat.tempo
    t = tempo_fn(onset_envelope=onset_env, sr=sr)
    if len(t) == 0:
        return None
    bpm = float(t[0])
    return round(bpm, 1) if BPM_MIN <= bpm <= BPM_MAX else None


def build_entry(library: Path, rel: Path, force_meta: dict | None = None) -> dict:
    """1ファイルぶんのエントリを作る。force_meta はテスト用の差し替え口。"""
    path = library / rel
    parts = rel.parts
    is_contrast = parts[0] == "_contrast"
    pack = parts[2] if is_contrast and len(parts) > 3 else (parts[1] if len(parts) > 2 else "")

    meta = force_meta if force_meta is not None else parse_filename_meta(rel.stem)
    analyzed = analyze_audio(path)
    features = analyzed["features"]

    bpm, bpm_source = meta["bpm"], "filename"
    if bpm is None:
        bpm = estimate_bpm(analyzed["_onset_env"], analyzed["_sr"])
        bpm_source = "estimate" if bpm is not None else None

    key_root, key_mode, key_source, key_conf = meta["key_root"], meta["key_mode"], "filename", None
    if key_root is None:
        ranked = estimate_key(analyzed["_y_harm"], analyzed["_sr"])
        top = ranked[0]
        name, mode = top["key"].split()
        key_root, key_mode = PITCHES.index(name), mode
        key_source, key_conf = "estimate", round(top["corr"], 4)

    st = path.stat()
    return {
        "path": rel.as_posix(),
        "kind": "loop",
        "is_contrast": is_contrast,
        "pack": pack,
        "size": st.st_size,
        # 秒precisionだと「同サイズで1秒以内の上書き」を差分検出できない → ns で持つ
        "mtime_ns": st.st_mtime_ns,
        "bpm": bpm,
        "bpm_source": bpm_source,
        "key_root": key_root,
        "key_mode": key_mode,
        "key_source": key_source,
        "key_confidence": key_conf,
        "loop_bars_estimate": estimate_bars(analyzed["_y"], analyzed["_sr"], bpm) if bpm else None,
        "features": features,
    }


def scan_files(library: Path) -> list[Path]:
    """インデックス対象（loops/ と _contrast/loops/ の音声ファイル）をライブラリ相対で列挙する。"""
    rels = []
    for root in (library / "loops", library / "_contrast" / "loops"):
        if not root.is_dir():
            continue
        for p in sorted(root.rglob("*")):
            if p.is_file() and p.suffix.lower() in AUDIO_EXTS and not p.name.startswith("."):
                rels.append(p.relative_to(library))
    return sorted(rels)  # ルート横断で決定的な順序にする（index の diff を安定させる）


def build_index(library: Path, force: bool = False) -> tuple[dict, dict]:
    """index.json の中身と、何をしたかのサマリーを返す。書き込みは main() 側。"""
    index_path = library / "index.json"
    prev = {}
    if index_path.exists():
        try:
            data = json.loads(index_path.read_text())
            if data.get("schema_version") == SCHEMA_VERSION:
                prev = {e["path"]: e for e in data.get("entries", [])}
        except (json.JSONDecodeError, KeyError, TypeError):
            prev = {}  # 壊れた index は作り直す（原子的書き込みなので通常は起きない）

    entries, summary = [], {"new": 0, "reused": 0, "skipped": 0, "removed": 0}
    seen = set()
    for rel in scan_files(library):
        seen.add(rel.as_posix())
        st = (library / rel).stat()
        old = prev.get(rel.as_posix())
        if old and not force and old["size"] == st.st_size and old.get("mtime_ns") == st.st_mtime_ns:
            entries.append(old)
            summary["reused"] += 1
            continue
        try:
            entries.append(build_entry(library, rel))
            summary["new"] += 1
        except Exception as e:  # 壊れたファイルで全体を止めない（読めた分だけの index にする）
            print(f"skip: {rel} ({type(e).__name__}: {e})", file=sys.stderr)
            summary["skipped"] += 1
    summary["removed"] = len([p for p in prev if p not in seen])

    index = {"schema_version": SCHEMA_VERSION, "entries": entries}
    return index, summary


def write_atomic(index_path: Path, index: dict) -> None:
    tmp = index_path.with_name(f"{index_path.name}.tmp-{os.getpid()}")
    tmp.write_text(json.dumps(index, ensure_ascii=False, indent=1) + "\n")
    os.replace(tmp, index_path)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--library", type=Path, default=DEFAULT_LIBRARY)
    ap.add_argument("--force", action="store_true", help="size/mtime が同じでも全ファイル再分析する")
    args = ap.parse_args()

    library = args.library.expanduser()
    if not library.is_dir():
        print(f"ERROR: ライブラリが見つかりません: {library}（先に setup.sh を実行）", file=sys.stderr)
        sys.exit(1)

    index, summary = build_index(library, force=args.force)
    write_atomic(library / "index.json", index)
    n = len(index["entries"])
    print(f"index.json: {n}件（新規 {summary['new']} / 再利用 {summary['reused']}"
          f" / 読めず除外 {summary['skipped']} / 消滅 {summary['removed']}）")


if __name__ == "__main__":
    main()
