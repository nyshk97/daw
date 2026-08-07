#!/usr/bin/env python
"""report.md の【機】充填と完成検査 — レポート生成のうち機械で決まる部分の全て。

report-template.md の {{...}} を analysis/*.json から充填したドラフト（report.md.next）と、
【判】の執筆に必要な値だけを集めたダイジェスト JSON を出力する。Claude Code（report.sh が呼ぶ）は
ドラフトの【判】マーカーの内側だけを書く。数値の転記と判定語の線引きを AI に任せない
（転記ミスをゼロにし、曲をまたいで目盛りを揃える）ための分業。

判定の所有:
- gates.py = 測定可否・確度（テンポ安定・小節頭・スウィング測定可否・キー確度・ハーモニー可否）。
  ここでは再実装せず gates.json の結論をレンダリングするだけ（card.py と真実の源を共有する）
- report.py = ゲート通過後の表現分類だけ（ハネの程度・クオンタイズ・ループ長/コード確信度・
  ステム分離・basic-pitch の線引き）。閾値を変えるときは docs/labs/reference-beat.md に理由を残す

使い方:
  report.py fill <ref> [--out PATH] [--digest PATH]   ドラフトとダイジェストを書く
  report.py check <file> --digest PATH                完成検査（AI が【判】を書き終えたか）

exit code（fill）: 0=OK / 3=BPM系ゲート落ち（ドラフトを出さない） / 1=バグ・入力不足
exit code（check）: 0=完成 / 1=不合格（理由を stderr に出す）
"""
import argparse
import json
import re
import sys
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gates import HARMONY_STEMS  # noqa: E402  コード合議の対象ステム（真実の源は gates.py）

TOOLS = Path(__file__).resolve().parent
TEMPLATE = TOOLS / "report-template.md"

PC_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
STEM_LABELS = {"drums": "ドラム", "bass": "ベース", "vocals": "ボーカル",
               "guitar": "上モノ（guitar）", "piano": "上モノ（piano）", "other": "上モノ（other）"}
SECTION_LABELS = {"drums": "ドラム", "bass": "ベース", "vocals": "声",
                  "guitar": "上モノ(guitar)", "piano": "上モノ(piano)", "other": "上モノ(other)"}

# 完成検査が見る必須見出し（先頭一致。「— 結論」部分は AI が書き換えるので前方だけ）
REQUIRED_HEADINGS = ["## 基本情報", "## ハーモニー", "## グルーヴ", "## 音色と帯域", "## 構成",
                     "# 付録: この分析について"]


# ---------------------------------------------------------------- 判定語（表現分類）

def swing_word(ratio: float) -> str:
    """スウィング比 → 判定語。0.500=イーブン・0.667=3連（★2026-08-03 第1号で仮決め）"""
    if ratio < 0.52:
        return "無し"
    if ratio <= 0.58:
        return "軽くハネる"
    return "はっきりハネる"


def quantize_word(max_abs_dev_ms: float) -> str:
    if max_abs_dev_ms <= 10:
        return "完全（打ち込み）"
    if max_abs_dev_ms <= 25:
        return "甘い（人力 or 意図的なヨレ）"
    return "生演奏、またはグリッドがずれている（先にグリッドを疑う）"


def separation_word(residual_below_mix_db: float) -> str:
    if residual_below_mix_db <= -20:
        return "使える"
    if residual_below_mix_db <= -15:
        return "条件つき（分析には使えるが音として使うのは避ける）"
    return "疑う"


def basicpitch_word(chord_tone_ratio: float, within_30ms: float) -> str:
    if chord_tone_ratio >= 0.8 and within_30ms >= 0.7:
        return "統計用途OK"
    return "統計も疑う"


def loop_confident(similarity_by_lag: dict, adopted: str) -> bool:
    """採用ラグが他より +0.05 以上高ければ断定してよい"""
    others = [v for k, v in similarity_by_lag.items() if k != adopted]
    return not others or similarity_by_lag[adopted] - max(others) >= 0.05


# ---------------------------------------------------------------- 書式ヘルパ

def mmss(sec: float) -> str:
    s = int(round(sec))
    return f"{s // 60}:{s % 60:02d}"


def db(v: float) -> str:
    return f"+{v:.1f}" if v > 0 else f"{v:.1f}"


MAJOR_STEPS = (0, 2, 4, 5, 7, 9, 11)
MINOR_STEPS = (0, 2, 3, 5, 7, 8, 10)  # 自然的短音階を比較基準にする（和声・旋律的短音階の変位音はスケール外扱いになる）


