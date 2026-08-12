# 近接した否定例10曲との対照分析 — 「作りたいビートの境界」を連続条件として出す

## 概要・やりたいこと

[`docs/labs/reference-beat-contrast-picks.md`](../labs/reference-beat-contrast-picks.md) の10曲を
**近接した否定例**として既存コーパスへ追加し、正例23曲との対照から「自分が作りたいビートの境界」を
**制作に使える連続的な条件**として取り出す。

出口は好き／嫌いの分類器ではない。LaLaの上モノ・ドラム・ベース・構成のガチャへ渡せる
**軸ごとの目標レンジと、避ける方向**である。分類精度を上げること自体を目的にしない。

前回の[23曲横断分析](2026-08-09-reference-beat-taste-corpus.md)で作った `tools/taste/` の
sync → analyze → grid → features → cluster → sensitivity → review をそのまま再利用し、
正例23曲の既存成果物は再計算しない。

## 前提・わかっていること

### 前回の結論のうち、今回の土台になるもの

- 正例23曲は5 base view（上モノ／ハーモニー、ドラム配置、ドラム音響、ベース／ハーモニー、曲全体の構成）と
  総合viewで比較済み。総合は**群なしの連続体**、離散的に再現したのはbass 2極だけ
- grid最終状態は `auto_verified_4_4` 15曲、`human_verified_4_4` 7曲、`unknown` 1曲（お嫁においで）。
  拍位置依存viewは22曲、drum audioは23曲
- distance schemaはbass v2。view別の `required_features`・参加条件・完全距離表・average linkageの契約は既存
- 正例だけのコーパスでは「全曲共通の音楽的核」は出せなかった。**今回の対照群は、まさにこの
  「共通核が出せない」を埋めるために足す**

### 対照10曲の実体（メタデータ確認済み・2026-08-10）

全10本がYouTubeのtype beat＝**インスト**。142〜260秒。日本語圏2本（Omamurin / WICSTONE）、
US系8本（Larry June型、Isaiah Rashad×Aaron May型、21 Savage×JID型、R&B型、UK Rap型ほか）。

これは分析上、無視できない非対称を作る。

- **正例23曲**: ボーカル入り完成曲をDemucsで剥がした残り。非ボーカルstemにボーカル残留（bleed）が乗る
- **対照10曲**: 元からボーカルが無い。bleedがゼロ
- **正例**: 商用マスター（音圧・サチュレーション・バス処理あり）。**対照**: type beatのラフマスター

したがって「上モノの質感が違う」「ドラムのキャラクターが違う」に見えるものが、好みの差ではなく
bleedとマスタリング差で説明されうる。**Phase 3のconfound監査を境界分析より先に置く**のはこのため。

### 今回のために新しく決めること

#### 否定例の入力契約

- 正例と否定例は**別ファイル**にする。`reference-beat-picks.md` は正例の単一の真実の源のまま
- manifestの各曲へ `role: positive | contrast` を持たせる。roleは**featureにも距離にも入れない**。
  アーティスト名をfeatureへ入れないのと同じ循環防止で、roleは結果の解釈時にだけ使う
- 同一video IDが両ファイルに現れたら診断エラーにする（黙ってどちらかを採らない）
- 正例側の `taste_features` / `distance_cluster` は、曲集合が変わることによってのみ再計算する。
  featureコードを変える場合はschema revisionを上げ、正例・対照の両方を同一revisionで再計算する

#### 「境界」の定義（分析前に固定する）

境界を「正例と否定例を分ける決定面」とは定義しない。**featureごとに、正例が占める値域と否定例が
占める値域がどれだけ重ならないか**として定義する。したがって出力は次の3種類だけ。

1. **分離する軸**: 正例と否定例の分布の重なりが小さく、permutation nullとleave-one-outの両方を
   通ったfeature。これだけを制作条件の候補にする
2. **分離しない軸**: 重なりが大きいfeature。「ここは好みの境界ではない」という結論も同格の成果で、
   ガチャで自由に振ってよい範囲を意味する
3. **confoundに汚染された軸**: Phase 3で高感度と判定されたfeature。分離して見えても境界として使わない

#### 統計的な健全性の条件（結果を見る前に固定する）

23対10でfeature数は数十ある。何も縛らなければ偶然分離するfeatureが必ず出る。

