#!/usr/bin/env python
"""`claude -p --output-format stream-json` の出力を、経過時間つきの1行ログに畳む。

レポート生成は5〜10分かかる。既定の出力は全部書き終わるまで無言なので、
止まったように見えて Ctrl-C したくなる。何をしているかだけ流す。

使い方: claude -p ... --output-format stream-json --verbose | stream_progress.py
"""
import json
import os
import sys
import time

t0 = time.time()


def elapsed() -> str:
    s = int(time.time() - t0)
    return f"[{s // 60}:{s % 60:02d}]"


def short(x: str, n: int = 70) -> str:
    x = str(x).replace("\n", " ")
    return os.path.basename(x)[:n] if x.startswith("/") else x[:n]


def main() -> None:
    for line in sys.stdin:
        try:
            d = json.loads(line)
        except Exception:
            continue
        kind = d.get("type")
        if kind == "assistant":
            for c in d["message"].get("content", []):
                if c["type"] == "tool_use":
                    inp = c.get("input", {})
                    target = inp.get("file_path") or inp.get("pattern") or inp.get("command") or ""
                    print(f"{elapsed()} {c['name']:6s} {short(target)}", flush=True)
                elif c["type"] == "text" and c["text"].strip():
                    print(f"{elapsed()} …      {short(c['text'].strip().splitlines()[0], 90)}", flush=True)
        elif kind == "result":
            print(flush=True)
            print(d.get("result", ""), flush=True)


if __name__ == "__main__":
    main()