def scale_notes(pitch_class_weight: list, key_value: str) -> str:
    """ベースの音名重み上位7音を、キーのルートから時計回りの順で（ピアノロール表記=#）。

    実測の転記なのでキーのスケール音とは限らない。差分を注記しないと
    スケール音の一覧に見えて紛らわしい（キー確度「中」の曲で実例あり）。
    重みが最大の1/10未満の音は落とす — 3音リフの曲で出席率0.1%の音まで
    「使う音」に昇格し、スケール注記が意図的な音使いに見えてしまうため
    （docs/labs/reference-beat.md 2026-08-07）。
    """
    root_name, mode = key_value.split(" ")
    root_name = root_name.replace("♯", "#").replace("♭", "b")
    flat_to_sharp = {"Cb": "B", "Db": "C#", "Eb": "D#", "Fb": "E", "Gb": "F#", "Ab": "G#", "Bb": "A#"}
    root_pc = PC_NAMES.index(flat_to_sharp.get(root_name, root_name))
    floor = max(pitch_class_weight) / 10
    kept = sorted((pc for pc in range(12) if pitch_class_weight[pc] >= floor),
                  key=lambda pc: pitch_class_weight[pc], reverse=True)[:7]
    ordered = sorted(kept, key=lambda pc: (pc - root_pc) % 12)
    notes = " ".join(PC_NAMES[pc] for pc in ordered)

    scale = {(root_pc + s) % 12 for s in (MAJOR_STEPS if mode == "major" else MINOR_STEPS)}
    missing = sorted(scale - set(kept), key=lambda pc: (pc - root_pc) % 12)
    extra = sorted(set(kept) - scale, key=lambda pc: (pc - root_pc) % 12)
    m = "・".join(PC_NAMES[p] for p in missing)
    e = "・".join(PC_NAMES[p] for p in extra)
    if len(kept) == 7:
        # スケール7音と同数なので、欠けと外は必ず対で出るか、完全一致か
        note = f"{key_value} のスケールの {m} は踏まず、外の {e} を踏む" if missing else "キーのスケールと一致"
        return f"**{notes}**（ベース実測の上位7音。{note}）"
    pct = round(sum(pitch_class_weight[pc] for pc in kept) * 100)
    note = f"{e} はスケール外" if extra else f"いずれも {key_value} のスケール内"
    return f"**{notes}**（ベース実測。この{len(kept)}音で全体の{pct}%。{note}）"


# ---------------------------------------------------------------- 楽器編成・構成

def active_ranges(sections: list, stem: str) -> list:
    """stem が active な連続小節範囲 [(start, end), ...]"""
    ranges = []
    for s in sections:
        if stem in s["active"]:
            if ranges and ranges[-1][1] == s["bar_start"] - 1:
                ranges[-1] = (ranges[-1][0], s["bar_end"])
            else:
                ranges.append((s["bar_start"], s["bar_end"]))
    return ranges


def in_out_text(sections: list, stem: str, downbeat_ok: bool) -> str:
    if not downbeat_ok:
        return "—（小節頭が取れず小節位置は測れない）"
    ranges = active_ranges(sections, stem)
    if not ranges:
        return "—"
    last_bar = sections[-1]["bar_end"]
    start = "冒頭から" if ranges[0][0] == 1 else f"{ranges[0][0]}小節目から"
    end = "最後まで" if ranges[-1][1] == last_bar else f"{ranges[-1][1]}小節目まで"
    gaps = len(ranges) - 1
    return f"{start}{end}" + (f"。途中{gaps}回抜ける" if gaps else "")


def upper_stems(stems6: dict, sections: list) -> list:
    """楽器編成に載せる上モノ（arrangement で一度でも active になったもの。音量降順）"""
    present = {s for sec in sections for s in sec["active"]}
    ups = [s for s in ("piano", "guitar", "other") if s in present]
    return sorted(ups, key=lambda s: stems6["stems"][s]["rms_db"], reverse=True)