- **feature群単位で先に宣言**する。個別featureの探索的な当たり引きを主結論にしない
- **permutation test**: 33曲のroleラベルをシャッフルして同じ統計量を計算し、null分布を作る
  （反復10000、seed固定）。観測値がnullの上位5%に入らない軸は採用しない
- **leave-one-out安定性**: 正例を1曲、否定例を1曲それぞれ抜いた33通りで同じ軸が選ばれるか。
  1曲抜くと消える軸は「その1曲の性質」であって境界ではない
- **多重比較**: 群単位でBenjamini–Hochbergを適用し、生のp値だけで語らない
- 上記を通っても、**耳確認で支持されるまで「境界」とは書かない**。前回のdrum audioと同じ扱い

#### 「近接した否定例」であることの検証

否定例が正例から遠すぎると、境界ではなく「ジャンルが違う」を測ることになる。一方で、
**遠いという理由で曲を捨てない**（捨てると境界が手前に寄る）。

- view別・総合で、各否定例の最近傍正例距離を、正例内の最近傍距離分布と比較して申告する
- 正例内分布の外側に大きく出る曲は「遠い候補」としてフラグを立てるが、機械が自動除外しない
  （Round Aは削除したため、フラグは結果への併記だけに使う）
- 遠い曲を除いた場合／含めた場合の両方で境界分析を出し、結論が曲の出入りで反転しないかを見る

### 既存パイプラインで再利用できるもの／足りないもの

再利用できる。

- `corpus.py sync/analyze`（差分同期・再開・provenance DAG・external read-only契約）
- `grid-audit.py` / `grid-followup.py`（拍子・小節グリッド監査と人間確認素材）
- `segments.py` / `features.py`（view別代表区間とfeature生成。bass v2まで）
- `cluster.py`（view別の完全距離表。今回はクラスタリング結果ではなく**距離表**を使う）
- `comparison-sensitivity.py`（混入条件を変えてfeatureの頑健性を測る枠組み。Phase 3で流用する）
- `review.py` / `review-followup.py`（試聴素材生成と自己検査）

足りない。

- roleの概念（Phase 1で追加）
- confound監査（Phase 3で新規。loudness・bleed・区間選定バイアス）
- 「ラップの余白」feature（Phase 4で新規。既存5 viewのどれにも無い軸）
- 対照分析そのもの（Phase 5で `contrast.py` を新規）

### 容量

対照10曲で+6〜8GB。現在corpus 14GB、空き52GB。grid確定後に `source.wav` を消す既存契約は
対照曲にもそのまま適用する。

## 実装計画

### Phase 1: roleを持つ入力契約への拡張 [AI🤖]

- [x] ~~`common.py` に `CONTRAST_PICKS_PATH` を追加し~~ 入力契約は `inputs.py` へ分け、
  `corpus.py sync` が正例・否定例の2ファイルを読むようにする
  - 各曲へ `role`（`positive` / `contrast`）と `picks_source`（相対path）を記録する
  - 同一video IDが両ファイルに出た場合は `role_conflict` として診断し、manifestを更新しない
  - 既存23曲のmanifestエントリはroleが増えるだけで、`status`・`stage_status`・`grid`・
    `active_artifact_path` を変更しない（＝再分析を誘発しない）
- [x] `taste-provenance.json` の `taste_features` / `distance_cluster` stageへ、
  対象曲集合（video ID列とrole）をfingerprint入力として追加する。曲が増えた事実が
  下流stageのinvalidationとして正しく伝わることをテストで固定する
- [x] roleが `features.py` / `cluster.py` / `distances.py` のどの計算経路にも入らないことを
  テストで固定する（roleを反転させても距離表がバイト一致すること）
- [x] `.mise.toml` の `taste:sync` はそのまま両ファイルを読む。descriptionを更新する
- [x] 表記揺れfixtureは既存のものを流用しつつ、両ファイル同時読み込み・role衝突・
  片方だけ空ファイルのケースを追加する

### Phase 2: 対照10曲の取得・分析・grid監査 [AI🤖 → 問題曲のみ人間👨‍💻]

