# ボーカルのピッチ補正（メロダイン型・非破壊・クリップ単位）

設計の参照先: `docs/design/region-settings.md`（可視性の原則・非破壊パラメータの扱い。**本planで見送りリストを一部書き換える**）/
`docs/plans/2026-08-18-1028-audio-transpose-stretch.md`（v20 の非破壊レンダー基盤。本planはその延長）/
`docs/design/feature-scope.md`

## 概要・やりたいこと

録ったボーカル（ラップ・歌もの）の**音程のズレだけを直す**道具を内蔵する。
解決する問題は「テイクの良さ（ノリ・感情）を残したまま音程を直す」— 録り直すと音程は合うが
ノリが死ぬ、という状況。メロダイン（Logic なら Flex Pitch）相当。

- 対象は**ボーカル1本（モノフォニック）だけ**。和音・楽器・ポリフォニック分離は永久に対象外
  （メロダインの価格の大半はポリフォニック分離＝DNA。捨てると自作できる規模になる）
- 主用途は**チル系ラップ/メロディアスなサビの「ノートの中心を寄せる」補正**。ケロケロ
  （Auto-Tune のハードチューン）は**たまに使う**ので、同じエンジンの「吸い付きの速さ＝最速」の
  プリセットとして持つ（別実装にしない）
- リバーブ・コンプ・サチュレーター等をすべて内蔵で作ってきた流れに乗せ、外部ツールを持たない
- メロダインの「高い・UIが古い」への不満が動機。**自分が使う操作だけ**に絞る

### やらないこと

- リアルタイムのトラックFX型（Auto-Tune 型）。録音中にモニターへ効かせる用途は持たない
- ポリフォニック（和音）の検出・編集
- メロダイン型の3つまみ（Pitch Center / Drift / Modulation）。ビブラートの深さ編集は持たない
- タイミングの**一括**クオンタイズ（ラップのノリはグリッドを外すこと自体が味。遅れた1音を手で直す
  だけにする）
- フォルマントのつまみ（常時ON固定）
- セント単位のノート移動（半音単位のみ。`region-settings.md` の既決「ファインチューンは持たない」
  と揃える）

## 前提・わかっていること

### /dig で確定した仕様（2026-08-20）

| 論点 | 決定 |
|---|---|
| 処理方式 | **オフライン・クリップ単位・非破壊**。v20 の `RenderCache` → `ClipStretcher` 基盤の延長。オーディオスレッドは無改修 |
| 対象 | ボーカル（モノフォニック）専用 |
| ピッチ検出 | **アプリ内 C++ 完結**。YIN/pYIN 系の自作を第一候補とし、Phase 0 の lab で CREPE と比較して足りるかを確定 |
| 再合成エンジン | **Phase 0 の lab で確定**。候補: signalsmith-stretch（導入済み・位相ボコーダー＋フォルマント補正）/ **TD-PSOLA（有声）＋素通し（無声）のハイブリッド（自作）** / WORLD（BSD-3）。Rubber Band R3 は **lab の「品質の天井」参照のみ**（GPL。`daw` / `daw-releases` は PUBLIC リポジトリなので本体に組み込むなら LaLa 全体の GPL 化か商用ライセンス購入が要る。採用したくなったらその判断を別途行う） |
| フォルマント | **常時ON（内部固定）**。補正量は半音以下が大半で、声が細く/太くなる副作用を消すためだけに使う。つまみは出さない |
| 編集の粒度 | **自動スナップ＋ノート単位の手直し** |
| スケール | **プロジェクトの `ProjectKey` を既定**（`Project.h:502`・v13・optional）。未設定ならクリップの解析結果から最尤キーを推定して提案。クリップ側で「クロマチック（全半音）」等へ上書き可。**スナップは「固定」方式**: スケールはスナップ実行時にノートの `targetMidi` を決めるためだけに使い、保存するのは確定した `targetMidi`。後から `ProjectKey` を変えても音は変わらない（手直し済みノートを勝手に変えない）。合わせ直したいときはエディタの「キーに合わせ直す」を明示的に押す（レビュー指摘4・2026-08-20） |
| クリップ全体のつまみ | **強さ（%）＋速さ（ms）の2つ**。強さ=ノート中心を目標音へ寄せる割合（開いた直後は 0%。ダブルクリックで 80%＝「合っているが機械的でない」の目安）。速さ=ノート内の動きを目標へ引き寄せる時定数（既定 200ms・ダブルクリックでも 200ms。ビブラート・しゃくりを残す）。各ラベルの右の「?」でホバー＝ツールチップ・クリック＝吹き出しの説明（Strength はノートの縦位置＝手移動と同じ層・Speed はノート内の揺れ＝別の層、を読める）。**ケロケロはプリセットボタン**（速さ0ms・強さ100%） |
| ノート操作 | ①上下ドラッグで目標音を変える（半音単位・スケール外も可）。**手で目標を置いたノートは `pinned`** になり Strength に関係なく 100% で目標へ寄る（自動スナップ分だけが Strength に従う）。pinned が付くのは「目標が開始時と違う」とき（ドラッグの各ステップで判定。往復して戻せば付かず、モデルも変わらない）。外れるのは bypass にしたとき（bypass＝素に戻す。解除しても戻らない）。bypass 中は音程を動かせない（横移動は可）。Re-analyze は検出をやり直すので pinned も含めてゼロから（Cancel で戻せる）。Scale 変更の付け直し（resnap）は pinned を飛ばす。結合は片側だけ pinned ならその側の目標、両方なら長い側 ②ノート境界の分割・結合（検出誤りの救済） ③ノート単位バイパス（しゃくり・話し声区間を素に戻す） ④**横ドラッグでタイミング**（隣接区間が吸収・クリップ長不変・グリッドスナップ・⌥で解除） |
| タイミング一括クオンタイズ | **入れない** |
| v20 移調との関係 | **合算して1パス**でレンダー（各時刻の移動量 = 補正量 + transposeSemitones）。キー変更で補正が消えず、二重加工の劣化も無い |
| エディタの場所 | 右クリック →「ピッチ補正…」→ **独立ウィンドウ**（`MixerWindow` の型。1枚を使い回し・別リージョンを開くと中身が入れ替わる・位置サイズはセッション内維持）。デュアルモニター前提。ダブルクリックは空けておく |
| エディタの見え方 | 鍵盤＋グリッド（ピアノロールのレイアウト流用）にノートを**ブロブ**（矩形＋元ピッチカーブ＋補正後カーブ）で描く |
| 試聴 | **メイン再生（Space）でミックスごと聴く**。編集ジェスチャー中（ドラッグ・スライダー）は `Clip::previewDomain` に中間結果を載せて即座に鳴り、確定後は本レンダー（デバウンス→裏レンダー）が追いついた時点で previewDomain を外して差し替える（＝途中は新しい音・確定直後も旧音へ戻らない）。previewDomain の寿命は `Clip::dropPreviewIfCurrent`（活動中 or 待っている本レンダーと同内容なら残す）の 1 文。**ブロブクリックでそのノートだけ単独試聴**（`effectiveDomain(clipId)`＝「previewDomain があればそれ、無ければ activeDomain」を解決する共通関数の結果のメモリ内範囲を鳴らす専用 audition 経路。`AudioFilePreview` はファイル先頭からのデコード専用で流用不可） |
| 開いただけの副作用 | **project・undo・dirty への副作用なし**（例外: 解析キャッシュ＝サイドカーだけは保存する。派生データで再生成可能・undo 対象外）。自動スナップの結果は**未確定プレビュー**として**メイン再生でも聴ける**（`MainComponent` 所有の `clipId → previewDomain` 一時マップ。再生 snapshot 構築・描画・単独試聴はすべて共通の `effectiveDomain(clipId)` で解決し、`Clip::activeDomain` は触らない。ガチャ仮配置と同じ「メモリ内＋baseline で復元」の型。ミックスの中で聴いて判断するのが目的なので、聴けないプレビューは意味がない）。ウィンドウを閉じる・対象クリップを切り替えると baseline へ戻す。最初の明示編集または「補正を有効化」で**1件の undo として確定**し、そこで初めて dirty になる。「再解析」も確定まで旧解析を保持しキャンセル可 |
| 保存 | 解析結果（ピッチカーブ＋有声度）は **`clip-NNN.<curveDigest>.pitch` サイドカー**（WAV の隣・バイナリ・**世代ごとに不変ファイル**。再解析は**解析内容が変わった場合のみ**新しい digest のファイルを追加し（同じ内容なら同じ digest＝既存ファイルを再利用）、旧世代を上書きしない。補正状態は自分が乗っている `curveDigest` を保持し、undo/キャンセル/同じ WAV を共有する別クリップが旧世代を参照し続けられる）。ノート編集・つまみ・スケール上書きは **project.json（v21）**。レンダー結果は読込時に再生成（v20 同様） |
| 痕跡 | リージョン隅に**バッジ**（移調の `+2` と同じ流儀。補正が1ノートでも有効なときだけ） |
| 進め方 | **1plan**。Phase 0 = lab でエンジン確定 → 実装 |

### 解析結果をサイドカーに保存する理由

ノート境界・目標音の手直しは「解析結果の上に乗った差分」。将来検出アルゴリズムを改良すると
再解析でノート境界がズレ、既存プロジェクトの手直しが壊れる。解析を保存しておけば、改良後も
古いプロジェクトは古い解析のまま編集が維持され、「再解析」を明示的に選んだときだけ更新される。

### 「メロダイン並み」の分解（/dig での整理）

品質は2部品で決まり、**差が出るのは再合成側**:

1. **検出**（どの音程か分かる）: ボーカル1本なら pYIN / CREPE / メロダインの差は「オクターブ飛び・
   子音境界の誤り」の頻度。手直し前提なら目で直せるので致命傷になりにくい
2. **再合成**（直した音が自然か）: ①声が二重にぼやける（位相ボコーダー特有の滲み）②子音のにじみ
   ③フォルマント崩れ、が出るか

| 方式 | 仕組み | ボーカルでの性格（確度: 中。lab で検証） |
|---|---|---|
| signalsmith-stretch | 位相ボコーダー＋フォルマント補正 | 楽器ループには良い。声の小補正でも薄く滲みが乗る可能性 |
| TD-PSOLA（有声）＋素通し（無声） | 1周期ごとに波形を切り出し、間隔を詰め/広げて貼り直す。無声区間（s・k・息）は周期が無いので処理せず通す | 小〜中の補正で最も自然になり得る（フォルマントが構造上保たれ、滲みが原理的に出ない）。周期マークの精度に音質が直結。±5半音超で破綻。ケロケロは移動量が小さいので範囲内 |
| WORLD | 音程・声道の形・息成分の3つに分解して再合成するボコーダー | 音程を自由に書き換えても声質が保たれる。無加工でも薄い再合成臭。ケロケロに強い |
| Rubber Band R3 | 位相ボコーダーの高品質版（GPL） | signalsmith と同系統で一段上という評価 |

注意: 「初期メロダインが PSOLA」は推測で、Celemony はアルゴリズムを公開していない（確度低）。
メロダインのバージョンアップで上がったのは主にポリフォニック分離・無声区間の扱い・打楽器/汎用
アルゴリズムで、単旋律ボーカルの小補正の音質が劇的に変わった話は聞かない。
「古い手法＝劣る」とは限らない領域なので、**耳で決める**（Phase 0）。

### コード調査の結果

- **v20 の非破壊レンダー基盤がそのまま土台になる**:
  `RenderCache`（`Source/audio/RenderCache.h`・ワーカースレッド・指紋キー・デバウンス150ms・
  「完了までは古い音」）→ `ClipStretcher::render()`（`Source/audio/ClipStretcher.h`・オフライン専用）→
  `RenderedDomain` / `RenderFingerprint`（`Source/shared/RenderedDomain.h`）→
  `ClipDomains::collectRequests / attachRenderResult / reconcile`（`Source/shared/ClipDomains.h`）
