# FXバッチ3: 歪み系（サチュレーション → Lo-fi）

## 概要・やりたいこと

[fx-roadmap.md](../design/fx-roadmap.md) バッチ3。耳テーマ「質感・倍音」。

- **サチュレーション** = 音量の頭を軽く潰して倍音を足す道具。細い音を太く・埋もれる音を前に出す（Logic では Phat FX / Tape 系に相当）。**3つ目のトラックFX**としてここで FxBase 抽象化・Paramsヘッダの書き方・「新トラックFX追加の型」を確立する
- **Lo-fi** = 独立した劣化処理の束（ピッチ揺れ／ローパス／ノイズ／ビット低減）。役割は「オリジナルドラムをレコード上モノの質感に寄せる糊」（毎曲使う想定）。型の量産1号
- 耳セッション（質感・倍音）は**このplanに含めない**。fx-roadmap.md の「バッチ完了後にまとめて耳セッション」の記述がリマインダーとして生きており、バッチ3のチェック更新時にそこで拾う

## 前提・わかっていること

2026-08-16 の /dig で確定:

| 論点 | 決定 |
|---|---|
| スロット構成 | **[EQ][Comp][Sat][Lo-fi][Ext] の固定5枠**に拡張（ミキサー・FXパネルとも）。キャラクター枠択一は「両方掛けたいドラム」で詰むため不採用 |
| 歪みの担当分け | 目的で分ける: サチュ=「太くする・前に出す」倍音付加、Lo-fi=「劣化させる」質感。ビット低減は劣化目的なので Lo-fi 側（おまけ扱いのまま） |
| 処理順（固定） | **EQ→タップ→Comp→Sat→Lo-fi**。コンプでレベルを揃えた後に歪ませるとドライブ量が暴れず、Lo-fi 最後尾ならノイズがコンプに持ち上げられない |
| Satノブ | **Drive / Mix** の2つ＋自動レベル補償（Driveを上げても音量ほぼ一定＝倍音の変化だけをA/Bできる。バッチ2でLUFSを作ったのと同じラウドネスバイアス潰し） |
| Satカーブ | **非対称ソフトクリップ1種固定**（偶数＋奇数倍音・暖かい方向。タイプ切替なし）。非対称はDCオフセットが出るため内部にDC除去フィルタを持つ |
| エイリアス対策 | **ADAA**（1次・アンチデリバティブ法）。追加バッファ・レイテンシなしでエイリアスを大幅低減。トラックFXにレイテンシを入れない＝トラック間タイミング補正機構が不要のまま、が最大の利点 |
| Satエディタ | **伝達カーブ＋倍音バー**（Driveから理論値の倍音構成を静的表示）。倍音バーは新規の見せ方なので実装前にHTMLモック確認 |
| Lo-fiノブ | **Wow / Tone / Noise / Crush の成分別4ノブ・Mixなし**（ピッチ揺れ成分とdryを混ぜるとコーラス＝別エフェクトになるため full wet） |
| Wow | Depth のみ・**レート 0.55Hz 固定**（レコード33⅓回転の偏心と同じ周期）＋わずかな不規則成分。**L/R同一の揺れ**（別々に揺らすと広がり系の別エフェクトになる） |
| Noise | ヒス＋クラックルを内部合成。**入力エンベロープ追従**（無音では鳴らない＝バウンスのテール有限・止め忘れ事故なし）。常時ノイズ床が欲しい場合はノイズループ素材をトラックに置く運用 |
| Crush | **ビット深度＋内部サンプルレート低減を1ノブ連動**（SP-1200/初期MPCの12bit/26kHz的質感。「あの音」はビットよりレート低減の折り返し成分が大きい） |
| FxBase | **共通状態機械を基底に抽出し、既存EQ/Compも載せ替える**。音の不変はバッチ1のビット一致ハッシュゲートで検証（まさにこのための基盤） |
| 初期値 | 両FXとも**中立スタート**（ONにしても音が変わらない状態から自分で上げる。EQ/Compと同じ作法） |
| undo | 対象外（既存のEQ/Comp/Limiterパラメータと同じ流儀） |

コード調査で確定した実装アンカー:

