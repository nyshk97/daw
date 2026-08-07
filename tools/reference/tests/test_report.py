#!/usr/bin/env python
"""report.py（【機】充填・判定語・ゲート焼き込み・ダイジェスト・完成検査）の回帰テスト。

test_card.py と同じ流儀: プレーン assert・pytest 不要・最小 fixture。
ゲート分岐は fixture を増やさず gates.json をテスト内で書き換えて網羅する。

使い方: .venv/bin/python tests/test_report.py
"""
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import card  # noqa: E402
import report  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "fixtures"
checks = 0


def ok(cond: bool, msg: str) -> None:
    global checks
    assert cond, msg
    checks += 1


def run_fill(mutate=None, with_info: dict | None = None):
    """normal fixture を一時フォルダにコピーして report.py fill を走らせる"""
    with tempfile.TemporaryDirectory() as tmp:
        ref = Path(tmp) / "normal"
        shutil.copytree(FIXTURES / "normal", ref)
        if mutate:
            gp = ref / "analysis" / "gates.json"
            g = json.loads(gp.read_text())
            mutate(g)
            gp.write_text(json.dumps(g))
        if with_info is not None:
            (ref / "source.info.json").write_text(json.dumps(with_info))
        out = ref / "report.md.next"
        digest = ref / "logs" / "report-digest.json"
        rc = report.fill(ref, out, digest)
        return (rc,
                out.read_text() if out.exists() else None,
                json.loads(digest.read_text()) if digest.exists() else None)


def complete(draft: str) -> str:
    """AI が全部書いた状態を疑似的に作る（可視プレースホルダを文章に置換。コメントは触らない）"""
    parts = []

    def stash(m):
        parts.append(m.group(0))
        return f"\0{len(parts) - 1}\0"

    tmp = re.sub(r"<!--.*?-->", stash, draft, flags=re.DOTALL)
    tmp = re.sub(r"〈[^〉\n]*〉", "検証用の文章段落。数値が聴こえ方に効く理由を書いた体のダミー。", tmp)
    return re.sub(r"\0(\d+)\0", lambda m: parts[int(m.group(1))], tmp)


def run_check(text: str, digest: dict) -> tuple:
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp) / "report.md"
        d = Path(tmp) / "digest.json"
        t.write_text(text)
        d.write_text(json.dumps(digest))
        return report.check(t, d)


# ---------------------------------------------------------------- 判定語の境界値

ok(report.swing_word(0.519) == "無し", "swing 0.52未満は無し")
ok(report.swing_word(0.52) == "軽くハネる", "swing 0.52は軽くハネる")
ok(report.swing_word(0.58) == "軽くハネる", "swing 0.58は軽くハネる")
ok(report.swing_word(0.581) == "はっきりハネる", "swing 0.58超ははっきり")

ok(report.quantize_word(10).startswith("打ち込みか100%クオンタイズ"), "quantize 10ms以下は打ち込み判定")
ok(report.quantize_word(10.1).startswith("人力の揺れ"), "quantize 10ms超は人力判定")
ok(report.quantize_word(25.1).startswith("生演奏"), "quantize 25ms超は生演奏疑い")

ok(report.separation_word(-20).startswith("使える"), "分離 -20dB以下は使える")
ok(report.separation_word(-19.9).startswith("条件つき"), "分離 -20〜-15は条件つき")
ok(report.separation_word(-14.9).startswith("疑う"), "分離 -15dB超は疑う")

ok(report.basicpitch_word(0.8, 0.7) == "統計用途OK", "basic-pitch 0.8/0.7 は統計OK")
ok(report.basicpitch_word(0.79, 0.9) == "統計も疑う", "chord_tone 0.8未満は疑う")
ok(report.basicpitch_word(0.9, 0.69) == "統計も疑う", "within30 0.7未満は疑う")

ok(report.loop_confident({"1bar": 0.70, "2bar": 0.90, "4bar": 0.75}, "2bar"), "0.05以上の差は断定")
ok(not report.loop_confident({"1bar": 0.88, "2bar": 0.90}, "2bar"), "0.05未満の差は断定しない")

# ---------------------------------------------------------------- 正常系の fill