- `RenderFingerprint` は `source / domainOffset / domainLength / semitones / ratio / sampleRate`。
  **補正内容のハッシュを追加**すれば既存のキャッシュ・dedup・失敗巻き戻しがそのまま使える
- signalsmith-stretch は `setTransposeSemitones()` をブロック単位で更新できる（時変シフト可）。
  `setFormantFactor()` / `setFormantBase()` あり。v20 は固定シード・commit SHA pin
  （`CMakeLists.txt:31-48`）
- `Clip`（`Project.h`）: `transposeSemitones` / `stretchRatio` / `renderDomainOffset` は**要求値**、
  `activeDomain` が実効値。`Clip::audio` は分割・複製・ペーストで**共有参照**（原音は書き換え禁止）。
  `Clip` に安定IDは無く、結果は指紋で引く設計
- `Project::currentVersion = 20` → **v21** に上げる。**v21 で `Clip::id`（`juce::uint64`）を新設**:
  `MidiRegion::id`（`Project.h:238`・分割の右側は呼び出し側で採番）と同じ方式。独立ウィンドウは
  trackIndex/clipIndex のポインタでなく **clip id で毎回引き直す**（index は vector 更新・undo で
  無効化される。v20 はレンダー結果を指紋で引くことで id を避けたが、エディタの「対象」には安定 id が
  要る）。分割・複製・ペースト・読込（旧版で id=0）で新 id を採番し、id 重複はテストで検出
- **プロジェクトにキーはある**: `std::optional<ProjectKey> key`（`Project.h:502`。root 0..11 ×
  major/minor・未設定=nullopt）。`ProjectKeys::displayName()` / `fromCardText()` あり
- **独立ウィンドウの前例**: `MixerWindow`（`Source/ui/MixerWindow.h`・`DocumentWindow`・
  ネイティブタイトルバー・閉じる=非表示・キーは中身経由で `MainComponent` の集中ハンドラへ転送）
- **ピアノロール**: `PianoRollView`（鍵盤＋グリッド）。MIDIリージョンのダブルクリックで下パネルに
  開く（`MainComponent::openPianoRoll`・`BottomPanelHistory`）。ピッチエディタは**下パネルに
  入れない**（別ウィンドウ）が、鍵盤・グリッド描画の部品は流用候補
- オーディオリージョンの右クリックメニュー: 「ゲイン…」「移調・伸縮…」が `TimelineView.cpp` に
  ある（吹き出し）。「ピッチ補正…」はここに並べる
- 録音は**モノ24bit WAV**。取り込み素材はステレオあり得る → Mid（L+R）で解析し両chに同じ変換
- lab の型: `tools/noiselab/`（`setup.sh`・`requirements.txt`・`measure.py`・`listen/`）と
  `tools/stemlab/`（`blindprep.py`・`answer_template.py`）。耳の回答は
  `docs/labs/reference-beat-human-answers/` にコピー。記録は `docs/labs/*.md` 追記専用

### 私の側で決めた細部

- **解析はソースWAV単位**（共有原音ごとに1回。サイドカーは WAV に紐づく）、**ノート編集は
  クリップ単位**。**分割時は補正状態（ノート列・つまみ）を左右へそのまま丸ごとコピーし、表示だけ
  クリップ範囲で絞る**（ノート列を切らない）→ 親子の recipe digest が同じになり、親の
  `activeDomain` を共有したまま**分割直後は再レンダーが起きない**。子を編集した瞬間にその子だけ
  自範囲へ detach（範囲外のノートを捨て、境界をまたぐノートを切る）して以後は別 digest。
  複製・ペーストは編集ごとコピー（レビュー指摘・2026-08-20 第2回）
- undo は「1ドラッグ＝1件」「つまみはクリック列の先頭で区切る」（`GOTCHAS.md` の既存ルール）。
  プレビュー段階の副作用（dirty化・undo 追加）は確定操作まで遅らせる（`CLAUDE.md` 操作設計の方針）
- ショートカットは `Shortcuts.h` 経由。ウィンドウ内でも Space / ⌘Z / , . がメインへ転送される
  （`MixerWindow` と同じ経路）
- 無音・無声区間はノートにしない（有声度の閾値で切る）。検出器が「ノート」と取った話し声区間は
  バイパスで戻す。**レンダー時も有声度マスクを掛け、無声フレームは常に移動量0で素通し**
  （ノート区間内にあっても）。ノートは「有声フレームに対する目標のまとまり」
- ノートの代表音程は**中央値**（ビブラート・語尾のしゃくりに引きずられない）。
  ノート分割の規則: 有声区間が続き、かつ半音の半分以上のジャンプが無い限り同じノート
- タイミング編集の時間写像はピッチ写像と同じ1パスに入れる（エンジンが PSOLA なら PSOLA の
  時間伸縮、signalsmith なら入出力長の差。どちらでも「区分線形の時間写像」を渡す形に統一）。
  **`RenderedDomain` は現状一様 `ratio` しか表現できない**（`mapBoundary` = `(src-offset)*ratio`・
  `Project::splitClip` もその逆変換 `sourceForBoundary` に依存）→ Phase 2 で**単調な区分線形マップ**
  に置き換える（一様 ratio はノード2点の特殊形）。分割後も同じドメインを共有できる
- ワーカーへは**不変の `RenderRecipe`**（ピッチカーブの対象部分・解決済み目標カーブ・時間写像・
  transpose・エンジン設定・全入力の digest）を `Request` が強参照で持つ。ワーカーが可変な `Clip` や
  サイドカーキャッシュ・エディタ状態を読まない（編集中・undo・再解析・削除との競合を構造で塞ぐ）。
  `RenderedDomain` も同じ digest を保持し、装着・pending 判定・失敗巻き戻しの一致判定に使う
- `region-settings.md` の書き換え: 「ボーカルのタイミング直しは⌘Tで足りるため永久に不要」→
  「ピッチエディタ内のノート横ドラッグ（隣接吸収）で扱う。リージョン単位のオーディオクオンタイズ
  （Flex Time 相当・生ドラム用途）は引き続き不要」。「吹き出しは2つで打ち止め」は維持し、
  ピッチエディタは「独立ウィンドウは別枠（ミキサーと同格）」として整理する

## 実装計画

### 事前準備 [人間👨‍💻]
- [x] lab 素材は既存の `~/Music/daw/2026-08-18-tundra`（uta = clip-008・rap = clip-001。
  noise-removal lab で実測済み・SM58 → mk5・48kHz・モノ24bit）を使う（2026-08-20 確定）。
  「わざと外した歌」「サ行・息多め」の箇所が素材に無ければ、lab 側で人工デチューン
  （±30セントのゆっくりした揺れを付加）と、素材中の子音・ブレス箇所の切り出しで代用する

### Phase 0: lab — 検出と再合成の聴き比べでエンジンを確定 [AI🤖]

記録先: `docs/labs/pitch-correction.md`（追記専用・新規）。ツール: `tools/pitchlab/`。
耳の回答は `docs/labs/reference-beat-human-answers/` にコピー。
**生成物（PNG・WAV・JSON・ログ）は必ず `tools/pitchlab/work/`（中間）と `tools/pitchlab/listen/`
（ブラインド用）配下へ出す**（`.gitignore` 済み。ルート直下へ出さない。各スクリプトは出力先を
引数で受けず固定する）。lab の結論に引用する数値・小さなPNGは `docs/labs/` 側へコピーする
（実声 WAV はコピーしない）。

- [x] `tools/pitchlab/` を作る（`setup.sh`・`requirements.txt`・venv。`noiselab` の型）。
  依存: librosa（pyin）・crepe（または torchcrepe）・pyworld・soundfile・numpy。
  Rubber Band は `rubberband` CLI（brew → Brewfile 経由）
- [x] `analyze.py`: 素材ごとに pYIN と CREPE の f0 カーブ＋有声度を出し、**重ね描きPNG**と
  不一致率（オクターブ飛び・有声/無声の食い違い）を数値化。数値の読み方を lab に1〜2文で書く
- [x] `notes.py`: f0 カーブ → ノート列（中央値・分割規則）。ノートのPNG（ブロブ風）も出す。
  この規則は後で C++ に移植するので**Python側を仕様の原型**にする
- [x] `resynth.py`: 4エンジンで同一の目標カーブを合成する
  - ケース A: スケールスナップ・強さ100%・速さゆるめ（半音以内の補正が大半）
  - ケース B: 全ノート +3 半音（大きめのシフト。移調との合算の代表）
  - ケース C: ケロケロ（速さ0ms）
  - **ケース D: 1音を横移動し隣接区間が吸収**（実装予定のタイミング編集の代表）。評価は耳に加えて
    「出力サンプル数が入力と一致するか」「子音境界でクリック/二重化が出ないか」を数値・波形で見る
  - エンジン: signalsmith（Python binding が無ければ C++ の小さな CLI を `tools/pitchlab/` に置く）/
    TD-PSOLA＋無声素通し（自作・numpy）/ WORLD（pyworld）/ Rubber Band R3（CLI・`--formant`・
    ピッチマップ。**参照のみ**: 公式 CLI はピッチマップ使用時に realtime mode になり全体長が厳密に
    ならないので、ケース D の固定長評価は対象外と明記する。採用には GPL/商用ライセンスの判断が要る）
- [x] `blindprep.py`: エンジン名を伏せてシャッフルした WAV と回答テンプレートを出す
  （`stemlab/blindprep.py` の型）。ソロとオケ込み（簡易ミックス）の両方
- [x] 評価軸を lab に先に書く（実装前に固定）: ①滲み（二重感）②子音・息の荒れ ③声質の変化
  ④ケロケロの「らしさ」 ⑤無加工（補正量0）の透明性。各5段階
- [x] 検出側の判定は **CREPE を正解にしない**（両者が同時に誤る・CREPE 側が誤る場合を区別できない）。
  正解付きの素材を2種用意する: ①**既知 f0 の合成素材**（のこぎり波＋ビブラート＋無音・子音代わりの
  ノイズバースト。実声のフォルマントに寄せるため簡易フィルタ付き）②実声の短い区間（各5秒程度）に
  **人間が重ね描き PNG を見て「明らかな誤り（オクターブ飛び・有声/無声の取り違え）」の箇所を
  マークした目視ラベル**（フレーム単位の手動 f0 ラベルは現実的でないので、誤りの有無と箇所だけ）。
  指標: **gross pitch error**（正解から半音以上外したフレームの割合。小さいほど良く、5%を超えると
  ブロブがゴミだらけになる目安）と **voicing の precision / recall**（有声と判定した中で本当に有声
  だった割合 / 本当に有声のうち拾えた割合。recall が低いとノートが欠け、precision が低いと息が
  ノートになる）。判定基準: 合成素材で **GPE < 3%・voicing precision ≥ 0.95・recall ≥ 0.90**
  （precision 優先: 息がノートになる方が、ノートが欠けるより手直しコストが高い）・実声で目視誤りが
  ノート数の 10% 未満かつ連続しない、をすべて満たせば自作 YIN/pYIN で進む

### Phase 0 の耳判定 [人間👨‍💻]
- [x] ブラインドで聴いて回答を埋める（8010AM と MDR-7506 の両方）
- [x] 結果を見てエンジンを確定する（**WORLD を採用**・2026-08-21）。**足切り: 候補3エンジン（signalsmith / PSOLA ハイブリッド /
  WORLD）はケース D を通過すること**（出力サンプル数が入力と一致・子音境界にクリック/二重化なし・
  耳で3以上）。通過者を **A → C → B** の順で順位付け。Rubber Band は天井参照で順位付け・採用対象外。
  全滅なら lab にその旨を書いて設計に戻る（タイミング編集の方式を見直す）

