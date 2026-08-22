# 耳確認の回答（再取得できない人間判断）

リファレンス分析で本人が耳で答えた内容。**音声・分析JSON・距離表はすべて再計算できるが、
ここだけは再取得に耳確認のやり直しが要る。**

ローカルの `~/Music/daw/reference-beat-corpus/review/` が正本で、ここはそのコピー。
複数マシン・複数クローンで開発しているため、別環境のセッションが結論の根拠を辿れるように
リポジトリへ入れている。video IDと選択肢しか含まず、絶対パスも音声も入らない。

| ファイル | 内容 | 正本 |
|---|---|---|
| `grid-positive.json` | 正例8曲の拍子・小節頭の確認（うち1曲は`drift`で`unknown`） | `review/grid/answers.json` |
| `grid-contrast.json` | 否定例6曲の拍子・小節頭の確認 | `review/grid/contrast-answers.json` |
| `contrast-review.md` | 対照分析の耳確認（Round B〜E）の全回答 | `review/contrast-human-review.md` |
| `pack-loops-in-band.md` | 2軸帯内のパックループ5本の耳確認（それでも洋物っぽい） | このファイルが正本 |
| `2026-08-22-drum-pattern-answers.json` | ドラムステム単体の好み判定（否定例10・正例23＋混入3・検証A/B 4本） | `review/drum-pattern-answers.json` |

## 更新のしかた

耳確認を追加したらローカルの正本を更新し、ここへコピーしてコミットする。
grid回答は `grid-audit.py --apply-answers <path>` の入力形式そのままなので、
manifestが壊れてもここから復元できる（実際に一度、再監査で正例7曲の回答が消えて復元した）。

## 注意

- `contrast-review.md` の Round A は**手順ごと削除済み**。否定例として選んだ曲を「否定例として
  妥当か」と聞き直すだけで選定時点の情報を超えず、対照曲がインストなので素材が元音源と
  ほぼ同じだった。記録としてだけ残している。`review/contrast/round-a/` はもう生成されない
- 回答は再生成される機械成果物（`review/contrast/` 配下）と同じ場所へ置かない。
  ディレクトリ境界で分けることで、素材の作り直しで回答を失わないようにしている

結論と根拠は[対照分析レポート](../reference-beat-contrast-analysis.md)、
分析の作法は[corpus-comparison.md](../../design/corpus-comparison.md)。
