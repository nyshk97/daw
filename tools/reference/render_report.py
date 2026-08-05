#!/usr/bin/env python
"""report.md → 自己完結 report.html — LaLa のレポートウィンドウ（WKWebView）が表示する1枚。

WKWebView は file:// のローカルリソース参照に制限があるため、外部参照ゼロの
自己完結 HTML にして「1ファイル渡せば表示できる」形にする（docs/plans/2026-08-05-1804）。

自己完結の定義:
- 外部 subresource（img src / css / script）を含まない。画像は data URI でインライン化
- a[href] は許可（wav・外部 URL へのリンクは残す。開くかどうかは LaLa 側の
  pageAboutToLoad が決める）
- CSP の meta タグで script 実行と外部 subresource 読み込みを禁止する
  （pageAboutToLoad はトップレベル遷移しか制御しないため、埋め込みは CSP で塞ぐ）

出力: <ref>/report.html（同ディレクトリの一時ファイルに書いてから atomic rename。
失敗時は古い HTML に触れず非0で終了する）

使い方: render_report.py <リファレンスフォルダ>
"""
import argparse
import base64
import mimetypes
import os
import re
import sys
import tempfile
from pathlib import Path

import markdown

# LaLa の Theme.h に合わせたダーク配色（windowBg=#2e2e33 系の無彩色地 + 青アクセント）
CSS = """
  body { background: #26262b; color: #d6d6da; margin: 0; padding: 2.2em 2.5em 4em;
         font-family: -apple-system, "Hiragino Sans", sans-serif;
         font-size: 15px; line-height: 1.75; }
  main { max-width: 46em; margin: 0 auto; }
  h1 { color: #a8c4ee; font-size: 1.5em; line-height: 1.4; }
  h2 { color: #a8c4ee; font-size: 1.2em; margin-top: 2.2em;
       border-bottom: 1px solid #4a4a52; padding-bottom: 0.3em; }
  h3 { color: #9ab4de; font-size: 1.05em; margin-top: 1.8em; }
  a { color: #8fbf8f; }
  strong { color: #f0f0f4; }
  table { border-collapse: collapse; margin: 1em 0; }
  th, td { border: 1px solid #4a4a52; padding: 5px 12px; text-align: left; }
  th { background: #34343a; }
  tr:nth-child(even) td { background: #2b2b30; }
  blockquote { border-left: 3px solid #7a9ede; margin: 1em 0; padding: 0.1em 0 0.1em 1em;
               color: #b0b0b8; }
  code { background: #34343a; padding: 0.1em 0.4em; border-radius: 3px; font-size: 0.9em; }
  pre { background: #232327; padding: 1em; border-radius: 5px; overflow-x: auto; }
  pre code { background: none; padding: 0; }
  hr { border: none; border-top: 1px solid #4a4a52; margin: 2.5em 0; }
  img { max-width: 100%; }
"""

CSP = "default-src 'none'; img-src data:; style-src 'unsafe-inline'"


def inline_images(html: str, base: Path) -> str:
    """<img> の相対 src を data URI に置き換える。

    見つからない画像は目に見えるプレースホルダにする（黙って外部参照を残すと
    自己完結が壊れ、黙って消すと「図があったはず」に気づけない）。
    """

    def replace(m: re.Match) -> str:
        src = m.group("src")
        if src.startswith("data:"):
            return m.group(0)
        if re.match(r"^[a-z][a-z0-9+.-]*:", src, re.IGNORECASE):  # http(s) 等のスキーム付き
            return f'<em>[外部画像は表示しない: {src}]</em>'
        path = (base / src).resolve()
        if not path.is_file():
            return f'<em>[画像が見つからない: {src}]</em>'
        mime = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        data = base64.b64encode(path.read_bytes()).decode("ascii")
        return m.group(0).replace(src, f"data:{mime};base64,{data}", 1)

    # markdown 由来の <img> に加え、report.md に混ざりうる raw HTML（大文字タグ・
    # シングルクォート・引用符なし）も対象にする — 「自己完結」を CSP 頼みでなく構造でも保証する
    html = re.sub(
        r'<img\b[^>]*\bsrc=(?P<q>["\'])(?P<src>.+?)(?P=q)[^>]*>',
        replace,
        html,
        flags=re.IGNORECASE,
    )
    return re.sub(
        r'<img\b[^>]*\bsrc=(?P<src>[^"\'\s>]+)[^>]*>',
        replace,
        html,
        flags=re.IGNORECASE,
    )


# subresource を持ち込める・script を実行しうる raw タグ。レポートは自前パイプライン産だが、
# 事故で混ざっても構造として残さない（実行は CSP も塞ぐ。中身のテキストは可視のまま残る）
BLOCKED_TAGS = r"script|iframe|video|audio|embed|object|link|source"


def strip_blocked_tags(html: str) -> str:
    html = re.sub(
        rf"<(?P<name>{BLOCKED_TAGS})\b[^>]*>",
        lambda m: f'<em>[未対応タグを除去: {m.group("name").lower()}]</em>',
        html,
        flags=re.IGNORECASE,
    )
    return re.sub(rf"</(?:{BLOCKED_TAGS})\s*>", "", html, flags=re.IGNORECASE)


def render(ref: Path) -> Path:
    """<ref>/report.md を変換して <ref>/report.html を atomic に書く。"""
    report_md = ref / "report.md"
    if not report_md.is_file():
        raise FileNotFoundError(f"report.md が無い: {report_md}")

    text = report_md.read_text(encoding="utf-8")
    body = markdown.markdown(text, extensions=["tables", "fenced_code"])
    body = inline_images(body, ref)
    body = strip_blocked_tags(body)

    m = re.search(r"<h1[^>]*>(.*?)</h1>", body, re.DOTALL)
    title = re.sub(r"<[^>]+>", "", m.group(1)).strip() if m else ref.name

    html = f"""<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="{CSP}">
<title>{title}</title>
<style>{CSS}</style>
</head>
<body><main>
{body}
</main></body>
</html>
"""

    out = ref / "report.html"
    fd, tmp = tempfile.mkstemp(dir=ref, prefix=".report.html.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(html)
        os.replace(tmp, out)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="report.md を自己完結 report.html に変換する")
    parser.add_argument("ref", type=Path, help="リファレンスフォルダ（report.md がある場所）")
    args = parser.parse_args()
    try:
        out = render(args.ref)
    except Exception as e:  # noqa: BLE001 — 失敗理由を stderr に出して非0で返すのが仕事
        print(f"render_report: {e}", file=sys.stderr)
        return 1
    print(f"書き出し: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