### Phase 0 の結論の記録 [AI🤖]
- [x] `docs/labs/pitch-correction.md` に結論・数値・回答へのリンクを追記。
  回答を `docs/labs/reference-beat-human-answers/` にコピー
- [x] 採用エンジンに応じて Phase 2 の手順を書き換える（ログ > 方針変更）

### Phase 1: 解析（ピッチ検出）とサイドカー [AI🤖]
- [x] `Source/audio/PitchAnalyzer.{h,cpp}`: YIN（lab の結論次第で pYIN の平滑化も）。
  入力: モノ化した原音バッファ＋SR。出力: `PitchCurve`（ホップ 5〜10ms ごとの f0[Hz]・有声度・
  RMS）。**オフライン専用**（`ClipStretcher` と同じ但し書き）。ステレオは Mid で解析
- [x] `Source/shared/PitchCurve.h`: 構造体と**サイドカーの読み書き**（GC は Phase 2 の `PitchCorrection` 実装時に行う — 保持対象の `curveDigest` 集合がそれまで存在しない）（`clip-NNN.<curveDigest>.pitch`・
  バイナリ・**世代ごとに不変**。再解析は**解析内容が変わった場合のみ**新ファイルを追加し、同一内容
  なら同じ digest＝既存ファイルを再利用。旧世代は消さない）。WAV と同じディレクトリ。
  整合性の規則（レビュー指摘5）:
  - ヘッダ: フォーマット版・アルゴリズムID・ホップ・SR・**元WAVの識別子（フレーム数・ch数・SR・
    内容 digest）**。WAV と食い違えば未解析扱い
  - **ピッチカーブ自体の digest** を持ち、レンダー指紋（RenderRecipe）に含める（再解析でカーブだけ
    変わったときに古いレンダーを使わない）
  - 書き出しは一時ファイル → atomic replace（途中まで書かれたファイルを読まない）
  - 読込検証: サイズ・frame count・有限値・値域（f0 は 0 または 30〜2000Hz、有声度 0..1）
  - GC: `Project::save` の未参照 WAV 掃除（`Project.cpp:894` 付近）を拡張し、**project・undo 履歴・
    clipboard のいずれかの補正状態が参照する `curveDigest` の世代**に加えて、**開いているエディタの
    プレビュー（初回自動スナップ・明示的な変更プレビュー）が乗っている世代**も保持対象にする
    （保存入口 `MainComponent.cpp:4626` から `keepReferencedWavs` と同じ経路で「保持する digest 集合」を
    渡す）。それ以外の世代と WAV ごと消えたものを削除。**⌘S はプレビューを破棄しない**（保存は
    永続モデルだけを書き、プレビューの一時マップは保存対象外のまま維持）。テスト: 解析→未確定の
    まま ⌘S→確定でサイドカーが残っている／**内容が変わる再解析**のプレビュー中に ⌘S→適用で新世代が
    残り、キャンセルで旧世代が残っている／**同一内容の再解析**では新ファイルが作られず既存世代が
    そのまま残る
- [x] `Source/shared/PitchNotes.{h,cpp}`: `PitchCurve` → **検出ノート列**（lab の `notes.py` を移植）。
  `DetectedPitchNote { startFrame, endFrame, medianMidi(float) }` **だけ**（目標音・バイパス・
  タイミングは持たない。編集状態は Phase 2 の `PitchCorrection` が唯一の持ち主で、タイミングは
  `TimeNode` に分離済み＝二重化しない）。**フレームは WAV 先頭基準の絶対座標**（解析はソース WAV
  全体が単位）。自動スナップ時の変換規則: 各検出ノートを絶対サンプル座標
  `[startFrame × hopSamples, endFrame × hopSamples)` へ変換し（`domainOffset` は**足さない**＝
  二重オフセット防止）、**現在の domain `[domainOffset, domainOffset + domainLength)` と交差**させて
  開始・終了の両方をクランプ、交差が空のノートは除外。残った境界から `TimeNode`（Δ=0）を作り、
  隣接ノートの終了と次の開始が同じ sourceSample なら1つの共有ノードにまとめ、`Note { start, end: BoundaryRef, targetMidi = スケール
  スナップ(medianMidi), bypass = false }` を構築する。domain 端に一致する境界は `domainStart/End` を
  参照。**判断ロジックは shared/ に置く**（UI に書かない・`daw_tests` で固定）
- [x] スケール推定: ノート列の音高分布から最尤キー（Krumhansl 系の相関でよい。数値の読み方を
  コメントに書く）。プロジェクトキー未設定時の提案に使う
- [x] ワーカー（`Source/audio/PitchAnalysisWorker`。エディタからの起動は Phase 3）: クリップの「ピッチ補正…」初回オープン時に解析を走らせる（`juce::Thread`＋atomic
  status の pull 型。`GOTCHAS.md`「pull型ワーカー」の3点）。進捗はエディタ内に表示。
  **古い結果の着地規則**: リクエストは原音の強参照・WAV digest・generation を持つ。
  UI へ反映するのは現在の generation/source と一致する結果だけ。対象切替・再解析・ウィンドウ破棄・
  プロジェクト切替では cancel＋join。サイドカー書き出しは完了時に WAV の存在と digest を再確認して
  から（削除済み WAV の孤児 `.pitch` を書かない）。テスト: A 解析中→B へ切替・閉じる・再解析連打
- [x] テスト: 合成サイン/のこぎり波（既知 f0・ビブラート付き・無音挟み）で検出誤差とノート分割を
  固定。**自動テストは決定論的な合成音のみ**（実声は `daw` が PUBLIC リポジトリなのでコミットしない。
  本人の声・未公開曲は `~/Music/daw/` と gitignore 済みの `tools/pitchlab/work/` に留める）

### Phase 2: 補正カーブの生成とレンダー（1パス合算） [AI🤖]
- [x] `Source/shared/PitchCorrection.h`: クリップが持つ編集状態
  `{ curveDigest, scaleOverride(optional ProjectKey | chromatic), strength(0..1), speedMs,
  timeNodes[], notes[] }`、`TimeNode = { sourceSample, timingDeltaSamples }`（**原音サンプル座標**。
  カーブのフレーム＝ホップ単位だと分割境界を表せない）、
  `BoundaryRef = domainStart | domainEnd | node(index)`（型で区別。index の大小で端点を表さない）、
  `Note = { start: BoundaryRef, end: BoundaryRef, targetMidi, bypass, pinned }` と JSON 入出力
  （v21 の `clips[].pitchCorrection`）。**タイミングのずれはノードが持ち、ノートはノードを参照する**
  （隙間0の `e_A == s_B` は同じノード index を指す＝共有ノードの Δ が1つしか無く、再読込で曖昧に
  ならない。B を +50 動かす例では共有ノード `e_A/s_B` と `e_B` の Δ が +50、A の `s_A` は 0）。
  テスト: 隙間0の B を移動→保存→再読込→A を移動→undo/redo で timeMap が一致。
  **domain 端点と time node の統合規則**: timeMap のノード列は「始点 `domainStart`（`domainOffset`, 0）＋
  `timeNodes[]`＋終点 `domainEnd`（`domainOffset + domainLength`, `round(domainLength × stretchRatio)`）」。
  端点は `timeNodes` に含めず Δ は常に 0（固定）で保存しない。`timeNodes[].sourceSample` は
  **開区間 `(domainOffset, domainOffset + domainLength)` に限る**（分割後の子 domain の端点はカーブ途中や
  フレーム境界外にも来るので、カーブのフレーム数ではなく**現在の domain 範囲**で判定）。
  **読込検証は `BoundaryRef` を原音サンプル座標に解決してから**行う: `timeNodes` は昇順・重複なし・
  開区間内、Δ は有限値、各 `Note` は `resolve(start) < resolve(end)`、node index は配列範囲内、
  ノート同士は解決後の座標で重ならない。1つでも破れば**サイドカー欠損時と同じく補正を無効化＋
  警告トースト＋dirty 化**（範囲外参照を起こさない）。
  テスト: 範囲外 index・逆順 node・NaN Δ・重なるノート・端点と同座標の node の JSON を読んで無効化
  される／**全域ノート（`domainStart`〜`domainEnd`）・終点だけ端点に一致するノート・フレーム境界外で
  分割→detach→保存→再読込**で境界と音が一致する。
  **`curveDigest` は必須項目**で、読込時にどの世代の
  サイドカーを使うかを決める（該当世代が無ければ「サイドカー欠損」扱い）。**内容ハッシュ**（指紋用）。
  `TimeNode::timingDeltaSamples` は**初期配置（現在の `stretchRatio` による一様写像）からの render
  座標のずれ**で、初期配置自体は保存せず毎回再計算する。テスト: 複数世代のサイドカーが存在する状態で
  保存→再読込→undo/redo で各状態が自分の世代を引く
- [x] `PitchCorrection::targetCurve()`: 元カーブ＋ノート列＋強さ・速さ＋スケール →
  各フレームの移動量[半音]（＋時間写像）。**ここに transposeSemitones を足して1パス**。
  判断ロジックは shared/・テストで固定（強さ0=無変化（pinned ノートは 100%）・速さ0=ノート内で一定・バイパス=そのノートは0）
- [x] **時間写像の不変条件**（横ドラッグの規則。shared/ に置きテストで固定）:
  - ノードは「各ノートの開始・終了境界」。**無声区間（隙間）は独立の区間**で、隣接ノートの境界が
    動いた分だけ一様に伸縮する（＝隙間が吸収する。どちらの隣接ノートにも属さない）
  - **`timeMap` に v20 の `stretchRatio` を合成する**（外側で別途掛けない＝二重適用を防ぐ）:
    始点 = (`domainOffset`, 0)・終点 = (`domainOffset + domainLength`, `round(domainLength × stretchRatio)`)。
    ノートの横移動は**この最終 render 座標の中**で行う。「クリップ長不変」は「現在の stretch 済み
    実効長を維持する」の意味。先頭・末尾ノード（= domain 端点。`timeNodes` には含めない）は固定
  - ノードは各ノート i の開始 `s_i` と終了 `e_i` の2つ。**横ドラッグは `s_i` と `e_i` を同じ量 Δ
    動かす**（ノート自身の長さは不変）。伸縮するのは**ドラッグしたノートの両隣の区間**:
    手前の区間 `[e_{i-1}, s_i]` と後ろの区間 `[e_i, s_{i+1}]`。隙間があれば隙間が、**隙間0
    （`e_{i-1} == s_i`）なら隣接ノートがその分だけ伸縮する**（共有境界はドラッグ中のノートに付いて動く）。
    数値例: A=[0,500ms]・B=[500,900]・C=[1000,1400]（A-B は隙間0、B-C は隙間100）。B を +50 →
    A=[0,550]（伸びる）・B=[550,950]・隙間 B-C は 50 に縮む・C は不動。B を −50 → A=[0,450]・
    B=[450,850]・隙間 B-C は 150。A を +50 → 先頭ノードは固定なので A の手前（原音長0の区間）は
    伸ばせない → **原音長0の区間は吸収できない＝そのドラッグはクランプされ動かない**（先頭無音が
    あれば無音が吸収する）
  - 隙間0の `e_A == s_B` は**1つの共有ノードとして格納**する（ノード列は座標の集合で、ノートは
    ノードへの参照を持つ）。出力側の境界は**厳密な昇順**。**最小長 10ms の制約は原音長が正の区間だけ**に
    適用（原音長0の区間はノードが一致していて区間を持たない）。**初期配置が実現可能であることを
    保証する**: v20 の `stretchRatio` は 0.25 まで許す（`RenderedDomain.h:24`）ので、原音 20ms の
    隙間は初期出力 5ms になり、短い無声区間は普通に発生する。規則は「**各区間の下限 = min(10ms,
    初期配置での出力長)**」（既に 10ms 未満の区間は現在長を下限とし、それ以上は縮めない。伸ばすのは
    自由）。初期配置（ドラッグ前）は定義上つねに下限を満たす。テスト: 5ms 隙間・ratio 0.25・
    多数の短区間（各 2〜8ms）で初期 timeMap が不変条件を満たし、ドラッグがクランプされる。
    10ms の根拠はピッチ周期数個分（これ未満は PSOLA でも位相ボコーダーでも破綻する）。
    **算出済みの区間別下限**を下回るドラッグは交差直前でクランプ
  - **timeMap の構築は純関数**: 入力 =（現在の `stretchRatio` による初期配置, `timeNodes[]` の保存済み
    `timingDeltaSamples`）→ **`sourceSample` 昇順に各ノードの候補位置 = 初期位置 + Δ を作り、左から
    順に「直前ノードの確定位置 + その区間の区間別下限」へ決定的に射影**（右側は末尾ノード固定の
    制約から逆向きにも射影し、両方を満たす位置に収める）して出力（保存値は書き換えない）。補正後に `stretchRatio` を変えると初期配置と下限が
    変わるが、同じ入力なら常に同じ timeMap になる。テスト: 保存済み Δ が新しい ratio の下限を破る
    ケースで、再読込・undo/redo・ratio 往復のいずれでも同じ timeMap が得られる
  - テスト: 連続する2音を逆方向へ動かす・端までドラッグ・無声区間をまたぐドラッグ・隙間0の2音