def instrumentation_section(stems6: dict, arrangement: dict, timbres: dict, downbeat_ok: bool) -> str:
    """### 楽器編成 のブロック全体（インライン【判】マーカー入り）を返す"""
    sections = arrangement["sections"]
    present = {s for sec in sections for s in sec["active"]}
    rows_order = [s for s in ("drums", "bass") if s in present] \
        + upper_stems(stems6, sections) \
        + (["vocals"] if "vocals" in present else [])
    max_active = max(len(s["active"]) for s in sections)
    lines = [f"### 楽器編成 — {len(rows_order)}系統、同時に鳴るのは最大{max_active}", "",
             "| 系統 | 中身 | 出入り |", "|---|---|---|"]
    for stem in rows_order:
        st = stems6["stems"][stem]
        facts = [f"重心{st['spectral_centroid_hz_median']:.0f}Hz", f"幅{st['stereo_width']:.2f}"]
        if stem in timbres:
            t = timbres[stem]
            facts.append(f"アタック{t['attack_ms_median']:.0f}ms・250ms後{t['sustain_ratio_250ms']:.2f}")
        marker = f"inst-{stem}"
        cell = (f"<!-- 判i:{marker}:start -->〈呼び名（〜系）と一言〉<!-- 判i:{marker}:end -->"
                f" — {('・'.join(facts))}")
        lines.append(f"| {STEM_LABELS[stem]} | {cell} | {in_out_text(sections, stem, downbeat_ok)} |")
    lines += ["", "系統の名前は demucs の分類なので、実楽器と1対1ではない（呼び名は音色の測定値からの推定）。"]
    return "\n".join(lines)


def structure_table(arrangement: dict, downbeat_ok: bool) -> str:
    if not downbeat_ok:
        return ("セクション表は載せない — 小節頭の位相が取れておらず（downbeat ゲート落ち）、"
                "小節番号・区切りが信用できない。\n"
                "<!-- 【判】への指示: 小節番号に依存する記述をしない。音量・編成の大まかな推移だけ書く -->")
    lines = ["| 小節 | 編成 | ミックスRMS |", "|---|---|---|"]
    for s in arrangement["sections"]:
        label = " + ".join(SECTION_LABELS[a] for a in s["active"]) if s["active"] else "—（閾値未満の要素のみ）"
        lines.append(f"| {s['bar_start']}-{s['bar_end']} | {label} | {s['rms_db_mean']:.1f}dB |")
    return "\n".join(lines)


# ---------------------------------------------------------------- ハーモニー・付録ブロック

def harmony_block(gates: dict, topline: dict) -> str:
    if not gates["harmony"]["ok"]:
        return ("上モノがほぼ無いため、コード進行は推定しない（6分割の全ステムがかぶりノイズしか\n"
                "見ておらず、揃って同じ誤答を出すため合議が成立しない）。\n"
                "<!-- 【判】への指示: コード進行の代わりに、この曲が何で構成されているかを書く -->")
    if not gates["downbeat"]["ok"]:
        return ("コード進行の表は載せない — 小節頭の位相が取れておらず（downbeat ゲート落ち）、\n"
                "スロット割り（小節×拍）が無意味なため。\n"
                "<!-- 【判】への指示: 進行を書かず、響きの傾向（7th の有無・明暗）だけ書く -->")
    prog = topline["loop_progression"]
    slots = prog["progression"]
    confs = [p["conf"] for p in slots if p.get("conf") is not None]
    conf_floor = (max(confs) - 0.2) if confs else None
    by_bar = {}
    for p in slots:
        by_bar.setdefault(p["bar"], {})[p.get("sub", 0)] = p
    lines = [f"{prog['loop_bars']}小節ループを{prog.get('repetitions_folded', '?')}回ぶん重ねてから推定した結果"
             f"（採用ステム: {gates['harmony']['source_stem']}。ステム間の一致は付録「ループ長の判定」）。", "",
             "| 小節 | 前半（1-2拍） | 後半（3-4拍） |", "|---|---|---|"]

    def cell(p):
        if p is None:
            return "—"
        chord = f"**{p['chord']}**"
        if conf_floor is not None and p.get("conf") is not None and p["conf"] < conf_floor:
            chord += "（確信度低め）"
        return chord

    for bar in sorted(by_bar):
        lines.append(f"| {bar} | {cell(by_bar[bar].get(0))} | {cell(by_bar[bar].get(1))} |")
    lines += ["", "「確信度低め」= 同ループ内の最大confから-0.2より低いスロット。耳での確認（listen/02）に回す。"]
    return "\n".join(lines)


def measured_16th_block(groove: dict, downbeat_ok: bool) -> str:
    if not downbeat_ok:
        return ("16分ごとの出現強度は載せない — 小節頭の位相が取れておらず（downbeat ゲート落ち）、"
                "16分グリッド上の位置が無意味なため。")

    def rel(values):
        top = max(values) or 1.0
        return [round(v / top * 100) for v in values]

    head = "16th :  " + "".join(f"{s:>4}" for s in ["1", "e", "&", "a", "2", "e", "&", "a",
                                                    "3", "e", "&", "a", "4", "e", "&", "a"])
    rows = [("kick", rel(groove["drums"]["low"]["profile_by_16th"])),
            ("snare", rel(groove["drums"]["mid"]["profile_by_16th"])),
            ("hat", rel(groove["drums"]["high"]["profile_by_16th"])),
            ("bass", rel(groove["bass"]["onset_rate_by_16th"]))]
    body = "\n".join(f"{name:<5}:  " + "".join(f"{v:>4}" for v in vals) for name, vals in rows)
    return ("```\n" + head + "\n" + body + "\n```\n\n"
            "snare 行=中域（スネア・クラップの胴）、hat 行=高域（ハット。クラップのアタックが混入する）、\n"
            "bass 行=オンセット率。kick 行（低域）の16分位置は時間分解能不足で1〜2個ぶん滲む — 拍単位で読む。")


