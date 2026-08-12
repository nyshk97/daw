# 音声が要る分析の生出力（再現できないもの）

リファレンス分析の中間出力のうち、**音声を入力に取るため再生成できないもの**だけを置く。

音声はローカルの `~/Music/daw/reference-beat-corpus/` にしかなく、別マシンでは
`taste:sync` → `taste:analyze` で数時間かけて取り直す必要がある。しかも Demucs は
**同じ入力でもバイト一致しない**ため、厳密には二度と同じ数値は出ない。

| ファイル | 内容 | 入力 |
|---|---|---|
| `contrast-confound.json` | loudness依存・ボーカルbleed感度・区間選定バイアスの全数値とconfoundタグ | stem音声＋Demucs再実行 |
| `comparison-sensitivity.json` | Cymatics系loopのDemucs混入感度。どのfeatureを比較に使えるかの判定 | loop音声＋Demucs |

`comparison-sensitivity.json` の `selected_loops` は市販ループの相対パス（商品名）で、
音声も絶対パスも含まない。どの8本を標本にしたかの来歴として残している。

## ここに置かないもの

`taste-data.json` から決定的に再生成できるものは置かない。履歴がノイズで埋まるため。

| ファイル | 再生成コマンド |
|---|---|
| `analysis/machine-draft.json`（764KB） | `mise run taste:cluster` |
| `analysis/contrast.json`（120KB） | `mise run taste:cluster` → `mise run taste:contrast` |

どちらも入力は `docs/labs/reference-beat-taste-data.json`（コミット済み・音声不要）だけで、
2回実行して同じ結果になることを確認している。曲を1つ足すと全ペアの距離が変わるため
毎回ほぼ全書き換えになり、`generated_at` も毎回変わる。

**ただしこれは再生成する手段が壊れていない限りの話。** `taste-data.json` のスキーマを変えたり
`cluster.py` / `contrast.py` を改修したら、過去の出力は復元できなくなる。
そのときは復元したい版を先にここへ退避してから改修する。

## 関連

- 結論と根拠: [対照分析レポート](../reference-beat-contrast-analysis.md)
- 再取得できない人間判断: [耳確認の回答](../reference-beat-human-answers/)
- 分析の作法: [corpus-comparison.md](../../design/corpus-comparison.md)
