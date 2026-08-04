#!/usr/bin/env python
"""ドラムガチャ — 制約カード(card.json)からドラム候補を生成する（Phase 2 の最小版）。

リファレンスの「グルーヴの性格」（スウィング・クオンタイズ度合い・16分の出現強度）だけを
借りて、1小節のドラムパターンをルールベースで振る。パターンそのものはコピーしない —
プロファイルは平均オンセット強度であってヒット列ではないので、骨格（強度0.35以上=必ず鳴る）と
装飾（それ未満=強度を確率としてサンプリング）の2層に解釈し、ガチャの振れは装飾層から出す。

出力は GM ドラム（kick=36 / snare=38 / closed hat=42、ch10=mido の channel 9）の .mid と、
耳チェック用の合成 .wav、再現に必要な全情報を持つサイドカー .json の3点セット。
生成は完全に決定的: レーン別 seed（全体 seed から SHA-256 導出）＋実効設定が同じなら
バイト単位で同じ出力になる。ファイル名にレーン seed と設定ハッシュを埋めるので、
「気に入った候補の seed を控える」がファイル名で完結する。

使い方: drums.py <リファレンスフォルダ|card.json> [--seed N] [--count N] [--bars N]
                 [--lock kick=HEX8,snare=HEX8,hat=HEX8] [--out DIR]

設計は docs/plans/2026-08-04-1158-drum-gacha-cli.md と docs/design/reference-beat.md。
"""
import argparse
import hashlib
import json
import math
import os
import secrets
import sys
import tempfile
from pathlib import Path

import mido
import numpy as np
import soundfile as sf
from scipy.signal import butter, lfilter

GENERATOR_VERSION = 2  # v2: note_off を bars×TICKS_BAR にクランプ（LaLa 取り込みで5小節化しない）
PPQ = 480          # MIDI の分解能（4分音符あたり tick）
TICKS_16TH = PPQ // 4
TICKS_BAR = PPQ * 4
NOTE_DURATION = 60  # ドラムは note-off をほぼ無視するので短い固定値でよい
SAMPLE_RATE = 44100
SKELETON_THRESHOLD = 0.35  # 分析側 groove.py の active_16ths と同じ線（骨格/装飾の境界）

LANES = ("kick", "snare", "hat")
GM_NOTE = {"kick": 36, "snare": 38, "hat": 42}
DRUM_CHANNEL = 9  # 人間表記の ch10。mido は 0 始まり

PROFILE_FIELD = {
    "kick": "kick_profile_by_beat",
    "snare": "snare_profile_by_16th",
    "hat": "hat_profile_by_16th",
}
PROFILE_LEN = {"kick_profile_by_beat": 4, "snare_profile_by_16th": 16, "hat_profile_by_16th": 16}

# ゲート落ちで省略されたフィールドの既定値（「無い＝生成器の無難な既定」）。
# snare は2・4拍のバックビート、kick は拍1・3強、hat は8分刻み（表拍強）。
DEFAULTS = {
    "swing_ratio": 0.5,
    "quantize_dev_ms": 0.0,
    "kick_profile_by_beat": [1.0, 0.25, 0.85, 0.25],
    "snare_profile_by_16th": [0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0,
                              0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0],
    "hat_profile_by_16th": [1.0, 0.0, 0.75, 0.0] * 4,
}


class GachaError(Exception):
    """カードの壊れた値・候補ファイルの衝突など、黙って進んではいけない状態。"""


# ---------------------------------------------------------------- カード読み込み

