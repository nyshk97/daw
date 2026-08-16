# FXバッチ4: 空間系（Delay → Reverb）

## 概要・やりたいこと

[fx-roadmap.md](../design/fx-roadmap.md) バッチ4。send用固定バス3本（Reverb A / Reverb B / Delay）は現在**素通し**（send値ぶんdryが加算されるだけの仮の姿）。ここにFXの実体を入れ、send が初めて「wet量」の意味を持つようにする。

- 耳テーマは「奥行き・広がり」。hiphopボーカルの奥行きはディレイで作るのが定石なので **Delay を先に**実装する
- **このバッチ完了で曲作り再開**（ロードマップの再開ライン）
- 耳セッション（バッチ3の質感・倍音＋バッチ4の奥行き・広がり）は**このplanに含めない**。完了後に別枠で設計・実施する（2026-08-16 dig で確定）

## 前提・わかっていること

### 確定した仕様（2026-08-16 dig）

**共通**

- send バス方式は現行どおり。バスFXは**常在・バイパスなし**（`FxSlotLayout::busLayout` の1枠スロットのまま）・**Mixノブ無しの full wet**（dryはトラック本体から出ているため。Logicのsend運用でTape DelayのMixを100%にするのと同じ）
- 旧プロジェクトのマイグレーション無し。過去に send>0 で保存したプロジェクトは開くと音が変わる（音量増し→残響）が、テスト用プロジェクトしか無いため許容
- 経路は **RT（PlaybackEngine）＋バウンス（BounceRenderer::mixBusesAndMaster）の2本のみ**。トラックFXの6経路問題は無い

**Delay（バス3・先行実装）**

- ノブ4点:
  - **Time**: テンポ同期4値 **1/16・1/8・1/4・1/2**（初期値 1/4 = ボーカルエコーの定石）。付点・3連は見送り（欲しくなったら足す）
  - **Feedback**: 0〜**90%キャップ**。自己発振なし＝テールが必ず有限時間で収束し、バウンスの尻尾計算が閉じる
  - **Tone**: **フィードバックループ内**のローパス（繰り返すたびに暗くなる＝遠ざかる距離感。ループ外だと本体と同じ明るさの繰り返しが並んで邪魔になる）
  - **Ping-pong**: トグル。入力モノ化→L/R交互出しの定石構成
- BPMは `TransportState::bpm`（atomic・曲中固定）でオーディオスレッドに届いている。テンポ同期は素直に計算できる

**Reverb（バスA/B・同一DSPの初期値違い）**

- **juce::dsp::Reverb から開始**。不満が出たら差し替え（mixer-fx.md の方針どおり）
- ノブ5点: **Size・Damp・Width**（juce::dsp::Reverb のパラメータ）＋自前前置の **Pre-delay**（0〜100ms）・**Low Cut**（OFF(20Hz)〜500Hz）
  - Pre-delay: 原音が鳴ってから残響が始まるまでの隙間。30〜80ms空けると子音が濡れる前に届き、深くかけても声が手前に残る（Logic ChromaVerb の Pre-Dly 相当）。juce::dsp::Reverb に無いので共有ディレイラインを前置する
  - Low Cut: 残響は音を引き伸ばすので低域（〜150Hz）が入ると床にモヤが溜まる。「リバーブの前にハイパス」はボーカルのハイパスと同じ因果。`BiquadFilter.h` を前置する
- **種類セレクタ（Room/Plate/Hall）は作らない**。単一アルゴリズムのプリセットに過ぎず、ノブ4〜5個しか無いのにセレクタを挟むと中身が見えなくなる（原理を学ぶ目的にも反する）→ mixer-fx.md の当初記述を更新する
- **A=短めRoom系・B=長めHall系の初期値違い**。B は曲作りで使わなかったら消す（ロードマップどおり）

### コードの現状（調査済み）

