#!/usr/bin/env python
"""ベースガチャ — 制約カード(card.json)とプロジェクトのキーからベース候補を生成する。

ベースは「進行を運ぶ楽器」。chords スライスがあればループ1周ぶんのルート列（進行）を
性格（root_motion / root_changes_per_loop）に従ってガチャで振り、無ければルート連打の
1小節に退化する — ジャンルスイッチは持たず、リファレンス選びがジャンルを決める。
リズムは bass スライスの onset_rate_by_16th を重みに notes_per_bar を目標本数として
サンプリングし、ドラムのキック位置（--kick-ticks / --drums）を同時打ちブーストする。
タイミングはグリッド正位置＋キック実 tick へのスナップのみ（ジッターは掛けない）。

キーは常に呼び出し側（LaLa のプロジェクト or --key）が決める。カードの global.key は
生成時に読まない（reference-beat.md の既存決定）。

出力は GM ベース想定の .mid（ch1・program 33）と耳チェック用の合成 .wav、再現に必要な
全情報を持つサイドカー .json の3点セット。--drums 指定時はドラムと重ねた使い捨ての
mix wav も出す（候補の同一性・skip 判定には関与しない）。

生成は完全に決定的: レーン別 seed（prog=ルート列 / rhythm=リズム・装飾）＋実効設定
（key・実効 BPM・折り畳んだ kick ticks を含む）が同じならバイト単位で同じ出力になる。

使い方: bass.py <リファレンスフォルダ|card.json> --key ROOT:MODE [--bpm N] [--seed N]
                [--count N] [--bars N] [--lock prog=HEX8,rhythm=HEX8]
                [--roots <looproots契約.json>]（ループ追従: 進行を固定しリズムだけガチャ）
                [--kick-ticks 0,480,960 | --drums <drums-sidecar.json>] [--out DIR]

設計は docs/plans/2026-08-06-1809-bass-gacha.md と docs/design/reference-beat.md。
"""
import argparse
import json
import math
import secrets
import sys
from pathlib import Path

import mido
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import drums as drums_mod  # noqa: E402  （--drums の再構成・mix 合成に使う）
from common import (  # noqa: E402
    GachaError, candidate_hash, candidate_status, cfg_hash, derive_lane_seed, is_num,
    parse_locks, publish_candidate, resolve_card_path, resolve_lane_seeds, write_mix_wav,
)

# ループ追従モード（--roots）の契約は looproots.py の docstring が真実の源。
# looproots のモジュール import は stdlib のみ（librosa 系は関数内）なので起動は重くならない
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "library"))
from looproots import load_contract as load_roots_contract, validate_roots_core  # noqa: E402

# v2: 装飾音をスケール構成音でフィルタ / 最低1音の保証を小節単位→パターン単位に
# v3: 最低1音フォールバックも同時打ちスナップを通す（疎な候補だけキックとフラムになっていた）
# v4: ジッター廃止（タイミングはグリッド正位置＋同時打ちスナップのみ。カードの
#     bass.quantize_dev_ms は読まない — 実聴で無相関ランダムの揺れが「ヨレ」にしか
#     聞こえず、低域のオンセット検出誤差で値自体も膨らんでいる疑いが強いため）
GENERATOR_VERSION = 4
PPQ = 480
TICKS_16TH = PPQ // 4
TICKS_BAR = PPQ * 4
SAMPLE_RATE = 44100

LANES = ("prog", "rhythm")  # 進行（ルート列）/ リズム（発音位置・装飾）
BASS_CHANNEL = 0
GM_FINGER_BASS = 33         # 0始まり 33 = GM #34 Electric Bass (finger)。LaLa 側の既定と揃える

# 音域は2段構え（プランで確定）:
# - ルートのオクターブ選択窓 = MIDI 28..39 の12半音幅（どのキーのルートも必ず1箇所に置ける）
# - 装飾込みの全ノートのハード範囲 = MIDI 28..51（+12 のオクターブ装飾が 39+12=51 で収まる）
# 表記注意: MIDI 28..39 は科学的表記で E1〜D♯2、LaLa のピアノロール表示（C3=60）では E0〜D♯1
ROOT_WINDOW_LOW = 28
ROOT_WINDOW_HIGH = 39
HARD_RANGE = (28, 51)

MAJOR_DEGREES = (0, 2, 4, 5, 7, 9, 11)
MINOR_DEGREES = (0, 2, 3, 5, 7, 8, 10)  # ナチュラルマイナー
NOTE_PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}

KICK_BOOST = 3.0      # キック最近傍スロットの重み倍率（同時打ちで1つのグルーヴに噛ませる）
ORNAMENT_PROB = 0.18  # 装飾音（5度・オクターブ・ダイアトニック7度）の割合。「少量」の実装値
WEIGHT_EPS = 0.02     # 全スロットに最低限の重み（プロファイル0のスロットも稀に鳴らす）

# ゲート落ちで bass スライスのフィールドが省略されたときの既定値。
# onset の既定は 1拍目・3拍目強＋2・4拍と8分裏を弱く（ルート弾きの無難な骨格）。
# カードの quantize_dev_ms は読まない（v4 — タイミングはグリッド正位置＋同時打ちスナップのみ）
BASS_DEFAULTS = {
    "notes_per_bar": 2.0,
    "note_length_16ths": 2.0,
    "onset_rate_by_16th": [1.0, 0.0, 0.0, 0.0, 0.4, 0.0, 0.3, 0.0,
                           0.8, 0.0, 0.0, 0.0, 0.4, 0.0, 0.3, 0.0],
}