- [x] `taste:sync --dry-run` → `taste:sync` で10曲がmanifestへ `pending` として入ることを確認
- [x] `taste:analyze` を実行。曲単位直列・失敗継続・再開の既存契約のまま。1曲の失敗で残りを止めない
- [x] 各曲の成功判定は既存契約（track非無音・主要JSON・両Demucs stem・provenance再計算可）を使う
- [x] `taste:grid` で10曲をトリアージ。`needs_review` の曲だけclick素材を生成し、
  `grid-followup` の-7dB／+7.6dB再確認フローを既存のまま使う。未回答を暗黙の4/4にしない
  - type beatはイントロが無音〜フェードのものがあり、自動トリムが小節頭を外す可能性がある。
    trim境界も監査対象に含める
- [x] grid確定曲だけ既存契約で `source.wav` をcleanupする。`needs_review` / `unknown` は保持
- [x] 10曲のBPM・キー・尺・ゲート結果・grid状態・容量・所要時間をサマリーへ出す
- [x] YouTube側の削除・年齢制限・タイトル取り違えをmanifestへ申告する

### Phase 3: confound監査（境界分析より先に置く） [AI🤖]

境界を1つも出さないうちに、「好みの差と誤認してはいけない差」を先に洗い出して封じる。

- [x] `contrast-confound.py` を新設し、次の3つを測ってJSONへ残す
- [x] **(a) loudness依存**: 現行の抽出経路で、各featureが入力ゲインにどれだけ依存するかを測る。
  同一音源を-12dB／0dB／+6dBで通し、値が動くfeatureを列挙する
  - band_balance・centroid・hpss_ratio・hit character（hit単位正規化済み）は不感なはず。
    `rms_db`・`activity_rms_db`・区間選定の活性閾値は感応するはずで、**予想と実測が食い違ったら
    そこにバグがある**という読み方をする
  - 感応するfeatureは、対照分析の前に**曲単位のintegrated loudness正規化**（beat-only mixを
    -23 LUFSへ）を挟んでから再抽出する。正規化で消えない差だけを残す
- [x] **(b) ボーカルbleed感度**: 正例23曲は非ボーカルstemにbleedを持ち、対照10曲は持たない。
  この非対称が効くfeatureを特定する
  - 正例の各曲で、`arrangement.json` のsectionからボーカル**活性区間**と**非活性区間**（間奏・
    イントロ等）を分け、同じview featureを両方で計算する
  - 両者の差が、正例間のfeature分散に対して大きいfeatureを `bleed_sensitive` とタグする
  - 判定条件は分析前に固定する: |median差| が正例内IQRの0.5を超える、または区間内順位相関
    Spearman `rho < 0.6` のどちらかで感度ありとする
  - 非活性区間が足りない曲は「測定不能」として欠損に残し、0で埋めない
- [x] **(c) 区間選定バイアス**: `segments.py` の代表区間選定が、正例と対照で系統的に違う性格の窓を
  選んでいないかを見る。選定理由の分布・窓の曲内位置（先頭寄り／中央寄り）・活性RMS・
  採用小節数をrole別に集計する。偏りがあれば選定条件を変えるのではなく、**偏りを結果に併記**する
- [x] 上記で `loudness_sensitive` / `bleed_sensitive` / `selection_biased` のいずれかが付いたfeatureは、
  Phase 5で分離が出ても**境界候補にしない**。除外理由と実測値をレポートへ必ず載せる
- [x] マスタリング差そのもの（音圧・サチュレーション・帯域上限）は、正例＝商用マスター、
  対照＝type beatラフマスターという制作条件の差なので、**分離しても境界として書かない**。
  ただし「差がある」ことは事実として記録し、隠さない

### Phase 4: 「ラップの余白」featureの追加 [AI🤖]

既存5 viewのどれも「ボーカルが載る場所が空いているか」を測っていない。今回の観点として明示されており、
かつ正例・対照を**同条件（beat-only mix）で測れる**ので新規に追加する。

- [x] `features.py` へ `vocal_space` view（meter_dependent、代表区間ベース）を追加する
  - **帯域の空き**: ボーカルの主要帯域（おおよそ200Hz〜4kHz。人声のfundamental〜第一・第二
    フォルマント帯を覆うため）のエネルギー比と、その帯域内のspectral flatness。
    詰まっているほど比が高く、flatnessが低い
  - **時間の空き**: 同帯域の短時間エネルギーが、区間中央値に対して一定以下へ落ちる時間の割合。
    上モノが常時鳴り続けるビートは空きが少ない
  - **マスキング指標**: 同帯域を上モノ／ドラム／ベースのどのstemが埋めているかの寄与配分
  - 帯域端・閾値はコードの定数にし、33曲の結果に合わせて後から動かさない。動かす場合は
    schema revisionを上げてログへ理由を残す