- [x] **`RenderedDomain` の時間写像を区分線形化**: `timeMap`（原音座標→render座標の単調ノード列。
  一様 ratio はノード2点＝`stretchRatio` だけのクリップ）を持ち、`mapBoundary()` / `sourceForBoundary()` /
  `renderedDomainLength()` / `peakBetween()` をマップ対応にする。`ratio` フィールドは timeMap から
  導出する表示用（倍率バッジ）に格下げし、座標計算には使わない。`Project::splitClip`・フェード・ループ・描画・単独試聴は
  すべてこの経由（`GOTCHAS.md`「× ratio / ÷ ratio 直書き禁止」の規則を拡張）。分割後は子クリップが
  同じドメインを共有し再レンダーしない
- [x] `Source/shared/RenderRecipe.h`: 不変のレンダー入力（ピッチカーブの対象部分・解決済み目標
  カーブ・時間写像・transpose・エンジン設定・digest）。`ClipDomains::Request` が強参照で持ち、
  ワーカーはこれだけを読む。`RenderedDomain` に同じ digest を保存
- [x] `Source/audio/VocalResynth.{h,cpp}`（WORLD。`ClipStretcher` と並置）: `RenderRecipe` を受けてレンダー。
  CheapTrick / D4C は原音から、f0 は自作 YIN のカーブ × 目標シフト（無声は 0）、時間写像は出力フレームごとに
  入力フレームを最近傍参照して synthesize。フォルマント保持は WORLD の構造上自動（スペクトル包絡を触らない）。
  WORLD は `mmorise/World` を FetchContent（SHA pin・BSD-3）で取得しソース直接コンパイル。
  補正なしクリップは従来の `ClipStretcher`（signalsmith）のまま（無加工ビット一致の原則）。失敗は nullptr
- [x] `RenderFingerprint` に recipe digest を追加。`ClipDomains::collectRequests` で補正付き
  クリップの要求（recipe 生成）を作る。`Clip` に `PitchCorrection` を持たせ、**分割では丸ごとコピー
  （子の初回編集時に自範囲へ detach）**・複製でコピー。**プロジェクトキーの変更では再レンダーしない**（固定方式。`targetMidi` が真実の源）。
  分割は「丸ごとコピー＋表示範囲で絞る」（上記細部）。テスト: **分割直後 `renderPending()==false`・
  親子の recipe digest が一致・子の初回編集で detach され digest が分かれる**
- [x] project.json **v21**: `clips[].id` を保存（採番は既存の `Project::allocateId()` / `nextId`
  （`Project.h:554,596`）を MidiRegion と共用）。読込時に id=0（旧版）・重複 id は再採番し `nextId` を
  更新（`nextId` はすべての id より大きくなるよう補正）。補正状態の保存・読込（欠損=補正なし）。**サイドカーが無い/壊れている/
  WAV と食い違う場合は補正を無効化**（補正状態を捨てて原音を鳴らす・読込時に警告トースト・
  dirty 化）。ノート編集の座標は原音サンプルで自立しているが、目標カーブの計算には元カーブ（有声度
  マスク・元ピッチ）が必須なので、カーブ無しでは補正を鳴らせない。
  「永続状態＝鳴っている音」（v20 の失敗巻き戻しと同じ規則）を守る
- [x] 読込時: **project.json から `PitchCorrection`（ノート・タイミング・つまみ）を復元し、その
  `curveDigest` に対応するサイドカーから `PitchCurve` を読み、両者で recipe を構築** → 既存の
  「読込時に再生成」経路で一括レンダー。検出ノートの再生成はしない（保存済みの `targetMidi`・分割結合・
  バイパス・タイミングを失わない）
- [x] テスト: 合成音で「目標通りに f0 が動いたか」を再検出で検証（±20セント以内）。
  無加工（補正なし・移調0）は **wet==dry のビット一致**（`GOTCHAS.md` 中立境界の規則）。
  **タイミング補正済みクリップの分割・フェード・ループ・保存→再読込**で境界と音が一致すること
  （区分線形マップの退化ケース: ノード上で割る・ノード間で割る・ループ境界）。
  サイドカー: WAV 差し替え・途中まで書かれたファイル・カーブ digest 変更で古いレンダーを使わないこと

### Phase 3前: UI モック [AI🤖 → 人間👨‍💻]
- [x] 単一 HTML モック（scratchpad・絶対パスを返答に明記）でブロブ・元/補正後カーブ・境界の分割結合・
  上下/横ドラッグ（隣接吸収の見え方）・バイパス表示・レンダー中の減光・上部バーを確認してから
  ネイティブへ写す（`CLAUDE.md`「UIの見た目の相談」）。**判断が分かれる3点は最低2案をカードで
  並べて同一 HTML 内で比較する**: ①未確定プレビューの表示（全体を点線/半透明 vs バナー＋ボタン強調）
  ②上部バーの構成（つまみ横並び vs スケール・確定系を左右に分離） ③タイミング吸収の見せ方
  （隣接ブロブが伸び縮みする vs 隙間を斜線で示す）。擬似データでドラッグ可能にする
- [x] 人間: モックを触って方向を確定する（2026-08-21: 3点とも B 案 — ①バナー＋Enable ボタン ②左＝音の決め方／右＝確定系（Apply/Cancel は右端に差し替わる）③吸収区間に斜線＋元位置のゴースト）

### Phase 3: ピッチエディタ（独立ウィンドウ） [AI🤖]
- [x] `Source/ui/PitchEditorWindow.h`（`MixerWindow` の型）＋ `PitchEditorView.{h,cpp}`。
  右クリック →「ピッチ補正…」で開く。1枚を使い回し。キーはメインへ転送
- [x] 描画: 左に鍵盤（`PianoRollView` の鍵盤描画を部品化して共有）・グリッド（拍/小節・
  タイムラインと同じ解像度上限 `grid-resolution.md`）・ブロブ（矩形＝ノートの目標音、中に元カーブ
  （薄）と補正後カーブ（濃））・再生ヘッド（メインと連動）・スケール外の段を薄く塗る
- [x] 上部バー: スケール（既定=プロジェクトキー・クロマチック切替・未設定時は推定値を提案して
  「プロジェクトに設定」ボタン）・**「キーに合わせ直す」**（明示的な再スナップ。キー変更では自動で
  動かない）・強さ・速さ・**ケロケロ**プリセット・再解析（確定まで旧解析を保持・キャンセル可）・
  すべてリセット・**「補正を有効化」**
- [x] **開いたときの2経路**（補正の有無で分岐）:
  - **補正なしのクリップ**: 解析 → 自動スナップを**未確定プレビュー**として開始（下記）
  - **補正ありのクリップ**: 保存済みの `curveDigest` 世代のサイドカーとノート編集（`targetMidi`・
    バイパス・タイミング・つまみ）を**確定状態のまま**表示する。**自動スナップはしない**（保存済みの
    手直しを隠さない・上書きしない）。テスト: 保存→再読込→開く→保存済み `targetMidi` がそのまま表示され
    dirty/undo が不変
- [x] **プレビューは2状態を区別する**:
  - **初回自動スナップ**（補正なしで開いたとき）: 「補正を有効化」または最初の手編集で確定
  - **明示的な変更プレビュー**（補正ありのクリップで「キーに合わせ直す」「再解析」を押したとき）:
    上部バーが**「変更を適用」「キャンセル」**に切り替わり、適用で1件の undo として確定、キャンセルで
    旧状態（旧 `curveDigest` 世代・旧ノート列）へ戻る。この状態中はノート操作・つまみを無効化
    （プレビューは自動結果のみ＝手作業を含まない）。**閉じる・対象切替・⌘B/⌘E はいずれも自動
    キャンセル＋トースト**（再スナップ・再解析は1クリックでやり直せるので確認ダイアログは置かない）。
    テスト: 変更プレビュー中に閉じる/⌘B で旧状態が維持され dirty が変わらない・**「キーに合わせ直す」の
    適用は `targetMidi` だけが変わり `curveDigest` は維持**・**「再解析」の適用は解析内容が変わった場合
    のみ新 `curveDigest` へ切り替わり**（同じ内容なら同じ digest のまま）undo で旧世代へ戻る
- [x] 確定のタイミング（補正なしで開いた場合）: 開いただけでは project・dirty・undo が変わらない。
  自動スナップは**未確定プレビュー**（previewDomain 一時マップでメイン再生可・画面上は「未確定」と
  分かる表示）。閉じる・対象切替で
  baseline へ復元。最初の明示編集または「補正を有効化」で1件の undo として確定し dirty 化。
  テスト: 開く→閉じるで project が baseline と一致・未確定中の Space 再生が補正音