# ---------------------------------------------------------------- キー

def parse_key(text: str) -> dict:
    """--key ROOT:MODE（例 F#:minor / Db:major）→ {"root": 0..11, "mode": "major"|"minor"}。"""
    root_text, sep, mode = text.partition(":")
    root_text, mode = root_text.strip(), mode.strip()
    if not sep or not root_text:
        raise GachaError(f"--key の形式が不正: {text!r}（例: F#:minor）")
    letter, accidental = root_text[0].upper(), root_text[1:]
    offsets = {"#": 1, "♯": 1, "b": -1, "♭": -1, "": 0}
    if letter not in NOTE_PC or accidental not in offsets:
        raise GachaError(f"--key のルートが不正: {root_text!r}")
    if mode not in ("major", "minor"):
        raise GachaError(f"--key のモードが不正: {mode!r}（major / minor）")
    return {"root": (NOTE_PC[letter] + offsets[accidental]) % 12, "mode": mode}


def scale_degrees(mode: str) -> tuple[int, ...]:
    return MAJOR_DEGREES if mode == "major" else MINOR_DEGREES


# ---------------------------------------------------------------- カード読み込み

def parse_card(card: dict, source: str) -> tuple[dict, float, list[str]]:
    """カードから bass / chords の実効設定を作る。

    不在のフィールドだけをデフォルト補完する（ゲート落ちの正当な省略）。存在するのに
    壊れている値はエラーで停止する（drums.py と同じ方針）。返り値の effective は
    {"bass": {...}, "chords": {...}|None}。chords 不在＝ルート固定1小節への退化。
    """
    if card.get("card_version") != 1:
        raise GachaError(f"{source}: card_version が 1 でない: {card.get('card_version')!r}")
    bpm = card.get("global", {}).get("bpm")
    if not is_num(bpm) or not math.isfinite(bpm):
        raise GachaError(f"{source}: global.bpm が数値でない: {bpm!r}")
    if not (40 <= bpm <= 300):
        raise GachaError(f"{source}: global.bpm が 40〜300 の外: {bpm}")

    bass = card.get("bass", {})
    if not isinstance(bass, dict):
        raise GachaError(f"{source}: bass がオブジェクトでない: {bass!r}")

    effective_bass: dict = {}
    defaulted: list[str] = []
    for field, default in BASS_DEFAULTS.items():
        if field not in bass:
            effective_bass[field] = default
            defaulted.append(f"bass.{field}")
            continue
        v = bass[field]
        if field == "notes_per_bar":
            if not is_num(v) or not (0 < v <= 16):
                raise GachaError(f"{source}: bass.notes_per_bar が 0〜16 の外: {v!r}")
        elif field == "note_length_16ths":
            if not is_num(v) or not (0 < v <= 32):
                raise GachaError(f"{source}: bass.note_length_16ths が 0〜32 の外: {v!r}")
        else:  # onset_rate_by_16th
            if not isinstance(v, list) or len(v) != 16:
                raise GachaError(f"{source}: bass.onset_rate_by_16th が 16 件のリストでない: {v!r}")
            if any(not is_num(x) or not (0.0 <= x <= 1.0) for x in v):
                raise GachaError(f"{source}: bass.onset_rate_by_16th に 0〜1 の外の値がある: {v!r}")
        effective_bass[field] = v

    effective = {"bass": effective_bass, "chords": parse_chords(card, source, defaulted)}
    return effective, float(bpm), defaulted


def parse_chords(card: dict, source: str, defaulted: list[str]) -> dict | None:
    """chords スライスを正規化する。不在は None（ルート固定1小節への退化）。

    - slots_per_bar 欠損は 2 を補完（旧カード互換。現行分析器のハーモニーグリッドは半小節固定）
    - root_changes_per_loop 欠損は changes_per_loop で代用（旧カード。クオリティ変化込みなので
      過大側に振れるが、ルート列の動きとしては許容）
    """
    if "chords" not in card:
        return None
    chords = card["chords"]
    if not isinstance(chords, dict):
        raise GachaError(f"{source}: chords がオブジェクトでない: {chords!r}")

    loop_bars = chords.get("loop_bars")
    if not isinstance(loop_bars, int) or isinstance(loop_bars, bool) or not (1 <= loop_bars <= 16):
        raise GachaError(f"{source}: chords.loop_bars が 1〜16 の int でない: {loop_bars!r}")

    if "slots_per_bar" in chords:
        slots_per_bar = chords["slots_per_bar"]
        if not isinstance(slots_per_bar, int) or isinstance(slots_per_bar, bool) \
                or not (1 <= slots_per_bar <= 8):
            raise GachaError(f"{source}: chords.slots_per_bar が 1〜8 の int でない: {slots_per_bar!r}")
    else:
        slots_per_bar = 2
        defaulted.append("chords.slots_per_bar")

    if "root_changes_per_loop" in chords:
        root_changes = chords["root_changes_per_loop"]
        if not isinstance(root_changes, int) or isinstance(root_changes, bool) or root_changes < 0:
            raise GachaError(f"{source}: chords.root_changes_per_loop が 0 以上の int でない: {root_changes!r}")
    else:
        root_changes = chords.get("changes_per_loop", 0)
        if not isinstance(root_changes, int) or isinstance(root_changes, bool) or root_changes < 0:
            raise GachaError(f"{source}: chords.changes_per_loop が 0 以上の int でない: {root_changes!r}")
        defaulted.append("chords.root_changes_per_loop")

    motion = chords.get("root_motion", "mixed")
    if motion not in ("static", "up", "down", "mixed"):
        raise GachaError(f"{source}: chords.root_motion が不正: {motion!r}")
    if "root_motion" not in chords:
        defaulted.append("chords.root_motion")

    has_7th = chords.get("has_7th_or_more", False)
    if not isinstance(has_7th, bool):
        raise GachaError(f"{source}: chords.has_7th_or_more が bool でない: {has_7th!r}")

    return {"loop_bars": loop_bars, "slots_per_bar": slots_per_bar,
            "root_changes_per_loop": root_changes, "root_motion": motion,
            "has_7th_or_more": has_7th}