rc, draft, digest = run_fill()
ok(rc == 0, "正常系は exit 0")
ok("{{" not in draft, "未置換の {{...}} が残らない")
ok("**120**" in draft, "BPM が転記される（末尾ゼロは落とす）")
ok("**A minor**（確度: 高）" in draft, "キーと確度が gates から転記される")
ok("**A B C D E F G**（ベース実測の上位7音。キーのスケールと一致）" in draft,
   "使う音がルートから並び、スケールと一致するときは一致と注記される")

# 使う音がキーのスケールと食い違う曲（キー確度「中」の実例: D major で G が無く A# が入る）
# → 実測の転記であることと差分を注記し、スケール音の一覧に見えないようにする
w = [0.0] * 12
for pc in (1, 2, 4, 6, 9, 10, 11):  # C# D E F# A A# B
    w[pc] = 1.0
ok(report.scale_notes(w, "D major")
   == "**D E F# A A# B C#**（ベース実測の上位7音。D major のスケールの G は踏まず、外の A# を踏む）",
   "スケールと不一致なら欠けた音と外れた音を対で注記する")

# 3音リフの曲（実例: Summer Situation。G#/G/C で96%、残りは出席率0.1〜3%のノイズレベル）
# → 重みが最大の1/10未満の音は「使う音」に昇格させない
w = [0.0] * 12
for pc, v in ((8, 0.432), (7, 0.307), (0, 0.221), (10, 0.029), (5, 0.007), (2, 0.004), (9, 0.001)):
    w[pc] = v
ok(report.scale_notes(w, "C minor")
   == "**C G G#**（ベース実測。この3音で全体の96%。いずれも C minor のスケール内）",
   "重み下限（最大の1/10）未満の音を落とし、音数と占有率で書く")

# 下限を超えたスケール外の音は少数でも注記する
w = [0.0] * 12
for pc, v in ((0, 0.5), (6, 0.3), (7, 0.15)):
    w[pc] = v
ok(report.scale_notes(w, "C major")
   == "**C F# G**（ベース実測。この3音で全体の95%。F# はスケール外）",
   "7音未満でもスケール外の音は注記される")
ok("**軽くハネる**（スウィング比 0.550" in draft, "スウィングの判定語と数値")
ok("グリッドからのズレ ±4ms — 打ち込みか100%クオンタイズ（人の手では出ない精度）" in draft,
   "クオンタイズ判定（mid の拍位置 |−4| が最大）が読み方つきで出る")
ok("**2小節**で1周する進行を、曲中ずっと繰り返す" in draft and "確信度低め — 他のラグ" not in draft,
   "ループ長は断定（マージン0.15）で、繰り返しの意味まで書かれる")
ok("平均RMS -12.0dBFS / ピーク +0.4dBFS（RMS=ならした平均音量・0dBFS=デジタルの上限。"
   "ピークが0超え＝上限いっぱいまで音圧を上げてある）" in draft,
   "マスター行（正のピークは+付き・読み方の注釈・0超えの一言つき）")
ok("2ステムが独立に同じ 2bar を出した" in draft, "コード骨格の信頼度根拠")
ok("| 2 | **F**（確信度低め） | **G** |" in draft,
   "進行表が畳んだ進行から組まれ、conf が最大-0.2より低いスロットに確信度低めが付く")
ok("分析元は LaLa のリージョン書き出し" in draft, "source.info.json 無しでも生成できる（曲名はフォルダ名）")
ok("# normal — 分析レポート" in draft, "info 無しの曲名はフォルダ名")
ok("```sh\nopen " in draft and "/listen\n```" in draft.replace("'", ""),
   "耳で検算する節に listen/ を開くコマンドが機械充填される")
ok("動画公開日" not in draft, "info 無しでは日付・チャンネル・URL 行を省略する")

# ダイジェスト
ok(set(digest["markers"]["body"]) == {"composition-summary", "harmony", "groove", "timbre", "structure", "essence"},
   "本文型マーカーの一覧")
ok("key-lean" in digest["markers"]["inline"] and "home-chord" in digest["markers"]["inline"],
   "インライン型マーカーの一覧")
flat = json.dumps(digest)
ok('"chords"' not in flat, "ダイジェストに区間ごとの生コード推定を入れない")
ok("rms_db_by_bar" not in flat, "ダイジェストに小節ごとの生RMS配列を入れない")
ok(digest["harmony"]["loop_bars"] == 2, "ダイジェストに畳んだ進行が入る")

