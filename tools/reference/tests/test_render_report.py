#!/usr/bin/env python
"""render_report.py の回帰テスト（プレーン assert・pytest 不要）。

自己完結の保証（外部 subresource ゼロ・CSP あり）と atomic 書き込みを固定する。
表示の見た目はテストできないので、構造（table 化・data URI 化・リンク温存）だけ見る。

使い方: .venv/bin/python tests/test_render_report.py
"""
import base64
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import render_report  # noqa: E402

checks = 0

# 1x1 の透過 PNG（最小の実画像）
PNG_1PX = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
)

REPORT_MD = """# テスト曲 — 完全に打ち込み

BPM は **142.0**。[外部リンク](https://example.com/) と [listen/hat.wav](listen/hat.wav)。

| 項目 | 値 |
|---|---|
| BPM | 142.0 |
| キー | D minor |

図: ![16分プロファイル](analysis/groove.png)
無い図: ![欠けた図](analysis/missing.png)

raw HTML の変種も自己完結にする:
<IMG SRC="https://example.com/upper.png">
<img src='analysis/groove.png'>
<img src=https://example.com/unquoted.png>
<script src="https://example.com/x.js">alert(1)</script>
<video src="https://example.com/x.mp4"></video>

> 作るときはハットを2段に割る。
"""


def ok(cond: bool, msg: str) -> None:
    global checks
    assert cond, msg
    checks += 1


def make_ref(tmp: str) -> Path:
    ref = Path(tmp) / "test-ref"
    (ref / "analysis").mkdir(parents=True)
    (ref / "report.md").write_text(REPORT_MD, encoding="utf-8")
    (ref / "analysis" / "groove.png").write_bytes(PNG_1PX)
    return ref


with tempfile.TemporaryDirectory() as tmp:
    ref = make_ref(tmp)
    out = render_report.render(ref)
    html = out.read_text(encoding="utf-8")

    ok(out == ref / "report.html", "出力パスは <ref>/report.html")
    ok("<table>" in html and "<th>項目</th>" in html, "表が <table> になる")
    ok("<blockquote>" in html, "引用が blockquote になる")
    ok('src="data:image/png;base64,' in html, "実在する画像は data URI でインライン化")
    ok("画像が見つからない: analysis/missing.png" in html, "欠けた画像は目に見えるプレースホルダ")
    ok('href="https://example.com/"' in html, "外部リンク（a[href]）は残る")
    ok('href="listen/hat.wav"' in html, "wav への相対リンクは残る")
    ok("Content-Security-Policy" in html and "img-src data:" in html, "CSP が入っている")
    ok("<title>テスト曲 — 完全に打ち込み</title>" in html, "タイトルは先頭 h1 から取る")

    # 自己完結: data: 以外の src が1つも無い（大文字タグ・シングルクォート・引用符なしも検査）
    external = [
        m.group(0)
        for m in re.finditer(r"""\bsrc\s*=\s*(?:(["'])(.+?)\1|([^"'\s>]+))""", html, re.IGNORECASE)
        if not (m.group(2) or m.group(3)).startswith("data:")
    ]
    ok(external == [], f"data: 以外の src が無い: {external}")
    ok("外部画像は表示しない: https://example.com/upper.png" in html,
       "大文字タグの外部画像もプレースホルダ化される")
    ok("外部画像は表示しない: https://example.com/unquoted.png" in html,
       "引用符なし src の外部画像もプレースホルダ化される")
    ok("<script" not in html.lower() and "<link" not in html.lower()
       and "<video" not in html.lower(),
       "script / 外部 css / video を含まない")
    ok("未対応タグを除去: script" in html, "raw script は可視マーカーに置き換わる")

    # 一時ファイルが残らない
    leftovers = [p.name for p in ref.iterdir() if p.name.startswith(".report.html.")]
    ok(leftovers == [], f"一時ファイルが残らない: {leftovers}")

    # 再実行（上書き）も成功する
    render_report.render(ref)
    ok((ref / "report.html").is_file(), "再実行で上書きできる")

# report.md が無いフォルダは CLI が非0 + stderr
with tempfile.TemporaryDirectory() as tmp:
    empty = Path(tmp) / "empty-ref"
    empty.mkdir()
    proc = subprocess.run(
        [sys.executable, str(Path(render_report.__file__)), str(empty)],
        capture_output=True,
        text=True,
    )
    ok(proc.returncode != 0, "report.md 不在は非0で終了")
    ok("report.md" in proc.stderr, "失敗理由が stderr に出る")
    ok(not (empty / "report.html").exists(), "失敗時に report.html を作らない")

print(f"ok ({checks} checks)")
