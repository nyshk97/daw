#!/usr/bin/env python3
"""Phase 0 耳判定の回答テンプレートを生成する（blindprep.py の後に実行）。

評価軸（plan で実装前に固定。各 1〜5、5 が良い）:
  ①滲み: 声が二重にぼやける・水っぽい（位相ボコーダー特有）が無い=5
  ②子音・息: s/k/息の荒れ・にじみ・消失が無い=5
  ③声質: 細く/太くなる・別人感（フォルマント崩れ）が無い=5
  ④ケロらしさ（ケース C のみ）: ハードチューンとして「使える」音か=5
  ⑤透明性（ケース Z のみ）: _ref_original と区別がつかない=5
  ⑥境界（ケース D のみ）: 動かした音の前後にクリック・二重化・もたつきが無い=5
出力: docs/labs/reference-beat-human-answers/<日付>-pitchlab-blind-answers.md
"""
from __future__ import annotations

import json

from common import ANSWERS

DATE = "2026-08-21"
AXES = {
    "Z": ["⑤透明性", "②子音・息", "③声質"],
    "A": ["①滲み", "②子音・息", "③声質"],
    "B": ["①滲み", "②子音・息", "③声質"],
    "C": ["①滲み", "②子音・息", "③声質", "④ケロらしさ"],
    "D": ["①滲み", "②子音・息", "⑥境界"],
}
CASE_DESC = {
    "Z": "無加工（強さ0・移調0）。_ref_original と同じに聴こえるか",
    "A": "スケールスナップ・強さ100%・速さ120ms（半音以内の補正が大半）",
    "B": "A ＋ 全体 +3 半音（移調との合算）",
    "C": "ケロケロ（速さ0ms）",
    "D": "A ＋ 1音を +40ms 横移動（隣接の隙間が吸収）。動いた音の前後に注目",
}


def main() -> int:
    m = json.loads((ANSWERS / f"{DATE}-pitchlab-blind-map.json").read_text())
    lines = [
        f"# ピッチ補正 lab ブラインド試聴 回答（{DATE}）",
        "",
        "各セル 1〜5（5 が良い）。ファイルは `tools/pitchlab/listen/<素材>/<ケース>/`。",
        "solo_* と mix_* は同じ処理の声（ソロ／オケ込み）。`_ref_original(.mix).wav` は無加工の基準。",
        "8010AM と MDR-7506 の両方で聴き、差が出た行はメモに書く。対応表（blind-map.json）は回答を書き終えるまで開かない。",
        "",
        "軸: ①滲み=二重にぼやける/水っぽさが無い ②子音・息=s/k/息の荒れ・消失が無い ③声質=細く/太くなる・別人感が無い",
        "    ④ケロらしさ=ハードチューンとして使える ⑤透明性=原音と区別がつかない ⑥境界=動かした音の前後にクリック/二重化/もたつきが無い",
        "",
    ]
    cur = None
    for e in m["entries"]:
        if e["material"] != cur:
            cur = e["material"]; lines += [f"## {cur}", ""]
        case = e["case"]; axes = AXES[case]
        lines += [f"### ケース {case} — {CASE_DESC[case]}", "",
                  "| 候補 | " + " | ".join(axes) + " | 総合(1-5) |", "|---|" + "---|" * (len(axes) + 1)]
        for label in sorted(e["assign"]):
            lines.append(f"| {label} | " + " | ".join("" for _ in axes) + " |  |")
        lines += ["", "メモ（任意）: ", ""]
    lines += [
        "## 目視ラベル（検出の実声判定）", "",
        "`tools/pitchlab/work/<素材>/overlay.png`（上段: 3検出器の重ね描き・下段: 有声度）と `notes_yin.png`（ブロブ）を見て、",
        "**明らかな誤り**の箇所だけ書く（フレーム単位の正解付けはしない）。種類: オクターブ飛び / 有声なのに欠け / 息・無声がノートになった / ノートの割れ・くっつき", "",
        "| 素材 | 時刻[s] | 種類 | メモ |", "|---|---|---|---|", "| rap-seg |  |  |  |", "| uta-seg |  |  |  |", "",
        "ノート数（notes.log）に対する誤り数の割合が 10% 未満・連続しなければ合格（plan）。", "",
    ]
    lines += ["## 総括", "", "- ケース D を通過した候補（⑥境界 ≥ 3 かつ長さ一致）: ", "- A → C → B の順位: ", "- 採用エンジン: ", ""]
    out = ANSWERS / f"{DATE}-pitchlab-blind-answers.md"
    out.write_text("\n".join(lines))
    print(f"回答テンプレート: {out.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