def parse_card(card: dict, source: str) -> tuple[dict, float, list[str]]:
    """カードから drums の実効設定を作る。

    不在のフィールドだけをデフォルト補完する（ゲート落ちの正当な省略）。
    存在するのに壊れている値はエラーで停止する — card.py の検算を通った正規カードでは
    起きないので、起きたら手編集かバージョン不整合。黙って直すと気づけない。
    """
    if card.get("card_version") != 1:
        raise GachaError(f"{source}: card_version が 1 でない: {card.get('card_version')!r}")
    bpm = card.get("global", {}).get("bpm")
    if not isinstance(bpm, (int, float)) or isinstance(bpm, bool) or not math.isfinite(bpm):
        raise GachaError(f"{source}: global.bpm が数値でない: {bpm!r}")
    if not (40 <= bpm <= 300):
        raise GachaError(f"{source}: global.bpm が 40〜300 の外: {bpm}")

    drums = card.get("drums", {})
    if not isinstance(drums, dict):
        raise GachaError(f"{source}: drums がオブジェクトでない: {drums!r}")

    effective: dict = {}
    defaulted: list[str] = []
    for field, default in DEFAULTS.items():
        if field not in drums:
            effective[field] = default
            defaulted.append(field)
            continue
        v = drums[field]
        if field == "swing_ratio":
            # 分析側の plausible 範囲（0.45〜0.80）に揃える。正規カードはこの外の値を
            # そもそも載せない上、範囲外（例 0.999）は単調化クランプで小節末にノートが
            # 積み重なり、生成結果自体が壊れる
            if not _is_num(v) or not (0.45 <= v <= 0.80):
                raise GachaError(f"{source}: drums.swing_ratio が 0.45〜0.80 の外: {v!r}")
        elif field == "quantize_dev_ms":
            if not _is_num(v) or not math.isfinite(v) or v < 0:
                raise GachaError(f"{source}: drums.quantize_dev_ms が不正: {v!r}")
        else:
            n = PROFILE_LEN[field]
            if not isinstance(v, list) or len(v) != n:
                raise GachaError(f"{source}: drums.{field} が {n} 件のリストでない: {v!r}")
            if any(not _is_num(x) or not (0.0 <= x <= 1.0) for x in v):
                raise GachaError(f"{source}: drums.{field} に 0〜1 の外の値がある: {v!r}")
        effective[field] = v
    return effective, float(bpm), defaulted