def load_card(path: Path) -> tuple[dict, float, list[str], str | None]:
    try:
        card = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise GachaError(f"{path}: カードを読めない: {e}") from e
    effective, bpm, defaulted = parse_card(card, str(path))
    reference = card.get("meta", {}).get("reference")
    return effective, bpm, defaulted, reference if isinstance(reference, str) else None


# ---------------------------------------------------------------- キック制約

def loop_bars_of(effective: dict) -> int:
    return effective["chords"]["loop_bars"] if effective["chords"] else 1


def fold_kick_ticks(ticks: list[int], pattern_ticks: int) -> list[int]:
    """リージョン先頭基準の tick 列をパターン長へ mod で折り畳み、sort・重複除去する。

    折り畳んだ後の値が候補の同一性（config / hash / 候補名）に入る —
    同じ seed でもドラムが違えば別候補名になる。
    """
    for t in ticks:
        if not isinstance(t, int) or isinstance(t, bool) or t < 0:
            raise GachaError(f"kick ticks に 0 以上の int でない値がある: {t!r}")
    return sorted({t % pattern_ticks for t in ticks})


def parse_kick_ticks_arg(text: str) -> list[int]:
    try:
        return [int(part) for part in text.split(",") if part.strip() != ""]
    except ValueError:
        raise GachaError(f"--kick-ticks は int のカンマ区切り: {text!r}") from None


