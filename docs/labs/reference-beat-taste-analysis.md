# 好きなビート23曲の横断分析

分析日: 2026-08-10
対象: 本人が好きな日本語hiphop周辺の23曲（正例のみ）

## 結論

23曲をきれいなジャンル箱へ分ける結果にはならなかった。ボーカルを除いた総合では、BPM差を考慮すると
**メロディアスなchill hiphopの1つの大きな連続体**として聴こえる。上モノ、ドラム配置、構成も、機械が
提案した2群より、複数軸の連続体として扱う方が耳の判断に合った。

追加曲にも再現した離散的な言葉はbassの2極だけだった。

- **動く支え**: シグナル、Hug。低音がある程度音程を移りながら上モノを支える
- **反復する土台**: LOVE、夏の魔法'18、ゆれる、salt water taffy、Summer Situation。
  同じ低音形へ戻ることで上モノとラップの居場所を固定する

ただしこれは22曲を2群へ割る分類ではなく、間もある連続軸。bass schema v2へ低音の
pitch motion／repetitionを足すと、この7曲の最近傍同型率は6/7から7/7へ改善したが、全距離を
人間ラベルへ強制分離できるほどではなかった。

## 対象と分析条件

- 23曲すべてを正例として収集。否定例や洋物対照曲は含まない
- grid監査: 自動4/4確認15曲、人間4/4確認7曲、判定不能1曲
- `お嫁においで`は曲中のgrid driftを解決できず、drum audio以外の拍位置依存viewから除外
- 拍位置依存viewは22/23曲、drum audioは23/23曲が参加
- Demucsの実音から、上モノ／ハーモニー、ドラム配置、ドラム音響、bass／ハーモニー、曲全体の構成を
  別々に測定。総合距離は5 viewすべてに参加した22曲だけ
- 機械候補はprecomputed距離のaverage-linkageで作り、singletonを拒否。耳確認はPhase 5の30 WAVと、
  不明点だけに絞ったfollow-up 14 WAVを一問一答で実施
- 現行ループ素材との比較はDemucs混入感度を通った共有featureだけ。corpus 22曲対Cymatics系8 loops
- 最終bass feature schemaはv2。音声・絶対パスを含まない派生値は
  `docs/labs/reference-beat-taste-data.json`、人間回答はローカルの
  `~/Music/daw/reference-beat-corpus/review/phase5-human-review.md`

対象曲は次の23曲。

| 曲 | grid |
|---|---|
| Mom - タクシードライバー | 人間確認4/4 |
| TEN'S UNIQUE - FALL IN LOVE | 人間確認4/4 |
| C.O.S.A. × KID FRESINO - LOVE | 自動確認4/4 |
| RIP SLYME - One | 自動確認4/4 |
| CHICO CARLITO × 唾奇 - memory trigger | 自動確認4/4 |
| STUTS × SIKK-O × 鈴木真海子 - 0℃の日曜 | 自動確認4/4 |
| 15MUS - YourSong | 人間確認4/4 |
| Teddy Habits - 君はともだち？ | 人間確認4/4 |
| STUTS × SIKK-O × 鈴木真海子 - Summer Situation | 自動確認4/4 |
| 604 × MAVEL, Hang & TOCCHI - August | 自動確認4/4 |
| TORAUMA × 水9％ - salt water taffy | 自動確認4/4 |
| 空音 × kojikoji - Hug | 自動確認4/4 |
| TEN'S UNIQUE - EXPLAIN | 自動確認4/4 |
| Def Tech - My Way | 自動確認4/4 |
| LIBRO × 元晴 - シグナル | 人間確認4/4 |
| Mom - 夏の魔法'18 | 自動確認4/4 |
| tofubeats - LONELY NIGHTS | 自動確認4/4 |
| 15MUS - 本当のこと | 人間確認4/4 |
| PUNPEE × 加山雄三 - お嫁においで | 不明（drift） |
| Ritto × CHOZEN LEE - kumonoue | 自動確認4/4 |
| 田我流 - ゆれる | 自動確認4/4 |
| ASOBOiSM - TOTSUKA | 自動確認4/4 |
| Sweet William × 唾奇 - Made my day | 人間確認4/4 |

## 全曲共通核

