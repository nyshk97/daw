# FXバッチ2: メーター＋Master Limiter（マスターセクション完成）

## 概要・やりたいこと

[fx-roadmap.md](../design/fx-roadmap.md) バッチ2。Masterに LUFS（short-term＋integrated）・相関・トゥルーピークの計測器と、lookahead brickwall の Master Limiter を実装し、マスターセクションを完成させる。

- 目的1: **ラウドネスバイアス潰し**。「大きい方が良く聴こえる」をLUFSで数値化できると、以降の全FX（バッチ3以降）のA/B比較が成立する
- 目的2: **配信安全の最終段**。Limiter＋トゥルーピーク測定で「この曲は -X LUFS / -Y dBTP」と言い切れる状態にする
- 耳セッションはほぼ不要のバッチ（メーターは読み方の道具、Limiterの耳確認はGain突っ込みの操作感のみ）

## 前提・わかっていること

2026-08-16 の /dig で確定:

| 論点 | 決定 |
|---|---|
| 表示場所 | Masterの `[Limiter]` スロットで開く下部詳細ビュー（FxDetailView）に全部同居: Limiterノブ3つ＋GR＋LUFS＋相関＋TP。ミキサーのMasterストリップは既存ピークメーターのまま変更なし |
| トゥルーピーク | **測定のみ** 4xオーバーサンプリング対応。LimiterのDSPはサンプルピーク天井（ceiling既定-1.0dBがサンプル間ピーク≈+0.3dBのマージンを兼ねる） |
| Limiter方式 | **自作 lookahead brickwall**（ディレイライン＋ゲインエンベロープ）。juce::dsp::Limiterはlookaheadなしで天井保証テストが書けないため不採用 |
| パラメータ | **Gain / Ceiling / Release** の3つ（Logicの Gain / Output Level / Release と同じ読み替え）。Lookaheadは固定値で隠す |
| 常在・既定値 | バイパスなし（mixer-fx.mdのバス/Master方針どおり）。既定 Gain 0dB / **Ceiling -1.0dB** / Release 中庸値。既存プロジェクトも-1dB超ピークの瞬間だけ叩かれる（＝配信で歪む危険箇所が守られる側の変化） |
| Integratedリセット | 再生開始で自動リセット・停止中は保持（maxSincePlayと同じ流儀）。リセットボタンは置かない。**計測は再生中常時稼働**（詳細ビューの表示有無と無関係。途中でビューを開いても再生開始からの値が出る）・ビューは表示中に読み出すだけ |
| バウンス計測 | 完了表示に「-9.8 LUFS / -0.8 dBTP」の1行を添える。計測は**BounceRendererワーカー内**で書き出し直後に行い、LUFS/TPをResultに載せてからsuccessにする（pollBounce()＝メッセージスレッドで数分のWAVを読んで4x TP計測するとUIが止まるため） |
| 相関メーター | -1〜+1の横バーのみ（Logic Correlation Meter同型）。マイナス域は警告色 |
| LUFSターゲットライン | **-14**（配信正規化の境界）と **-9**（hiphop音圧帯の入口）の2本を細線で |

コード調査で確定した実装アンカー:

- Master処理は **RT（`PlaybackEngine::process` 内1箇所・mixScratch→masterGain→出力）とバウンス（`BounceRenderer::mixBusesAndMaster`）の2箇所のみ**。Limiter統合はこの2点で済む
- 信号順は region-settings.md で確定済み: **mix → Limiter → 曲末フェード（SongFade）**。フェードをLimiterの後に置く（前だと突っ込み量が変わりフェード中に音色が変わる）
- 計測タップ位置 = **Limiter・フェード後、メトロノーム/カウントイン加算（post-master）の前**。書き出しファイルと同一の信号を測る＝クリック音が計測を汚さない
- `MeterScale` という型は実在しない。「一般化」の実体は `Meters::norm()`（StereoMeter.h）の -60〜0dBFS 決め打ち。LUFSバー・相関バーは別スケールの独自描画なので、**normの一般化は必要になった箇所だけ**行う（先回りしない）
- **AnalyzerTapは計測に使わない**。既存実装は表示用の欠落許容FIFO（容量約0.7秒・満杯時にサンプルを黙って捨てる = AnalyzerTap.h:26,112）で、UIが一度詰まるだけでIntegratedが不正になる。計測は**オーディオスレッド側で逐次処理**する: Limiter後にK-weighting・4x TP・相関を毎サンプル計算し、100ms刻みの**十分統計量**だけを事前確保リング（数時間分）でUIへ渡す。リングに載せるのは **①K-weighting後のch別二乗和＋サンプル数 ②相関用の ΣL²・ΣR²・ΣLR（平均減算なし定義を採用するためΣL/ΣRは不要） ③区間maxTP** で、**LUFSや相関の完成値は渡さない**（dB領域でLUFSを平均するのは誤り。400ms/3s窓は線形エネルギーを合算してからLUFSへ変換する）。万一リングがあふれたら計測無効フラグを立てる（黙って欠落させない）。ロードマップの「AnalyzerTap複数消費者対応」はこの設計では不要（バッチ完了時にfx-roadmap.mdへ反映）
- **リングの消費契約**（読み手の競合と世代混入を防ぐ）: 書き手はオーディオスレッドのみ・**読み手はMainComponentの常時30Hz Timerの一箇所のみ**。ビュー（LimiterEditorView）はリングを直接読まず、MainComponentが集約した表示値（MasterMeterFeed）を受け取って描くだけ。各統計エントリに**play-session世代**を付与し、リセット（integrated/TP蓄積のクリア）は**音声スレッドの再生開始エッジ（playing && !prevPlaying）のみ**で行う — シークやサイクルラップではリセットしない。UI側はエントリの世代が現在の再生セッションと一致するものだけ集計する
- **停止エッジの確定処理**: 停止（stoppedNow）時に、同一play-session世代のまま**100ms未満の部分統計を確定してリングへ出し**、TPのポリフェーズFIRにも必要な無音を流してflushしてから保持する（停止直前の区間に最大ピークがあっても取りこぼさない）。十分統計量はサンプル数を持つので部分区間はそのまま合算できる — **100msへのゼロ埋めはしない**。integratedの末尾の不完全な400ms窓は規格どおり完全な窓のみで構成する
- **K-weightingの所有者はMasterMeterSource（オーディオスレッド）の1箇所だけ**。LoudnessMeterはフィルタ係数・エネルギー→LUFS変換・ゲーティング集計の部品を提供する側で、RT経路でフィルタを二重適用しない（ファイル一括計測ではLoudnessMeter自身がフィルタも回す）
- UIの器は敷設済み: `FxSlotLayout.h` の `masterFxName()`＝"Limiter"・`FxEditorView::showMaster()`・`FxDetailView`（setBody差し替え式・高さ260px）。中身のエディタコンポーネントを作って載せるだけ
- DSPの流儀はEQ/Compと同じ自作（`TrackComp` の平滑化・snapTo・serial/timelineJumped・dry/wetの作法を踏襲）。ただしMaster常在なので needsActivePath / ON-OFFクロスフェードは不要
- project.json は `currentVersion = 16`（Project.h:375）。Limiter 3値の追加で **v17**
- 音を変える変更なので `scripts/check-render-hashes.sh` のベースライン更新が必要。**期待値は系統で分かれる**: bounce系ハッシュはflush契約で位置補償されるため素材が天井以下なら一致、**RTの hash-engine 系は遅延補償されないため2msシフトで意図的に変化する**（VERIFY.md:199）。この期待と違うパターンが出たら原因を特定してから更新する
- Compの表示値の流儀: `blockMaxGainReductionDb()` → atomic → MainComponentが30Hzで配布（MeterFeed）。LimiterのGRも同型でよいが、LUFS/相関はブロック統計では作れないためタップ経由
- undo: **対象外**。既存のEQ/Comp・フェーダーのパラメータ変更はundo対象外の流儀（MainComponent.cpp:266,282 でundo登録なし・`UndoStack::State` はMaster値を保持しない）。Limiterだけ対象にするとミキサー系の中で挙動が割れるため合わせる

DSP仕様の決め（planで固定する値）:

- **Lookahead: 2ms固定**（Logic既定と同程度。アタックを2msかけて目標GRへ滑らかに下げる＝波形の角を作らない）。RT再生では出力が2ms遅れるが、Master1本なので全トラック等遅延＝ズレなし。カウントイン/クリックはpost-master加算なので2msだけ相対が動くが聴感上無視できる
- **遅延契約**（L = lookahead 2ms相当のサンプル数。矛盾しやすいのでここに固定する）:
  - DSP単体の素通しビット一致テストは**Lサンプル位置合わせ**で比較する（同一サンプル位置では成立しない）
  - バウンスは**先頭Lサンプルを捨て、末尾にLサンプルの無音を流してflush**し、出力ファイルの長さ・頭出しを不変に保つ
  - 曲末フェード（SongFade）はLimiterの**後**なので、フェードゲインの評価位置を**遅延後の音声位置（segPos − L）**に合わせる（そのままだと音声より2ms早くフェードが掛かる）
  - segPos−L 評価にする以上、segPos = fadeEnd の時点で遅延出力はまだ無音ではない（入力 fadeEnd−L 時点の音が残っている）。よってRTも**フェード終端で即座に止めず、fadeEnd + L までLimiterへ無音を入力して遅延ラインをflush**し、その間も segPos−L のフェードゲインを掛け続ける（現状はfadeEndで出力処理を止める = PlaybackEngine.cpp:534 を変更）。バウンスも同じ契約（末尾Lの無音flush）で末尾を回収する
  - master経路を通る全書き出しモード（通常・サイクル・リージョン書き出し）で**出力長と先頭/末尾の整合をテスト**する（masterを通らないモードがあれば対象外であることを確認して明記）
- Gain範囲 0〜+12dB / Ceiling範囲 -6〜0dB（既定-1.0）/ Release範囲 5〜500ms・対数スライダー（既定 60ms 程度: 速いほど音圧は出るが低音が歪む、のトレードオフを動かせる範囲）
- ゲインエンベロープ: 検波はサンプルピーク（L/R max・ステレオリンク=両chに同量適用、Compと同じ理由で定位を動かさない）。GR目標 = max(0, peak_dB - ceiling_dB)。アタックはlookahead窓内の最小ゲイン包絡（未来2msの最悪値に向けてランプ）、リリースはワンポール
- **リセット条件はTrackCompのtimelineJumpedをそのまま流用しない**: 現行のtimelineJumpedは**サイクルラップを含む**（PlaybackEngine.cpp:210）ため、そこでディレイラインをリセットすると毎ループ先頭に2msの無音が入る。Limiterのディレイライン/GR包絡のリセットは**再生開始・明示シーク・SR変更のみ**に限定し、**サイクルラップでは継続**する（ラップは連続ストリームとして扱う。ラップとシークを区別する情報をエンジン側から渡す）
- **パラメータ操作中もbrickwallを保証する**: ①Gainの平滑適用は**検波・ディレイ格納より前**（検波が見る信号と出力される信号を同一にする）②Ceilingの実効値は音声と**Lサンプル整列**（GR計算時のceilingと出力時の天井判定を一致させる）＋最終段に整列後ceilingでの安全クランプ。**安全クランプもステレオリンク**（max(|L|,|R|)から安全ゲインを1個求めて両chへ同量適用。L/R個別のjlimitは大きい側だけ削れて定位が動くため禁止）。天井テストにはフルスケール入力でGain/Ceilingを**ブロック途中に急変**させるケースを含める
- **LUFS: ITU-R BS.1770-5 / EBU R128 準拠**（-4は廃止済み。K-weightingとゲーティングの定義は同一）。K-weighting（プリフィルタ2段＝shelf＋HPF、BiquadFilter.h流用）→ 400msブロック(75%オーバーラップ)。short-term=3s窓。integrated=絶対ゲート-70 LUFS→相対ゲート-10 LU の2段ゲーティング
- **TP: 4xオーバーサンプリング**（BS.1770-5 Annex 2のポリフェーズFIR）。表示は maxSincePlay と同じ「再生開始からの最大dBTP」数値
- **相関**: **平均減算なしの正規化相互相関** ΣLR/√(ΣL²·ΣR²)（音声はほぼゼロ平均でDC項は不要・業界の相関メーターと同じ定義。ΣL/ΣRは持たない）。数百ms窓で平滑。無音時は表示を0位置でグレーアウト（0除算と「無音=逆相」の誤読を避ける）
- **統計の取り口の区別**: LUFS用の二乗和は**K-weighting後**の信号から、相関・TP用は**加工前のpost-master信号**から取る（BS.1770のTPは重み付けなし。取り違えるとffmpeg照合が合わない）

検証方針（fx-roadmap.mdの「正しさは機械で検証」）:

- C++テスト（daw_tests）: **ITU-R BS.1770-5 / EBU Tech 3341 v4** 準拠で照合。Test 1 =「1000Hz正弦・ステレオ同相・各chピーク-23dBFS・20秒 → 期待値 -23.0 ±0.1 LUFS」を筆頭に、ゲーティングが効くケース（無音区間・レベル差シーケンス）とTPのサンプル間ピーク合成波形。加えて**EBU公開のテストWAV（seq-3341系）を最低1本**フィクスチャに使う（ダウンロードできない環境ならユーザーに取得を依頼）
- Limiter: 出力天井テスト（どんな入力でもサンプル値が ceiling を1サンプルも超えない）・GR実測一致・素通し条件（peak < ceiling かつ Gain=0）で**Lサンプル位置合わせのビット一致**
- ffmpeg `ebur128` フィルタとの数値照合スクリプト（integrated/TP。ffmpegは検証時のみ必要）
- `scripts/check-render-hashes.sh` ベースライン更新

## 実装計画

### 事前準備 [人間👨‍💻]

- [ ] なし（ffmpegの有無はAIが確認し、無ければ照合スクリプトの実行だけ依頼する）

### Phase 1: Limiter DSP＋単体テスト [AI🤖]

- [x] `Source/shared/LimiterParams.h` — 3パラメータの定義・範囲・既定値（EqParams/CompParamsの書式に合わせる）
- [x] `Source/audio/MasterLimiter.h/.cpp` — lookahead brickwall本体（ディレイライン・lookahead窓の最小包絡・release ワンポール・snapTo・serialの作法はTrackComp踏襲、ON/OFFなし）。リセットは「再生開始・明示シーク・SR変更」のみ（**サイクルラップでは継続** — timelineJumpedをそのまま使わない。上記「リセット条件」参照）。Gain平滑は検波・ディレイ格納より前・Ceiling実効値はLサンプル整列＋最終安全クランプ。UI向けに `blockMaxGainReductionDb()` を公開
- [x] daw_testsに追加: 出力天井テスト（インパルス列・矩形・フルスケール正弦で ceiling 超えゼロをサンプル単位確認・**Gain/Ceilingのブロック途中急変ケースを含む**）・定常正弦のGR実測一致・素通し条件のビット不変（Lサンプル位置合わせ）・リリース時定数の実測・左右レベル差のある入力でクランプ後もL/R比率が維持される（ステレオリンク検証）

### Phase 2: エンジン統合＋保存 [AI🤖]

- [x] `PlaybackEngine::process` の Masterゲイン適用後（SongFadeの前）に MasterLimiter を挿入。masterParamsにlimiter 3値のatomic＋GR atomic（limiterGrDb）を追加
- [x] `BounceRenderer::mixBusesAndMaster` に統合（開始時snapTo・先頭Lサンプル破棄＋末尾Lサンプル無音flushで出力長・頭出し不変＝上記「遅延契約」どおり）。無音判定（-60dB終了判定）はLimiter後の最終出力のまま
- [x] 遅延契約のテスト: 通常（ClippingProtectionのwrittenSamples・consistency系5本）・サイクル（testBounceCycleRange）・リージョン書き出し（同一renderPass経路）で出力長・先頭/末尾の整合を確認
- [x] リセット契約の回帰テスト: サイクルラップでLサンプルの無音が入らない（testPlaybackEngineCycleLoopの遅延付き期待列が兼務）・明示シークで旧位置のディレイ内容が漏れない（testEngineLimiterSeekReset）
- [x] project.json v17: limiter 3値の保存/読込（未保存は既定値・testLimiterParamsRoundtrip）。undoは対象外（既存EQ/Compと同じ流儀）
- [x] `scripts/check-render-hashes.sh` ベースライン更新済み。実測は期待どおり: **bounce系3本は完全一致**（flush契約で位置補償）・**hash-engine/hash-fx-engineの2本だけ変化**（2msシフト）

### Phase 3: 計測器（UI非依存・shared/） [AI🤖]

