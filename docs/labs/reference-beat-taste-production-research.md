# 好きなビート23曲 — bass 2タイプの制作方法調査

確認日: 2026-08-10

Phase 5の耳確認で追加曲にも再現し、schema v2のpitch motion／repetitionでも近傍が改善したbassの
2タイプだけを対象にする。音から制作方法を推測せず、クレジットまたは制作者本人の発言で確認できた範囲を書く。

## 結論

「動く支え」と「反復する土台」は、現時点では**素材の出どころの違いでなく、ベースラインの作曲上の違い**。
4代表曲ともトラック制作者は確認できたが、ベースが自作演奏・既存曲サンプリング・サンプリング用素材の
どれなのかは一次／公式情報で確認できなかった。唯一確認できた実演は「シグナル」の元晴によるサックスで、
bassそのものではない。したがって「この型にはこの入手経路が本場」という結論は出さない。

## 代表曲と確認できた事実

| bass型 | 代表曲 | 確認できた制作情報 | 素材由来の判定 |
|---|---|---|---|
| 動く支え | LIBRO feat. 元晴「シグナル（光の当て方次第影の形）」 | アルバム全曲のサウンドプロデュースはLIBRO。元晴は同曲にサックス演奏で参加 | **セッション演奏あり**（sax）。beat／bassの由来は不明 |
| 動く支え | 空音 feat. kojikoji「Hug」Original Ver. | Produced by RhymeTube。本人も空音への提供曲を「僕が曲を作った」と説明 | 自作演奏／既存曲sample／素材loopの別は**不明** |
| 反復する土台 | C.O.S.A. × KID FRESINO「LOVE」 | Prod.／Composerはjjj、mixはThe Anticipation Illicit Tsuboi | 自作演奏／既存曲sample／素材loopの別は**不明** |
| 反復する土台 | STUTS × SIKK-O × 鈴木真海子「Summer Situation」 | All Music Produced & Mixed by STUTS。3人で制作したコラボレーション | 自作演奏／既存曲sample／素材loopの別は**不明** |

### 根拠

- 「シグナル」: [ULTRA-VYBEの商品情報](https://www.ultra-vybe.co.jp/release/4526180571452/)は
  『なおらい』全曲のサウンドプロデュースをLIBROと明記。
  [LIBRO本人インタビュー](https://natalie.mu/music/pp/libro02/page/2)は、元晴が前作「シグナル」に
  参加し、その演奏がよかったため次作でもサックスを依頼したと説明している。
- 「Hug」: [空音の公式MV説明](https://www.youtube.com/watch?v=syHCwaounPc)に
  `Produced by RhymeTube`。[RhymeTube本人の対談](https://spincoaster.com/special-cross-talk-rhymetube-x-pecori-part-1)でも
  空音らへの提供曲を自分が作った曲として説明している。MPCを使ってビートメイクを始めたという本人発言はあるが、
  「Hug」で使った機材や音源の由来までは述べていない。
- 「LOVE」: [SUMMIT公式MV説明](https://www.youtube.com/watch?v=4TRM3cps2Jo)に`Prod by jjj`と
  album credits、mix担当を掲載。[公式配信クレジット](https://www.youtube.com/watch?v=DEFDaSl7eoA)も
  jjjをcomposer／associated performerとしている。特定のsampleや演奏者は確認できない。
- 「Summer Situation」: [STUTS公式discography](https://stutsbeats.com/music)に
  `All Music Produced & Mixed by STUTS`。
  [STUTS本人インタビュー](https://realjapanesehiphop.com/stuts/)はSIKK-Oの提案から3人で制作した経緯を
  説明するが、この曲の音源の由来までは述べていない。

## LaLaでの素材調達

### 動く支え

- **第一候補: 採用した上モノloopの進行へ追従するbass MIDIを生成し、ピッチ付きone-shotで鳴らす。**
  2〜4音の移動をモチーフとして反復すれば、土台を保ちながら次のコードへ向かう感覚を作れる。
  音程とタイミングを後から直せるため、完成済みbass loopを探すより上モノとの衝突を早く解消できる
- **フォールバック: bass演奏loop／奏者への依頼。** スライドや音価の揺れまで一度に得られるが、
  キー・コード・BPMが合う素材を探す時間、または録音の往復が増える。使う場合も上モノloop確定後にする
- サックスやギターなどの実演は、今回確認できた「シグナル」のように上モノへ有機的な動きを足す候補。
  bass型そのものの代替にはしない

### 反復する土台

- **第一候補: 同じbass MIDI生成器でpitch changeを少なくし、1〜2小節モチーフを反復する。**
  root中心の短い型は、上モノとラップの空間を残しながら低域の帰着点を固定する。生成・比較が最も速く、
  既存のベースガチャとone-shot経路をそのまま使える
- **フォールバック: 長い808／subのone-shotをrootへ配置。** 音色選びだけで重さを作りやすい反面、
  sustainが長いほどキックと低域が重なるので、キックの短さとセットで選ぶ

2タイプの違いは新しい音源ホスティングを要求しない。既存のベースガチャに
`pitch motion`（低↔高）と`repetition`（変化↔反復）の制約を渡せれば、同じone-shot調達経路で両方を作れる。