`measured=23/23`かつ`support=23/23`を満たした機械的事実は
`drum_audio_measurable`だけ。これは全曲でドラム音響を測れたという分析条件であり、音楽的な共通点ではない。

したがって、今回の定義で断定できる**23曲すべての音楽的共通核は無い**。正例だけのコーパスなので、
「全曲に無い」ことは好みの軸が無いという意味でもない。好みは複数軸の範囲と組み合わせに現れている。

## view別の結果

### 上モノ／ハーモニー — コードだけでは型にならない

参加22/23。機械は19対3のk=2を安定候補としたが、耳では両群とも内部幅があり、群間差も同じ大きな型の
範囲だった。追加したOneはLOVE／memory triggerとコード感だけは近い一方、楽器構成、質感、刻みの細かさが
大きく違った。

結論は**複数軸の連続体**。コード進行の近さだけで上モノをまとめない。上モノloopの推薦では、コードに加えて
音色、スペクトル、onset密度、反復度を同格で残す必要がある。

### ドラム配置 — 小さな一角はあるが大分類ではない

参加22/23。機械の19対3は数値上安定していた。耳でもYourSongを含む小さい側はかなり近かったが、
大きい側は幅があり、群間も「少し違うが同じ大きな型」。Summer Situationは両側と同程度だった。

結論は**1つの連続体の中に、密度やアクセントが似る一角がある**。ドラムガチャへハードなタイプ切替を
増やす根拠にはせず、既存の密度・16分profile・swingの連続制約で扱う。

### ドラム音響 — 耳の型はあるが現featureでは固定しない

参加23/23。機械のk=2／k=3はfeature ablationで崩れ、どちらも棄却された。耳ではMade my dayが
memory trigger／Augustと明確に違い、0℃の日曜とHugもMade側、YourSongはmemory側、タクシードライバーは
中間だった。Made側は**キックの低域と重さ、ハットの明るさと細さ**で再現した。

ただし現行の帯域proxyがこの並びを再現せず、機械近傍は耳と逆転した。耳の候補型は記録するが、23曲へ
過適合する閾値や群ラベルは追加しない。将来ここを使う実装要件が生じたとき、キック／ハットのhit単位分離を
先に検証する。

### bass／ハーモニー — 「動く支え」と「反復する土台」

参加22/23。人間確認で追加曲にも再現した唯一の2極。

| 極 | 確認曲 | 聴こえ方 |
|---|---|---|
| 動く支え | シグナル、Hug | 音程を移りながらコード間をつなぎ、低域にも小さな流れを作る |
| 反復する土台 | LOVE、夏の魔法'18、ゆれる、salt water taffy、Summer Situation | 同じ形へ戻り、上モノと声が動いても帰着点を保つ |

v1ではこの7曲の同型平均距離`0.210855`、異型`0.185412`と逆転していた。pitch motion／repetitionを
追加したv2では同型`0.164738`、異型`0.147801`へ差が縮み、最近傍は7/7で同型になった。ただし平均はまだ
逆転しており、bass view全体には音域、コード相対、配置、密度の差も含まれる。機械のk=2も20対2かつ
stability不足で棄却した。

つまり2極は**生成のつまみ**として有効だが、全曲の所属ラベルではない。LaLaではタイプボタンでなく
pitch motionとrepetitionの連続制約として使う。

### 曲全体の構成 — 単純な2群でなく複数パターン

参加22/23。機械はFALL IN LOVE／EXPLAIN／TOTSUKA対残り19曲を提案したが、耳では支持されなかった。

- EXPLAIN: 大きな投入／削減と小さな足し引き
- FALL IN LOVE: 全体の変化量がかなり小さい。ただしEXPLAINと同じ大きな構成タイプ
- LONELY NIGHTS: ドラムから開始し、上モノを追加、後半で別の上モノへ交代
- kumonoue: ドラム投入と沖縄的な上モノ投入が大きな転換
- TOTSUKA: 要素を累積追加し、本編ではほぼ減らさない
- memory trigger: 最初から大部分を鳴らし、細かな減衰や短いキック抜きで動かす

機械距離でもTOTSUKAは構成viewの外れ値候補で、耳の「別パターン」と方向は合った。ただしこれらは
所属群でなく、投入、削減、交代、累積、変化量という別々の軸。構成ガチャを作る場合も1個のtype IDへ
潰さない。