def measured_stems_block(stems6: dict, timbres: dict) -> str:
    lines = ["| ステム | RMS | 重心 | 250Hz-2k | 2k以上 | 幅 | アタック | 250ms後 |",
             "|---|---|---|---|---|---|---|---|"]
    order = sorted(stems6["stems"], key=lambda s: stems6["stems"][s]["rms_db"], reverse=True)
    for stem in order:
        st = stems6["stems"][stem]
        bb = st["band_balance"]
        hi = bb["hi_2k_6k"] + bb["air_6k_11k"]
        t = timbres.get(stem)
        attack = f"{t['attack_ms_median']:.0f}ms" if t else "—"
        sustain = f"{t['sustain_ratio_250ms']:.2f}" if t else "—"
        lines.append(f"| {stem} | {st['rms_db']:.1f}dB | {st['spectral_centroid_hz_median']:.0f}Hz "
                     f"| {bb['mid_250_2k']:.3f} | {hi:.3f} | {st['stereo_width']:.2f} | {attack} | {sustain} |")
    return "\n".join(lines)


def loop_judgement_block(gates: dict, topline: dict | None) -> str:
    h = gates["harmony"]
    if not h.get("stems"):
        return "上モノの判定材料が無い。"
    lines = ["| ステム | 使用可否（ミックス比） | ループ判定 |", "|---|---|---|"]
    for s in h["stems"]:
        usable = "○" if s["usable"] else "×"
        lines.append(f"| {s['stem']} | {usable}（{s['rms_below_mix_db']:.1f}dB） | {s.get('loop', '—')} |")
    if h["ok"] and topline is not None and "loop" in topline:
        sim = topline["loop"]["similarity_by_lag"]
        adopted = topline["loop"]["most_likely"]
        sims = " / ".join(f"{k} {v:.3f}" for k, v in sim.items())
        verdict = "他ラグより +0.05 以上高く断定できる" if loop_confident(sim, adopted) else "他ラグと僅差（確信度低め）"
        lines += ["", f"採用 {h['source_stem']} の自己相似: {sims} → **{adopted}**（{verdict}）"]
    return "\n".join(lines)


def trust_table(gates: dict, topline: dict | None) -> str:
    ts = gates["tempo_stable"]
    contrast = ts.get("grid_contrast_by_eighth")
    bpm_basis = (f"grid_contrast 全区間 {min(contrast):.2f}–{max(contrast):.2f}（>1.0）・"
                 f"局所BPM標準偏差 {ts.get('local_bpm_std', float('nan')):.2f}") if contrast else "tempo_stable ゲート通過"
    dn = gates["downbeat"]
    down_conf = "高" if dn["ok"] else "低"
    down_basis = (f"バックビート強度 {dn.get('backbeat_strength', float('nan')):.2f}（2・4拍が突出）"
                  if dn["ok"] else "2・4拍が立っておらず位相が疑わしい")
    key = gates["key"]
    key_basis = f"ベース音名の上位7音が全体の {key['top7_mass'] * 100:.0f}%" if "top7_mass" in key else "gates.key の判定"
    h = gates["harmony"]
    if not h["ok"]:
        chord_conf, chord_basis = "—", "上モノが無く推定していない"
    elif not dn["ok"]:
        chord_conf, chord_basis = "—", "小節頭が取れず進行を推定していない"
    else:
        # 合議に数えてよいのは6分割の上モノだけ（gates.HARMONY_STEMS）。4分割 other は
        # 1本きりで独立の検算にならない（ループ長 8小節 の誤答実績あり）
        usable = [s for s in h.get("stems", []) if s["usable"] and s["stem"] in HARMONY_STEMS]
        adopted_loop = next((s.get("loop") for s in usable if s["stem"] == h["source_stem"]), None)
        agree = sum(1 for s in usable if s.get("loop") == adopted_loop)
        chord_conf = "高" if agree >= 2 else "中"
        chord_basis = f"{agree}ステムが独立に同じ {adopted_loop} を出した" if agree >= 2 else "6分割ステム間でループ長が一致しない"
    # 16分位置の2行は downbeat が取れているときだけ「測定済み」の顔をしてよい
    # （「低」は測った上での低確度で、「測っていない」とは別物）
    if dn["ok"]:
        hat_row = ("クラップ/ハットの16分位置", "中〜高",
                   "中高域はアタックが立つので位置は取れる。ただしハット帯域にクラップのアタックが混入する")
        kick_row = ("キックの16分位置", "低", "低域は時間分解能が原理的に足りず1〜2個ぶん滲む。拍単位でしか読んでいない")
    else:
        hat_row = ("クラップ/ハットの16分位置", "—", "小節頭が取れず位置を推定していない")
        kick_row = ("キックの16分位置", "—", "小節頭が取れず位置を推定していない")
    rows = [("BPM " + str(gates["bpm"]["value"]), "高", bpm_basis),
            ("小節頭の位置", down_conf, down_basis),
            ("キー", key["confidence"], key_basis),
            ("コード進行の骨格", chord_conf, chord_basis),
            ("コードの細かい種別（9th/sus/転回）", "中", "chroma のにじみで隣接音の区別が甘い"),
            hat_row,
            kick_row,
            ("ベースの音高", "中", "pyin がオクターブを取り違えるので 5-95% 区間で見ている"),
            ("音声→MIDI（basic-pitch）", "低〜中", "下の表")]
    lines = ["| 項目 | 信頼度 | 根拠 |", "|---|---|---|"]
    lines += [f"| {item} | **{conf}** | {basis} |" for item, conf, basis in rows]
    return "\n".join(lines)