- 新FXの統合点はバッチ1で集約済み: `TrackFxChain.h` の **evaluateActivity / process / producesTail の3点**＋`TrackFx::Settings`（loadSettings）。6経路（モノ/ステレオ/MIDI × RT/バウンス）へはここを触れば全部効く
- FX実体の置き場: RT側 = `TrackParams`（PlaybackSnapshot.h:29。`rtEq`/`rtComp` の並びに `rtSat`/`rtLofi` を追加）、バウンス側 = `BounceRenderer::TrackRender`（BounceRenderer.h:33。開始時 snapTo）
- スロット番号は外部契約（onOpenSlot / fxDetailSlot / BottomPanelHistory が同じ番号を指す）。既存 0=EQ / 1=Comp / 2=Ext / 3=Instrument を**動かさず Sat=4 / Lo-fi=5 を追加**し、表示順（EQ, Comp, Sat, Lo-fi, Ext）は `FxSlots::panelOrder` / ミキサー投影側で制御。`mixerSlots` 3→5
- **ミキサーは現在、表示位置 i をそのまま意味IDとして使っている**（MixerOverlay.cpp:23,147。ID 0..2 が偶然一致していた）。単に枠数を5へ増やすと位置3が ID3=Instrument に化け、Lo-fi が落ち、クリックも誤ったエディタへ届く。`FxSlots` に **`mixerOrder` = {eq, comp, sat, lofi, ext} の表示位置→意味ID投影**を追加し、ミキサーの描画・クリック・電源トグルを全て投影経由に改める（panelOrder と同じ流儀）
- エディタの器は敷設済み: `FxDetailView`（setBody差し替え式）＋`FxEditorView::showTrack`。EQ/Comp/Limiterエディタと同じ土台に載せるだけ
- SlotPill は名前＋ON/OFF点灯のみ（Compと同等。EQのようなカーブサムネイル専用描画は作らない）
- `Project.h` currentVersion = **17** → 今回 **v18**（sat/lofi の保存フィールド追加。未保存フィールドは既定値で読む＝planの途中版で保存したプロジェクトも壊れない）
- ハッシュゲートの期待値: **FxBase載せ替え（Phase 1）は全ハッシュ完全一致**（音を変えないリファクタ）。新FXは既定OFF＝中立なので追加自体でもハッシュは動かない。バッチ3完了時に**sat/lofi ON のフィクスチャを check-render-hashes.sh に追加**して以降の回帰対象にする
- Params ヘッダは `CompParams.h` の型を踏襲: Values / defaults / normalized（NaN/inf防御＋クランプ）/ Params（個別atomic）/ store / load ＋「描画と音が同じ数式」の共有計算関数

DSP仕様の決め（planで固定する値）:

**FxBase（共通状態機械）**
- TrackEq/TrackComp が別々に手書きしている共通部を基底クラス `Source/audio/TrackFxBase.h` へ抽出: serial連続性判定（番兵初期値含む）・timelineJumped/SR変更時のリセット契約・ON/OFFの dry/wet クロスフェード（chainMix）・settled（needsActivePath の「OFF後クロスフェード完了まで active」）・snapTo の骨組み。派生側は「wet信号の計算」「パラメータ平滑の更新」「リセットすべき内部履歴」を実装する
- 抽象化の線引き: **状態機械だけを共通化**し、検波・平滑・係数計算などDSP本体は共有しない（EQとCompで本当に違う部分。抽象化のしすぎ注意）
- std::function / 仮想関数のオーディオパス使用は避ける設計（テンプレートまたは非仮想の合成）を優先。仮想呼び出しを使う場合はブロック単位1回まで（サンプル単位では呼ばない）