- [x] `vocal_space` は**正例のボーカルbleedに感応する**可能性が高いので、Phase 3(b)の
  bleed感度測定へ必ず含める。感度が高ければ「参考値」に降格し、境界としては使わない
- [ ] 既存5 viewのfeature値・距離表がこの追加で変わらないことをテストで固定する
  （`vocal_space` は総合viewのmandatoryへ入れない。前回固定した5 view契約を今回の都合で変えない）

### Phase 5: 境界分析 `contrast.py` [AI🤖]

- [x] `cluster.py` の完全距離表を入力に、次の4つを出す。**クラスタリングも分類器も作らない**
- [x] **(1) 近接性**: view別・総合で、各否定例の最近傍正例距離を、正例内最近傍距離の分布
  （median / IQR / max）と並べる。外れる曲へ `distant_candidate` フラグを立てるが除外しない
- [x] **(2) view別の分離度**: 各viewで「正例×正例」距離と「正例×否定例」距離の2分布を比較する
  - 統計量はAUC（Mann–Whitney U を正規化したもの。しきい値に依存せず順位だけで測れるため）
  - permutation nullとBH補正を適用する
  - **分離しなかったviewも同じ紙面で報告する**。それは「その軸は自由に振ってよい」を意味する
- [x] **(3) feature別の重なり**: 各scalar featureで正例／否定例の median・IQR・10–90%帯を出し、
  overlapping coefficientとrank-biserial correlationを計算する
  - 分布型（16分profile、band_balance）と列型（root列、section列）は、正例側のmedoidからの
    schema距離で1次元化してから同じ扱いにする。medoidは正例だけで決め、否定例を混ぜない
  - 出力は「正例の目標帯」と「否定例が集まる方向」の2つ。**閾値ではなくレンジで書く**
- [x] **(4) 安定性**: leave-one-out（正例1曲抜き23通り＋否定例1曲抜き10通り）で、(2)(3)の
  採用軸が変わらないかを見る。1曲で消える軸はレポートで「不安定」と明記して候補から落とす
- [x] `distant_candidate` を含む／除く両方で全体を再計算し、結論が反転しないかを併記する
- [x] 全出力に参加曲数（`n_positive` / `n_contrast`）、除外理由、schema revision、confoundタグを付ける。
  母数を書かない主張を1つも出さない
- [x] 決定性（seed固定・入力順非依存）をテストで固定する

### Phase 6: 耳確認 [AI🤖が素材生成 → 人間👨‍💻が一問一答]

前回同様、**1問ずつ**。一度に大量の比較を出さない。

- [x] `contrast-review.py` で素材を生成し、`review/contrast/` へ置く。全WAVの尺・RMS・peakを自己検査
- [x] ~~**Round A: 否定例としての妥当性**（10問）~~ 削除（ログ参照）
  - 各否定例のbeat-only 20秒前後を1曲ずつ。「良いが自分の方向ではない」で合っているか、
    「実は好き」「遠すぎて比較にならない」ではないかを確認する
  - `distant_candidate` フラグの有無は**本人へ見せずに**聴いてもらい、後で照合する
    （機械の判定に回答が引きずられるのを防ぐ）
- [x] **Round B: 分離軸の確認**（軸ごとに1問）
  - Phase 5で残った軸ごとに、正例代表と否定例代表のA/B素材を出す。素材は軸に対応したstemだけ
    （上モノ軸なら上モノのみ、ドラム軸ならドラムのみ、余白軸ならbeat-only）
  - 「この違いは自分にとって作りたい／作りたくないの差か、それとも単に違うだけか」を聞く
  - 試聴loudnessを揃える。音量差で判断させない
- [x] **Round C: 境界の位置**（残った軸ごとに1問）
  - その軸の値が正例と否定例の中間にある曲を出し、どちら側に感じるかを聞く。
    連続軸のどこに線が引かれるかを、値ではなく耳で確かめる
- [x] 回答は再生成で消えない `review/contrast-human-review.md`（機械生成ディレクトリの外）へ記録する
- [x] 機械案と食い違った箇所だけ追加素材を作る。全曲総当たりへ戻らない

### Phase 7: 反映とガチャ条件への接続 [AI🤖]