def kick_slot_map(kick_ticks: list[int], pattern_ticks: int) -> dict[int, int]:
    """折り畳み済み kick ticks → {16分スロット番号: 実 tick}。

    同一スロットに複数のキックが落ちたら、スロットの正位置に最も近い tick を採る
    （同時打ちスナップの行き先を1つに決める）。ブースト判定は最近傍16分スロット単位。
    """
    n_slots = pattern_ticks // TICKS_16TH
    slots: dict[int, int] = {}
    for t in kick_ticks:
        slot = min(n_slots - 1, (t + TICKS_16TH // 2) // TICKS_16TH)
        nominal = slot * TICKS_16TH
        if slot not in slots or abs(t - nominal) < abs(slots[slot] - nominal):
            slots[slot] = t
    return slots


# ---------------------------------------------------------------- ルート列（prog レーン）

def generate_root_sequence(chords: dict | None, key: dict, rng: np.random.Generator) -> list[int]:
    """ハーモニースロットごとのルート（ピッチクラス）列を返す。

    chords 無し → [tonic]（1スロット＝ルート連打への退化）。
    chords 有り → loop_bars × slots_per_bar スロット。循環（末尾→先頭を含む）で数えた
    ルート変化数が root_changes_per_loop（クランプ後）に一致するよう、変化位置を先に
    ガチャで選び、区間ごとにスケール内のルートを root_motion の方向性で割り当てる。
    スロット0は必ずトニック（ループ頭で調が見える＝キーに寄り添う骨格）。
    """
    tonic = key["root"]
    if chords is None:
        return [tonic]

    n = chords["loop_bars"] * chords["slots_per_bar"]
    target = min(max(chords["root_changes_per_loop"], 0), n)
    if target == 1:
        target = 2  # 循環では変化1回のルート列が構成できない（1回変われば戻りでもう1回）
    if target == 0 or n < 2:
        return [tonic] * n

    # 変化位置: n個の循環境界（境界 i = スロット i-1→i の間）から target 個選ぶ。
    # スロット0の頭は必ず境界にする（ループ頭でトニックに戻る性格。my-way 実測でも
    # ループ頭は変化点）。残りは乱数で選ぶ
    others = rng.choice(n - 1, size=target - 1, replace=False) + 1 if target > 1 else []
    boundaries = sorted({0, *map(int, others)})

    # 区間（境界から次の境界まで）ごとにルートを割り当てる。スロット0を含む先頭区間は
    # トニック。以降は直前と異なるルートを方向性に従って選び、最終区間は先頭区間
    # （トニック）とも異なるようにする（境界0が変化として成立するため）
    degrees = scale_degrees(key["mode"])
    motion = chords["root_motion"]
    roots_per_run: list[int] = [tonic]
    for i in range(1, len(boundaries)):
        exclude = {roots_per_run[-1]}
        if i == len(boundaries) - 1:
            exclude.add(tonic)
        roots_per_run.append(next_root(roots_per_run[-1], exclude, degrees, tonic, motion, rng))

    sequence = [tonic] * n
    for i, start in enumerate(boundaries):
        end = boundaries[i + 1] if i + 1 < len(boundaries) else n
        for slot in range(start, end):
            sequence[slot] = roots_per_run[i]
    return sequence


def next_root(prev_pc: int, exclude: set[int], degrees: tuple[int, ...], tonic: int,
              motion: str, rng: np.random.Generator) -> int:
    """直前のルートから次のルートをスケール度数のステップで選ぶ。

    root_motion の方向性: up/down は 1〜3度のステップを同方向に、mixed/static は両方向。
    exclude（直前・必要ならトニック）を避けられるまで引き直し、詰んだらスケール内の
    残りから一様に選ぶ（7音あるので exclude 2個で詰みはしない）。
    """
    prev_degree = next((i for i, d in enumerate(degrees) if (tonic + d) % 12 == prev_pc), 0)
    for _ in range(16):
        step = int(rng.integers(1, 4))
        if motion == "down":
            step = -step
        elif motion != "up" and rng.random() < 0.5:  # mixed / static は方向も振る
            step = -step
        candidate = (tonic + degrees[(prev_degree + step) % 7]) % 12
        if candidate not in exclude:
            return candidate
    pool = [pc for d in degrees if (pc := (tonic + d) % 12) not in exclude]
    return int(pool[rng.integers(0, len(pool))]) if pool else prev_pc


# ---------------------------------------------------------------- リズム＋音高（rhythm レーン）

def generate_notes(effective: dict, key: dict, kick_ticks: list[int],
                   lane_seeds: dict[str, int], bars: int, bpm: float,
                   roots_cfg: dict | None = None) -> list[tuple[int, int, int]]:
    """(tick, midi_note, velocity) のリスト（tick 昇順・モノフォニック前提の発音列）。

    パターン（loop_bars）を生成して bars まで繰り返す。装飾もパターン内で
    確定するので、繰り返しは完全なコピー（同一 seed で --bars だけ変えても同じパターン）。

    roots_cfg（ループ追従モード）があれば進行はその固定ルート列 — prog レーンの
    ガチャは走らず、振り直しで変わるのはリズムだけになる。
    """
    chords = effective["chords"]
    bass = effective["bass"]
    if roots_cfg is not None:
        loop_bars = roots_cfg["loop_bars"]
        pattern_ticks = loop_bars * TICKS_BAR
        harmony_ticks = TICKS_BAR // roots_cfg["slots_per_bar"]
        roots = list(roots_cfg["roots"])
    else:
        loop_bars = loop_bars_of(effective)
        pattern_ticks = loop_bars * TICKS_BAR
        slots_per_bar = chords["slots_per_bar"] if chords else 1
        harmony_ticks = TICKS_BAR // slots_per_bar if chords else pattern_ticks

        roots = generate_root_sequence(chords, key, np.random.default_rng(lane_seeds["prog"]))
    rng = np.random.default_rng(lane_seeds["rhythm"])

    kicks = kick_slot_map(kick_ticks, pattern_ticks)

    profile = bass["onset_rate_by_16th"]
    # スロット選択 → (グローバルスロット, 実tick, プロファイル重み) をパターン全体で集める。
    # 小節ごとの本数は確率的丸め（0本の小節も正当 — notes_per_bar < 1 の疎なカードを
    # 小節単位の最低1音で増量しない）。無音パターンだけは避け、パターン全体で最低1音を保証する
    picked: list[tuple[int, int, float]] = []
    for bar in range(loop_bars):
        frac, base_count = math.modf(bass["notes_per_bar"])
        count = int(base_count) + (1 if rng.random() < frac else 0)
        count = min(16, count)
        weights = np.array([profile[s] + WEIGHT_EPS for s in range(16)])
        for s in range(16):
            if bar * 16 + s in kicks:
                weights[s] *= KICK_BOOST
        chosen = sorted(int(s) for s in rng.choice(16, size=count, replace=False,
                                                   p=weights / weights.sum())) if count > 0 else []
        for slot in chosen:
            gslot = bar * 16 + slot
            # タイミングはグリッド正位置か、キックのあるスロットなら実 tick へのスナップの2択
            # （ジッターは掛けない — v4。ノリはキック同時打ちとスロット選択で出す）
            tick = kicks[gslot] if gslot in kicks else gslot * TICKS_16TH
            picked.append((gslot, tick, profile[slot]))
    if not picked:
        # 全小節が0本 → 1小節目のプロファイル最大スロットに1音だけ置く（決定的。乱数は使わない）。
        # tick の解決は通常経路と同じ規則を通す — キックのあるスロットなら実 tick へスナップ
        # （正位置に直置きすると疎な候補だけキックとフラムになる）。ジッターは掛けない（乱数を使わない）
        slot = int(np.argmax(profile))
        tick = kicks[slot] if slot in kicks else slot * TICKS_16TH
        picked.append((slot, tick, profile[slot]))

    # 単調性と範囲（スナップ後でも順序が逆転しない）
    events: list[tuple[int, int, float]] = []
    prev_tick = -1
    for gslot, tick, w in picked:
        tick = max(tick, 0, prev_tick + 1)
        tick = min(tick, pattern_ticks - 1)
        events.append((gslot, tick, w))
        prev_tick = tick

    # 音高: ハーモニースロットのルートを窓（28..39）に置き、先頭以外は少量の装飾。
    # 装飾の候補は 完全5度(+7)・オクターブ(+12)・minor のダイアトニック7度(+10。
    # has_7th_or_more のカードだけ)。major の M7 は半音下のリードトーンがベースでは
    # 不協和に鳴りやすいので 5度系に倒す（プラン確定）。
    # 候補は**曲のスケール構成音でフィルタする** — ダイアトニックなルートに対する
    # 完全5度・短7度が常にスケール内とは限らない（例: C major のルート B +7 = F♯）。
    # +12 は同じピッチクラスなので常に残り、フィルタ結果が空になることはない
    ornament_pool = [7, 12]
    if key["mode"] == "minor" and chords and chords["has_7th_or_more"]:
        ornament_pool.append(10)
    scale = {(key["root"] + d) % 12 for d in scale_degrees(key["mode"])}
    seen_harmony: set[int] = set()
    notes: list[tuple[int, int, int]] = []
    for gslot, tick, w in events:
        hslot = (gslot * TICKS_16TH) // harmony_ticks % max(1, len(roots))
        root_pc = roots[hslot]
        midi = ROOT_WINDOW_LOW + (root_pc - ROOT_WINDOW_LOW) % 12
        is_first = hslot not in seen_harmony
        seen_harmony.add(hslot)
        roll = rng.random()  # 乱数消費を装飾の有無に依らせない（毎ノート1回）
        if not is_first and roll < ORNAMENT_PROB:
            valid = [iv for iv in ornament_pool if (root_pc + iv) % 12 in scale]
            midi += valid[int(rng.integers(0, len(valid)))]
        midi = min(max(midi, HARD_RANGE[0]), HARD_RANGE[1])
        vel = int(np.clip(round(72 + 36 * w + rng.normal(0, 6)), 1, 127))
        notes.append((tick, midi, vel))

    # bars までパターンの繰り返しで埋める（bars は loop_bars の倍数であることを呼び出し側が保証）
    reps = bars // loop_bars
    return [(tick + r * pattern_ticks, midi, vel) for r in range(reps) for tick, midi, vel in notes]


# ---------------------------------------------------------------- MIDI

def build_midi(notes: list[tuple[int, int, int]], effective: dict, bpm: float, bars: int) -> mido.MidiFile:
    """モノフォニック: 次ノートの開始で前ノートを切る。note_off は bars 境界にクランプ。"""
    mid = mido.MidiFile(ticks_per_beat=PPQ)
    track = mido.MidiTrack()
    mid.tracks.append(track)
    track.append(mido.MetaMessage("time_signature", numerator=4, denominator=4, time=0))
    track.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(bpm), time=0))
    track.append(mido.Message("program_change", channel=BASS_CHANNEL, program=GM_FINGER_BASS, time=0))

    end_tick = bars * TICKS_BAR
    dur = max(1, round(effective["bass"]["note_length_16ths"] * TICKS_16TH))
    events = []  # (tick, 順序キー: off=0/on=1, note, velocity)
    ordered = sorted(notes)
    for i, (tick, note, vel) in enumerate(ordered):
        off = min(tick + dur, end_tick)
        if i + 1 < len(ordered):
            off = min(off, ordered[i + 1][0])  # モノフォニック: 次ノートで切る
        events.append((tick, 1, note, vel))
        events.append((off, 0, note, 0))
    events.sort()

    now = 0
    for tick, kind, note, vel in events:
        delta = tick - now
        now = tick
        if kind == 1:
            track.append(mido.Message("note_on", channel=BASS_CHANNEL, note=note, velocity=vel, time=delta))
        else:
            track.append(mido.Message("note_off", channel=BASS_CHANNEL, note=note, velocity=0, time=delta))
    track.append(mido.MetaMessage("end_of_track", time=0))
    return mid