**サチュレーション（`Source/audio/TrackSaturator.h/.cpp`・`Source/shared/SatParams.h`）**
- 伝達カーブ: 非対称ソフトクリップ `f(x) = (tanh(g·x + b) − tanh(b)) / (g·sech²(b))`、バイアスは**ドライブ比例** `b = β·g`。この形は **g→0 で f(x)→x に連続収束**し（Drive を0から動かした瞬間に不連続が出ない）、小信号傾きは全 g で厳密に f'(0)=1。`β` が偶数倍音の源で、値は実装時にFFTで2次/3次バランスを見て決定し定数コメントに理由を書く
- Driveノブ: 0〜100%表示（既定0）→ `g = knob² · gmax`（2乗カーブで浅域の分解能を確保。gmax≈10 は実装時にFFT/耳で決定）。Mix 0〜100%（既定100%）。**中立判定は `drive==0 || mix==0`**（どちらも音響的に素通し。offline判定・RTの needsActivePath とも同条件）。knob==0 は高速パスで完全素通し、微小 g（< ε）は線形近似で数値安定化
- 自動レベル補償の実体: 小信号正規化は「小音量でゲイン1」の保証であって知覚音量一定の保証ではない（RMSは入力レベル・波形に依存する）。補償は**基準入力に対する出力補償ゲイン**として定義する: **-18dBFS正弦**を伝達カーブに通したRMSを64点求積で求め、その逆比を出力に掛ける（gの決定的な関数。drive変化時のみブロック頭で再計算・SmoothedValueで適用）。実素材では「ほぼ一定」と割り切る
- ADAA 1次: 不定積分は −tanh(b) の**線形項を含む** `F(x) = ( logcosh(g·x + b) / g − x·tanh(b) ) / (g·sech²(b))`（線形項を落とすとADAA出力がUIの伝達カーブと定数分ずれ、DC除去で隠す形になる）。`logcosh(z)` は素の log(cosh(z)) が大入力でオーバーフローするため安定形 `|z| + log1p(exp(−2|z|)) − log 2` で実装。差分 `(F(x[n]) − F(x[n−1])) / (x[n] − x[n−1])` が微小差分のときは中点評価 `f((x[n]+x[n−1])/2)` にフォールバック（0除算と桁落ち防御）。パラメータ平滑中は F・f とも現在サンプルの g/b で評価（曲線が動くことによる微小誤差は許容・クリックは出ない）
- DC除去: 出力段に一次HPF（〜5Hz）。ADAAのwetは実効約0.5サンプル遅れを持ち、Mix<100%でdryと混ぜるとナイキスト近傍がわずかに削れる（既知のトレードオフ。FFTで量を確認しコメント化）。**整数サンプルのレイテンシ申告・PDCは不要。ただし処理トラックと未処理トラック（複製トラック等）の間に高域の位相差が生じることは受容する**（「位相に影響しない」ではない）
- UI共有計算: `SatParams.h` に伝達カーブ関数を置き、DSP・エディタのカーブ描画・倍音バー（正弦をカーブに通して小FFT＝メッセージスレッドで計算）の全部が同じ式を呼ぶ

**Lo-fi（`Source/audio/TrackLofi.h/.cpp`・`Source/shared/LofiParams.h`）**
- 内部の成分順: **Wow → Crush → Tone → ノイズ加算**（Tone が Crush の折り返しの効かせ具合も握る＝実機サンプラーの出力フィルタと同じ構図）
- Wow: 変調ディレイ。LFO = 0.55Hz正弦＋ゆっくりした平滑乱数のドリフト（不規則成分）。深さ最大 ≈ ±1.5%（≈±26セント。実装時に耳で微調整可）。ディレイ中心はDepthに比例させ（中心 = 振幅＋数サンプルのマージン）、**Depth→0 でディレイ→ほぼ0に連続収束**。補間は3次エルミート（GOTCHAS の「補間カーネルは整数中心」の知見に注意）。L/Rは同一LFO値
- Wowの実時間遅延はトレードオフとして受容する: 中心=振幅の構造上、最大Depth（±1.5%）ではディレイが 0〜約8.7ms を振れ、**打点が平均約4.3ms後ろへ動く**（ピッチ揺れだけでなくキックとベースの噛み合わせに効く）。**PDCは作らず「Lo-fiの質感の一部」として許容**する（実用域の浅いDepthなら1〜2ms程度）。耳確認でNGなら最大Depthを下げて対処する（構造は変えない）
- Crush: サンプル&ホールドのレート低減（アンチエイリアスなし＝折り返しがキャラクター）＋量子化。ノブ0=中立（ネイティブレート・量子化なし）→ 中間 ≈ 12bit/26kHz（SP-1200帯）→ 最大 ≈ 8bit/6kHz
- Tone: biquad LPF（Q=0.707固定）。ノブ0=開放（20kHz・処理スキップ）→ 最大で〜500Hzまで閉じる。対数カーブ
- Noise: ヒス=フィルタ白色雑音＋クラックル=ランダムインパルス列（ポアソン的発生＋整形フィルタ）。乱数はRT安全なxorshift（rand()禁止）。入力エンベロープフォロワー（attack≈10ms / release≈300ms）でノイズ量をスケール。ノブ0=無音
- 中立判定: 全ノブ0 で isNeutral＝高速パス完全素通し。各成分もノブ0で個別スキップ（Wowのディレイ・Toneのフィルタを踏まない）
- `producesTail` 追加: Sat = enabled && drive>0 && mix>0（DC除去HPFのIIR履歴。EQと同族。mix==0 は音響的素通しなので対象外）、Lo-fi = **enabled && (wow>0 || crush>0 || tone>0 || noise>0)** — wow=ディレイライン・noise=エンベロープ減衰に加え、**Tone の biquad は IIR 履歴・Crush の S&H は最後の保持値**を持つため、Tone単独/Crush単独でもテール対象（漏れると範囲終端で余韻が切れる）