- [x] 耳の回答を正解ラベルとして機械へ強制せず、ズレの原因をfeature不足・区間選定・confound・
  本当に境界ではない、へ切り分ける。妥当なものだけ反映し、変更前後をログへ残す
- [x] `docs/labs/reference-beat-contrast-analysis.md` を作成する
  - 対象（正例23＋否定例10）と分析条件、否定例の性質（全曲インストtype beat）
  - confound監査の結果と、境界から除外したfeature一覧＋除外理由
  - **分離した軸**: 上モノ／音の質感／刻み／ドラム／ベース／構成／ラップの余白の別に、
    正例の目標レンジと否定例が集まる方向。support / n / 効果量 / 安定性を必ず併記
  - **分離しなかった軸**: 「好みの境界ではない＝自由に振ってよい」として明記
  - 耳確認で機械と食い違った箇所とその解釈
  - 測定限界（10曲・インストのみ・US偏り・マスタリング差を統制しきれない点）
- [x] LaLaのガチャ条件への接続を、**軸ごとの連続レンジ**として書く。タイプ切替UIは作らない
  （前回の「上モノ・ドラム配置・構成・overallのタイプ切替は作らない」方針を維持する）
- [x] `docs/labs/reference-beat.md` へ日付見出しで短い要約とリンクを追記（既存エントリは書き換えない）
- [x] `docs/design/reference-beat.md` の仕様本文は、設計方針が実際に変わる場合だけ更新する
- [ ] 次の実装候補を列挙するが、このplanでは実装しない。必要なものだけ別途 `/dig` → `/plot`

### 動作確認 [AI🤖]

- [x] `mise run taste:test`、既存 `ref:test`、`lib:test` がexit 0
- [ ] `taste:sync --dry-run` を2回実行して2回目が変更なし。正例23曲のmanifestエントリが
  role追加以外に差分ゼロであることをdiffで確認
- [ ] roleを反転させたfixtureで距離表がバイト一致すること（roleが計算へ漏れていない）
- [ ] 10曲すべてがmanifest上で成功または理由付き欠損。無言の除外0件
- [ ] 対照曲のgrid確定後、`source.wav` が消えて `disposed_after_verified` になり、
  `needs_review` / `unknown` 曲は `present` のまま保持されること
- [ ] 正例23曲の `analysis-provenance.json` の全output hashが、今回の作業前後で不変であること
- [ ] Phase 3のloudness試験で、band_balance・centroid・hit characterが不感、`rms_db` 系が感応する
  という**事前予想と実測が一致**すること。食い違えば原因を特定してから先へ進む
- [ ] permutation testで、roleをシャッフルした場合にどのviewも有意にならないこと（自明な健全性検査）
- [ ] leave-one-out 33通りが例外なく完走し、採用軸の出入りが記録されること
- [ ] `contrast.py` を2回実行して出力がバイト一致（決定性）
- [x] review WAV全件が1秒以上・非無音・peak 0dBFS以下。各roundの素材が揃っていること
- [ ] 実行コマンド・出力抜粋・pass/fail根拠をplanログと最終報告へ残す
- [ ] 再利用可能な手順が `VERIFY.md` に無ければ既存構造へ追記する

## 未決事項

- 実際にどの軸が分離するかは分析結果そのもの。事前に固定しない
- 否定例10曲はすべて近接した否定例として妥当と本人が一括回答した。
  機械の `distant_candidate` は参考であって判定ではない
- `vocal_space` featureがbleed感度を通るかは未知。通らなければ「ラップの余白」は
  この対照コーパスでは測れないと結論し、無理に使わない
- 正例23曲は日本語hiphop、否定例10曲はUS偏り。「日本／海外の差」を今回も断定しない。
  日本語圏type beatは2本しか無く、国別の対照としては不足している

## ログ

### 試したこと・わかったこと

- 2026-08-11 confound監査の結果。loudnessは27 feature中`bass_harmony.density.rms_db`の1つだけが感応し、
  band_balance／centroid／rolloff／hpss／hit character／16分profileは不感で事前予想と一致した。
  bleedは11 featureを除外。区間選定はdrum_audio／drum_placementで否定例窓が2.9〜3.2dB大きい