- [x] **プレビューの差し込みは `Clip::activeDomain` を触らない**: `Clip::renderPending()`
  （`Project.h:94`）は要求指紋と activeDomain を比較するので、preview domain を activeDomain に入れた
  瞬間「永続要求と不一致」になり、次の `requestSync()` / `collectRequests`（`ClipDomains.cpp:78`）で
  中立/確定済み domain へ巻き戻される。代わりに **`MainComponent` が `clipId → previewDomain` の
  一時マップを所有**し、**playback snapshot 構築時と描画時だけ** previewDomain を優先する。
  `ClipDomains::renderPending / collectRequests / reconcile` からは見えない。**再生 snapshot 構築・
  描画・単独試聴は共通の解決関数 `effectiveDomain(clipId)`（previewDomain 優先・無ければ
  activeDomain）だけを通す**（どれか1つが activeDomain を直読みすると未確定中に補正前の音が鳴る）。
  閉じるときは一時マップを消して snapshot を再 push。確定時だけ正式な `PitchCorrection` と
  activeDomain へ移す。
  **⌘B（バウンス）/ ⌘E（リージョン書き出し）との関係**: 両者は永続モデルから snapshot を作る
  （`startBounceFlow`＝`MainComponent.cpp:2329`・`BounceRenderer.cpp:62`）ので一時マップは見えず、
  「補正音を聴いているのに書き出しは補正前」になる。ガチャ仮配置と同じく（`MainComponent.cpp:2249`
  `cancelGachaPreview()`）**書き出し開始時にプレビューを自動破棄**する（未確定プレビューは自動
  スナップの結果だけで手作業を含まない＝捨てても失うものがない。最初の手編集で確定済みになるので
  手作業が未確定のまま残ることはない）。テスト: preview 中の ⌘B / ⌘E で一時マップが空になり、
  書き出し結果が永続モデルと一致する
  **プレビューの有効性は毎回再照合する**: undo は project 全体を置き換える（`UndoStack.h:135`）ので、
  対象 id が存在しても source・範囲・移調・stretch・補正状態が変わり得る（例: 移調+2 で作った
  プレビュー中に undo で移調0へ戻ると previewDomain だけ +2 のまま残る）。モデル変更（undo/redo・
  通常編集・`RenderCache` 完了）のたびに現在の状態から recipe digest を作り直し、previewDomain の
  digest と**不一致なら破棄して再生成**（エディタが開いていれば現在の状態で新しいプレビューを要求）。
  破棄（不一致・閉じる・⌘B/⌘E）のたびに **editor generation を進め**、遅れて届いた旧 generation の
  結果はプレビューを復活させない。
  テスト: preview 中に別トラックのゲイン変更・通常の `RenderCache` 完了が起きても preview 音が
  維持される／undo で移調が変わると旧プレビューが消えて再生成される／⌘B で破棄後に遅着した結果が
  無視される
- [x] **プレビュー専用のレンダー経路**（通常の `RenderCache` 経路に載せない）: editor generation 付きの
  preview request/result を持ち、`attachRenderResult` / `rollbackFailedRequest` を通さない。
  閉じた後に届いた preview 結果は破棄。preview 失敗は dirty 化も永続値の巻き戻しもしない（エディタ内
  表示のみ）。baseline 復元後はオーディオ snapshot を再 push。**確定したときだけ** preview の recipe を
  正式な要求へ昇格（同一 digest ならレンダー結果をそのまま引き継ぐ）
- [x] **確定の前提条件 = サイドカーが書けていること**。サイドカーは解析完了時（＝編集が可能になる前）に
  書くので、**書き込みに成功するまでエディタの編集操作自体を無効化する**（「補正を有効化」・ノート
  操作・つまみをすべて disabled にし、再試行ボタン付きのエラーを表示。プレビューの試聴だけは可）。
  これにより「未確定プレビューは手作業を含まない」が常に成立し、⌘B/⌘E・閉じるでの自動破棄で失う
  ものがない。不変条件: 「補正が有効なら対応するサイドカーが存在し digest が一致する」
  （テストで固定）
- [x] **対象の識別は `Clip::id`**。分割・削除・移動・undo/redo のたびに id で引き直し、見つからなければ
  プレビューを baseline へ戻して閉じる（別クリップへ baseline を戻さない）。テスト: 未確定プレビュー中に
  対象を分割/削除/undo → 閉じても他クリップが変化しない
- [x] ノート操作: 上下ドラッグ（半音・ドラッグ中ポップアップ）・横ドラッグ（隣接吸収・グリッド
  スナップ・⌥で解除）・境界の分割（ハサミ or ⌘クリック）・結合（隣接2つを選んで）。
  **分割・結合の time node 規則**（shared/ に置きテストで固定）: 分割は新しい境界ノードを**現在の
  timeMap 上へ線形補間で挿入**（Δ = 補間位置 − 初期位置。分割前後で timeMap が一致＝音が変わらない）。
  左右のノートは元の `targetMidi`・`bypass` を継承。結合は**左ノートの開始ノードと右ノートの終了
  ノードの間にある中間ノードをすべて削除**（隙間0なら共有ノード1個、隙間ありなら `e_A` と `s_B` の
  2個。隙間だった原音区間は結合後のノートに取り込まれ、ピッチ補正の対象になる。結合後の区間は両端
  ノードの線形で内部の折れが消える＝音が変わり得る。これは操作の意味どおり）。**ただしピッチ補正は
  有声フレームにしか掛からない**（レンダー全体の原則: 有声度が閾値未満のフレームは移動量0で素通し。
  PSOLA ハイブリッドの「無声は処理しない」と同じマスク）ので、結合ノートが隙間を跨いでも子音・息は
  加工されない（「無声区間はノートにしない」原則と両立。ノートは有声フレームに対する目標の
  まとまりであって、区間内の全フレームを動かす指示ではない）。`targetMidi` が
  異なる2音の結合は**長い方（フレーム数）のノートの `targetMidi`**、`bypass` は**両方 true のときだけ
  true**を継承。テスト: 分割→結合で timeMap が元と一致（中間ノードが分割で挿入された補間ノードだけの
  場合）／隙間ありの2音の結合で2ノードが消え隙間が取り込まれる／異なる target・bypass の結合の継承
- [x] バイパス（b キー or 右クリック）。**undo は1ドラッグ1件**
- [x] ブロブクリックで単独試聴: `shared_ptr<const AudioBuffer>`＋render範囲を受ける**専用 audition
  request** を追加（`AudioFilePreview` の交換・ミックス構造だけ流用。API はファイル専用なので
  そのままは使えない）。排他規則: ファイルブラウザ試聴とは相互排他（後勝ち）、メイン再生中は
  クリックで単独試聴しない（再生を止めない）、録音中は無効。再レンダー中なら古い音
- [x] 変更 → `RenderCache::requestSync()`（デバウンス）→ 完了で差し替え。
  レンダー中はブロブを少し減光（「完了までは古い音」が見えるように）
- [x] タイムラインのリージョンにバッジ（`+2` の隣・補正有効時のみ）。右クリック項目名に
  「（有効）」併記
- [x] `Shortcuts.h` に追加（エディタ内のキー）。⌘? 一覧に自動で載る

### Phase 4: 仕上げ・ドキュメント [AI🤖]
- [x] `docs/design/region-settings.md` の見送りリストとオーディオトランスポーズ節を更新
  （上記「私の側で決めた細部」の文言）
- [x] `CLAUDE.md` 実装済み一覧に1行追加（「ボーカルのピッチ補正（クリップ単位・非破壊・独立
  ウィンドウ）」）
- [x] `GOTCHAS.md`: 実装で踏んだ罠（PSOLA の周期マーク・時変シフトのブロック境界・
  サイドカーの整合など）を追記
- [x] `VERIFY.md`: 確認手順（合成音での再検出・無加工ビット一致・ウィンドウ操作）を追記
- [x] `mise run test` 全通し・警告ゼロ

### AI 側の実アプリ動作確認 [AI🤖]
着手前に `~/Library/CloudStorage/Dropbox/dotfiles/.claude/references/mac-app-verification.md` を読む。
- [x] `mise run build` → dev 版起動（ユーザーの常用版を止めない）
- [x] オフスクリーン描画（既存の `fx-snapshots.sh` の型）でブロブ・元/補正後カーブ・未確定表示・
  レンダー中の減光・バッジを画像で確認
- [x] 右クリック →「ピッチ補正…」で独立ウィンドウが開く／別リージョンで中身が入れ替わる／
  Space・⌘Z がメインへ転送される、を CGEvent 合成＋`~/Library/Logs/daw/` のログで確認
- [x] 開く→閉じるで baseline 復元（project が変化しない・dirty でない）をログで確認
- [x] `VERIFY.md` の関係手順（v20 の移調・分割・保存再読込）を再実行
- [ ] 人間確認は**音質の判断とデュアルモニターでの使用感に限定**する

### 動作確認 [人間👨‍💻]
- [x] 実際のラップ/サビのクリップで: 開く → 自動スナップの状態を聴く → 1音だけ上下に動かす →
  しゃくりをバイパス → ケロケロプリセット → 保存して再起動し同じ音が鳴る（2026-08-21: 大枠 OK。細かな使い勝手は順次直す方針で本人合意）
- [x] 移調（v20）を後から変えても補正が残ることを確認（テスト・実機で確認）
- [ ] デュアルモニターで別画面にウィンドウを置き、Space / ⌘Z がメインに効くことを確認（未確認。常用しながら）

## 実装完了（2026-08-21）と残課題

大枠は実装完了（本人判断 2026-08-21）。細かな使い勝手は常用しながら順次直す。残課題（優先順）:

1. 使い勝手の細部（常用で拾う。VERIFY.md のピッチ補正セクションが確認手順）
2. WORLD のメモリ上限: 補正対象は約 2 分まで（`maxRenderBytes`）。長いテイクを補正したくなったらフレーム分割処理
3. 同じ recipe の preview／本レンダー二重合成（preview 結果を renderCache に seed する最適化）
4. 未実装のまま意図的に残すもの: スクロールバー（ホイール/スワイプのみ）・ドラッグ中の数値ポップアップ・鍵盤描画の PianoRollView 共有
5. Re-analyze は同一検出器では到達しない（検出器を改良したときに効く保守入口）。pinned の引き継ぎはしない（ゼロから・Cancel で戻せる）
6. デュアルモニターでのキー転送の実使用確認

## ログ
### 試したこと・わかったこと
- 2026-08-22 本人指摘「目標の枠がどの行にもはまっていない」: 鍵盤・グリッドの縞は半音 m を行の**下端**
  （top = yForMidi(m+1)）、目標の枠とピッチ線は m を行の**中心**（top = yForMidi(m+0.5)）で描いていて半行ずれていた。
  枠側が正しい（枠の中心＝目標音・ぴったり C3 の音が C3 の行の真ん中に来る）ので縞と鍵盤を m+0.5 基準に合わせた。
  ドラッグの y→半音は相対 dy なので影響なし
- 2026-08-21 Phase 0 の AI 側を完了（`tools/pitchlab/`・`docs/labs/pitch-correction.md` に数値）。
  自作 YIN は合成素材で GPE 0%・P 0.992 / R 0.994（合格線クリア）、実声でも pYIN/CREPE と音程 1% 台で一致。
  有声閾値は 0.8→0.6 に掃引で変更（0.8 は実声 rap で recall 0.63）。ノート分割の保持時間は 40→90ms
  （ビブラート半周期より長く。40ms だとビブラート音が割れた）
- 再合成の数値: ケロケロ（ケース C）は位相ボコーダー系（signalsmith / Rubber Band）が追えない
  （中央値 36〜57cent・p95 2 半音超）。PSOLA / WORLD は追える。PSOLA は無加工でビット一致、WORLD は
  無加工でも 0.25〜0.73 の差（再合成臭）。全エンジン ケース D の長さ一致・クリック比 1.1 以下
- 罠: signalsmith `wav.h` の `length()` は offset を引いた残量／PSOLA の次マーク同期は同じ有声区間に限る／
  pyworld は setuptools<81（詳細は lab 記録）
- 2026-08-21 Phase 1 実装: `PitchCurve`（128bit FNV 内容 digest・`clip-NNN.<hex32>.pitch` 世代不変・
  atomic replace・読込検証）/ `PitchAnalyzer`（YIN 移植。合成素材で Python 原型と同じ GPE 0 / P 0.993 /
  R 0.994 / 1.43cent）/ `PitchNotes`（分割規則＋Krumhansl キー推定）/ `PitchAnalysisWorker`（pull 型・
  generation 着地・孤児サイドカー防止）。テスト 6 本追加・全通過