def _is_num(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def load_card(path: Path) -> tuple[dict, float, list[str], str | None]:
    try:
        card = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise GachaError(f"{path}: カードを読めない: {e}") from e
    effective, bpm, defaulted = parse_card(card, str(path))
    reference = card.get("meta", {}).get("reference")
    return effective, bpm, defaulted, reference if isinstance(reference, str) else None


# ---------------------------------------------------------------- seed 導出

def derive_lane_seed(global_seed: int, lane: str) -> int:
    """全体 seed からレーン seed を決定的に導出する（32bit）。

    Python の hash() は PYTHONHASHSEED でプロセスごとに変わるので SHA-256 を使う。
    """
    digest = hashlib.sha256(f"drums:{lane}:{global_seed}".encode()).digest()
    return int.from_bytes(digest[:4], "big")


def derive_audio_seed(lane: str, lane_seed: int) -> int:
    """音声合成用 seed。パターン生成用と独立に導出する（wav の決定性のため）。"""
    digest = hashlib.sha256(f"audio:{lane}:{lane_seed:08x}".encode()).digest()
    return int.from_bytes(digest[:8], "big")


def resolve_lane_seeds(global_seed: int, locks: dict[str, int]) -> dict[str, int]:
    return {lane: locks.get(lane, derive_lane_seed(global_seed, lane)) for lane in LANES}


# ---------------------------------------------------------------- パターン生成

def generate_pattern(effective: dict, lane_seeds: dict[str, int], bars: int, bpm: float) -> dict[str, list[tuple[int, int]]]:
    """レーンごとに1小節パターンを振り、bars 回繰り返した (tick, velocity) 列を返す。"""
    swing = effective["swing_ratio"]
    dev_ms = effective["quantize_dev_ms"]
    notes: dict[str, list[tuple[int, int]]] = {}
    for lane in LANES:
        rng = np.random.default_rng(lane_seeds[lane])
        profile = effective[PROFILE_FIELD[lane]]
        bar_notes = _generate_bar(lane, profile, swing, dev_ms, bpm, rng)
        notes[lane] = [(tick + b * TICKS_BAR, vel) for b in range(bars) for tick, vel in bar_notes]
    return notes


def _generate_bar(lane: str, profile: list[float], swing: float, dev_ms: float,
                  bpm: float, rng: np.random.Generator) -> list[tuple[int, int]]:
    # kick は拍単位4値（16分位置を信用しない、という分析側の判断を引き継ぐ）
    slots = [(i * 4, v) for i, v in enumerate(profile)] if lane == "kick" \
        else list(enumerate(profile))

    # マイクロタイミング: quantize_dev_ms は |ズレ| の平均（半正規分布の平均とみなし
    # σ = dev×√(π/2) に換算）。±3σ とスロット間隔の40%（分析側の許容 tol と同じ）でクリップ
    sigma_ms = dev_ms * math.sqrt(math.pi / 2)
    ms_16th = 60000.0 / bpm / 4
    clip_ms = min(3 * sigma_ms, 0.4 * ms_16th)
    ticks_per_ms = PPQ * bpm / 60000.0

    events: list[tuple[int, int]] = []
    prev_tick = -1
    for slot, v in slots:
        roll = rng.random()  # 発音判定は毎スロット1回引く（乱数消費を profile の値に依らせない）
        fired = v >= SKELETON_THRESHOLD or roll < v
        if not fired:
            continue
        vel = int(np.clip(round(20 + v * 96 + rng.normal(0, 5)), 1, 127))
        tick = slot * TICKS_16TH
        if slot % 4 == 2:  # 8分裏だけスウィングで動かす（符号付き。分析側の定義の逆変換）
            tick += round((swing - 0.5) * PPQ)
        jitter_ms = float(np.clip(rng.normal(0, sigma_ms), -clip_ms, clip_ms)) if sigma_ms > 0 else 0.0
        tick += round(jitter_ms * ticks_per_ms)
        # スウィング適用後の最終 tick 列で単調性を守る（swing 0.8 は裏拍が次スロットを追い越す）
        tick = max(tick, 0, prev_tick + 1)
        tick = min(tick, TICKS_BAR - 1)  # 小節をはみ出して次小節の頭と衝突させない
        events.append((tick, vel))
        prev_tick = tick
    return events


# ---------------------------------------------------------------- MIDI

def build_midi(notes: dict[str, list[tuple[int, int]]], bpm: float, bars: int) -> mido.MidiFile:
    mid = mido.MidiFile(ticks_per_beat=PPQ)
    track = mido.MidiTrack()
    mid.tracks.append(track)
    track.append(mido.MetaMessage("time_signature", numerator=4, denominator=4, time=0))
    track.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(bpm), time=0))

    # note_off は最終小節の境界でクランプする。note_on は小節末 1 tick 前まで許すため、
    # +NOTE_DURATION がスウィング＋正方向ジッターの最終ハットで境界を越えることがあり、
    # 取り込み側（小節切り上げ）で bars+1 小節のリージョンに化けるため
    end_tick = bars * TICKS_BAR
    events = []  # (tick, 順序キー: off=0/on=1, note, velocity)
    for lane, lane_notes in notes.items():
        note = GM_NOTE[lane]
        for tick, vel in lane_notes:
            events.append((tick, 1, note, vel))
            events.append((min(tick + NOTE_DURATION, end_tick), 0, note, 0))
    events.sort()

    now = 0
    for tick, kind, note, vel in events:
        delta = tick - now
        now = tick
        if kind == 1:
            track.append(mido.Message("note_on", channel=DRUM_CHANNEL, note=note, velocity=vel, time=delta))
        else:
            track.append(mido.Message("note_off", channel=DRUM_CHANNEL, note=note, velocity=0, time=delta))
    track.append(mido.MetaMessage("end_of_track", time=0))
    return mid


# ---------------------------------------------------------------- wav 合成（耳チェック用）

# 聴きたいのはグルーヴであって音色ではない。kick/snare/hat の帯域が分かれてさえいれば
# 判定できるので、依存を増やさず numpy/scipy で合成する（本番の音色は LaLa の Drum Kit）。
_SNARE_BAND = butter(2, [700, 6000], btype="bandpass", fs=SAMPLE_RATE)
_HAT_HIGH = butter(4, 7000, btype="highpass", fs=SAMPLE_RATE)
_MASTER_GAIN = 0.42  # 3声同時（0.9+0.65+0.45）でも peak が 0dBFS を超えない値