- 2026-08-11 境界分析の結果。BH通過は3軸（ドラム占有率0.864／ベース音数0.854／ドラム倍音比0.826）。
  遠い候補2曲を除いても3軸とも残り、むしろ強まる。上モノは7軸がp≤0.06に並ぶが43軸一括BHで全滅した
- 2026-08-11 耳確認。Round Bはドラム占有率=好みの差、ベース音数=保留、ドラム倍音比=単に違うだけ。
  Round Cで本人が反応したのは占有率でなく「ハットが細かすぎ・キック/スネアが規則的にゆったりでない」
  だったため、Round Bの占有率の回答自体が疑わしくなった
- 2026-08-11 本人の仮説を測る軸を追加（hat_sixteenth/kick_on_quarter/snare_backbeat/各spread）。
  bleed非感応だが1本もBHを通らず、方向も仮説と逆（正例のほうが散らばる）。
  さらに実時間の打点/秒を追加したところ、TWISTEDのハットは相対時間32%tile→実時間95%tileと逆転した。
  ただし打点/秒も正例と否定例を分離しない（ハットAUC 0.555・p=0.64）
- 2026-08-11 omnibus検定を追加し、上モノがp=0.0002で6 view中最強と判明。耳でも
  「memory triggerのほうが明確に好み」と支持。交差ペア（軸の値とroleを食い違わせる）で
  明るさとコードの動きの2軸が値の側を選ばれ、haloでないことを確認した
- 2026-08-11 軸の重複を実測。明るさ↔上の伸び r=0.94、反復↔コード変化 r=-1.00（実装上の二重計上）。
  上モノの独立軸は明るさ／コードの動き／粒立ちの3つで、粒立ちは弱い傾向どまり
- 2026-08-11 レポート・labs索引・design仕様本文を更新。taste:test 58件、ref:test、lib:testがexit 0

- 2026-08-10 対照10 URLのメタデータを確認。全10本がtype beat＝インストで142〜260秒、
  日本語圏2本・US系8本。正例23曲（ボーカル入り完成曲）との非対称が判明したため、
  confound監査（Phase 3）を境界分析より前に置く構成にした
- 2026-08-10 Phase 1完了。manifest revision 2で `role`・`picks_source`・`role_counts` を追加し、
  `inputs`（positive/contrast）を持たせた。sync後の実manifest diffは、既存23曲が
  `role`・`picks_source` の2キー追加だけで `status`／`grid`／`active_artifact_path` は不変。
  `taste:analyze --dry-run` は正例23曲 `unchanged`＋否定例10曲 `would_analyze_from:acquire`。
  `taste:test` 28件pass

- 2026-08-10 対照10曲を取得・分析。10/10 `analyzed`、計5.2GB、1曲あたり66〜134秒。WICSTONEだけ
  「vocals stemの確認clipが無音」でwarning付き成功（インストなので構造的に当然。主要成果物とゲートは正常）。
  他9曲のvocals stemは無音にならず、サンプルやアドリブが乗っている
- 2026-08-10 grid監査の再実行で正例7曲の人間回答（human_verified_4_4）とお嫁においでのunknownが
  needs_reviewへ戻る事故。`review/grid/answers.json` が残っていたため全8件を復元した。
  原因は再監査が全曲を機械判定で上書きしていたこと。人間判断済みの曲をスキップする修正と回帰テストを入れ、
  再実行して保護されることを確認した（`--force-reaudit` で明示的な上書きは可能）
- 2026-08-10 対照曲のgridは6曲が `needs_review`。louder素材で一問一答した結果、
  Omamurin=OK（機械bestは6/8だったが4/4）、REHAB=OK（downbeatゲート落ちだが耳では合致）、
  Chemistry／Missing You／Love／OCEAN VIEWS=D（小節頭を3拍ぶん後ろへ補正）。
  NGの4曲が全部Dで一致した。**人手で直さなければ、対照側だけ小節頭が1拍ずれた状態で16分profileを
  比較し「ドラム配置が違う」という偽の境界が出ていた**。role別の補正量をPhase 3で記録する
- 2026-08-10 最終grid: 正例 auto 15＋human 7＋unknown 1、対照 human 6＋auto 4。
  `source.wav` はunknownの1曲だけ保持、他32曲はcleanup済み

- 2026-08-10 33曲のfeatureをschema v3で生成。参加はtopline 32／drum_placement 32／drum_audio 33／
  bass_harmony 31／arrangement 32／vocal_space 32。除外は`お嫁においで`（grid unknown）と
  WICSTONE（bass stemが実質無音）の2曲だけで、いずれも理由付き