def basicpitch_block(toplines: dict) -> str:
    rows = []
    for name, t in sorted(toplines.items()):
        m = t.get("midi")
        if not m:
            continue
        verdict = basicpitch_word(m["chord_tone_ratio"], m["grid_dev_ms_within_30ms"])
        rows.append(f"| {name} | {m['count']} | {m['notes_per_bar']:.1f} | {m['chord_tone_ratio']:.3f} "
                    f"| {m['grid_dev_ms_within_30ms']:.3f} | {verdict} |")
    if not rows:
        return "音声→MIDI の出力が無い。"
    return "\n".join(["| 入力ステム | 音数 | 音/小節 | コード構成音率 | 16分±30ms内 | 判定 |",
                      "|---|---|---|---|---|---|"] + rows
                     + ["", "判定の線: コード構成音率 ≥0.8 かつ ±30ms内 ≥0.7 →「統計用途OK」（譜面としては使えない。"
                        "持続音を短い音に刻む）。ボーカルの ±30ms内 が低いのは歌がグリッドに乗らないだけで正常。"])


def separation_block(stems4: dict | None, stems6: dict) -> str:
    parts = []
    if stems4:
        v4 = stems4["reconstruction"]["residual_below_mix_db"]
        parts.append(f"4分割は加算残差 {v4:.1f}dB（{separation_word(v4)}）")
    v6 = stems6["reconstruction"]["residual_below_mix_db"]
    parts.append(f"6分割は {v6:.1f}dB（{separation_word(v6)}）")
    floors = " / ".join(f"{s} {stems6['stems'][s]['floor_below_peak_db']:.1f}dB"
                        for s in sorted(stems6["stems"], key=lambda s: stems6["stems"][s]["floor_below_peak_db"]))
    return ("、".join(parts) + "。\n\n休符区間の残留（ピーク比）: " + floors + "。\n"
            "曲中ずっと鳴っているステム（drums 等）ではこの数字は意味を持たない — 休符が無いだけで分離が汚いわけではない。")


# ---------------------------------------------------------------- fill

def load_json(path: Path):
    return json.loads(path.read_text())