- バス素通し加算: `PlaybackEngine.cpp` 536行付近（RT）・`BounceRenderer.cpp::mixBusesAndMaster` 755行付近（バウンス）。ここがFX挿入点
- `SendBuses::names = { "Reverb A", "Reverb B", "Delay" }`（Project.h）・バスindex 0/1=Reverb・2=Delay（`FxSlotLayout::busFxName`）
- バスのパラメータは `PlaybackSnapshot::busParams[numSendBuses]`（TrackParams共有）。FXパラメータの置き場は新設が必要
- 単一インスタンスFXの統合前例は **Master Limiter**（バッチ2）: RT側は `PlaybackEngine::masterLimiter`＋スナップショットのparams毎セグメント読み、バウンス側は専用インスタンス＋`snapTo`。この型を踏襲する
- エディタUIの前例: `LimiterEditorView`（バス/Master詳細エディタ）・`CompEditorView`/`SatEditorView`（ノブ構成）。バススロットは `busLayout` で表示済み、クリック→`FxDetailView` の結線は Limiter と同型
- Paramsヘッダの書式は `CompParams.h` / `LofiParams.h`（plain Values＋isNeutral＋normalized＋atomic Params）
- Lo-fi の Wow が変調ディレイを既に持つ（`TrackLofi`）。ディレイライン基盤の共有候補
- `Project::currentVersion = 18` → 19 へ
- 検証基盤: `scripts/check-render-hashes.sh`（Debug/Release全経路ビット一致）・`scripts/seed-fx-test.sh`（確認用プロジェクト）・`daw_tests`

### 設計上の注意（GOTCHAS/過去バッチの知見）

- **中立境界はビット一致dryへ収束させてから高速パス切替**（バッチ3の知見）。バスFXは「無音入力→無音出力」（ディレイ・リバーブとも内部状態ゼロなら0in→0out）なので、send全0の既存プロジェクトはハッシュゲートが素通しでgreenのはず。崩れたら挿入位置を疑う
- **テスト閾値は事前数値評価で固定し、音決め定数（初期値・カーブ）に従属させない**（バッチ3の知見）
- dB平均禁止・コールバック内static初期化禁止等（GOTCHAS.md）
- **バスFXのノブはundo対象外**（既存FXエディタと同じ「atomic直書き・undo対象外＝フェーダーと同じ扱い」。`UndoStack` は TrackParams を共有したまま持つ設計で、値の undo を足すには State への値コピー機構が要る。バスFXだけ undo 可能にすると既存FXと挙動が割れるため、揃えて対象外にする — 2026-08-16 レビュー反映）
- **バウンステールの現行実装はバスFXを知らない**: `resolveWantTail()`（BounceRenderer.cpp:724）はトラックFX/synthしか見ない＝中立トラックからのsendだけの曲はテール処理に入らない。テールループ（同:529）は**1ブロックでも-60dB未満なら終了**＝エコー間の無音で最初の反復前に打ち切られる。上限も5秒固定（同:436）。Phase 2 で契約ごと拡張する

## 実装計画

### Phase 1: 共有ディレイライン基盤 [AI🤖]

- [x] `Source/audio/DelayLine.h` を新設（整数サンプルタップのみ。Delay本体・Reverbのpre-delayで共用 — テンポ同期Delay・pre-delayは整数遅延で足りる。補間読みは不要と判明し仕様から削除、ログ > 方針変更 参照）
  - **容量の契約**: 必要最大長は「1/2音符 × BPM下限30 = 4秒」。サンプル数固定ではSR依存で不足するため、**prepare時のSRから「最大4秒＋補間マージン」を確保**する
  - 境界テスト: BPM 30・高SR（96k/192k）で1/2音符が収まり、読み位置が壊れない
- [x] Lo-fi Wow の移行判定: DelayLine で置き換えて `check-render-hashes.sh` が**ビット一致するときのみ**移行。一致しなければ TrackLofi は現状維持し、ログに理由を記録（無理に共通化しない）
- [x] `mise run test`＋ハッシュゲート green

### Phase 2: Delay DSP＋エンジン統合＋保存 [AI🤖]