- 2026-08-10 WICSTONEのbass stemは-72dBだが、曲の低域は20〜80Hzで19%・80〜250Hzで56%あった。
  Demucsが低域をbass以外へ振り分けた分離失敗で、「ベースが無い曲」ではない。
  `vocal_space` の窓条件から bass stem 活性を外し、mix側の低域エネルギー比（250Hz以下が10%以上）に
  変えてschema v4で再生成。bass stem音そのものから作る `bass_harmony` は除外のままにした
- 2026-08-10 区間選定バイアス監査: drum_audioとdrum_placementで、否定例の選定窓が正例より
  約2.9〜3.2dB大きい（pool IQRの0.52〜0.59倍）。positionは全viewで基準内だが、
  否定例の窓は一貫して曲の前寄り（0.37〜0.45対0.51〜0.59）

### 方針変更

- 2026-08-11 Round A（否定例としての妥当性確認）を削除した。否定例として選んだ曲を「否定例として
  妥当か」と聞き直すだけで、対照曲は元がインストなので素材がYouTube音源とほぼ同じになり、
  選定時点の情報を超えない。本人の指摘で判明。10曲すべて妥当として一括記録した
- 2026-08-11 群単位の軸を「正例medoidからの距離」で1次元化する設計を破棄し、roleを見ない
  距離表の上で測る方式へ変更。medoidを正例から決める以上、正例が近くなるのは構成上ほぼ自明で、
  ラベル入れ替えのpermutationでは補正できない（AUC 0.177の「採用軸」が1本出ていた）
- 2026-08-11 bleed監査を「正例のボーカル活性区間 対 非活性区間」から「否定例へ実ボーカルを混ぜて
  Demucsし直す」方式へ変更。前者は区間の音楽的な差とbleedを分離できず、drum_placement.profiles.lowが
  曲間IQRの2.7倍動くという物理的にありえない値が出た。方式変更後は同じ指標が0.49倍に落ちた
- 2026-08-11 相関した軸への一律BHが厳しすぎたため、同じシャッフルを全軸へ同時に当てる
  omnibus検定を追加した。統計量・有意水準は全viewへ同じものを適用し、上モノを特別扱いしていない
  （drum_placementはp=0.051で落ちている）
- 2026-08-11 ステム単体のA/Bだけでは halo と真の軸を区別できないため、交差ペア
  （軸の値とroleを食い違わせた2曲）を確認手順へ追加した

- 2026-08-10 否定例は正例と同じ `reference-beat-picks.md` へ混ぜず別ファイルにした。
  正例の単一の真実の源を汚さず、roleの取り違えを構造で防ぐため
- 2026-08-10 `selection_biased` 単独では軸を不採用にしない方針へ変更した。この監査が測っているのは
  窓の活性RMS＝音量域の差で、音量に不感なfeature（band_balance・16分profile・hit character等）の
  値は窓の音量が違っても壊れない。view単位の粗いフラグで全ドラム軸を捨てるのは過剰なので、
  採用を止めるのは `loudness_sensitive` と `bleed_sensitive` の2つにし、selection_biasedは
  レポートへ必ず併記する注記として持ち回る（planの「偏りを結果に併記する」に沿う扱い）
- 2026-08-10 view単位のconfoundタグが配下の軸へ伝播しないバグを修正した。
  prefix一致を片方向だけで見ていたため、`drum_audio` に付いたタグが
  `drum_audio.hit_character` に届いていなかった
- 2026-08-10 実行順をPhase 2 → **Phase 4** → featureを1回生成 → **Phase 3** → Phase 5 に入れ替えた。
  Phase 4の `vocal_space` 追加は `features.py` を変えるので、先にPhase 3を回すと33曲分の
  feature生成（前回22曲で約26分）を2回走らせることになる。節番号は参照が壊れるので変えない
- 2026-08-10 picksのpath定数を `common.py` へ置くと、`acquire`／`trim` stageの
  `stage_source_hash` が変わって既存23曲が全部再取得対象になった（dry-runで33曲すべてが
  `would_analyze_from:acquire`）。取得やトリムの挙動を一切変えない入力契約は `inputs.py` へ
  分離し、per-song stageのsource hash対象へ入れない契約をテストで固定した