### 総合 — 22曲は1つの大きな連続体

5 viewすべてに参加した22/23曲で、singletonなしの安定群候補は成立しなかった。耳でもシグナル、EXPLAIN、
ゆれるはBPM差を除けば同じ大きな型に収まった。総合の正式結果は**群なし**。部分的な違いを保ったまま、
メロディアスなchill hiphop周辺の連続体として扱う。

## 現行Cymatics系上モノloopとの比較

Demucs混入感度を通ったのはonset/rhythm、pitch motion/repetition、spectrum/textureの3群。harmonyは
分離条件で安定せず比較から除外した。耳で上モノ2群を棄却したため、最終解釈は22曲全体対8 loopsで行う。

| 共有feature | corpus 22曲 median | library 8 loops median | 標準化median差 |
|---|---:|---:|---:|
| onset rate | 3.238 | 2.688 | +0.324 |
| pitch motion | 0.106 | 0.100 | +0.136 |
| repetition | 0.771 | 0.923 | -0.697 |
| centroid | 856.7 Hz | 843.9 Hz | +0.020 |

音高の動きと明るさは近く、**完成loopを上モノの主経路にする設計は維持できる**。最大の差はlibrary側の
反復の強さで、corpus側はonsetもやや多い。素材を探し直すだけでなく、同じloopを切り替える、休符を作る、
別の短い要素を足すことで、完成loopの質感を保ちながら曲側の細かな動きへ近づけられる。

これは8本のCymatics系素材との比較であり、市販loop一般、日本／海外の差ではない。

## 代表曲の制作方法と素材調達

調査対象はbass 2極の代表4曲。確認できたのは、シグナル=LIBRO sound produce＋元晴のsax演奏、
Hug=RhymeTube、LOVE=jjj、Summer Situation=STUTSの制作。bass音源が自演奏、既存曲sample、素材loopの
どれかは公開根拠がなく不明だった。制作経路と2極の対応は断定しない。URLと確認日は
[制作方法調査](reference-beat-taste-production-research.md)にまとめた。

素材調達は両極とも、採用した上モノloopへ追従するbass MIDI＋pitch付きone-shotを第一候補にする。

- 動く支え: 2〜4音の移動モチーフ。fallbackはbass演奏loop／奏者依頼。音価やスライドまで得られるが、
  キー・コード・BPM合わせや録音の往復が増える
- 反復する土台: root中心の1〜2小節モチーフ。fallbackは長い808／sub one-shot。速く重さを作れるが、
  sustainが長いほどキックと低域が衝突する

どちらも既存のベースガチャとone-shot経路で作れる。新しいプラグインホスティングや音源経路は要らない。

## 測定限界

- 好きな曲だけなので、嫌いな曲との差や日本固有の特徴は言えない
- 1曲だけgrid driftが解決せず、拍位置依存viewは22曲
- Demucs stemは完全分離ではない。特にドラム音響のkick／hat性格とコード推定は混入の影響を受ける
- 耳確認は全23曲総当たりでなく、機械代表・境界・不明点の追加14素材に限定した
- bass 2極は7曲で再現したが、22曲全体の排他的クラスタではない
- 制作クレジットは制作者を示しても、音源の由来までは示さない。非公開部分を音から推測していない
- Cymatics比較は8 loopsだけで、harmonyを含まない

## 次の実装候補

このplanでは実装しない。必要になったものだけ別途`/dig`→`/plot`する。

1. bass card／生成器へpitch motionとrepetitionの連続制約を明示的に渡し、同じ上モノloopに対して
   「動く側／土台側」の候補を聴き比べる
2. loop推薦の現行top 5が、反復の強い素材だけに偏っていないか対照群で確認する。今回の8 loopsだけを
   根拠にweightを即変更しない
3. Made型のドラム音響を制作判断へ使いたくなった時だけ、kick／hat hit分離featureを小さなfixtureで検証する
4. 上モノ・ドラム配置・構成・overallのタイプ切替UIは作らない。連続featureと人間の試聴を保つ

機械ドラフトは[こちら](reference-beat-taste-machine-draft.md)、共有feature比較の生ドラフトは
[こちら](reference-beat-taste-comparison-draft.md)。