# カードとレポートでキーが一致する（真実の源が gates.py で共有されている）
with tempfile.TemporaryDirectory() as tmp:
    ref = Path(tmp) / "normal"
    shutil.copytree(FIXTURES / "normal", ref)
    card.generate(ref)
    c = json.loads((ref / "card.json").read_text())
    ok(f"**{c['global']['key']['root']} {c['global']['key']['mode']}**" in draft,
       "カードとレポートのキーが一致する")

# ---------------------------------------------------------------- source.info.json あり

info = {"title": "TEST SONG", "channel": "TEST CH", "upload_date": "20200102",
        "webpage_url": "https://example.com/watch"}
rc, draft_i, _ = run_fill(with_info=info)
ok("# TEST SONG — 分析レポート" in draft_i, "info の曲名を使う")
ok("動画公開日 2020-01-02" in draft_i, "upload_date は「動画公開日」として転記（発売日とは書かない）")
ok("チャンネル TEST CH" in draft_i, "チャンネルを転記")
ok("[原曲](https://example.com/watch)" in draft_i, "URL を転記")

# ---------------------------------------------------------------- コード合議は6分割のみ

# 4分割 other が usable でループ一致しても、合議の頭数に数えない（gates.HARMONY_STEMS の外。
# 1本きりで独立の検算にならず、ループ長 8小節 の誤答実績がある）
rc, draft_4s, _ = run_fill(mutate=lambda g: g["harmony"]["stems"].append(
    {"stem": "other", "usable": True, "rms_below_mix_db": -10.0, "loop": "2bar"}))
ok("2ステムが独立に同じ 2bar を出した" in draft_4s, "4分割 other を足しても合議は6分割の2ステムのまま")
ok("3ステム" not in draft_4s, "4分割 other が頭数に入らない")

# ---------------------------------------------------------------- downbeat ゲート落ちの省略

rc, draft_d, digest_d = run_fill(mutate=lambda g: g["downbeat"].update(ok=False))
ok("16分ごとの出現強度は載せない" in draft_d, "downbeat 落ちでは16分表を載せない")
ok("コード進行の表は載せない" in draft_d, "downbeat 落ちでは進行表を載せない")
ok("セクション表は載せない" in draft_d, "downbeat 落ちではセクション表を載せない")
ok("小節位置は測れない" in draft_d, "downbeat 落ちでは楽器編成の出入りを省略")
ok("測れなかった（小節頭が取れず" in draft_d, "downbeat 落ちではコードループ行も測れなかった")
ok("profile_by_16th" not in json.dumps(digest_d), "downbeat 落ちのダイジェストに16分プロファイルを入れない")
ok("onset_rate_by_16th" not in json.dumps(digest_d), "downbeat 落ちのダイジェストにオンセット率を入れない")
ok(digest_d["harmony"] is None and digest_d["arrangement"] is None,
   "downbeat 落ちのダイジェストは進行・セクションを省略")
ok("| クラップ/ハットの16分位置 | **—** | 小節頭が取れず位置を推定していない |" in draft_d,
   "downbeat 落ちでは16分位置の信頼度行が「測っていない」になる")
ok("| キックの16分位置 | **—** | 小節頭が取れず位置を推定していない |" in draft_d,
   "downbeat 落ちではキックの信頼度行も「測っていない」になる")

# ---------------------------------------------------------------- マーカー抽出（テンプレが真実の源）

m, probs = report.extract_markers(
    "<!-- 判:a:start -->x<!-- 判:a:end --> <!-- 判i:b:start -->y<!-- 判i:b:end -->")
ok(not probs and m == {"body": ["a"], "inline": ["b"]}, "マーカー抽出の正常系")
_, probs = report.extract_markers("<!-- 判:a:start -->x")
ok(any("a のマーカー対が壊れている" in p for p in probs), "end 欠落を検出")
_, probs = report.extract_markers("<!-- 判:a:start --><!-- 判:a:end --><!-- 判:a:start --><!-- 判:a:end -->")
ok(any("壊れている" in p for p in probs), "名前の重複を検出")
_, probs = report.extract_markers("なにもない")
ok(any("1つも無い" in p for p in probs), "本文型マーカーゼロを検出")