**パラメータ平滑化とリセット契約（新FX2種。テスト要件の前提になるためここで固定する）**
- Sat: Drive(g)・Mix・補償ゲインはサンプル単位 SmoothedValue（補償ゲインの再計算は drive 変化時のみブロック頭）
- Lo-fi Wow: Depth変更はディレイ読み取り位置の跳び＝クリックになるため、**ディレイ時間そのものをサンプル単位で平滑**する（跳びは短いピッチベンドに化ける＝Wowの性格と整合）。換算式: ピッチ偏差 p（比率）・LFO周波数 f に対しディレイ振幅 **A = p / (2π·f)** → ±1.5% @ 0.55Hz で **A ≈ 4.34ms**。バッファは 2A＋補間マージンを最大SR想定で事前確保（テストがこの式を独立に参照できる＝自己参照にならない）
- Lo-fi Tone: カットオフの SmoothedValue を **skip(numSamples) で進め、biquad係数はブロック頭で1回再計算**（TrackEqと同じ「ブロック単位の係数階段」。サンプル単位の係数再計算はしない）
- Lo-fi Noise: レベルは SmoothedValue。乱数は xorshift（整数演算のみ＝Debug/Release で決定的。rand()禁止）
- Lo-fi Crush: レート比・ビット深度は連続値として平滑（ブロック頭更新）。S&H の保持サンプルと位相はレート変更時も連続維持（リセットしない）
- リセット契約（timelineJumped・SR変更・snapTo で共通）: LFO位相=0・ドリフト/ノイズの乱数seed=固定値・S&H位相=0・ディレイ履歴/エンベロープフォロワー/DC除去HPF履歴=クリア。バウンスは snapTo で同一初期化＝**出力が決定的**（sat/lofi ONフィクスチャをハッシュゲートに載せる前提条件）

検証方針（fx-roadmap.md の「正しさは機械で検証」）:

- **FxBase載せ替え**: `scripts/check-render-hashes.sh` 全ハッシュ完全一致＋既存テスト全green
- **Sat（daw_tests）**: ①正弦入力のFFTで倍音次数照合 — 偶数倍音（2次）の存在（非対称の証明）・**THD（基本波比）が Drive で単調増加**（各倍音の絶対値でなく基本波比で判定） ②自動補償 — 補償の定義信号そのもの（-18dBFS正弦）で Drive 全域 **±0.1dB**。別レベル（-6dBFS正弦）は基準入力だけを補償する仕様上、絶対値保証も「補償なし比の改善」も保証できない（-18dBFS基準の固定補償は、より熱い入力の圧縮量まで直せず、誤差はβによって単調にもならない）ため、機械判定は**「NaN/infなし・Drive掃引の隣接点に不連続なし・|誤差| ≤ 6dB」のみ**とし、聴感の「ほぼ一定」は人間の操作感確認へ寄せる ③ADAA有効性 — 高域正弦（**f0=5kHz・振幅-6dBFS @ 48kHz・Driveノブ50%** を代表点に、倍音 k·f0 がナイキストを超える構成）で、**折り返し周波数 |k·f0 − m·fs| を列挙してそのビンを直接測り**、素朴実装比で**「各主要折り返しビンが悪化しない」かつ「合算エイリアス電力が 5dB 以上改善」**を合格基準とする（1次ADAAの実力に合わせた値。事前の数値評価で最悪ビン単独の改善は約6〜9dBであり、12dB級を求めるなら高次ADAA/オーバーサンプリングが必要＝設計判断が変わってしまう。補助として16xオーバーサンプリング参照との残差も記録） ④drive==0 と mix==0 それぞれのビット一致素通し ⑤DCオフセット — **0dBFS・100Hz正弦・最大Drive**の非対称出力で、**先頭1秒（DC除去HPFの整定区間）を捨てた後の1秒間**の出力平均が **-80dBFS 以下**
- **Lo-fi（daw_tests）**: ①Crush — 既定レートでの折り返し線が理論周波数（内部レートの鏡像）に立つ・量子化ステップの実測一致 ②Wow — 正弦入力の瞬時周波数偏差が A=p/(2πf) からの理論値に対し**相対誤差 ±10% 以内**・L/R偏差同一 ③Noise — 入力を無音にしてから**1秒以内に -60dBFS 以下**へ減衰・入力ありで追従 ④全ノブ0のビット一致素通し ⑤テール — wow単独・**tone単独・crush単独**・noise単独それぞれのバウンスでリングアウトが回収される ⑥Depth急変（0→最大を1サンプルで）時、正弦入力の出力の隣接サンプル差最大値が **Depth固定時の最大値の1.5倍以下**（クリックなしの数値定義）
- **RT安全**: 新FX2種のヒープ確保ゼロテスト（バッチ2の thread_local 計数 operator new 差し替え方式）
- 完了時に sat/lofi ON のレンダリングフィクスチャをハッシュゲートへ追加（以降のリファクタの回帰対象）

## 実装計画

### 事前準備 [人間👨‍💻]

- [ ] なし

### Phase 1: FxBase抽出（音を変えないリファクタ） [AI🤖]

- [x] `Source/audio/TrackFxBase.h` — 共通状態機械（serial連続性・リセット契約・dry/wetクロスフェード・settled・snapTo骨組み）を抽出（CRTP・仮想呼び出しなし・フックはブロック単位）
- [x] TrackEq / TrackComp を基底に載せ替え（DSP本体・検波・平滑はそのまま。状態機械だけ差し替え）
- [x] 検証: `scripts/check-render-hashes.sh` compare で **Debug/Release 全5ハッシュ完全一致**＋`mise run test` 全green（2026-08-16）

### Phase 2: サチュレーション DSP＋単体テスト [AI🤖]

- [x] `Source/shared/SatParams.h` — Drive/Mix の定義・範囲・normalized・atomic Params（CompParams.h の書式）＋伝達カーブ関数（DSP/UI共有。β=0.15・gmax=10 の決定理由をコメント化）
- [x] `Source/audio/TrackSaturator.h/.cpp` — 非対称ソフトクリップ（b=β·g・g→0連続収束形）＋ADAA 1次（線形項込みF・安定logcosh）＋基準入力RMSの出力補償ゲイン＋DC除去HPF＋Mixクロスフェード。平滑化は契約どおり（Drive/Mix/補償ゲイン=SmoothedValue）。FxBase に載せた（3例目＝型の確立）
- [x] daw_tests: 3本（testTrackSaturatorHarmonics / Aliasing / Transitions）追加・全green（2026-08-16）。閾値は検証方針の事前固定値のまま合格

### Phase 3: サチュレーション エンジン統合＋保存 [AI🤖]

- [x] `TrackParams` に satEnabled＋Sat::Params（rtSat 実体）を追加。`TrackFxChain.h` の Settings/loadSettings/evaluateActivity/process/producesTail に Sat を組み込み（処理順 EQ→タップ→Comp→Sat。PlaybackEngine 4箇所・BounceRenderer 8箇所を更新）
- [x] `BounceRenderer::TrackRender` に TrackSaturator を追加（開始時 snapTo・trackWantsTail の対象化・MainComponent のバウンス要求ビルドにも値詰め）
- [x] project.json v18: sat（enabled/drive/mix）の保存/読込（欠損は既定値 ON・中立）。testSatParamsRoundtrip＋testEngineSatBounceConsistency（RT/バウンス一致・EQ/Compの整合テストと同型）追加・全green
- [x] ハッシュゲート: 既定中立なので**Debug/Release全5ハッシュ不変**を確認（2026-08-16）。satEnabled の既定は **ON**（Drive0が中立を保証＝EQと同じ理屈。Compだけが「保証された中立なし」でOFF）

