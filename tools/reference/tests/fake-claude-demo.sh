#!/bin/bash
# 疑似 claude — レポート生成フローの見た目（経過行→トースト→自動再読込）を
# トークン消費なし・約10秒で確認するためのデモ。
#
#   CLAUDE_BIN="$PWD/tools/reference/tests/fake-claude-demo.sh" \
#     build/daw_artefacts/Debug/LaLa-dev.app/Contents/MacOS/LaLa-dev
#
# report.sh は CLAUDE_BIN があれば実 claude の代わりにこれを呼ぶ。
# cwd はリファレンスフォルダ。report.py fill 済みの report.md.next があるので、
# 実フローと同じく【判】プレースホルダを埋める（完成検査を通すため。中身はダミー文章）
emit() {
  printf '{"type":"assistant","message":{"content":[{"type":"text","text":"%s"}]}}\n' "$1"
}
emit "（疑似実行）ドラフトを読んでいます"
sleep 4
emit "（疑似実行）グルーヴの節を書いています"
sleep 4
emit "（疑似実行）【判】を書き上げています"
python3 - <<'EOF'
import re, pathlib
p = pathlib.Path("report.md.next")
t = p.read_text()
parts = []
def stash(m):
    parts.append(m.group(0)); return "\0" + str(len(parts) - 1) + "\0"
t = re.sub(r"<!--.*?-->", stash, t, flags=re.DOTALL)
t = re.sub(r"〈[^〉\n]*〉",
           "（疑似実行のダミー文章。実レポートを作るときは CLAUDE_BIN を付けずに実行する。）", t)
t = re.sub("\0(\\d+)\0", lambda m: parts[int(m.group(1))], t)
p.write_text(t)
EOF
sleep 2
emit "（疑似実行）完了"