# ---------------------------------------------------------------- ゲート焼き込み

rc, draft_g, _ = run_fill(mutate=lambda g: g["swing"].update(ok=False))
ok("測れなかった（ハットが無く" in draft_g, "swing ゲート落ちはハネ行が「測れなかった」")
ok("swing ゲート落ち" in draft_g, "swing ゲート落ちの指示コメントがグルーヴ節に入る")

rc, draft_g, _ = run_fill(mutate=lambda g: g["downbeat"].update(ok=False))
ok("downbeat ゲート落ち" in draft_g, "downbeat ゲート落ちの指示コメントが入る")
ok("| 小節頭の位置 | **低** |" in draft_g, "信頼度表の小節頭が低になる")

rc, draft_g, _ = run_fill(mutate=lambda g: g["harmony"].update(ok=False))
ok("コード進行は推定しない" in draft_g, "harmony ゲート落ちは進行表の代わりに理由を書く")
ok("測れなかった（上モノがほぼ無い）" in draft_g, "コードループ行も測れなかったになる")

# BPM 系ゲート落ちはドラフトを出さず exit 3
rc, draft_g, _ = run_fill(mutate=lambda g: g["bpm"].update(octave_ok=False))
ok(rc == 3 and draft_g is None, "bpm.octave_ok 落ちは exit 3・ドラフト無し")
rc, draft_g, _ = run_fill(mutate=lambda g: g["tempo_stable"].update(ok=False))
ok(rc == 3 and draft_g is None, "tempo_stable 落ちは exit 3・ドラフト無し")

# ---------------------------------------------------------------- 完成検査

rc, draft, digest = run_fill()
ok(run_check(draft, digest) == 1, "未執筆のドラフトは不合格")
done = complete(draft)
ok(run_check(done, digest) == 0, "全部書けば合格")

# 見出しだけ直して本文を書かない（可視プレースホルダを消しただけ）→ 不合格
lazy_parts = []
lazy_tmp = re.sub(r"<!--.*?-->", lambda m: (lazy_parts.append(m.group(0)), f"\0{len(lazy_parts)-1}\0")[1],
                  draft, flags=re.DOTALL)
lazy_tmp = re.sub(r"〈[^〉\n]*をここに〉", "", lazy_tmp)          # 本文は書かない
lazy_tmp = re.sub(r"〈[^〉\n]*〉", "ダミー", lazy_tmp)             # 見出し・セルは埋める
lazy = re.sub(r"\0(\d+)\0", lambda m: lazy_parts[int(m.group(1))], lazy_tmp)
ok(run_check(lazy, digest) == 1, "見出しだけ直して本文が空だと不合格（機の表があっても通らない）")

# 本文型マーカーを消す → 不合格
no_marker = done.replace("<!-- 判:groove:start -->", "").replace("<!-- 判:groove:end -->", "")
ok(run_check(no_marker, digest) == 1, "本文型マーカーを消すと不合格")

# インライン型: マーカー間を空にする（機の数値はセルに残る）→ 不合格
empty_inline = re.sub(r"(<!-- 判i:key-lean:start -->).*?(<!-- 判i:key-lean:end -->)", r"\1\2", done)
ok(run_check(empty_inline, digest) == 1, "インラインのマーカー間を空にすると不合格")

# 表・単独強調ラベルだけでは「文章段落」にならない
table_only = re.sub(r"(<!-- 判:essence:start -->).*?(<!-- 判:essence:end -->)",
                    "\\1\n**ラベルだけ**\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\\2",
                    done, flags=re.DOTALL)
ok(run_check(table_only, digest) == 1, "表・強調ラベルだけの【判】は不合格")

# 箇条書きは【判】の本文として数える（見本レポート自体が箇条書き主体。
# 除外すると正しく書いた出力が落ちる — 2026-08-06 の high effort 実出力で実測）
bullets = re.sub(r"(<!-- 判:essence:start -->).*?(<!-- 判:essence:end -->)",
                 "\\1\n- **中域が空いている。** 和音の情報が耳を占めないので声が前に出る\n\\2",
                 done, flags=re.DOTALL)
ok(run_check(bullets, digest) == 0, "箇条書きで書かれた【判】は合格")

print(f"OK: {checks} 件のチェックが通った")