- 2026-08-21 Phase 2 実装: `PitchCorrection`（TimeNode/BoundaryRef/Note・JSON・検証・digest）/ `TimeMap`（区分
  線形写像。`RenderedDomain::mapBoundary` は折れのある写像だけ timeMap を使い、一様は v20 の丸め式のまま＝
  既存の分割・ループ計算と食い違わない）/ `PitchCorrections`（buildTimeMap の決定的射影・targetCurve・
  autoSnap・resnap・moveNote/splitNote/mergeNotes/detachToDomain）/ `RenderRecipe` / `VocalResynth`（WORLD を
  FetchContent・SHA pin・6 ファイル直コンパイル・f0 は YIN カーブ）/ `RenderCache` の分岐 / `Clip::id`・
  `pitchCorrection`・`pitchCurve` / project.json v21 / サイドカー GC（`save(keepSidecars)`・UndoStack の参照世代）。
  テスト 8 本追加・全通過: WORLD は合成音で目標との差 中央値 1.2 cent（A/B/C/D とも）・横移動で長さ不変・
  同 recipe でビット一致・分割は親子 digest 一致で再レンダーなし＋左右結合が分割前とビット一致・
  保存→再読込でビット一致・サイドカー欠損/不正 JSON は無効化＋警告＋modifiedOnLoad
- WORLD のメモリ: CheapTrick/D4C のスペクトログラムが 48kHz・f0_floor 60Hz（fft 4096）で 32KB/フレーム。
  `maxRenderBytes`（768MB）で約 2 分の domain が上限（超えると失敗＝巻き戻し）。フルテイク 3 分を補正したく
  なったらフレーム分割処理が要る（Phase 4 以降の課題として残す）
- 2026-08-21 Phase 3 前の HTML モックを作成（`/private/tmp/claude-501/-Users-d0ne1s-daw/ffa3083a-28cd-4287-b0f2-d0a09ee3c63b/scratchpad/pitch-editor-mock.html`。
  scratchpad はセッション限りなので、方向が決まったら決定だけを plan に書く）。判断待ち: ①未確定プレビュー
  （点線/半透明 vs バナー）②上部バー（横並び vs 左右分離）③吸収の見せ方（伸縮のみ vs 斜線＋ゴースト）
- ノート分割の境界は平滑化の遅れで 20ms 遅れていた（C++ テストで露見）→ 分割決定後に「生の値が新しい音に
  近い限り手前へ巻き戻す」を C++/Python 両方に追加。短い音同士が合わさって十分な長さになる場合に捨てていた
  バグも両方で修正（Python 原型の結果は不変: synth 10 音・rap 33・uta 27）
- 2026-08-21 Phase 3 実装: `PitchEditorSession`（shared・遷移テスト）/ `PitchEditorView` + `PitchEditorWindow` /
  `MainComponentPitch.cpp`（配線・プレビュー専用 RenderCache・解析ワーカー・確定=1 undo・⌘B/⌘E/閉じる/undo の破棄・
  id で引き直し）/ `BufferAudition`（単独試聴・post-master）/ ♪ バッジ・メニュー項目・Shortcuts 2 件 / dev 版の検証フック
  `--pitch-editor` `--pitch-action` `--pitch-snapshot`。実アプリで 5 シナリオ（未確定プレビュー・Enable→編集→保存・
  再読込・⌘B 破棄・Enable→undo）をログとスクショで確認（VERIFY.md に手順）。27s のクリップで解析 0.5s・
  WORLD のプレビューレンダー 7s（デバウンス込み）
- 人間の耳判定（完了）: `tools/pitchlab/listen/`（rap-seg / uta-seg × Z/A/B/C/D × W/X/Y/Z）と
  `docs/labs/reference-beat-human-answers/2026-08-21-pitchlab-blind-answers.md`（目視ラベル節を含む）

### 方針変更
- 2026-08-22 本人判断: 右クリックの「再解析」を**廃止**（メニューから削除。`onReanalyze`・`--pitch-action reanalyze` は
  検出器を変えたときの差分確認用に残す）。理由: 検出は決定的で同じ検出器なら結果が変わらず、検出器が変わるのはアプリ
  更新時だけ。既存の補正は自分の世代のサイドカーで整合し続けるので取り直す必要がない。「検出器が変わったのに
  気づかない」だけは CTest `PitchAnalyzer fingerprint`（合成音の検出結果を cent 丸めでハッシュし algoId ごとに固定）で
  防ぐ。バナーで取り直しを促す案（サイドカーに検出器版を記録・裏で再解析・差があるときだけ通知）は「1 曲の編集途中で
  検出器が変わることは無い」ので見送り。Reset は「ノートの手直しを捨てて目標音を付け直す」に改名（「自動スナップ」は
  内部用語）
- 2026-08-22 本人判断: Reset（自動スナップからやり直す）は**つまみ（Strength / Speed）を現在値のまま残す**。
  autoSnap の戻り値をそのまま使っていたため構造体の初期値（100%）に跳ねていた。Reset はノートの手直しを捨てる
  操作で、つまみは全体設定（Logic の Flex Pitch「Reset All」も同じ切り分け）
- 2026-08-22 本人指摘「スケール外の段に枠が付いている」: プロジェクトキー未設定時のスナップはクロマチック
  （`effectiveScale` の規則どおり）だが、段のハイライトは推定キーで塗っていたので見た目と動作が食い違っていた。
  推定キーで勝手に付けるのは避け（確度が低く外すと手直しが増える）、Scale 項目の表示を
  `Project key: unset (guess F#m)` → `Chromatic (key unset · guess F#m)` にして実際の付け方を先頭に出す。ハイライトは
  「Use F#m」を押す判断材料として残す
- 2026-08-22 本人判断（ユースケース＝ケロケロにせず、ずれた所だけ自然に合わせる）: Speed の既定を 120ms → **200ms**
  （`PitchCorrection::defaultSpeedMs`）、Strength の「よく使う位置」を **80%**（`defaultStrength`）としてスライダーの
  ダブルクリックで戻る先にした。開いた直後の Strength 0%（2026-08-21 の判断）は維持。Strength / Speed の右に「?」
  （ホバー＝ツールチップ・クリック＝吹き出し）を追加。独立ウィンドウには TooltipWindow が無く既存のスライダーの
  ツールチップも出ていなかった（JUCE は同じ peer 内にしか出さない）ので `PitchEditorView` が自前で持つ
- 2026-08-21 Phase 3: 未確定プレビューの置き場を「MainComponent の clipId→previewDomain 一時マップ」でなく
  **`Clip::previewDomain`（永続化せず・undo state に積まず・指紋判定に出ない）＋ `Clip::effectiveDomain()`** にした。
  理由: 再生・描画・フェード換算・試聴が使う Clip のヘルパー（view 境界・renderedLength 等）が多く、外部マップを
  全経路に配るより Clip 内の 1 関数に解決点を置く方が「どれか 1 箇所が activeDomain を直読みする」事故を構造で防げる。
  plan の意図（activeDomain を触らない・renderPending に巻き戻されない・共通の解決関数）は満たす
- 2026-08-21 Phase 3: 鍵盤は PianoRollView から部品化せず PitchEditorView 内に簡素に描いた（48px・音名のみ）。
  部品化は両者の描画を揃えたくなった時点で行う。横ズーム・スクロールは本人確認で「拡大できない」と指摘 → 同日追加（⌘←/→・ピンチ・⌘ホイールで
  カーソル中心ズーム、ホイール/スワイプでスクロール。1〜64 倍）。「Reset」は「現在のスケール設定で自動スナップし直し」（変更プレビュー経由）。
  「再解析」は内容が変わった場合だけ、新カーブで自動スナップし直した案を変更プレビューにする（つまみは維持）
- 2026-08-21 Phase 3: ドラッグ中ポップアップ（目標音の数値）は未実装（鍵盤の段で読める）。ノートの横ドラッグ中の
  プレビュー更新はドラッグ終了時のみ（途中はブロブだけ動く）
- 2026-08-21 本人レビュー: 上部バーに層の違う操作が同じ見た目で並んでいた → 2 層に整理。バー = Scale・Show scale・
  Re-snap｜Strength・Speed・Hard tune｜右端の確定系（未確定中だけ Enable、変更プレビュー中だけ Apply/Cancel。
  「Correction ON」の常時表示は廃止＝状態は ♪ バッジとバナーで分かる）。Reset・Re-analyze・推定キーの設定は
  グリッドの右クリックメニューへ。キー未設定はバナーに推定キー＋「Use Dm」。略語（Kero・Hl）は Hard tune・Show scale に
- 2026-08-21 本人レビュー2: 「Re-snap」「Hard tune」は押すまで結果が見えない → 両方ボタンを廃止。
  Hard tune は Speed スライダーの左端（両端に hard / natural の目盛り文字。位置＝状態）。Re-snap は
  「Scale を選ぶ＝そのスケールで付け直すプレビュー」に統合（元の目標を点線ゴーストで残し、変わるノートが
  一目で分かる。Apply/Cancel。未確定プレビュー中なら作り直すだけ）。plan の「キーに合わせ直す」ボタンは
  この形で実現
- 2026-08-21 本人レビュー3（ブロブの見せ方）: メロダイン比較のモック（`blob-mock.html`・`scale-highlight-mock.html`）で
  **帯型ブロブを採用**（太さ＝RMS・中心＝補正後ピッチ・塗りは移動量で accent→暖色・目標の段は枠だけ・元ピッチは細線・
  無声は下端レーンに音量だけ）。スケール音の見せ方はグレーの縞（現状）のまま
- 2026-08-21 本人レビュー4（レンダーが 7 秒）: 内訳は dev（-O0）の WORLD で分解 4.7s＋合成 2.9s。WORLD のソースだけ
  -O2 でビルド（1.5s＋0.6s）＋分解結果のキャッシュ（ブロブ移動時の再レンダーは合成のみ 0.6s）。ドラッグ中は
  `VocalNoteAudition` でその音だけ合成して鳴らす（メロダインの挙動）
- 2026-08-21 本人判断: **開いた直後は何も動かさない**（Strength 0%）。自動スナップは目標音を付けるだけで、鳴りは原音。
  つまみを上げた分だけ掛かり、色（補正量）も上げた分だけ暖色になる。聴感上中立（Strength 0・Δ 0）の間は WORLD を
  通さず previewDomain も外す（＝原音そのもの）。plan の「強さの既定 100%」はこの形に改める
- 2026-08-21 実装レビュー前の棚卸し — plan に書いていなかった実装上の判断（以下を追記）:
  ① `PitchCorrection::digest()` から scaleMode / customKey を除外（音に関係しない設定で再レンダーしない）
  ② `Clip::id` の採番は各生成サイトでなく `ClipDomains::reconcile` → `ensureUniqueIds` で一括（複製・ペースト・分割右は
  id=0 にして入れる）
  ③ プレビューの遅着判定は「editor generation」でなく **recipe digest ＋ モード一致**で代替（digest はモードが変わるたびに
  変わるため同等に働く。generation は session 側にあり解析結果の着地に使用）
  ④ レンダー失敗の巻き戻しは `RenderedDomain::correction`（レンダーに使った補正状態のコピー）から復元。補正付きで
  鳴っていたなら補正ごと戻し、無ければ補正を捨てる
  ⑤ 分割（splitNote）前後の timeMap 一致は**ノード上で厳密・ノード間は ±1 サンプル**（補間ノードの丸め）。テストもその定義
  ⑥ サイドカーの書き込みはワーカー内（解析完了直後）。失敗時は `sidecarBlocked` で編集不可・Retry ボタン
  ⑦ ドラッグ中の単音試聴（`VocalNoteAudition`）・分解キャッシュ（`VocalResynth` 内 1 エントリ）・WORLD の -O2 は
  plan 外の追加（性能・操作感のため。モデル・永続化には影響しない）
  ⑧ 未実装のまま意図的に残すもの: 横ズーム以外のスクロール UI（スクロールバー無し・ホイールのみ）、ドラッグ中の数値ポップ
  アップ（帯型ブロブで目標の段が読めるため不要と判断）、鍵盤の PianoRollView との共有