- [x] `Source/audio/MasterMeterSource.h`（DSP本体・オーディオスレッド側。ヘッダのみで完結）＋`Source/shared/MasterMeterStats.h`（リングのEntry・MasterMeterFeed等の受け渡し構造。audio/とshared/のスレッド境界方針どおりの分割）— オーディオスレッドで毎サンプル処理（K-weighting・4x TP・相関）し、100ms刻みの十分統計量（ch別二乗和＋サンプル数・ΣL²/ΣR²/ΣLR・区間maxTP・play-session世代）を事前確保リング（数時間分・あふれたら計測無効フラグ）でUIへ渡す共有構造。PlaybackEngineのLimiter・フェード後（クリック加算前）に接続し、**再生中は常時稼働**（ビュー表示と無関係。「再生開始からのIntegrated/TP」を成立させる）。リセットは音声スレッドの再生開始エッジのみ・停止エッジでは部分統計の確定＋TP FIRの無音flush（上記「停止エッジの確定処理」参照）
- [x] `Source/shared/LoudnessMeter.h/.cpp` — K-weightingフィルタ係数・エネルギー→LUFS変換・short-term/integrated（2段ゲーティング）の計算部品＋`MasterMeterAggregator`（リングの唯一の読み手＝MainComponentの30Hz Timerから）とファイル一括計測（measureFile・バウンス用）
- [x] `Source/shared/TruePeakDetector.h` — 4x ポリフェーズFIR（整数中心カーネル。半整数中心だと補間格子が1/8ずれてfs/4のピークを-0.2dB過小評価する実測知見をコメント化）
- [x] ~~`Source/shared/CorrelationMeter.h`~~ 相関は十分統計量（ΣL²/ΣR²/ΣLR）の集計に畳んだため独立ファイル不要（Aggregator内で算出）
- [x] daw_testsに追加: Tech 3341 v4 Test 1（44.1k/48k両方で -23.0 ±0.1）＋Test 3相当の相対ゲートケース・TPのサンプル間ピーク（fs/4・位相π/4 → 1.0検出）・相関の既知信号（同相/逆相）。EBU公開テストWAVは個別URLが無効でDL不可 → ユーザー取得依頼事項へ（合成規格ケース＋ffmpeg独立実装照合でカバー済み）
- [x] 計測契約の回帰テスト: 100ms未満で停止しても最大TPを保持する・play-session世代の旧Entryを集計に混ぜない・リングoverflow時に計測無効フラグが立ち黙って継続しない（回復は次セッション）
- [x] RT安全性: ヒープ確保ゼロの決定的テスト（thread_local計数のoperator new差し替え・192kHz/64サンプルブロックで1秒分ゼロ確認）＋Release専用ベンチマーク（報告のみ）
- [x] ffmpeg照合スクリプト `scripts/check-loudness.sh` — 実測: 正弦 ΔI=0.04/ΔTP=0.04・ピンクノイズ ΔI=0.05/ΔTP=0.13（許容 I±0.15/TP±0.5 内）

### Phase 4前の確認 [人間👨‍💻]

- [x] Limiter詳細ビューのHTMLモック確認 → **案A（左=操作/右=計測）で確定**（2026-08-16）

### Phase 4: Limiter詳細ビューUI [AI🤖]

- [x] `Source/ui/LimiterEditorView.h/.cpp` — 案Aの配置で実装。EQ/Compエディタと同じ土台（FxDetailViewのsetBody・30Hz配布・ダブルクリックで既定値復帰。undoは対象外＝既存流儀）
- [x] LUFS表示: short-termバー＋数値、integrated数値＋バー下の▲マーカー、-14/-9のターゲットライン
- [x] 相関バー（マイナス域で警告色・無音は非表示）・TP数値（再生開始からの最大・ceiling超えは赤/以下は緑）
- [x] MainComponent結線: `[Limiter]`スロットクリック→詳細ビュー（リングの読み手はtimerCallbackの一箇所・ビューは集約済みMasterMeterFeedを描くだけ）。ミキサー/FXパネルのMaster`[Limiter]`SlotPillにミニGRバー（limiterGrDb）。dev版検証フック `--limiter-editor` を追加し実機スナップショットで自走確認済み

### Phase 5: バウンス完了時の自動計測 [AI🤖]

- [x] バウンス完了表示（BounceOverlay）に「-X.X LUFS / -X.X dBTP」の行を追加。計測は**BounceRendererワーカー内**で書き出し直後にファイル一括計測し、Resultに載せてからsuccess（完了表示は数値が読めるよう約3秒に延長）
- [x] VERIFY.md 追記（Limiter/メーターの確認手順・check-loudness.sh・bounce.doneのlufs/tpDb）・fx-roadmap.md へ「MeterScale一般化・AnalyzerTap複数消費者対応は不要と判明」を反映（バッチ2のチェックは人間の操作感確認後に付ける）

### 動作確認 [人間👨‍💻]