### Phase 4前の準備 [AI🤖]

- [x] 単一HTMLモックを作成・open・絶対パス提示（2026-08-16。コード調査でミキサーのピルは縦積み＝5枠化は幅でなく高さ+46pxの問題と判明し、モックは現行3枠との比較で提示）

### Phase 4前の確認 [人間👨‍💻]

- [x] Satエディタ = **案A（カーブ｜倍音バー｜ノブの3列）で確定**（2026-08-16）。Lo-fiエディタ・5枠縦積みは提示案どおり

### Phase 4: サチュレーション UI＋5枠化 [AI🤖]

- [x] `FxSlotLayout.h` — Sat=4 / Lo-fi=5 を追加・mixerSlots 5・**`mixerOrder` 投影（表示位置→意味ID）＋ mixerPositionOf（GRバー逆引き）を新設**し、Layout を count 方式→used フラグ方式へ変更（IDが非連続になったため）。MixerOverlay の描画・クリック・電源トグルを投影経由へ改修。FxEditorView は numSlots の上限比較を isValidSlot（名前の有無）へ置換（履歴復元の判定も同）
- [x] 投影の回帰テスト testFxSlotProjection: mixerOrder の表示順とID対応・GRバー逆引き・enabled ポインタが正しい atomic を指すこと・パネル表示順（オーディオ/MIDI別）
- [x] ミキサーストリップ / FXパネルの5枠表示（SlotPill は名前＋ON/OFF点灯のみ。Lo-fi は Phase 6 と同時実装のため placeholder 期間なし＝方針変更参照）
- [x] `Source/ui/SatEditorView.h/.cpp` — 案Aで実装。伝達カーブ（Sat::transfer 共有式）＋倍音バー（-18dBFS正弦を同カーブに通した理論値・512点相関・drive変更時のみ再計算）＋Drive/Mixノブ（ダブルクリック既定値復帰・undo対象外）
- [x] dev版検証フック `--sat-editor` / `--lofi-editor` を追加（スクショ自走確認は起動中インスタンスが未保存状態のため保留 → 動作確認へ委譲。ログ参照）

### Phase 5: Lo-fi DSP＋単体テスト [AI🤖]

- [x] `Source/shared/LofiParams.h` — 定義・範囲・normalized・atomic Params＋ノブ→物理量マッピング（toneCutoffHz/crushBits/crushRateRatio/wowDepthRatio/quantize。UI表示と共有）
- [x] `Source/audio/TrackLofi.h/.cpp` — Wow（0.55Hz＋控えめドリフト±0.05rad・4点エルミート・d<2は線形補間でゼロへ連続収束・L/R同揺れ・リング2048事前確保）→ Crush（S&H＋量子化・位相連続）→ Tone（biquad LPF・スキップ→処理遷移で履歴リセット）→ Noise（ヒス＋クラックル・xorshift・エンベロープ追従・モノ合成を両chへ）。FxBase の型で量産
- [x] daw_tests: testTrackLofiComponents / Transitions（Crush折り返し15kHz・量子化一致・Wow偏差±10%・Noise減衰/追従・Depth急変1.5倍・決定性ビット一致・全ノブ0素通し・ヒープゼロ）全green

### Phase 6: Lo-fi エンジン統合＋UI [AI🤖]

- [x] TrackParams / TrackFxChain / BounceRenderer への組み込み（処理順末尾・producesTail = !isNeutral）。テール回収テスト testBounceLofiTail（**wow/tone/noise/crush 各単独**で wantTail・テール書き出し・RT一致を確認）
- [x] project.json v18 に lofi（enabled＋4値）を追加。testLofiParamsRoundtrip 全green
- [x] `Source/ui/LofiEditorView.h/.cpp` — Toneカーブ（RBJ設計式で描画・SR追従）＋内部並びラベル＋4ノブ（Wow=±セント・Tone=Hz・Noise=%・Crush=bit表示）
- [x] dev版検証フック追加済み（スクショ自走確認は保留 → 動作確認へ委譲）