def _voice_kick(vel: int) -> np.ndarray:
    t = np.arange(int(0.35 * SAMPLE_RATE)) / SAMPLE_RATE
    freq = 45 + 75 * np.exp(-t / 0.03)  # 120Hz→45Hz のピッチスイープ
    phase = 2 * np.pi * np.cumsum(freq) / SAMPLE_RATE
    x = np.sin(phase) * np.exp(-t / 0.13)
    return _finish(x, 0.9 * vel / 127)


def _voice_snare(vel: int, rng: np.random.Generator) -> np.ndarray:
    t = np.arange(int(0.25 * SAMPLE_RATE)) / SAMPLE_RATE
    noise = lfilter(*_SNARE_BAND, rng.standard_normal(len(t))) * np.exp(-t / 0.06)
    tone = np.sin(2 * np.pi * 185 * t) * np.exp(-t / 0.05) * 0.5  # 胴鳴り
    return _finish(noise / (np.max(np.abs(noise)) + 1e-12) + tone, 0.65 * vel / 127)


def _voice_hat(vel: int, rng: np.random.Generator) -> np.ndarray:
    t = np.arange(int(0.12 * SAMPLE_RATE)) / SAMPLE_RATE
    x = lfilter(*_HAT_HIGH, rng.standard_normal(len(t))) * np.exp(-t / 0.035)
    return _finish(x, 0.45 * vel / 127)


def _finish(x: np.ndarray, amp: float) -> np.ndarray:
    """ピークを amp に正規化し、クリック防止の短いフェードを付ける。"""
    x = x / (np.max(np.abs(x)) + 1e-12) * amp
    fade_in = min(len(x), int(0.001 * SAMPLE_RATE))
    x[:fade_in] *= np.linspace(0.0, 1.0, fade_in)
    fade_out = min(len(x), int(0.005 * SAMPLE_RATE))
    x[-fade_out:] *= np.linspace(1.0, 0.0, fade_out)
    return x


def synth_wav(notes: dict[str, list[tuple[int, int]]], bpm: float, bars: int,
              lane_seeds: dict[str, int]) -> np.ndarray:
    total_sec = bars * 240.0 / bpm + 0.6  # 末尾に余韻ぶんの尻尾
    buf = np.zeros(int(round(total_sec * SAMPLE_RATE)))
    voice = {"kick": _voice_kick, "snare": _voice_snare, "hat": _voice_hat}
    for lane, lane_notes in notes.items():
        # ノイズも決定的に（未 seed の RNG を使うと同じ MIDI でも wav が毎回変わる）
        rng = np.random.default_rng(derive_audio_seed(lane, lane_seeds[lane]))
        for tick, vel in lane_notes:
            x = voice[lane](vel) if lane == "kick" else voice[lane](vel, rng)
            pos = int(round(tick / PPQ * 60.0 / bpm * SAMPLE_RATE))
            end = min(pos + len(x), len(buf))
            buf[pos:end] += x[: end - pos]
    return buf * _MASTER_GAIN


def verify_wav_data(data: np.ndarray, label: str) -> None:
    """書き込む前の float 配列を検算する。peak は PCM 化で黙ってクリップされ読み戻しでは検出できない。"""
    if not np.all(np.isfinite(data)):
        raise GachaError(f"{label}: wav に有限でないサンプルがある")
    peak = float(np.max(np.abs(data))) if len(data) else 0.0
    if peak > 0.999:
        raise GachaError(f"{label}: wav が 0dBFS を超えている（peak={peak:.3f}。同時発音のクリップ）")