- 2026-08-21 /code-review（high・10 件＋軽微 8 件・全件 CONFIRMED）を全件反映:
  ①⑦⑨ 分割子の detach を「編集中」から**開いた時点**へ（エディタの working は常にクリップ自身の範囲。クリップ側は最初の
  書き込み `pitchWriteWorkingToClip` で自範囲へ移る＝それまで親のドメイン共有・再レンダーなし）。index ずれ・空 undo・
  Apply 経路の取りこぼしを同根で解消 ② previewDomain をコピー経路（⌘C・複製・ペースト・splitClip の右）で外す
  ③⑧ 中立判定（Strength 0・Δ 0）を `requestedRecipeDigest()` に移し、プレビューと確定で同じ規則。移調のみは signalsmith
  経路のまま（開くだけで WORLD に差し替わらない）④ 解析結果の回収時にサイドカーの存在を再確認し無ければ書き直す
  （⌘S の GC との競合）⑤ 移調・伸縮の変更で previewDomain を作り直す（長さ不変条件）⑥ changePreview 中の undo で
  backup が陳腐化したら変更プレビューを破棄 ⑩ 分解キャッシュは対象切替・閉じるで解放＋原音解放で無効。
  軽微: cachedTarget に transpose/domain、null InputStream、オクターブ表記 C3=60、B/M を Shortcuts 経由、切替時の試聴停止、
  WORLD の BSD-3 を Resources に同梱、ensureUniqueIds を set で O(n log n)、BufferAudition の範囲クランプ
- 2026-08-21 /code-review 2 回目（前回 18 件は全件クローズ。修正が持ち込んだ 4 件＋軽微）を反映: ①② 「補正を自範囲で」の
  判断を `Clip::pitchCorrectionInOwnDomain()` / `sharesInheritedDomain()`（Project.h・テストあり）に集約し、open／committed 同期／
  changePreview 同期／`applyStretchRequest` の4箇所をそこへ ③ 中立補正はレンダー失敗の巻き戻しで保持 ④ 初回確定の枝で
  dirty＋requestSync。軽微: `Clip::cloneForNewId()` で複製 5 経路を統一・重複行削除・「有効」は音に出ているときだけ。
  見送り: 変更プレビュー中の移調ホイール連打（最大 2 件で実害小）・再解析提案とタイミング編集の食い違い（同一検出器では到達しない）
- 2026-08-21 /code-review 3 回目: ① `Clip::resetRenderDomainToSelf()` 自体が「共有中の補正を自範囲へ写してから戻す」
  ようにして API で塞いだ（reconcile・巻き戻し・読込・ガチャ・applyStretchRequest の全呼び出しが自動で従う。2 回続けて
  同じ型の漏れが出たため呼び出し側の規律をやめた）② 巻き戻しの「中立補正は保持」を `hasNeutralPitchCorrection()` で両枝に
  統一 ③ 保存経路の述語・初回確定の重複・debug の同一分岐を整理。「（有効）」は補正データを持てば出し、中立は「編集あり・0%」
- 2026-08-21 /code-review 4 回目: レビュー 3 の「中立補正を両枝で保持」は誤り（鳴っている音に補正が入っている枝で
  保持すると要求≠実効のまま renderPending が解けず同じ失敗を繰り返す）→ d.correction ありなら必ずそれに合わせる・
  中立保持は d.correction 無し／前例なしの枝だけ。テストで「巻き戻し後に renderPending が解ける」を固定。
  `pitchWriteWorkingToClip` は代入→reset の順（無駄な detach なし）。reset 単独で自範囲化する経路もテストに追加
- 2026-08-21 /code-review 5 回目: ① 本レンダーの失敗巻き戻し後に `pitchSyncAfterModelChange()`（エディタの working が古い
  編集を持ち続けて同じ失敗を繰り返さない）② `RenderedDomain::curve` を追加し、巻き戻しで correction とカーブを一緒に戻す
  （curveDigest と pitchCurve の食い違い防止）③ コメントの嘘 2 箇所（detach の冪等性・中立保持の条件）を修正
- 2026-08-21 /code-review 6 回目: ① 失敗トーストを先に出してからエディタ同期（上書きされない）② recipe 付きの失敗は
  「ピッチ補正の処理に失敗」文言＋ログ `pitch.render_failed`（recipe digest 付き）③ 巻き戻しでカーブも戻ることをテストで固定
  （別世代に差し替えてから巻き戻す）④ `d.curve` は jassert で契約化・旧サイドカー依存をコメントに
- 2026-08-21 /code-review 7 回目: トーストは単一スロットの後勝ちなので順序入替は逆効果 → `pitchSyncAfterModelChange(quiet)`
  で巻き戻し経路は同期側を黙らせ（エディタ内の状態表示に逃がす）、失敗トーストに「（変更プレビューも取り消しました）」を
  付ける。巻き戻し直後の再プレビューが同じ原因で失敗したときの青トーストも 1 回だけ抑止。`ContentDigest::toShortHex()`・
  VERIFY.md の失敗経路に `pitch.render_failed` を追記
- 2026-08-21 /code-review 8 回目: プレビュー失敗トーストの抑止を bool から「抑止対象の recipe digest」に（成功・キャッシュ命中・
  別要求で自然に無効）、quiet は失敗がエディタ対象クリップのものだったときだけ、committed 枝の巻き戻しもエディタ内に状態表示、
  `pitchSyncAfterModelChange` が「変更プレビューを取り消したか」を返す（文言の併記を推定でなく事実で）、
  dismiss 直前の状態表示は出さず open 時にクリア
- 2026-08-21 本人レビュー（Strength 0% 起点の副作用）: 目標を動かしても Strength 0 では音も帯も動かず、掴めるのも枠だけ
  だった → `PitchNote::pinned`（手で目標音を置いた印。JSON "pinned"・digest に含む）を追加し、pinned のノートは Strength に
  関係なく 100% で目標へ（メロダインの挙動）。自動スナップ分だけがつまみに従う。中立判定も pinned を考慮。帯（声の実体）
  でも掴めるようヒットテストを帯まで広げた。/code-review 9 回目: ownFailure は巻き戻し前に要求指紋で判定・undo で補正が消えて
  changePreview を畳むときのトースト復活・プレビュー成功/キャッシュ命中で抑止 digest をクリア
- 2026-08-21 /code-review 10 回目（pinned の付く/外れる条件）: 先に仕様表へ「付くのはドラッグ終了時に目標が開始時と違うとき・
  外れるのは bypass・resnap は飛ばす・結合は片側 pinned ならその側の目標」を書き、`PitchCorrections::setNoteTarget /
  setNoteBypass` に集約（UI・debug の全経路がこれを通る。テストで固定）。ヒットテストは描画と同じ目標カーブ（transpose 込み・
  再描画前でも更新）を使い、帯の y はノート共通で 1 回計算。失敗トースト抑止 digest の代入は要求の前に
- 2026-08-21 /code-review 11 回目: ドラッグは各ステップで `setNoteTarget` を通し（途中状態＝確定状態・往復で戻せば何も起きない＝
  dirty も undo も無し）、mouseUp は状態差で moved を決めるだけに。bypass 中はピッチドラッグを開始しない・`setNoteTarget` は
  bypass なら pin しない・読込/結合で bypass ⇒ pinned=false に正規化。ドラッグ中に working が差し替わったら（期待 digest 不一致）
  捨てる。`toggleNoteBypass`・digest 1 回計算・debug の no-op ガード・抑止クリアを `pitchRequestPreview` 先頭へ。
  Re-analyze は「ゼロから」で割り切り、手で置いた音が戻る旨を状態表示（Cancel で戻せる）。ドラッグの undo 区切り（onBeginEdit）は
  モード決定時でなく**最初の実変更の直前**に（往復して同じ段で離す／クランプで動かないドラッグは undo を積まない）
- 2026-08-21 /code-review 12 回目（空 undo の本丸）: `UndoStack::abandonLast()`（begin で退避した redo も戻す・間に undo/begin が
  入れば no-op）＋エディタの `onCancelEdit`。ドラッグ（上下・横とも）が開始時の状態に戻って終わった／クランプで動かなかった／
  split・merge が false、の経路は begin を取り消す（初回確定を伴った begin は実変更なので残す）。差し替え検出は
  `PitchEditorSession::revision()`（O(1)・両モード）。範囲チェックと差し替え検出を mode 決定の前に移し、中断時は試聴も止める。
  `setNoteTarget` は bypass なら拒否（UI/debug 共通）、digest は bypass 中の pinned を含めない（旧 JSON 正規化で指紋不変）
- 2026-08-21 /code-review 13 回目: abandon の対象確認を「深さ」から **begin のトークン**へ（`begin()` がトークンを返し
  `abandon(token)`。無効トークン・間に undo/redo/pushCommitted があれば no-op。maxDepth で押し出した 1 件も退避して戻す。
  退避分は abandon 不可になった時点で捨てる）。`pitchBeginEdit` は begin しない早期 return でもトークンを 0 に。
  `revision` は working が実際に置き換わったときだけ進める（no-op 同期でドラッグを殺さない）。スライダーのクリックだけ・
  bypass が 1 つも変わらない経路も onCancelEdit。選択は revision 変化でクリア。mouseUp の moved は各ステップの結果を使う
- 2026-08-21 /code-review 14 回目（構造）: 「begin したが変化なしなら cancel」を UI の各所で手書きしていたのをやめ、
  編集ジェスチャーを `pitchBeginEdit`（トークン＋working の snapshot）〜 `pitchEndEdit`（working と snapshot を比較し、変化なしなら
  abandon・ありなら確定）の対に閉じ込めた。View は onBeginEdit/onEndEdit で囲むだけ（判定しない）。スライダーの中間値は
  `onPreviewEdit`（鳴りだけ・dirty は終了時）。begin が重なれば前を先に閉じる。ドラッグ中の ⌘Z/⇧⌘Z は View が飲む（Logic と同じ）。
  選択のクリアは keyPressed/右クリックの先頭で同期的に。`syncCommitted` は bool を返す。maxDepth テストは復元内容まで固定
- 2026-08-21 /code-review 15 回目: `pitchEndEdit` は revision が begin 時と違えば（外部置換）変化に関係なく abandon。
  スライダー途中のプレビューは clip を書かず previewDomain 経路（`pitchPreviewActive()` = 初回/変更プレビュー or 編集
  ジェスチャー中の committed）で鳴りだけ追従し、終了時に必ず外す。undo/redo は receiver（performUndo/Redo）がジェスチャー中は
  無視（View 側もスライダー中を飲む）。dead state 削除
- 2026-08-21 /code-review 16 回目（previewDomain 経路化の聴こえ方）: 中立 working のプレビューは「activeDomain に補正が無ければ外す／
  補正付きでレンダー済みなら補正なしの鳴り（無加工は中立ドメインを即時・移調伸縮ありは recipe 無しの signalsmith をプレビュー
  経路に）」に分岐。previewDomain を外すのは「取消（pitchEndEdit）」と「本レンダーの装着（renderCache.onRenderReady）」の 2 箇所
  だけにして、確定直後に旧音へ戻る音飛びを無くした。外部置換で終わったジェスチャーは変更プレビューを鳴らし直す。
  プレビューの照合は recipe digest でなく指紋全体。残課題: 同じ recipe を preview/本レンダーで二重合成（preview 結果の seed）