# ---------------------------------------------------------------- wav 合成（耳チェック用）

# 聴きたいのは進行とリズムであって音色ではない。基音＋弱い倍音のシンプルな減衰音で足りる
# （本番の音色は LaLa の GM Finger Bass）。乱数を使わないので audio seed は不要
_MASTER_GAIN = 0.5


def _voice_bass(midi: int, dur_sec: float, vel: int) -> np.ndarray:
    freq = 440.0 * 2 ** ((midi - 69) / 12)
    release = 0.04
    t = np.arange(int((dur_sec + release) * SAMPLE_RATE)) / SAMPLE_RATE
    x = (np.sin(2 * np.pi * freq * t)
         + 0.35 * np.sin(4 * np.pi * freq * t) * np.exp(-t / 0.25)
         + 0.12 * np.sin(6 * np.pi * freq * t) * np.exp(-t / 0.12))
    env = np.minimum(1.0, t / 0.004) * np.exp(-t / 1.2)         # 短いアタック＋緩い減衰
    gate = len(t) - int(release * SAMPLE_RATE)
    env[gate:] *= np.linspace(1.0, 0.0, len(t) - gate)           # ゲート後の短いリリース
    x = x * env
    peak = np.max(np.abs(x)) + 1e-12
    return x / peak * (0.9 * vel / 127)


def synth_wav(notes: list[tuple[int, int, int]], effective: dict, bpm: float, bars: int) -> np.ndarray:
    total_sec = bars * 240.0 / bpm + 0.6
    buf = np.zeros(int(round(total_sec * SAMPLE_RATE)))
    dur = max(1, round(effective["bass"]["note_length_16ths"] * TICKS_16TH))
    ordered = sorted(notes)
    for i, (tick, midi, vel) in enumerate(ordered):
        end = min(tick + dur, bars * TICKS_BAR)
        if i + 1 < len(ordered):
            end = min(end, ordered[i + 1][0])  # モノフォニック（MIDI と同じ規則で切る）
        dur_sec = max(0.02, (end - tick) / PPQ * 60.0 / bpm)
        x = _voice_bass(midi, dur_sec, vel)
        pos = int(round(tick / PPQ * 60.0 / bpm * SAMPLE_RATE))
        stop = min(pos + len(x), len(buf))
        buf[pos:stop] += x[: stop - pos]
    return buf * _MASTER_GAIN


# ---------------------------------------------------------------- 候補の同一性