- [x] Gainを突っ込んでいったときの音の潰れ方・Releaseの速い/遅いの聴感差（Limiterの耳確認はこれだけ）
- [x] LUFS/相関/TPの読みやすさ・ノブの操作感・dirty→保存→復元・既存プロジェクトの聴感（2026-08-16 人間確認OK）

## ログ

### 試したこと・わかったこと
- 実装後レビュー（3巡目）で2件修正: ①short-termの窓を「**末尾から遡った完全ブロック30個**」に統一（停止時の部分ブロックを窓に入れると判定=完全30個/計算=完全29個+部分1個とずれて3秒未満窓で表示していた）②あふれの汚染判定を `generation == tainted` から `generation <= tainted` へ（リング満杯中に複数世代がまとめて破棄されると、リングに残った古い世代が汚染世代と不一致で有効表示されていた。世代は単調増加なので「最後に破棄された世代以前は全部無効」で安全側に倒す）。どちらも回帰テスト追加済み
- 実装後レビュー（2巡目）で4件修正: ①フェード終端後もLimiterへ**無音を入力して回し続ける**構造へ変更（fadeEnd以降=入力無音・fadeEnd+L以降=加算スキップのみ。止めると凍結した古い2msがサイクル折り返し後の先頭へ漏れる — testEngineFadeCycleLimiterState で回帰固定。bounce側もfadeEnd以降の入力を無音化して対称に）②リングを**満杯時新規破棄型のSPSC有界キュー**へ変更（上書き型は1周遅れの読み手とスロットを共有してUB。droppedフラグ＋汚染世代atomicで通知）③K-weighting係数計算を `prepareToPlay`（非RT）へ移動・TP係数テーブルはコンストラクタで静的初期化（RTテストは再生開始エッジ込みで計測するよう修正）④short-termは**完全100msブロック×30（3秒窓）が揃うまで非表示**（揃う前はMomentary相当の短窓値になるため）
- Releaseベンチマーク実測: Limiter＋常時計測（96タップTP FIR込み）は 192kHz・64サンプルブロックで **0.66% realtime**（1秒分の音声を6.6msで処理）。ヒープ確保ゼロも決定的テストで確認
- TP FIRのタップ数は24/相（96タップ相当）に増量: 12/相だとクリック等の広帯域過渡でISPを最大0.9dB取りこぼしffmpegと照合が割れた（音楽的素材=正弦/ピンクノイズはΔTP≤0.04dB、単発インパルス系は原理的にフィルタ長依存で正解が無い → check-loudness.shに注記）
- TruePeakDetectorのwindowed-sincカーネルは**中心を整数（total/2）にする**こと。慣例の (total-1)/2＝半整数中心だと4相の補間点が1/8サンプルずれた格子になり、fs/4のサンプル間ピークを常に-0.2dB過小評価する（Pythonシミュレーションで特定・修正後 fs/4→0.997検出）
- ffmpeg ebur128 との照合実測: 正弦 ΔI=0.04 LU / ΔTP=0.04dB、ピンクノイズ ΔI=0.05 / ΔTP=0.13（scripts/check-loudness.sh。許容 I±0.15 / TP±0.5）
- EBU公開テストWAV（seq-3341系）は個別ファイルのURLが無効（HTMLが返る）でDL不可。フルセットzipは巨大なため見送り → ユーザーへ取得依頼（合成規格ケース＋ffmpeg照合で実質カバー済みなので必須ではない）
- DC矩形素材のバウンスTPはceilingより高く出る（立ち上がりエッジのギブス現象によるサンプル間オーバーシュート。テストの期待値をceiling一致→範囲(-1.05, 0)に修正）
- Phase 2エンジン統合後、既存のエンジン系テスト42件がfail。原因はRT出力のLサンプル遅延（遅延契約どおりの挙動）で、サンプル位置を直接検証する既存テストが軒並みずれた。hash-engine系だけでなく**assertion系のエンジンテスト全部が遅延契約の追従対象**だった（planの見積もり漏れ）。対応: 各テストをL補償（エンジン出力を+Lで読む/余分にL回す）に書き換え。エンジンvsバウンス比較はバウンス側が整列済みなのでエンジン側だけ+Lシフトして比較する
- BounceRendererClippingProtectionテストはLimiter導入で前提が変わった（ピーク>1.0がそもそも出ない＝スケーリングが発動しない）。テストを「Limiterがceilingで抑える・スケール不発動」の検証に書き換え

### 方針変更
（実装中に随時追記）