def build_fills(ref: Path) -> tuple:
    """テンプレの {{...}} に入れる値と、ダイジェスト・マーカー一覧を組み立てる"""
    a = ref / "analysis"
    gates = load_json(a / "gates.json")
    basics = load_json(a / "basics.json")
    overview = load_json(a / "overview.json")
    groove = load_json(a / "groove.json")
    arrangement = load_json(a / "arrangement.json")
    stems6 = load_json(a / "stems-6s.json")
    stems4 = load_json(a / "stems-4s.json") if (a / "stems-4s.json").exists() else None

    toplines = {p.stem.removeprefix("topline-"): load_json(p) for p in sorted(a.glob("topline-*.json"))}
    timbres = {name.removeprefix("6s-"): t["timbre"] for name, t in toplines.items()
               if name.startswith("6s-") and "timbre" in t}
    topline_src = toplines.get(gates["harmony"]["source_stem"]) if gates["harmony"]["ok"] else None

    info_path = ref / "source.info.json"
    info = load_json(info_path) if info_path.exists() else None

    title = (info or {}).get("title") or ref.name
    today = date.today().isoformat()
    header_parts = []
    if info:
        if info.get("channel"):
            header_parts.append(f"チャンネル {info['channel']}")
        if info.get("upload_date"):
            d = info["upload_date"]
            header_parts.append(f"動画公開日 {d[:4]}-{d[4:6]}-{d[6:]}")
    header_parts.append(f"分析日 {today}")
    if info and info.get("webpage_url"):
        header_parts.append(f"[原曲]({info['webpage_url']})")

    # 基本情報の各行
    sw, gsw = gates["swing"], groove.get("swing") or {}
    if sw["ok"]:
        ratio = sw["ratio"]
        swing_line = f"**{swing_word(ratio)}**（スウィング比 {ratio:.3f} / 0.500がイーブン・0.667が3連）"
    else:
        swing_line = "測れなかった（ハットが無く高域が別の音を拾っている。「ハネ無し」とは別）"

    beat_devs = [abs(groove["drums"]["mid"]["dev_ms_by_16th"][i])
                 for i in (0, 4, 8, 12) if groove["drums"]["mid"]["dev_ms_by_16th"][i] is not None]
    if beat_devs:
        quantize_line = f"{quantize_word(max(beat_devs))}（グリッドからのズレ ±{max(beat_devs):.0f}ms）"
    else:
        quantize_line = "測れなかった（中域の拍位置に十分なオンセットが無い）"

    downbeat_ok = gates["downbeat"]["ok"]
    if not gates["harmony"]["ok"]:
        loop_line = "測れなかった（上モノがほぼ無い）"
    elif not downbeat_ok:
        loop_line = "測れなかった（小節頭が取れずスロット割りが無意味）"
    elif topline_src is not None and "loop" in topline_src:
        sim = topline_src["loop"]["similarity_by_lag"]
        adopted = topline_src["loop"]["most_likely"]
        n = adopted.removesuffix("bar")
        loop_line = f"**{n}小節**" + ("" if loop_confident(sim, adopted) else "（確信度低め — 他のラグと僅差）")
    else:
        loop_line = "測れなかった（採用ステムのループ判定が無い）"

    inst_block = instrumentation_section(stems6, arrangement, timbres, downbeat_ok)

    gate_notes = []
    if not sw["ok"]:
        gate_notes.append("<!-- 注意: swing ゲート落ち。「ハネ無し」と書かず「測れなかった」と書く -->")
    if not gates["downbeat"]["ok"]:
        gate_notes.append("<!-- 注意: downbeat ゲート落ち。小節頭の位置が疑わしい。16分プロファイルの解釈を書かない -->")

    fills = {
        "title": title,
        "header_line": " / ".join(header_parts),
        "duration_mss": mmss(basics["duration_sec"]),
        "n_bars": str(groove["n_bars"]),
        "bpm": f"{gates['bpm']['value']:.3f}".rstrip("0").rstrip("."),
        "bpm_note": "（曲中固定）",
        "key_value": gates["key"]["value"],
        "key_confidence": gates["key"]["confidence"],
        "scale_notes": scale_notes(groove["bass"]["pitch_class_weight"], gates["key"]["value"]),
        "swing_line": swing_line,
        "quantize_line": quantize_line,
        "loop_line": loop_line,
        "master_line": f"平均RMS {db(overview['mean_rms_dbfs'])}dBFS / ピーク {db(overview['peak_dbfs'])}dBFS",
        "instrumentation_section": inst_block,
        "harmony_block": harmony_block(gates, topline_src),
        "groove_gate_note": ("\n".join(gate_notes) + "\n") if gate_notes else "",
        "structure_table": structure_table(arrangement, downbeat_ok),
        "analysis_conditions": analysis_conditions(ref, info, basics, groove),
        "listen_cmd": listen_cmd(ref),
        "measured_16th_block": measured_16th_block(groove, downbeat_ok),
        "measured_stems_block": measured_stems_block(stems6, timbres),
        "loop_judgement_block": loop_judgement_block(gates, topline_src),
        "trust_table": trust_table(gates, topline_src),
        "basicpitch_block": basicpitch_block(toplines),
        "separation_block": separation_block(stems4, stems6),
    }

    digest = {
        "meta": {"title": title, "bpm": gates["bpm"]["value"], "n_bars": groove["n_bars"],
                 "duration_sec": basics["duration_sec"], "key": gates["key"],
                 "master": {"mean_rms_dbfs": overview["mean_rms_dbfs"], "peak_dbfs": overview["peak_dbfs"]}},
        "gates": {k: {kk: vv for kk, vv in v.items() if kk in ("ok", "octave_ok", "value", "confidence", "ratio", "source_stem", "note")}
                  for k, v in gates.items() if isinstance(v, dict)},
        # 小節内の位置に依存する配列（16分プロファイル・位置別ズレ・オンセット率）は
        # downbeat が取れているときだけ渡す（card.py の省略と同じ判断。位置が無意味な
        # データを渡すと、警告コメントを越えて解釈されうる）
        "groove": {
            "swing": gsw,
            "drums": {band: {k: groove["drums"][band].get(k) for k in
                             (("means", "profile_by_16th", "dev_ms_by_16th", "active_16ths")
                              if downbeat_ok else ("means",))}
                      for band in ("low", "mid", "high")},
            "bass": {k: v for k, v in groove["bass"].items()
                     if k != "count" and (downbeat_ok or k != "onset_rate_by_16th")},
        },
        "stems": {name: {**{k: st[k] for k in ("rms_db", "band_balance", "spectral_centroid_hz_median",
                                               "stereo_width", "harmonic_percussive_ratio")},
                         **({"timbre": timbres[name]} if name in timbres else {})}
                  for name, st in stems6["stems"].items()},
        "harmony": ({"source_stem": gates["harmony"]["source_stem"],
                     "loop_bars": topline_src["loop_progression"]["loop_bars"],
                     "repetitions_folded": topline_src["loop_progression"].get("repetitions_folded"),
                     "progression": topline_src["loop_progression"]["progression"]}
                    if gates["harmony"]["ok"] and downbeat_ok and topline_src is not None else None),
        "arrangement": arrangement["sections"] if downbeat_ok else None,
    }
    return fills, digest