def cfg_payload(effective: dict, key: dict, kick_ticks: list[int], bpm: float, bars: int,
                roots_cfg: dict | None = None) -> dict:
    """「有効な生成入力」だけ。正規化した key・実効 BPM・折り畳んだ kick ticks を含める —
    同じ seed でもキー・BPM・ドラムが違えば別候補名になる（同名なら同内容の契約）。

    roots_cfg（ループ追従）は生成に効く最小セット {slots_per_bar, loop_bars, roots} だけを
    含める（confidence 等は音を変えないので同一性にも入れない）。無いときはキー自体を
    出さない — 従来のサイドカーとの payload 一致を壊さないため。"""
    payload = {
        "generator": "bass",
        "generator_version": GENERATOR_VERSION,
        "bpm": bpm,
        "bars": bars,
        "wav_sample_rate": SAMPLE_RATE,
        "key": key,
        "kick_ticks": kick_ticks,
        "bass": effective["bass"],
        "chords": effective["chords"],
    }
    if roots_cfg is not None:
        payload["roots"] = {"slots_per_bar": roots_cfg["slots_per_bar"],
                            "loop_bars": roots_cfg["loop_bars"],
                            "roots": list(roots_cfg["roots"])}
    return payload


def candidate_basename(lane_seeds: dict[str, int], cfg_sha: str) -> str:
    return f"bass-p{lane_seeds['prog']:08x}-r{lane_seeds['rhythm']:08x}-{cfg_sha[:6]}"


def load_sidecar(path: Path) -> tuple[dict, dict, list[int], float, int, list[str], dict[str, int], int, str | None, dict | None]:
    """サイドカーから1候補を完全再現するための入力一式を読む（--from）。

    config の bass / chords / key / kick_ticks をカードと同じ検証に通し、現在の生成器で
    正規化し直した payload と完全一致することを要求する（drums.py と同じ契約）。
    同時打ちスナップ位置は kick_ticks（折り畳み済みの実 tick）から再現される。
    """
    try:
        sc = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise GachaError(f"{path}: サイドカーを読めない: {e}") from e
    try:
        config = sc["config"]
        bars = config["bars"]
        bpm = config["bpm"]
        # 実効 BPM（30〜300）はカード BPM（40〜300）と範囲が違うため parse_card に通さない
        # （ダミーの card BPM を置き、bass/chords の検証だけを借りる）
        pseudo_card = {"card_version": 1, "global": {"bpm": 120.0}, "bass": config["bass"]}
        if config["chords"] is not None:
            pseudo_card["chords"] = config["chords"]
        key_root = config["key"]["root"]
        mode = config["key"]["mode"]
        kick_ticks = config["kick_ticks"]
        lane_seeds = {lane: int(sc["lane_seeds"][lane], 16) for lane in LANES}
        global_seed = sc["global_seed"]
        recorded_cfg_sha = sc["cfg_sha256"]
        recorded_cand_sha = sc["candidate_sha256"]
    except (KeyError, TypeError, ValueError) as e:
        raise GachaError(f"{path}: サイドカーの形式が不正: {e!r}") from e
    effective, _dummy_bpm, _ = parse_card(pseudo_card, str(path))
    if not is_num(bpm) or not math.isfinite(bpm) or not (30 <= bpm <= 300):
        raise GachaError(f"{path}: config.bpm が 30〜300 の外: {bpm!r}")
    bpm = float(bpm)
    if not isinstance(key_root, int) or isinstance(key_root, bool) \
            or not (0 <= key_root <= 11) or mode not in ("major", "minor"):
        raise GachaError(f"{path}: config.key が不正: {config.get('key')!r}")
    key = {"root": key_root, "mode": mode}
    if not isinstance(bars, int) or bars < 1:
        raise GachaError(f"{path}: config.bars が不正: {bars!r}")
    if not isinstance(kick_ticks, list):
        raise GachaError(f"{path}: config.kick_ticks がリストでない: {kick_ticks!r}")
    roots_cfg = config.get("roots")
    if roots_cfg is not None:
        try:
            validate_roots_core(roots_cfg.get("slots_per_bar"), roots_cfg.get("loop_bars"),
                                roots_cfg.get("roots"))
        except (ValueError, AttributeError) as e:
            raise GachaError(f"{path}: config.roots が不正: {e}") from e
    pattern_bars = roots_cfg["loop_bars"] if roots_cfg else loop_bars_of(effective)
    kick_ticks = fold_kick_ticks(kick_ticks, pattern_bars * TICKS_BAR)
    if any(not (0 <= s <= 0xFFFFFFFF) for s in lane_seeds.values()):
        raise GachaError(f"{path}: lane_seeds が32bitの範囲外: {sc['lane_seeds']}")
    rebuilt = cfg_payload(effective, key, kick_ticks, bpm, bars, roots_cfg)
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
    return (effective, key, kick_ticks, bpm, bars, defaulted, lane_seeds, global_seed,
            reference if isinstance(reference, str) else None, roots_cfg)


# ---------------------------------------------------------------- mix（--drums・使い捨て）

def load_drums_for_mix(path: Path, bass_bars: int) -> tuple[dict, dict[str, int], list[int], str]:
    """drums サイドカーからキック制約と mix 用ノートを導出する。

    パターンの tick はドラム候補自身の BPM で再構成する（＝配布済み .mid と同一の tick 列。
    決定的なので sidecar だけで正確に再現できる）。ドラム sidecar の bars がベースより
    短くても、1小節パターンの繰り返しなので bass_bars ぶん生成すれば同じパターンが続く
    （正規候補・sidecar には触れない。mix 合成時だけの操作）。
    返り値: (レーン別ノート, ドラムのレーン seed, キック tick 列, 識別子=candidate_sha256 先頭8桁)
    """
    effective, drums_bpm, _bars, _defaulted, lane_seeds, _gseed, _ref = drums_mod.load_sidecar(path)
    notes = drums_mod.generate_pattern(effective, lane_seeds, bars=bass_bars, bpm=drums_bpm)
    kick_ticks = [tick for tick, _vel in notes["kick"]]
    try:
        ident = json.loads(path.read_text())["candidate_sha256"][:8]
    except (OSError, json.JSONDecodeError, KeyError, TypeError):
        ident = "unknown"
    return notes, lane_seeds, kick_ticks, ident


