#!/bin/bash
# 疑似 claude — レポート生成フローの見た目（経過行→トースト→自動再読込）を
# トークン消費なし・約10秒で確認するためのデモ。
#
#   CLAUDE_BIN="$PWD/tools/reference/tests/fake-claude-demo.sh" \
#     build/daw_artefacts/Debug/LaLa-dev.app/Contents/MacOS/LaLa-dev
#
# report.sh は CLAUDE_BIN があれば実 claude の代わりにこれを呼ぶ。
# cwd はリファレンスフォルダで呼ばれ、stream-json 風の行を吐いて report.md.next を書く
emit() {
  printf '{"type":"assistant","message":{"content":[{"type":"text","text":"%s"}]}}\n' "$1"
}
emit "（疑似実行）gates.json を読んでいます"
sleep 4
emit "（疑似実行）グルーヴの節を書いています"
sleep 4
emit "（疑似実行）report.md.next を書き出しています"
cat > report.md.next <<'MD'
# 疑似レポート — フロー確認用

これは fake claude（tests/fake-claude-demo.sh）が書いたレポートです。
実レポートを作るときはターミナルで `CLAUDE_BIN` を付けずに実行してください。

| 項目 | 値 |
|---|---|
| 生成 | 疑似（トークン消費なし） |
MD
# report.sh のサイズ下限（部分出力ガード・2000B）を満たす詰め物
for i in $(seq 1 60); do
  echo "本文行 $i — 疑似レポートのサイズ下限を満たすための行です。" >> report.md.next
done
sleep 2
emit "（疑似実行）完了"