def listen_cmd(ref: Path) -> str:
    """listen/ を Finder で開くコマンド。ホーム配下は ~ 省略形（スペースを含むパスだけクォート付き絶対パス）"""
    p = str((ref / "listen").resolve())
    if " " in p:
        return f"open '{p}'"
    home = str(Path.home())
    if p.startswith(home + "/"):
        p = "~" + p[len(home):]
    return f"open {p}"


def analysis_conditions(ref: Path, info: dict | None, basics: dict, groove: dict) -> str:
    lines = []
    if info:
        src = f"分析元は YouTube「{info.get('title', ref.name)}」"
        if info.get("channel"):
            src += f"（チャンネル: {info['channel']}）"
        lines.append(src + "。")
    else:
        lines.append("分析元は LaLa のリージョン書き出し（`track.wav`）。")
    lines.append(f"分析対象は `track.wav`（{mmss(basics['duration_sec'])}・{groove['n_bars']}小節）。"
                 "小節番号はすべてこのファイルの頭からの通し番号。")
    return "\n".join(lines)


def extract_markers(draft: str) -> tuple:
    """ドラフトから【判】マーカー一覧を抽出する（真実の源はテンプレ＝生成済みドラフト）。

    ハードコードの一覧だと、テンプレに新しい【判】領域を足したとき完成検査の対象から漏れる。
    対の欠落・名前の重複はテンプレのバグなのでエラーにする。
    """
    problems = []
    result = {}
    for kind, key in (("判", "body"), ("判i", "inline")):
        starts = re.findall(rf"<!-- {kind}:([\w-]+):start -->", draft)
        ends = re.findall(rf"<!-- {kind}:([\w-]+):end -->", draft)
        for name in sorted(set(starts) | set(ends)):
            if starts.count(name) != 1 or ends.count(name) != 1:
                problems.append(f"{kind}:{name} のマーカー対が壊れている"
                                f"（start {starts.count(name)}個 / end {ends.count(name)}個）")
        result[key] = starts
    if not result["body"]:
        problems.append("本文型【判】マーカーが1つも無い（テンプレが壊れている）")
    return result, problems