- 2026-08-21 /code-review 17 回目: previewDomain の寿命規則を `Clip::dropPreviewIfCurrent(sr, previewActive)`（shared・テスト）に
  集約し、activeDomain が変わりうる 4 経路（本レンダー装着・インライン装着 attachedAny・巻き戻し後・ジェスチャー終了）の後で
  `pitchDropStalePreview()` を呼ぶ。`RenderedDomain::fingerprint()` で 7 フィールドの手組みを 1 箇所に。プレビュー失敗の照合も
  指紋全体＋活動中のみ。新しいプレビュー要求の前に旧プレビューを外す（失敗時に嘘をつかない）。仕様表の試聴の記述を更新
- 2026-08-21 /code-review 18 回目: 寿命規則を「活動中、または待っている本レンダーと同内容（指紋一致）なら残す」に強化
  （取り消した recipe のレンダー待ちに別内容のプレビューが便乗しない。テスト）。signalsmith プレビュー要求前の pre-clear を
  削除し、失敗処理を `pitchPreviewCache.onRenderFailed` の 1 箇所に（そこで外す・文言は「確定済みの音／原音で鳴っています」を
  activeDomain で分岐）。~~非活動なら fingerprint/request を必ずリセット~~（19 回目で「待っている本レンダーと別内容のときだけ」に修正）。reconcile 後にも寿命規則。push は各経路で 1 回。
  「対象外クリップの previewDomain」は warn ログに
- 2026-08-21 /code-review 19 回目: 巻き戻し経路の push を drop の後に（外した結果を engine へ）。非活動時の要求リセットは
  「待っている本レンダーと別内容」のときだけ（ジェスチャー最後の in-flight を孤児にしない）。プレビュー失敗は
  `pitchClearPreview(keepMatchingPending)` で同内容は残す。失敗文言は 1 回組み立ててトーストとエディタ内表示へ。
  不変条件 sweep も dropped 扱い。reconcile 内の push は immediate のときだけ。仕様表の文言を更新
- 2026-08-21 /code-review 20 回目: 「同内容なら残す」の呼び出し側フラグ（keepMatchingPending）を廃止し、`Clip::awaitsRender` /
  `dropPreviewIfCurrent` を唯一の述語に。`pitchReconcilePreview(previewActive)` が全クリップ＋in-flight 要求へ同じ規則を適用し、
  `pitchClearPreview`（取消・閉じる）も `pitchDropStalePreview` もそれを呼ぶだけ。失敗文言は effectiveDomain で判定。
  undo 直後の本レンダー待ちはプレビューキャッシュの同内容で隙間を埋める。reconcile 内の push は削除（呼び出し元が push する契約）。
  レビューの総括どおり、次はレビューでなく実機の耳確認（ドラッグ確定・Cancel・閉じるで音飛びが無いか）へ
- 2026-08-21 Phase 0 結論: **再合成は WORLD を採用**（耳判定で全ケース上位・ケロケロは唯一/最良。
  自作 PSOLA は全補正ケースで「プツプツ」＝実装欠陥で不採用。位相ボコーダー系はケロケロで脱落）。
  検出は自作 YIN。Phase 2 の「`ClipStretcher` 拡張 or `VocalResynth`」は `VocalResynth`（WORLD）に確定し、
  f0 は harvest でなく YIN のカーブを渡す。詳細は `docs/labs/pitch-correction.md` の「Phase 2 への含意」
- 2026-08-21: 実声の目視ラベルはフレーム単位でなく「明らかな誤りの時刻と種類」を回答テンプレートの表に
  書く形式にした（plan どおり）。ブラインドの素材は実声 2 区間（rap 4–12s・uta 7.5–15.5s）に絞り、
  合成素材は数値評価のみ（耳で聴く意味が薄い）

### 方針変更（plan レビュー・2026-08-20・実装前）
- レビュー8件を反映（全件採用）: ①区分線形の時間写像を `RenderedDomain` に持たせる ②不変の
  `RenderRecipe` をワーカーへ渡す ③Phase 0 の検出判定を CREPE 正解でなく合成素材＋目視ラベルの
  GPE / voicing P-R に変更・ケース D（横移動）追加 ④スケールは「固定」方式（キー変更で再レンダー
  しない） ⑤サイドカーの整合性（WAV 識別子・digest・atomic replace・GC） ⑥開いただけでは副作用なし・
  明示確定で1件 undo ⑦単独試聴は専用 audition request ⑧Phase 3 前に HTML モック・Rubber Band は
  lab の参照のみ（公開リポジトリのため GPL 組み込み不可。採用には商用ライセンス判断）
- plan レビュー第2回（2026-08-20）7件を反映（全件採用）: ①分割は補正状態を丸ごとコピー＋表示範囲で
  絞り、子の初回編集で detach（親子で digest 一致・分割直後は再レンダーなし） ②ケース D を採用の
  足切りに・Rubber Band は順位付け対象外・voicing の閾値 P≥0.95/R≥0.90 ③未確定プレビューは一時
  activeDomain でメイン再生可（閉じる/切替で baseline 復元）・「副作用なし」の例外（サイドカー）を明記
  ④解析ワーカーの generation/source 一致・cancel＋join・孤児サイドカー防止 ⑤サイドカー欠損時は補正を
  無効化＋警告＋dirty ⑥自動テストは合成音のみ・実声はコミットしない（`.gitignore` に pitchlab 生成物を
  追加） ⑦モックは判断が分かれる3点を各2案カード比較
- plan レビュー第3回（2026-08-20）6件を反映（全件採用）: ①v21 で `Clip::id` を新設しエディタは id で
  引き直す（見つからなければ baseline 復元して閉じる） ②プレビュー専用レンダー経路（通常の装着・
  巻き戻しを通さない・確定時だけ昇格） ③時間写像の不変条件（隙間が吸収・先頭末尾固定・境界所有者は
  開始側ノート・厳密昇順・最小10ms・交差直前でクランプ） ④サイドカー書込失敗時は確定不可＋再試行
  ⑤AI 側の実アプリ動作確認セクションを追加（人間は音質とデュアルモニターのみ） ⑥pitchlab 生成物は
  work/・listen/ 配下に固定
- plan レビュー第4回（2026-08-20）3件を反映（全件採用）: ①プレビューは `Clip::activeDomain` を触らず
  `MainComponent` の `clipId → previewDomain` 一時マップを snapshot 構築・描画時だけ優先（`renderPending`
  に巻き戻されない） ②`stretchRatio` は `timeMap` に合成（終点 = `round(domainLength × stretchRatio)`・
  `ratio` は表示用に格下げ） ③分割の記述を「丸ごとコピー＋初回編集で detach」に統一・`clips[].id` の
  保存と旧版/重複 id の再採番・`nextId` 更新を v21 に明記（`allocateId()` を MidiRegion と共用）
- plan レビュー第5回（2026-08-20）3件を反映（全件採用）: ①再生・描画・単独試聴は共通の
  `effectiveDomain(clipId)`（previewDomain 優先）に統一し、旧「一時 activeDomain」表現を全て置換
  ②⌘B/⌘E は書き出し開始時にプレビューを自動破棄（ガチャと同じ・未確定は自動スナップのみで手作業を
  含まない） ③横ドラッグは `s_i`/`e_i` を同量移動・両隣の区間（隙間、隙間0なら隣接ノート）が吸収・
  原音長0の区間は吸収不可でクランプ、を数値例で固定
- plan レビュー第6回（2026-08-21）4件を反映（全件採用）: ①サイドカーは `clip-NNN.<curveDigest>.pitch`
  の世代不変ファイル（再解析は追加のみ・GC は project/undo/clipboard が参照する世代を残す）
  ②サイドカー書込成功まで編集操作を無効化（未確定＝手作業なしを常に成立させる） ③モデル変更のたびに
  recipe digest を再照合し不一致なら破棄＋再生成・破棄時に generation を進めて遅着を無視
  ④隙間0の境界は共有ノード1個・10ms 制約は原音長が正の区間のみ
- plan レビュー第7回（2026-08-21）2件を反映（全件採用）: ①開いたときは補正の有無で2経路（補正ありは
  確定状態のまま表示・自動スナップしない。「キーに合わせ直す」「再解析」だけが未確定プレビューを作る）
  ②区間の下限 = min(10ms, 初期配置での出力長)（ratio 0.25 で 5ms になった隙間も初期配置が実現可能）
- plan レビュー第8回（2026-08-21）3件を反映（全件採用）: ①プレビューを「初回自動スナップ」と
  「明示的な変更プレビュー（適用/キャンセル・閉じる/切替/⌘B/⌘E は自動キャンセル＋トースト）」の2状態に
  分離 ②`PitchCorrection` の JSON に `curveDigest` を必須項目として追加・`timingDeltaSamples` は初期配置
  からのずれで初期配置は保存しない ③区間下限の矛盾を「算出済みの区間別下限」に統一・timeMap 構築は
  純関数で保存 Δ を決定的にクランプ（保存値は書き換えない）
- plan レビュー第9回（2026-08-21）2件を反映（全件採用）: ①タイミングのずれは `timeNodes[]` が持ち
  `Note` は `startNodeIndex/endNodeIndex` で参照（共有ノードの Δ が一意） ②サイドカー GC は開いている
  プレビューの世代も保持・⌘S はプレビューを破棄しない（保持 digest 集合を保存入口から渡す）
- plan レビュー第10回（2026-08-21）3件を反映（全件採用）: ①timeMap 構築を `timeNodes` 昇順の候補位置→
  区間別下限への決定的射影に統一 ②分割は現在の timeMap 上へ補間ノード挿入（音不変）・結合は中間ノード
  削除・target は長い方/bypass は両方 true のときのみ継承 ③`timeNodes`/index の読込検証（昇順・範囲・
  有限・非重複）を追加し不正なら無効化＋警告＋dirty
- plan レビュー第11回（2026-08-21）3件を反映（全件採用）: ①domain 端点は `timeNodes` に含めず（開区間
  に限定・`Note` 側は index -1 で端点参照・Δ は常に0） ②結合は左開始〜右終了の間の中間ノードを
  すべて削除（隙間ありは2個・隙間は結合ノートに取り込む） ③文書崩れ2箇所を修正
- plan レビュー第12回（2026-08-21）2件を反映（全件採用）: ①`BoundaryRef = domainStart | domainEnd |
  node(index)` の型分け・`TimeNode` は原音サンプル座標・開区間は現在の domain 範囲・検証は解決後の
  座標で順序判定（全域ノート/終点一致/フレーム境界外分割のテスト） ②レンダーは有声度マスクで無声
  フレームを常に素通し（隙間ありの結合でも子音・息を加工しない）
- plan レビュー第13回（2026-08-21）2件を反映（全件採用）: ①`curveDigest` はカーブの内容ハッシュなので
  「キーに合わせ直す」では維持・「再解析」は内容が変わった場合のみ新世代（テストを分離・冒頭の記述も
  条件付きに） ②`sourceFrame` の旧名称を `sourceSample` に統一
- plan レビュー第14回（2026-08-21）2件を反映（全件採用）: ①Phase 1 のノート型を
  `DetectedPitchNote { startFrame, endFrame, medianMidi }` に限定し、自動スナップ時のフレーム→
  sourceSample 変換と共有ノード化・BoundaryRef 構築の規則を明記（タイミング状態の二重化を排除）
  ②サイドカー実装・GC テストも「内容が変わった場合のみ新世代」に統一
- plan レビュー第15回（2026-08-21）2件を反映（全件採用）: ①検出フレームは WAV 先頭基準の絶対座標・
  `domainOffset` を足さず現在の domain と交差させて両端クランプ・空区間は除外 ②読込は project.json の
  `PitchCorrection`＋対応世代のサイドカーから recipe を構築（検出ノートを再生成しない）・「フレーム座標に
  乗る」の旧表現を修正