def mix_with_drums(bass_wav: np.ndarray, drums_notes: dict, drums_lane_seeds: dict[str, int],
                   bpm: float, bars: int) -> np.ndarray:
    """ドラムとベースを重ねる。**ドラム側もベースの実効 BPM でレンダリングする** —
    Drums/Bass のカード BPM が違っても同じ小節境界で揃う（tick は音楽的位置なので不変）。"""
    drums_wav = drums_mod.synth_wav(drums_notes, bpm, bars, drums_lane_seeds)
    n = max(len(bass_wav), len(drums_wav))
    mix = np.zeros(n)
    mix[: len(drums_wav)] += drums_wav
    mix[: len(bass_wav)] += bass_wav
    peak = float(np.max(np.abs(mix))) if len(mix) else 0.0
    if peak > 0.98:
        mix *= 0.98 / peak  # 使い捨ての試聴用なので単純にピークを収める
    return mix


# ---------------------------------------------------------------- CLI

def run(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="制約カードとキーからベース候補をガチャ生成する")
    ap.add_argument("card", nargs="?", help="リファレンスフォルダ または card.json のパス")
    ap.add_argument("--key", type=str, default=None, help="キー ROOT:MODE（例 F#:minor）。--from 以外では必須")
    ap.add_argument("--bpm", type=float, default=None,
                    help="実効 BPM（LaLa はプロジェクト BPM を必ず渡す。省略時はカードの global.bpm）")
    ap.add_argument("--seed", type=int, default=None, help="全体 seed（省略時は乱数で決めて表示）")
    ap.add_argument("--count", type=int, default=None, help="候補数（全体 seed を +1 ずつ回す。既定 8）")
    ap.add_argument("--bars", type=int, default=None,
                    help="書き出す小節数（パターンの繰り返し。既定 loop_bars。loop_bars の倍数のみ）")
    ap.add_argument("--lock", type=str, default="", help="レーン seed の固定: prog=HEX8,rhythm=HEX8")
    ap.add_argument("--roots", type=str, default=None,
                    help="ループ追従モード: looproots.py の契約 JSON。進行をルート列で固定し、"
                         "リズムだけガチャする（契約の形式は looproots.py の docstring）")
    ap.add_argument("--kick-ticks", type=str, default=None,
                    help="キック位置（ドラムリージョン先頭基準の相対 PPQ tick のカンマ区切り。LaLa 連携用）")
    ap.add_argument("--drums", type=str, default=None,
                    help="ドラム候補のサイドカー .json（CLI 耳チェック用。キック制約の導出＋mix wav の出力）")
    ap.add_argument("--out", type=str, default=None, help="出力先（既定はカードと同じフォルダの gacha/）")
    ap.add_argument("--from", dest="from_sidecar", metavar="SIDECAR",
                    help="候補のサイドカー .json から1件を完全再現する（カード不要。他の生成条件はサイドカーの値を使う）")
    ap.add_argument("--porcelain", action="store_true", help="stdout を JSON Lines のみにする（機械可読）")
    args = ap.parse_args(argv)

    def note(*a, **kw):
        print(*a, **kw, file=sys.stderr if args.porcelain else sys.stdout)

    if args.kick_ticks is not None and args.drums is not None:
        raise GachaError("--kick-ticks と --drums は併用できない（キック制約の入力は片方だけ）")

    drums_sc_path = Path(args.drums).expanduser() if args.drums else None

    if args.from_sidecar:
        if (args.card or args.key is not None or args.bpm is not None or args.seed is not None
                or args.lock or args.count is not None or args.bars is not None
                or args.kick_ticks is not None or args.drums is not None or args.roots is not None):
            raise GachaError("--from は card / --key / --bpm / --seed / --count / --bars / --lock / "
                             "--kick-ticks / --drums / --roots と併用できない（生成条件はサイドカーの値を使う）")
        sc_path = Path(args.from_sidecar).expanduser()
        (effective, key, kick_ticks, bpm, bars, defaulted,
         lane_seeds, global_seed, reference, roots_cfg) = load_sidecar(sc_path)
        outdir = Path(args.out).expanduser() if args.out else sc_path.parent
        candidates = [(global_seed, lane_seeds)]
        drums_notes = None
        note(f"サイドカーから再現: {sc_path}（BPM {bpm:g} / {bars}小節）")
    else:
        if not args.card:
            raise GachaError("card（リファレンスフォルダ or card.json）か --from を指定する")
        if args.key is None:
            raise GachaError("--key ROOT:MODE が必要（生成は常にプロジェクトのキーで行う。カードの key は読まない）")
        key = parse_key(args.key)
        count = args.count if args.count is not None else 8
        if count < 1:
            raise GachaError("--count は 1 以上")
        card_path = resolve_card_path(args.card)
        effective, card_bpm, defaulted, reference = load_card(card_path)
        bpm = float(args.bpm) if args.bpm is not None else card_bpm
        if not math.isfinite(bpm) or not (30 <= bpm <= 300):
            raise GachaError(f"実効 BPM が 30〜300 の外: {bpm}")

        roots_cfg = None
        if args.roots is not None:
            try:
                roots_cfg = load_roots_contract(Path(args.roots).expanduser())
            except ValueError as e:
                raise GachaError(f"--roots: {e}") from e

        # 追従時のパターン長はループの実長（カードの loop_bars より契約が勝つ —
        # ドラムは曲A・進行は採用ループ、のパーツ別参照で食い違うのが普通のため）
        loop_bars = roots_cfg["loop_bars"] if roots_cfg else loop_bars_of(effective)
        bars = args.bars if args.bars is not None else loop_bars
        if bars < loop_bars or bars % loop_bars != 0:
            raise GachaError(
                f"--bars はパターン長 loop_bars={loop_bars} 以上かつその倍数のみ（指定: {bars}。切り詰めはしない）")

        drums_notes = None
        drums_lane_seeds: dict[str, int] = {}
        drums_ident = ""
        if drums_sc_path is not None:
            drums_notes, drums_lane_seeds, raw_kick_ticks, drums_ident = \
                load_drums_for_mix(drums_sc_path, bars)
        elif args.kick_ticks is not None:
            raw_kick_ticks = parse_kick_ticks_arg(args.kick_ticks)
        else:
            raw_kick_ticks = []
        kick_ticks = fold_kick_ticks(raw_kick_ticks, loop_bars * TICKS_BAR)

        locks = parse_locks(args.lock, LANES) if args.lock else {}
        if roots_cfg is not None and "prog" in locks:
            raise GachaError("--roots では進行がループ由来で固定されるため prog ロックは併用できない")
        outdir = Path(args.out).expanduser() if args.out else card_path.parent / "gacha"
        note(f"カード: {card_path}（実効 BPM {bpm:g} / キー {args.key} / パターン {loop_bars}小節 / 書き出し {bars}小節）")
        for field in defaulted:
            note(f"  [補完] {field} が無いため既定値を使う")
        if effective["chords"] is None and roots_cfg is None:
            note("  [退化] chords スライスが無いためルート固定・1小節パターン")
        if roots_cfg is not None:
            src = roots_cfg.get("source") or ""
            note(f"  [追従] ループのルート列で進行を固定"
                 f"（{roots_cfg['loop_bars']}小節×{roots_cfg['slots_per_bar']}スロット"
                 f"{'・' + src if src else ''}。振り直しで変わるのはリズムだけ）")
            if roots_cfg.get("degraded"):
                note("  [追従/退化] ループから進行が取れなかった契約（トニック連打）")
            if (roots_cfg.get("key_root"), roots_cfg.get("key_mode")) != (key["root"], key["mode"]):
                note("  [注意] ループのキーとプロジェクトのキーが違う（採用時の逆コピー漏れの可能性）")
        if kick_ticks:
            note(f"  [キック] {len(kick_ticks)} 打を同時打ちブーストに使う")
        if locks:
            note("  [ロック] " + ", ".join(f"{k}={v:08x}" for k, v in locks.items()))
        global_seed = args.seed if args.seed is not None else secrets.randbits(32)
        if args.seed is None:
            note(f"全体 seed: {global_seed}（--seed {global_seed} で再現できる）")
        candidates = [(global_seed + i, resolve_lane_seeds("bass", LANES, global_seed + i, locks))
                      for i in range(count)]
        if roots_cfg is not None:
            # 追従時は prog レーンを生成に使わない。seed を 0 に正規化して候補名・同一性から
            # 実質的に外す — さもないと「同じ音の候補が別名で複数生成される」
            # （rhythm ロック時は既存の seen チェックで 1 件に畳まれる）
            candidates = [(g, {**ls, "prog": 0}) for g, ls in candidates]

    outdir.mkdir(parents=True, exist_ok=True)
    payload = cfg_payload(effective, key, kick_ticks, bpm, bars, roots_cfg)
    cfg_sha = cfg_hash(payload)
    generated = skipped = 0
    seen: set[str] = set()

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
        notes_list = generate_notes(effective, key, kick_ticks, lane_seeds, bars, bpm, roots_cfg)
        wav_data = synth_wav(notes_list, effective, bpm, bars)
        if status == "skip":
            note(f"  [スキップ] {base}: 完成済み（同じ seed・同じ設定＝同じ内容）")
            emit(base, lane_seeds_hex, "skipped")
            skipped += 1
        else:
            mid = build_midi(notes_list, effective, bpm, bars)
            sidecar = {
                "cfg_sha256": cfg_sha,
                "candidate_sha256": cand_sha,
                "global_seed": gseed,
                "lane_seeds": lane_seeds_hex,
                "config": payload,
                "defaulted_fields": defaulted,
            }
            if reference is not None:
                sidecar["reference"] = reference
            publish_candidate(outdir, base, mid, wav_data, sidecar, SAMPLE_RATE)
            label = "再生成" if status == "regen" else "生成"
            note(f"  [{label}] {base}")
            emit(base, lane_seeds_hex, "regenerated" if status == "regen" else "generated")
            generated += 1
        # mix は使い捨て成果物（候補の同一性・skip 判定の外）。スキップ候補にも出す —
        # ドラム差し替え時にベース候補が skip でも新しいドラムとの mix を聴けるように
        if drums_notes is not None:
            mix = mix_with_drums(wav_data, drums_notes, drums_lane_seeds, bpm, bars)
            mix_path = write_mix_wav(outdir, f"{base}.mix-{drums_ident}.wav", mix, SAMPLE_RATE)
            note(f"    [mix] {mix_path.name}")

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