def fill(ref: Path, out: Path, digest_path: Path) -> int:
    gates = load_json(ref / "analysis" / "gates.json")
    if not gates["bpm"]["octave_ok"] or not gates["tempo_stable"]["ok"]:
        reason = "BPMのオクターブ" if not gates["bpm"]["octave_ok"] else "テンポの安定"
        print(f"レポートを書かない（{reason}のゲートが落ちている＝グリッドごと信用できず全分析が無意味）",
              file=sys.stderr)
        return 3

    fills, digest = build_fills(ref)
    template = TEMPLATE.read_text()

    unknown = [m for m in re.findall(r"\{\{(\w+)\}\}", template) if m not in fills]
    if unknown:
        print(f"ERROR: テンプレのプレースホルダに対応する充填値が無い: {unknown}", file=sys.stderr)
        return 1
    draft = re.sub(r"\{\{(\w+)\}\}", lambda m: fills[m.group(1)], template)

    markers, problems = extract_markers(draft)
    if problems:
        print("ERROR: 【判】マーカーの抽出に失敗（テンプレ or 充填ブロックのバグ）:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1
    digest["markers"] = markers

    out.write_text(draft)
    digest_path.parent.mkdir(parents=True, exist_ok=True)
    digest_path.write_text(json.dumps(digest, indent=1, ensure_ascii=False) + "\n")
    print(f"ドラフトを書いた: {out}（機械充填 {len(fills)}項目・【判】マーカー "
          f"{len(digest['markers']['body'])}本文 + {len(digest['markers']['inline'])}インライン）")
    return 0


# ---------------------------------------------------------------- check（完成検査）

COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)
# 文章段落に数えない行: 見出し・表・水平線・コード塀・単独の強調ラベル。
# 箇条書きは数える — 【判】マーカーの内側に来る箇条書きは AI が書いた本文でしかあり得ず、
# 見本レポート自体がグルーヴ・構成節を箇条書きで書いている（除外すると正しく書いた high effort の
# 実出力が落ちた。2026-08-06 実測）。「見出しだけ直して本文空」は空行・ラベルだけになるので今も検出できる
NON_PROSE_RE = re.compile(r"^(#|\||> |---|```|\*\*[^*]+\*\*$)")


def region(text: str, kind: str, name: str) -> str | None:
    m = re.search(rf"<!-- {kind}:{re.escape(name)}:start -->(.*?)<!-- {kind}:{re.escape(name)}:end -->",
                  text, re.DOTALL)
    return m.group(1) if m else None


def has_prose(body: str) -> bool:
    no_comments = COMMENT_RE.sub("", body)
    for line in no_comments.splitlines():
        line = line.strip()
        if line and not NON_PROSE_RE.match(line):
            return True
    return False


def check(target: Path, digest_path: Path) -> int:
    text = target.read_text()
    markers = load_json(digest_path)["markers"]
    problems = []

    # ① 未充填・未執筆の可視プレースホルダが残っていないか（HTMLコメント内は除く）
    visible = COMMENT_RE.sub("", text)
    if re.search(r"\{\{\w+\}\}", visible):
        problems.append("機械充填プレースホルダ {{...}} が残っている")
    for m in re.finditer(r"〈[^〉\n]*〉", visible):
        problems.append(f"可視プレースホルダが残っている: {m.group(0)[:40]}")

    # ② 必須見出し
    for h in REQUIRED_HEADINGS:
        if not any(line.startswith(h) for line in text.splitlines()):
            problems.append(f"必須見出しが無い: {h}")

    # ③ 本文型【判】: マーカーが残っていて、内側に文章段落がある
    for name in markers["body"]:
        body = region(text, "判", name)
        if body is None:
            problems.append(f"本文型【判】マーカーが消えている: {name}")
        elif not has_prose(body):
            problems.append(f"本文型【判】に文章が書かれていない: {name}")

    # ④ インライン型【判】: マーカー間が空でない（機の数値が同セルに残っていても判の削除を検出）
    for name in markers["inline"]:
        body = region(text, "判i", name)
        if body is None:
            problems.append(f"インライン型【判】マーカーが消えている: {name}")
        elif not COMMENT_RE.sub("", body).strip():
            problems.append(f"インライン型【判】が空: {name}")

    if problems:
        print("完成検査に不合格:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1
    return 0


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_fill = sub.add_parser("fill", help="ドラフトとダイジェストを書く")
    p_fill.add_argument("ref")
    p_fill.add_argument("--out", default=None, help="既定: <ref>/report.md.next")
    p_fill.add_argument("--digest", default=None, help="既定: <ref>/logs/report-digest.json")
    p_check = sub.add_parser("check", help="完成検査")
    p_check.add_argument("target")
    p_check.add_argument("--digest", required=True)
    args = ap.parse_args()

    if args.cmd == "fill":
        ref = Path(args.ref).expanduser()
        out = Path(args.out) if args.out else ref / "report.md.next"
        digest = Path(args.digest) if args.digest else ref / "logs" / "report-digest.json"
        sys.exit(fill(ref, out, digest))
    else:
        sys.exit(check(Path(args.target).expanduser(), Path(args.digest).expanduser()))


if __name__ == "__main__":
    main()