### Phase 7: 検証まとめ・記録 [AI🤖]

- [x] FXハッシュフィクスチャ（testTrackFxRegressionHash）の3トラックへ Sat/Lo-fi を配分し全6経路で新FXを回す。バウンスハッシュ不変で発覚したコピー漏れは `TrackRender::loadFxFrom()` に一元化（ログ参照）。新ベースライン capture 済み
- [x] `mise run test` 全green・ハッシュゲート最終確認（FXなし系2本は旧ベースラインと不変を確認してから capture）
- [x] VERIFY.md 追記（--sat-editor / --lofi-editor）・GOTCHAS.md 追記（ADAA double必須・線形域の中点平均・loadFxFrom一元化）。fx-roadmap.md のチェックは人間の操作感確認後に更新

### 動作確認 [人間👨‍💻]

- [x] Satの操作感: Driveを上げたときの音の変化・Mixの効き・伝達カーブ/倍音バーの読みやすさ（2026-08-16 人間確認OK）
- [x] Lo-fiの操作感: 4ノブそれぞれの効き・「レコードに寄る」か（2026-08-16 人間確認OK。確認用プロジェクトは scripts/seed-fx-test.sh）
- [x] Wowの打点遅れ: 許容範囲と確認（最大Depthは±1.5%のまま）
- [x] dirty→保存→復元・既存プロジェクトの聴感（2026-08-16 人間確認OK）
- [ ] （耳セッションは別途 — fx-roadmap.md の「バッチ完了後にまとめて」で実施）

## ログ

### 試したこと・わかったこと
- β=0.15・gmax=10 に確定（数値評価: knob50%で H2≈-25dBc/H3≈-46dBc の偶数優勢・-6dBFS補償誤差 -2.5〜0dB・ADAA合算改善6.9dB）
- **ADAAの伝達関数内部はdouble必須**: float の logcosh は `az + log1p(…) − log2` の桁落ちで絶対誤差 ~1e-7 が残り、小さい g では invDenom=1/(g·sech²b)（最大1e4）が増幅して Drive が0付近を平滑通過する瞬間に±189の巨大スパイクが出た（Drive急変テストで検出）。transfer/transferAD の内部を double 化して解消（maxJump 0.107 に）
- 線形域（g<ε）のADAAは中点平均（半サンプルLPF）になり高速パスの完全素通しと高域差が残るため、線形域は恒等 wet=dry に分岐

- FXハッシュフィクスチャ拡張時、バウンス側ハッシュだけ不変のままで**テストの TrackRender 手書きコピーが Sat/Lo-fi を落としていた**ことが発覚（コピー漏れ事故の実例）。`TrackRender::loadFxFrom()` に一元化して本番2箇所＋テスト6箇所を置換（GOTCHAS.md へ追記済み）
- レビュー指摘（2件・両方反映）: ①Sat の Drive→0 整定で「DCブロッカを通った出力→raw入力」へ不連続切替 → **neutralFade**（Drive目標0でwet経路をdryへ平滑フェード。fade=0で出力がdryとビット一致してから高速パスへ移る）②Lo-fi の Tone 0境界（開放20kHz LPF≠恒等）の段差 → **toneMix** で dry↔LPF をクロスフェード（0収束で完全スキップ＝素通しとビット一致）。両方に境界テスト追加（整定ブロック末尾のビット一致＋高速パス境界込みの跳躍チェック）。toneMix の演算変更（tm=1 でも `wet+(filtered−wet)` は `filtered` と非ビット一致）で FX系ハッシュが変化 → ベースライン再capture

### 方針変更
- Lo-fi スロットの「Phase 6 まで placeholder」は廃止: Phase 4〜6 のUIを連続実装したため placeholder 期間が存在せず、最初から動作するピルで実装した
- UIのスクショ自走確認は保留: 検証時に LaLa-dev の起動中インスタンス（ユーザーの実セッション・ウィンドウタイトルに未保存●）があり、シングルインスタンス制約で新バイナリを起動できないため quit を送らず中止。`--sat-editor` / `--lofi-editor` フックは実装済みで、dev版を閉じた後にいつでも自走確認できる