- [x] `Source/shared/DelayParams.h`（Values: timeIndex(4値)/feedback(0..0.9)/tone/pingPong・defaults・normalized。CompParams.h の書式）
- [x] `Source/shared/ReverbParams.h` も先に定義（size/damp/width/preDelayMs/lowCutHz・A/Bの初期値2組）— 保存形式を1回のbumpで閉じるため
- [x] `Source/audio/BusDelay.{h,cpp}`: テンポ同期ディレイ（BPM atomic読み・Time/BPM/PreDelay等の**ディレイ長変更**はクリック回避の切替処理）・ループ内Tone LP・Ping-pong（モノ化→L/R交互）・full wet
- [x] **連続パラメータの平滑化方針を定義**: Feedback・Tone（後続のReverb Size/Damp/Width/LowCutも同方針）はブロック境界の急変でジッパーノイズが出るため、係数/ゲインの平滑化（または変化時の短いクロスフェード）を仕様化。テスト:「再生中に端から端までスイープしても不連続・NaN・発散がない」
- [x] RT統合: `PlaybackEngine` のバス加算前にFX処理を挿入（Limiterの型: スナップショットparams毎セグメント読み・再生開始/明示シークで状態リセット）
- [x] **バスMute/Gain 0中のDSP状態を仕様化**: 現行はバス出力無効で早期continue（PlaybackEngine.cpp:538 / BounceRenderer.cpp:758）するため、この内側にDSPを置くとミュート中に状態が凍結し解除時に古いエコーが復活する。**「DSPは毎ブロック進め、Mute/Gainは出力加算だけを止める」**方式にする（実DAWのミュート挙動＝テールは内部で減衰し続ける。バス3本の定常コストは許容）。テスト: mute→数秒待機→解除で古いテールが出ない
- [x] バウンス統合: `BounceRenderer` に専用インスタンス＋設定ロード
- [x] **バステール契約の拡張**（レビューP1）:
  - `resolveWantTail()` に「send > 0 のトラックがあり、かつ送り先バスが有効出力」を追加（mute/soloの再判定は不要 — `beginBounce()` が可聴判定済みのトラックだけをRequestに焼き込む既存設計のため、Request内のsendを見れば足りる）
  - 終了条件を「単発の無音ブロック」から「**最長の有効ディレイ周期ぶんの窓が連続で-60dB未満**」へ変更（エコー間の無音で打ち切らない）。feedback=0 でも最初の1タップ（time＋α）までは必ずレンダ（対数式は fb=0 を特別扱い）
  - 上限: fb90%の-60dB到達は 1/2音符・120BPMで約66秒（30BPMなら264秒）と現行5秒を大きく超えるため、**バスFX有効時の最大テール秒数を30秒に確定**する。**上限到達時は末尾に短いフェードアウト（0.5秒程度）を掛けて閉じる**（ぶつ切り・クリック回避。30BPM×fb90%のような極端設定では減衰しきる前に閉じるがそれを仕様とする。警告UIは作らない）。テスト閾値はこの仕様値から導出し、音決め定数に従属させない
  - テスト: 「中立オーディオトラック＋send のみの曲でテールに入る」「最初のエコーが尻切れしない」「上限で止まる」
- [x] project.json **v19**: バスFXブロック（delay＋reverb A/B）。読込時 `normalized()` 経由
  - **バスindex別の既定値テーブルを単一の真実の源にする**（現在は3バスとも同一の `unityParams()` 生成。Project.h:429）: A=短めRoom系・B=長めHall系の初期値は「新規作成」「v18以下の読込（キー欠損）」の両経路がこのテーブルから初期化する
  - 保存は**常に明示書き出し**（既定値なら省略、の最適化はしない。省略判定がバス別既定値に依存して壊れる余地を消す）
  - テスト3本: 新規作成でA/Bが各既定値・v18読込でA/Bが各既定値・保存→再読込で値が往復
