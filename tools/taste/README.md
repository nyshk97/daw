# 好きなビートの横断分析と、近接した否定例との対照分析

`docs/labs/reference-beat-picks.md` の箇条書きを正例コーパス、
`docs/labs/reference-beat-contrast-picks.md` を**近接した否定例**（良いが自分の作りたい方向とは
ズレる市販ビート）として、上モノ、ドラム配置、ドラム音響、ベースとハーモニー、構成を
別々のviewで比較する。絶対パスと音声はローカルの `~/Music/daw/reference-beat-corpus/` にだけ置き、
小さい正規化前featureだけを `docs/labs/reference-beat-taste-data.json` に保存する。

## role（正例／否定例）の扱い

- manifestの各曲は `role: positive | contrast` を持つ。roleは**featureにも距離計算にも渡さない**。
  アーティスト名を距離へ入れないのと同じ循環防止で、結果の解釈時にだけ使う
- 同一video IDが両方のpicksに現れたら `role_conflict` を診断してmanifestを書かない
- picksのpath定数は `common.py` ではなく `inputs.py` に置く。`common.py` は `acquire` / `trim` の
  `stage_source_hash` 対象なので、定数を1つ足すだけで既存曲の取得・トリム成果物が全部invalidになる

## 実行順

```sh
mise run taste:sync --dry-run
mise run taste:sync
mise run taste:analyze
mise run taste:grid
mise run taste:features
mise run taste:cluster
mise run taste:sensitivity
mise run taste:review
mise run taste:review-followup  # 初回耳確認で不明点が出たときだけ
```

否定例を足した対照分析は、`taste:features` まで済んだあと次の順で走らせる。

```sh
mise run taste:cluster          # 33曲ぶんの完全距離表を作り直す
mise run taste:confound         # 境界を見る前にloudness・bleed・区間選定バイアスを封じる
mise run taste:contrast         # 近接性・view別分離度・feature別の重なり・LOO安定性
mise run taste:contrast-review  # 耳確認素材（Round B/C/D/E）
```

`taste:confound` を先に走らせないと、`taste:contrast` はconfoundタグを読めないまま採用軸を出す。
順番を入れ替えないこと。

bass schema v1の特徴量ファイルをv2へ移す場合は、既存の代表区間を選び直さず低音の動き／反復だけを計算できる。

```sh
mise run taste:features -- --upgrade-bass-motion
```

通常の新規分析では`mise run taste:features`だけでv2が生成される。差分更新後は
`taste:cluster` → `taste:sensitivity -- --reuse` → `taste:review`の順で下流成果物を更新する。

`taste:analyze --only <video-id>` と `--retry-failed` で中断再開できる。曲単位は直列で、1曲の失敗後も
残りを続行する。`taste:grid` が `needs_review` にした曲だけは
`~/Music/daw/reference-beat-corpus/review/grid/README.md` を確認し、`answers.json` を作って
`tools/reference/.venv/bin/python tools/taste/grid-audit.py --apply-answers <path>` で取り込む。
READMEへ`NG`／`?`を記入した後は`mise run taste:grid-followup`で、曲を下げてclickを大きくした
再確認素材を`review/grid-followup/`へ生成できる。`NG`には小節頭accentを1拍ずつ回したA〜Dも出す。
初回の`?`をlouder版でも`NG`と回答した場合は、そのREADMEを`--annotations`へ、別フォルダを
`--output`へ渡すと未解決曲だけA〜Dへ昇格でき、確定済み回答を上書きしない。

## 所有権と再実行

- `external`: 既存のreference分析。常にread-only。完全なstage fingerprint chainが一致するときだけ参照する。
- `corpus`: このコーパスの所有物。外部sourceを使う場合もbytesをコピーしてから処理する。
- syncは削除しない。入力から消えた曲はmanifestの`orphans`へ移すだけ。
- provenanceはacquire → trim → demucs → reference analysis → taste features → distance/clusterのDAG。
  git commitではなくstageが実際に使うsource bytesと設定・親fingerprint・出力hashで同一性を判定する。
- verified gridのcorpus曲だけ`source.wav`を削除する。`track.wav`、両Demucs stem、JSON、review音声は保持する。

## 成果物

- `manifest.json`: 入力、所有権、active artifact、stage/grid状態、孤児、失敗
- `tracks/<video-id>/`: corpus所有の曲別分析
- `analysis/machine-draft.json`: view別完全距離表、近傍、群候補、不参加理由
- `analysis/comparison-sensitivity.json`: clean loopとDemucs後の共有feature感度
- `review/phase5/`: 代表2曲＋境界1曲のview別WAVとチェックシート
- `analysis/contrast-confound.json`: loudness依存・ボーカルbleed感度・区間選定バイアスとconfoundタグ
- `analysis/contrast.json`: 近接性、view別分離度、feature群／個別featureの重なりと採用軸
- `review/contrast/`: 耳確認のWAVとチェックシート（回答は `review/contrast-human-review.md` へ）
- `docs/labs/reference-beat-taste-data.json`: 音声・絶対パスを含まない派生feature

### どれをリポジトリへ入れるか

判断軸はサイズではなく**音声なしで再生成できるか**。

- **入れる**: 音声を入力に取る出力（`contrast-confound.json`・`comparison-sensitivity.json`）と
  耳確認の回答。音声はローカルにしかなく、Demucsは同じ入力でもバイト一致しないので再現できない。
  置き場は `docs/labs/reference-beat-analysis-outputs/` と `docs/labs/reference-beat-human-answers/`
- **入れない**: `taste-data.json` から決定的に再生成できる出力（`machine-draft.json`・`contrast.json`）。
  曲を1つ足すと全ペアの距離が変わって毎回ほぼ全書き換えになり、`generated_at` も毎回変わるため、
  履歴が「再生成しただけ」のノイズで埋まる
- feature schemaや `cluster.py` / `contrast.py` を改修すると過去の出力を復元できなくなる。
  復元したい版があるときは、改修の前に `reference-beat-analysis-outputs/` へ退避する

## 既知の限界

- 4/4以外はmeter-dependent viewへ入れない。自動監査が曖昧なら人間確認へ送る。
- コード名の細分類とbasic-pitch採譜を正解ラベルとして使わない。相対root運動、反復、密度などの粗い性格を使う。
- ドラム音響は完全なキック／スネア／ハット分離ではない。正規化したhit characterと、配置にも影響されるproductionを分ける。
- 正例だけなので「日本固有」とは言えない。Cymatics比較も同じ抽出器で安定した上モノfeatureだけに限る。
- 否定例10曲は全部インストのtype beat。正例23曲はボーカル入り完成曲なので、ボーカル残留と
  マスタリング音圧の非対称が乗る。confound監査を通っていないfeatureの差を境界と読まない。
- grid監査の再実行は人間確認済みの曲を上書きしない（`--force-reaudit` を渡したときだけ上書きする）。
  人間回答は `review/grid/answers.json` と `review/grid/contrast-answers.json` に残す。