def verify_wav_file(path: Path, label: str) -> None:
    """書き出したファイルを読み戻して検算する（「ファイルがある・長さが正しい」では確認しない）。"""
    data, sr = sf.read(path)
    if sr != SAMPLE_RATE:
        raise GachaError(f"{label}: サンプルレートが不正: {sr}")
    if not np.all(np.isfinite(data)):
        raise GachaError(f"{label}: 読み戻した wav に有限でないサンプルがある")
    rms = float(np.sqrt(np.mean(np.square(data)))) if len(data) else 0.0
    if rms < 1e-3:
        raise GachaError(f"{label}: wav がほぼ無音（rms={rms:.2e}）")


# ---------------------------------------------------------------- 候補の公開（原子的）

def cfg_payload(effective: dict, bpm: float, bars: int) -> dict:
    """「有効な生成入力」だけ。生成時刻など非決定要素は入れない（同名なら同内容を保証する側）。"""
    return {
        "generator": "drums",
        "generator_version": GENERATOR_VERSION,
        "bpm": bpm,
        "bars": bars,
        "wav_sample_rate": SAMPLE_RATE,
        "drums": effective,
    }


def cfg_hash(payload: dict) -> str:
    return hashlib.sha256(json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def candidate_hash(payload: dict, lane_seeds_hex: dict[str, str]) -> str:
    """候補1件の完全な同一性（実効設定＋レーン seed）のハッシュ。

    cfg_hash は設定だけでレーン seed を含まないため、--from の照合を cfg_hash だけに
    頼ると seed の改変を検出できない（実測で hat seed 差し替えが素通りした）。
    """
    body = {"config": payload, "lane_seeds": lane_seeds_hex}
    return hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def candidate_basename(lane_seeds: dict[str, int], cfg_sha: str) -> str:
    return f"drums-k{lane_seeds['kick']:08x}-s{lane_seeds['snare']:08x}-h{lane_seeds['hat']:08x}-{cfg_sha[:6]}"


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


def publish_candidate(outdir: Path, base: str, mid: mido.MidiFile, wav_data: np.ndarray,
                      sidecar: dict) -> None:
    """一時ファイル→検算→リネームで原子的に公開する。サイドカーは最後＝完成マーカー。"""
    verify_wav_data(wav_data, base)
    tmp_paths = []
    try:
        for ext, write in ((".mid", lambda p: mid.save(p)),
                           (".wav", lambda p: sf.write(p, wav_data, SAMPLE_RATE, subtype="PCM_16"))):
            fd, tmp = tempfile.mkstemp(dir=outdir, prefix=".gacha-", suffix=ext)
            os.close(fd)
            tmp_paths.append((tmp, outdir / f"{base}{ext}"))
            write(tmp)
        verify_wav_file(Path(tmp_paths[1][0]), base)
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


# ---------------------------------------------------------------- CLI

def parse_locks(text: str) -> dict[str, int]:
    """--lock kick=8f3a21bc,hat=00ff00ff（ファイル名と同じ8桁hex）を読む。"""
    locks: dict[str, int] = {}
    for part in text.split(","):
        lane, sep, value = part.partition("=")
        lane = lane.strip()
        if not sep or lane not in LANES:
            raise GachaError(f"--lock の形式が不正: {part!r}（kick=HEX8,snare=HEX8,hat=HEX8）")
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


def load_sidecar(path: Path) -> tuple[dict, float, int, list[str], dict[str, int], int, str | None]:
    """サイドカーから1候補を完全再現するための入力一式を読む（--from）。

    実効設定はカードと同じ検証を通し（合成カードにして parse_card へ）、cfg ハッシュを
    現在の生成器で再計算してサイドカーの値と突き合わせる。不一致＝生成器のバージョンが
    変わったかサイドカーが編集されている＝同一出力は再現できないので、正直にエラーで止まる。
    """
    try:
        sc = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise GachaError(f"{path}: サイドカーを読めない: {e}") from e
    try:
        config = sc["config"]
        bars = config["bars"]
        pseudo_card = {"card_version": 1, "global": {"bpm": config["bpm"]}, "drums": config["drums"]}
        lane_seeds = {lane: int(sc["lane_seeds"][lane], 16) for lane in LANES}
        global_seed = sc["global_seed"]
        recorded_cfg_sha = sc["cfg_sha256"]
        recorded_cand_sha = sc["candidate_sha256"]
    except (KeyError, TypeError, ValueError) as e:
        raise GachaError(f"{path}: サイドカーの形式が不正: {e!r}") from e
    effective, bpm, _ = parse_card(pseudo_card, str(path))
    if not isinstance(bars, int) or bars < 1:
        raise GachaError(f"{path}: config.bars が不正: {bars!r}")
    if any(not (0 <= s <= 0xFFFFFFFF) for s in lane_seeds.values()):
        raise GachaError(f"{path}: lane_seeds が32bitの範囲外: {sc['lane_seeds']}")
    # config は「現在の生成器で正規化し直した payload」との完全一致を要求する。
    # ハッシュ比較だけだと wav_sample_rate / generator_version の改変（recorded 側も
    # 揃えて書き換えたケース）や、定数変更後のバージョン上げ忘れを検出できない
    rebuilt = cfg_payload(effective, bpm, bars)
    if config != rebuilt:
        raise GachaError(
            f"{path}: config が現在の生成器の正規化済み設定と一致しない"
            f"（生成器のバージョン・サンプルレートが変わったかサイドカーが編集されている）。同一出力は再現できない"
        )
    lane_seeds_hex = {lane: f"{s:08x}" for lane, s in lane_seeds.items()}
    if recorded_cfg_sha != cfg_hash(rebuilt) or recorded_cand_sha != candidate_hash(rebuilt, lane_seeds_hex):
        raise GachaError(
            f"{path}: 記録されたハッシュが再計算と一致しない（レーン seed か設定が編集されている）。同一出力は再現できない"
        )
    defaulted = sc.get("defaulted_fields", [])
    reference = sc.get("reference")
    return effective, bpm, bars, defaulted, lane_seeds, global_seed, reference if isinstance(reference, str) else None


def run(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="制約カードからドラム候補をガチャ生成する")
    ap.add_argument("card", nargs="?", help="リファレンスフォルダ または card.json のパス")
    ap.add_argument("--seed", type=int, default=None, help="全体 seed（省略時は乱数で決めて表示）")
    ap.add_argument("--count", type=int, default=None, help="候補数（全体 seed を +1 ずつ回す。既定 8）")
    ap.add_argument("--bars", type=int, default=None, help="書き出す小節数（1小節パターンの繰り返し。既定 4）")
    ap.add_argument("--lock", type=str, default="", help="レーン seed の固定: kick=HEX8,snare=HEX8,hat=HEX8")
    ap.add_argument("--out", type=str, default=None, help="出力先（既定はカードと同じフォルダの gacha/）")
    ap.add_argument("--from", dest="from_sidecar", metavar="SIDECAR",
                    help="候補のサイドカー .json から1件を完全再現する（カード不要。他の生成条件はサイドカーの値を使う）")
    # 機械可読出力（LaLa 連携用）。stdout は JSON Lines のみ（候補ごとに1行:
    # base=ファイル名の共通部・lane_seeds・status=generated|regenerated|skipped）。
    # 人間向けの進捗・補完申告は stderr へ、成否は exit code。
    # 「JSON でない行を読み飛ばす」形の文字列依存を作らないための契約
    ap.add_argument("--porcelain", action="store_true", help="stdout を JSON Lines のみにする（機械可読）")
    args = ap.parse_args(argv)

    # 人間向け出力の宛先を1箇所で切り替える（porcelain では stdout を JSON Lines 専用に保つ）
    def note(*a, **kw):
        print(*a, **kw, file=sys.stderr if args.porcelain else sys.stdout)

    if args.from_sidecar:
        if args.card or args.seed is not None or args.lock or args.count is not None or args.bars is not None:
            raise GachaError("--from は card / --seed / --count / --bars / --lock と併用できない（生成条件はサイドカーの値を使う）")
        sc_path = Path(args.from_sidecar).expanduser()
        effective, bpm, bars, defaulted, lane_seeds, global_seed, reference = load_sidecar(sc_path)
        outdir = Path(args.out).expanduser() if args.out else sc_path.parent
        candidates = [(global_seed, lane_seeds)]
        note(f"サイドカーから再現: {sc_path}（BPM {bpm:g} / {bars}小節）")
    else:
        if not args.card:
            raise GachaError("card（リファレンスフォルダ or card.json）か --from を指定する")
        count = args.count if args.count is not None else 8
        bars = args.bars if args.bars is not None else 4
        if count < 1 or bars < 1:
            raise GachaError("--count / --bars は 1 以上")
        card_path = resolve_card_path(args.card)
        effective, bpm, defaulted, reference = load_card(card_path)
        locks = parse_locks(args.lock) if args.lock else {}
        outdir = Path(args.out).expanduser() if args.out else card_path.parent / "gacha"
        note(f"カード: {card_path}（BPM {bpm:g} / swing {effective['swing_ratio']:g}）")
        for field in defaulted:
            note(f"  [補完] drums.{field} が無いため既定値を使う")
        if locks:
            note("  [ロック] " + ", ".join(f"{k}={v:08x}" for k, v in locks.items()))
        global_seed = args.seed if args.seed is not None else secrets.randbits(32)
        if args.seed is None:
            note(f"全体 seed: {global_seed}（--seed {global_seed} で再現できる）")
        candidates = [(global_seed + i, resolve_lane_seeds(global_seed + i, locks)) for i in range(count)]

    outdir.mkdir(parents=True, exist_ok=True)
    payload = cfg_payload(effective, bpm, bars)
    cfg_sha = cfg_hash(payload)
    generated = skipped = 0
    seen: set[str] = set()
    # porcelain: ユニークな候補1件につき1行だけ emit する（実行内重複＝同一ファイルは出さない。
    # 受け手はこの行の集合を「今回の実行の候補一覧」として使う）
    def emit(base: str, lane_seeds_hex: dict[str, str], status: str) -> None:
        if args.porcelain:
            print(json.dumps({"base": base, "lane_seeds": lane_seeds_hex, "status": status},
                             ensure_ascii=False))

    for gseed, lane_seeds in candidates:
        base = candidate_basename(lane_seeds, cfg_sha)
        if base in seen:  # 全レーンロック時は全候補が同一になる
            note(f"  [スキップ] {base}: この実行内で生成済み（全レーンがロックされている）")
            skipped += 1
            continue
        seen.add(base)
        lane_seeds_hex = {lane: f"{s:08x}" for lane, s in lane_seeds.items()}
        cand_sha = candidate_hash(payload, lane_seeds_hex)
        status = candidate_status(outdir, base, payload, lane_seeds_hex)
        if status == "skip":
            note(f"  [スキップ] {base}: 完成済み（同じ seed・同じ設定＝同じ内容）")
            emit(base, lane_seeds_hex, "skipped")  # 完成済み＝ファイルは揃っている（候補として有効）
            skipped += 1
            continue
        notes = generate_pattern(effective, lane_seeds, bars, bpm)
        mid = build_midi(notes, bpm, bars)
        wav_data = synth_wav(notes, bpm, bars, lane_seeds)
        sidecar = {
            "cfg_sha256": cfg_sha,
            "candidate_sha256": cand_sha,
            "global_seed": gseed,
            "lane_seeds": lane_seeds_hex,
            "config": payload,
            "defaulted_fields": defaulted,
        }
        # カードのパスは置き場で変わり sidecar のバイト同一性を壊すので、決定的な
        # meta.reference（カード内容の一部）だけを持つ
        if reference is not None:
            sidecar["reference"] = reference
        publish_candidate(outdir, base, mid, wav_data, sidecar)
        label = "再生成" if status == "regen" else "生成"
        note(f"  [{label}] {base}")
        emit(base, lane_seeds_hex, "regenerated" if status == "regen" else "generated")
        generated += 1

    note(f"完了: 生成 {generated} 件 / スキップ {skipped} 件 → {outdir}")
    return 0


def main() -> None:
    try:
        sys.exit(run(sys.argv[1:]))
    except GachaError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