- [x] `daw_tests`: インパルス応答でディレイタイム（n·time サンプル位置のピーク）・FB減衰（fb^n 一致）・Toneの高域減衰・Ping-pongのL/R交互を実測照合
- [x] **RT⇄バウンスの経路一致テスト**（レビューP2）: 同一fixture（send>0）を PlaybackEngine と BounceRenderer で処理し、本編＋テールを数値比較（Delay/Reverb A/Reverb B 各1本。パラメータ・バスindex・リセット条件の食い違いを検出する）
- [x] ハッシュゲート green（send全0の既存レンダはビット一致のまま）＋ fixture に send>0 のケースを追加

### Phase 3: Delay エディタUI [AI🤖]

- [x] `Source/ui/DelayEditorView.{h,cpp}`: Time 4値のボタン列＋Feedback/Toneノブ＋Ping-pongトグル（Comp/Satエディタの部品の流儀。値は atomic 直書き・undo対象外＝既存FXと同じ）
- [x] バススロット（Delay）クリック→`FxDetailView` で開く結線（Limiterエディタと同型）
- [x] ミキサーのバスストリップ側の表示整合（`SlotPill` 等）

### Phase 4: Reverb DSP＋エンジン統合 [AI🤖]

- [x] `Source/audio/BusReverb.{h,cpp}`: Low Cut（Biquad HP）→ Pre-delay（DelayLine）→ juce::dsp::Reverb（wet 100%固定）の直列
- [x] A/B 2インスタンスをRT・バウンスへ統合（Delayと同じ型）。初期値: A=短めRoom系・B=長めHall系（音決め定数はテスト閾値から独立させる）
- [x] 連続パラメータ（Size/Damp/Width/LowCut）はPhase 2の平滑化方針を適用。Pre-delay長の変更はディレイ長変更と同じクリック回避処理
- [x] バステール: Reverbは設定からの保守的な上限見積もり（実測減衰で妥当性確認）をテール長に合算（Phase 2の窓判定・最大テール秒数の契約に乗せる）
- [x] `daw_tests`: Pre-delayオフセットのサンプル一致・Low Cut応答（サイン振幅比）・テールが-60dBへ収束・Width=0でL≒R・再生中パラメータスイープでNaN/発散なし
- [x] RT⇄バウンス経路一致テストに Reverb A/B を追加（Phase 2 の枠組み）
- [x] ハッシュゲート green

### Phase 5: Reverb エディタUI [AI🤖]

- [x] `Source/ui/ReverbEditorView.{h,cpp}`: Size/Damp/Width/Pre-delay/Low Cut の5ノブ（A/B共通ビュー・対象バスで値が切り替わる）
- [x] バススロット（Reverb A/B）クリック→`FxDetailView` 結線

### Phase 6: 検証・確認環境・ドキュメント [AI🤖]

- [x] `scripts/seed-fx-test.sh` を拡張（send値＋バスFX設定入りの確認用プロジェクト。ボーカル相当＋ドラムで奥行きが聴き分けられる構成）
- [x] VERIFY.md にバスFXの確認手順を追記
- [x] fx-roadmap.md のバッチ4チェック更新・mixer-fx.md の「リバーブは種類セレクタを持つ」記述を更新・Reverb B の「使わなかったら消す」注記を残す

### 動作確認 [人間👨‍💻]

- [ ] send を上げて Delay/Reverb が意図どおり聴こえるか（Time切替・Feedbackの伸び・Toneの暗さ・Ping-pongの左右・Pre-delayの手前感・Low Cutのモヤ変化）
- [ ] ノブ・ボタンの操作感（再生中にドラッグしてもクリックノイズが出ないか）
- [ ] バウンス結果に残響の尻尾が切れずに入っているか（曲末直前の音の最初のエコーが残っているか）

## ログ

### 試したこと・わかったこと
- juce::Reverb はコンストラクタ既定が dry 0.4 で、`setParameters` は平滑**ターゲット**を変えるだけ。`setSampleRate` が平滑値をターゲットへスナップするので、**wet1/dry0 を設定してから setSampleRate** の順にしないと冒頭10msだけ原音が漏れる（Pre-delayのサンプル一致テストで発覚）
- `DelayLine` の read/write 順序契約: **read を write の前**に呼ぶと遅延がちょうどDサンプル。write後にreadすると1サンプル短くなる（BusReverbのpre-delayで踏んだ。ヘッダに契約を明記）
- レンダリングハッシュは全5本変化した — 既存fixtureが send>0 を含むため（素通し加算→full wet返しの**意図した仕様変化**）。新基準を再採取し、Debug/Release全経路のビット一致を確認済み。send全0の不変性は「同一ブロックは二重加算しない」テストとバスmuteテストで担保

### 試したこと・わかったこと（レビュー3巡目 2026-08-17）
- **Reverb単独テールの早期打ち切り**（P1・妥当）: 無音窓がDelayバスのみの考慮で、Reverbは「Pre-delay＋最初のコム（約25ms）」まで出力ゼロ＝曲末ぎりぎりの入力の残響が最初のテールブロックの無音で切られていた。窓を「バスごとの max（Delay1周期 / Pre-delay＋初期反射マージン0.05s）＋1ブロック」へ拡張し、曲末バースト＋Reverbのみの回帰テストを追加
- **上限フェード後のLimiter flush**（P2・妥当）: flushブロックがバスFXを再処理してリングに残ったエコーを未フェードで書いていた。上限到達時（tailClosedAtCap）はflush出力をゼロ保証（フェードの延長＝ゲイン0の続き）。capテストの末尾判定を1e-4へ厳格化
- **Low Cutの平滑化漏れ**（P2・妥当）: 係数を即時差し替えていた。TrackEqと同じ20msの周波数平滑を追加。スイープテストに隣接サンプル段差の上限（0.3）を追加したところ、**Delay側でPing-pong切替の実バグを検出**（下記）
- Ping-pong切替は出力ランプだけでは不十分だった: 書き込み式（トポロジ）の変更点がリング内容の不連続として**Dサンプル後**に出力へ現れる（実測 maxStep 0.457）。無音まで落とした時点でリングを組み直す方式へ変更（エコーは切替時に積み直し。修正後 0.221 = 自然なエコー立ち上がり相当）

### 方針変更
- DelayLine は**整数タップのみ**（補間なし）に確定。Lo-fi Wow の移行は**しない**: Wowは固定スタックリング＋4点エルミート補間＋連続可変遅延が要件で、ヒープ確保・整数タップのバスFX用DelayLineとは設計が別物。無理な共通化は抽象化のしすぎ（planの「一致しなければ現状維持」の判定を、置換実験の前に設計比較で確定）
- Reverb実装は `juce::dsp::Reverb` でなく `juce::Reverb` を直接使用（dsp版は同アルゴリズムの薄いラッパで、生API の方が smoothing/reset の制御が明確）
- アプリ実機のスモーク（seedプロジェクトのバウンス比較）は未実施: 起動中の dev 版が未保存変更あり（タイトル●）で quit を止めたため。実機確認は人間の動作確認に委ねる

### 方針変更
- 2026-08-16 plan段階レビューを反映: ①バステール契約の拡張（resolveWantTailへのsend考慮・窓ベースの無音判定・最大テール秒数の仕様化・fb=0特別扱い）②undoは既存FXに揃えて対象外へ変更③RT⇄バウンス経路一致テストとsend>0 fixtureを追加④連続パラメータの平滑化方針とスイープ耐性テストを追加
- 2026-08-16 plan段階レビュー2巡目を反映: ①最大テール30秒確定＋上限到達時は0.5秒フェードで閉じる（警告UIなし）②バスMute中もDSPは毎ブロック進め出力加算だけ止める（解除時の古いエコー復活を防ぐ）③DelayLine容量は「4秒＋補間マージン×prepare時SR」の契約＋30BPM/高SR境界テスト④Reverb A/B既定値はバスindex別テーブルを単一の真実の源にし保存は常に明示書き出し（省略最適化なし）
