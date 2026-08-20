# FXパネルのルック刷新（Illustrated hardware × 1FX 1色）

## 概要・やりたいこと

トラック周り（アレンジ画面・ヘッダー・LCD）のデザインは確定し、そのまま残す。一方で FX 周り（左の FX ラック・下部の FX 詳細パネル・ノブ）は「使えるが及第点・テンションが上がらない」状態なので、**FX 周りだけを別の文法で作り直す**。

他の DAW も「アレンジ画面＝無彩色の机」「プラグイン窓＝各社が好きな温度の楽器」と文法を分けており（Logic の純正プラグインも機種ごとに温度が違う）、host と plugin を両方自作している LaLa でも同じ分離をしてよい、というのが出発点。

決定した方向（モック 3 本で確定。scratchpad の `knob-mock.html` → `fx-panel-mock.html` → `fx-hardware-colors.html`）:

1. **質感: Illustrated hardware**（Soundtoys Decapitator の温度を借りる。配置は LaLa のまま、実機の配置は真似しない）
   - 地: わずかな質感（ヘアライン＋上からの照明の微グラデ）。現状より一段暗い
   - ノブ: 金属風の円盤（上下グラデ・縁ハイライト・内側キャップ・落ち影）＋白い針＋スカートに 11 本の目盛り。目盛りは値まで点灯
   - グラフ/メーター: 「メーター窓」として内側に沈める（黒地・外側に暗い縁・内側に薄い明るい縁・上端に落ち影）
   - ラベル: 大文字・トラッキング広め・やや暖色のグレー
2. **1FX 1色**: エフェクトごとに固有色を持ち、**使う場所を3箇所に限定する**（カーブ／メーターの線・ノブの点灯目盛り・ラックの LED）。地・ノブ本体・文字には使わない。面積を絞ることで 10 色まで増えても「おもちゃ箱」にならない
3. **ラック LED**: 左ラックのスロットピル（EQ/Comp/Sat/Lo-fi）の左端に小さな丸い LED。色＝その FX の固有色、ON で点灯（滲みあり）・OFF で消灯（暗い点）。下部で開いているスロットは縁が固有色で光る。パネルを開く前に「どの FX が効いているか」を色で読める

## 前提・わかっていること

### 配色テーブル（現行 8 ＋ 予備 3）

| FX | 色 | 由来 |
|---|---|---|
| EQ | `#7fa6d6` | 現 accent 青の系譜（EQ サムネイルのカーブ色と繋がる） |
| Comp | `#e8c27a` | アンバー（Decapitator 基準） |
| Sat | `#e0845a` | 銅（歪み＝熱） |
| Lo-fi | `#b08ad6` | 紫（テープ/VHS） |
| Reverb A | `#6fc3b8` | ティール（空間系は寒色） |
| Reverb B | `#5fa7d0` | 空色（A と明度差で区別） |
| Delay | `#8ccf7a` | 緑（SENDS の現行緑の系譜） |
| Limiter | `#e06a6a` | 赤（天井＝警告色・メーターのクリップ赤と同系） |
| 予備 1 | `#d98aa8` | ローズ |
| 予備 2 | `#d9d06a` | 黄 |
| 予備 3 | `#9fb8c8` | アイスグレー |

彩度は Decapitator のアンバーに揃えて一段落としてある。新 FX 追加時は予備から割り当て、テーブルに 1 行足すだけで済む設計にする。

### 現状のコード構造（調査済み）

- **FX を一意に指す ID が今は無い**: `FxSlots::Id` はトラック FX（eq/comp/ext/instrument/sat/lofi）だけで、バス/Master は `busLayout`/`masterLayout` が全てスロット番号 0 として構築する（`FxSlotLayout.h:103`）。色テーブルをスロット番号で引くと Reverb/Delay/Limiter が EQ と衝突する → 表示専用の **`enum class FxVisualKind`** `{ eq, comp, sat, lofi, reverbA, reverbB, delay, limiter, neutral }` を専用ヘッダ `Source/ui/FxVisualKind.h` に置き（`FxSlots::Id` は非スコープ enum なので同名列挙子の衝突を避けるため enum class 必須）、`FxSlots::Slot` に持たせる（`neutral` = Instrument・Ext など固有色なし＝LED なし）
- ノブ描画は `Source/ui/AppLookAndFeel.h` の `drawRotarySlider` 1 箇所（`logicKnob` プロパティ付きはパンノブで別関数 → **今回触らない**）。色は `rotarySliderFillColourId` / `rotarySliderOutlineColourId` を各 View が `Theme::accent` / `Theme::controlBg` で設定、SENDS は `Theme::sendArcGreen`
- 下部パネルの土台は `FxDetailView.h`（タイトル＋閉じるボタン＋差し替え式 body）。`paint` は `fillAll (Theme::timelineBg)` の後に **body 領域を `Theme::headerBg` の角丸で覆っている**（`FxDetailView.h:95`）ので、地を差し替えるならこの角丸を除去しないと大部分が旧地のまま残る。左ラック `FxEditorView::paint` も `Theme::timelineBg` 直 fill（`FxEditorView.cpp:371`）。body は `Comp/Sat/Lofi/Eq/Reverb/Delay/LimiterEditorView`（計 7 本・約 1900 行）。各 View がグラフ地を `Theme::timelineBg` で `fillRect` しておりスタイルが散っている
- 左ラックは `FxEditorView` → スロットは `SlotPill.h`（ON=`Theme::accent` 塗り・OFF=`Theme::controlBg`・開いているスロットは白枠 `setActiveOutline`）、SENDS は `SendRow.h`。スロット名は `FxSlotLayout.h` が真実の源
- 色は `Theme.h` 経由（16 進リテラル直書き禁止）。`juce::Colours::*` から inline 変数を初期化しない（GOTCHAS）
- `daw_tests` は UI の .cpp を含まない → 今回は描画のみなので回帰テストの対象外。確認は dev 版の目視＋スクショ

### 設計上の縛り（後から崩れやすいので明文化）

- 固有色を使ってよいのは **①カーブ/メーターの線 ②ノブの点灯目盛り ③ラック LED と選択枠** の 3 箇所のみ。①は「その FX の処理そのものを表す線」（伝達カーブ・GR・倍音・EQ カーブ）に限り、**数値の意味を色で伝えているメーター**（LUFS の緑黄赤・相関の正負・クリップ赤）は既存の意味色を維持する。地・ノブ本体・ラベル・数値・タイトルには使わない（モックのタイトル先頭の■は 4 箇所目になるので**採用しない**。どの FX かはラックの選択枠と線の色で足りる）
- パネルの地・ノブ・メーター窓のスタイルは **共通ヘルパーに集約**し、各 View から呼ぶだけにする（View ごとに描き方が散ると 1 つ直し漏れただけで「及第点」に戻る）
- トラック周り（TrackHeadersView・タイムライン・LCD・上部バー・ミキサーのフェーダー）は**触らない**。FX パネル上端の境界線 1 本で文法が切り替わる
- ミキサー（`MixerView`）のストリップにも `SlotPill` が載るので、LED はそこにも自動的に出る。問題なければそのまま、浮くようなら FX パネル限定フラグで切る

## 実装計画

### Phase 0: 変更前ベースラインの保存 [AI🤖]

実装後の比較対象が無いと「重くなった／崩れた」を判定できないので、先に同条件で採る。

- [x] 比較用プロジェクトを 1 つ決める（`--comp-demo` で GR が動くもの。無ければ seeder を足す）→ `2026-08-09-river`（VERIFY.md の `0-0-comp-test` はこのマシンに無い）
- [x] 全 FX エディタの自動スナップショットを一巡して保存: `--open <proj> --<fx>-editor --play --snapshot <scratchpad>/baseline/<fx>.png`（`--eq-editor` `--comp-editor` `--sat-editor` `--lofi-editor` `--reverb-editor 0/1` `--delay-editor` ＋ Limiter 用フック。無ければ追加）。**LaLa-dev は多重起動不可**で、起動中に次の `open --args` を打っても引数は渡らない（VERIFY.md）ので、**1 FX ごとに「ログで `debug.snapshot ok=1` を確認 → ウィンドウタイトルに `●`（未保存）が無いことを確認して quit → 次を起動」**のループにする（scratchpad にスクリプト化）
- [x] 同プロジェクト・同区間を再生中の CPU を記録（`ps -o %cpu -p $(pgrep -x LaLa-dev)` を 10 秒間隔で 5 回・中央値。`-f` だと他プロセスのコマンドライン引数に一致しうるので実行ファイル名完全一致の `-x`）。結果は plan のログに数値で残す
- [x] `mise run test` が green であることを確認（描画変更で壊れない前提の確認）

### Phase 1: 色テーブル＋ハードウェアノブ [AI🤖]

- [x] `Source/ui/FxVisualKind.h` に `enum class FxVisualKind` を新設し、`FxSlotLayout.h` の `Slot` 構造体に持たせる（trackLayout/busLayout/masterLayout の各構築箇所で設定。Instrument・Ext は `neutral`）
- [x] `Theme.h` に `fxHue (FxVisualKind)` の色テーブルを追加（`neutral` は中立グレーを返す）。予備 3 色も定義して未使用コメントを付ける
- [x] `Theme.h` にハードウェア質感用の色（パネル地の上下・ノブ円盤のグラデ 3 段・針・目盛り消灯色・メーター窓の地/縁・暖色ラベル）を役割名で追加
- [x] `AppLookAndFeel::drawRotarySlider` をハードウェアノブに差し替え（大: 目盛り 11 本＋円盤、SENDS 小径: 目盛りなし＋細アーク＋円盤の 2 モード。閾値は直径で自動判定）。点灯色は従来どおり `rotarySliderFillColourId` から取る（各 View が固有色を set する形にして LookAndFeel は色を知らない）
- [x] 各 EditorView・SendRow の `rotarySliderFillColourId` を固有色に差し替え（SENDS はバス固有色: Reverb A/B・Delay）
- [x] **`ReverbEditorView` は A/B で同じインスタンスを `setBus()` で切り替える**（`ReverbEditorView.cpp:63`）。色はコンストラクタで一度設定されるだけなので、`setBus()` 内で 5 ノブの色を `reverbA`/`reverbB` に再設定する（そうしないと先に開いた側の色が残る）
- [x] dev 版ビルド → Comp パネルと SENDS を撮影して寸法・質感を確認

### Phase 2 前の確認 [人間👨‍💻]

- [ ] dev 版でノブを触り、寸法比（円盤/目盛りの比率・針の太さ）と回し心地に違和感がないか確認。ここで微調整してから横展開に入る

### Phase 2: パネルの地とメーター窓（Comp で確立）[AI🤖]

- [x] 共通ヘルパーを新設（例: `Source/ui/HardwarePanelStyle.h`）: `paintPanelBackground (g, bounds)`（ヘアライン＋照明グラデ）、`paintMeterWindow (g, bounds, hue)`（黒地＋縁＋上端影＋固有色の薄いグリッド）、ラベル用フォント/色の取得
- [x] `FxDetailView` の地を `paintPanelBackground` に差し替え、**`bodyArea()` を `Theme::headerBg` で覆っている角丸を除去**（body は透過で地の質感を見せる。body 側で独自に fill している View は Phase 3 で外す）
- [x] `FxEditorView`（左ラック）の地も `paintPanelBackground` に差し替え。ラックと下部パネルで照明の向きが揃うよう、ヘルパーはコンポーネント座標でなく「パネル全体の矩形」を受けて描く
- [x] タイトルは **固定の FX 名だけ**大文字・トラッキング広めで描き、`— チャンネル名` はユーザー入力なので従来の `Fonts::forText`（CJK 補正）のまま別フォントで続けて描く（全体を大文字化すると自由入力名と CJK 補正が崩れる）。**Instrument スロットは第 1 要素もユーザー由来**（サンプラーはサンプル名が入る: `FxSlotLayout.h:95` → `MainComponent.cpp:1683` でそのままタイトルへ）なので、`FxDetailView::show` に kind を渡し、`neutral` のときは第 1 要素も大文字化せず `Fonts::forText` で描く
- [x] `CompEditorView` のグラフ・GR メーターを `paintMeterWindow` ＋固有色の線に置換。GR メーター右端に縦バー（現在値）を追加するかはここで判断（モックにはある）→ 既存の `meterArea` 縦バーがそのまま相当するので追加なし
- [x] ラベル（THRESHOLD 等）と数値のスタイルをヘルパー経由に統一
- [x] dev 版で Comp を撮影。トラック画面との境界の見え方を確認

### Phase 3: 他の View へ横展開 [AI🤖]

View × 要素の表で漏れを管理する（完了したら [x]）:

| View | 地/ラベル | メーター窓 | 線の色 | ノブ色 |
|---|---|---|---|---|
| Eq（ノブ無し・カーブ＋バンドハンドル） | [x] | [x] | [x] | N/A |
| Sat | [x] | [x] | [x] | [x] |
| Lofi | [x] | [x] | [x] | [x] |
| Reverb（ノブのみ・グラフ無し） | [x] | N/A | N/A | [x] |
| Delay（ノブ＋Time ボタン・グラフ無し） | [x] | N/A | N/A | [x] |
| Limiter | [x] | [x] | [x] | [x] |

N/A は該当要素が無いことを調査で確認済み（EQ: `EqEditorView.h` にスライダー無し、Reverb/Delay: `paint` はラベル描画のみ）。Delay の Time 4 値ボタン・Ping-pong トグルはボタン質感をラック側と揃える（固有色は使わない）。

- [x] 各 View の `Theme::timelineBg` 直 fill と独自の枠描画をヘルパー呼び出しに置換
- [x] Limiter の GR・Sat の倍音表示・EQ のカーブなど、線の色を固有色に。**ただし数値上の意味を色で表しているメーターは対象外**: Limiter の LUFS バー（緑→黄→赤）・相関バー（正＝緑／負＝赤）・True Peak・ターゲット基準線（`LimiterEditorView.cpp:178, 245`）は既存の意味色を維持する。Limiter 赤を使うのは GR 表示・ノブ・ラック LED だけ。同様に Comp の GR 以外に意味色のメーターが出てきた場合も同じ扱い
- [x] `InstrumentDetailView`（音源エディタ）は FX ではないが同じ土台に乗る。地だけ合わせ、固有色は付けない（中立色）ことを確認
- [x] 全 View を dev 版で一巡して撮影

### Phase 4: ラック LED と選択枠 [AI🤖]

- [x] `SlotPill::configure (..., FxVisualKind kind)` と **kind を渡す**（色だけ渡すと `neutral` の LED 無し判定ができない）。内部で `Theme::fxHue (kind)` と LED 有無を解決する。描画を「ON=青塗り」から「地は共通のボタン質感・左端に LED（ON: 固有色＋滲み / OFF: 暗い点）・文字は ON で白 / OFF で暖色グレー」に変更
- [x] `setActiveOutline` の白枠を固有色の枠に変更
- [x] hover 時の「電源｜エディタ」2 分割ハイライトが新しい見た目でも成立するか確認（LED 側が電源・右側がエディタ、が直感と合うか）
- [x] `SendRow` のバス名ピルにも同じ LED（send>0 で点灯）を適用
- [x] **`MixerStrip` にも `FxVisualKind` を渡す**（`MixerOverlay.h:26` のコンストラクタはバス FX 名しか受けず、Reverb A/B はどちらも "Reverb" なので名前から kind を推測すると同色になる。`busStrips` / `masterStrip` の構築箇所で reverbA/reverbB/delay/limiter を明示し、トラックストリップは `FxSlots` の Slot から取る）
- [ ] ミキサーのストリップに載る `SlotPill` の見え方を確認。浮くなら FX パネル限定にする（起動フックが無いため人間確認に回す）
- [x] `FxEditorView` の EQ サムネイルの枠をメーター窓スタイルに寄せ、**カーブ色も `Theme::eqThumbCurve` から EQ の固有色に**（`StripParts.h:63`。`eqThumbCurve` は他に使い手が無ければ削除）
- [x] `SlotPill` のミニ GR バー（`SlotPill.h:103`・現在アンバー固定）を固有色に。Master の Limiter も同じ部品を通るので、そのままだと Limiter だけアンバーになる
- [x] `SlotPill` は Instrument・Ext も通り、`enabled == nullptr` は ON 扱い（`SlotPill.h:61`）。**`FxVisualKind::neutral` のピルは LED を描かない**と定義し、Instrument が点灯しないことを確認

### 仕上げ [AI🤖]

- [x] `docs/design/ui-principles.md` に「机と機材の文法分離」「固有色は 3 箇所限定」「新 FX 追加時は予備色から」を追記
- [x] `VERIFY.md` に FX パネルの目視確認手順（各 FX を開いて撮影する一巡）を追記
- [x] GOTCHAS に該当があれば追記 → 該当なし（`DropShadow` は使わず楕円の重ね描き・ヘアラインはタイル画像で済ませたため CPU 増なし。`ps %cpu` の罠は VERIFY.md に記載）

### 自動確認 [AI🤖]

- [x] Phase 0 と同じ手順で全 FX のスナップショットと再生中 CPU を採り、baseline と並べて比較（スクショは scratchpad に before/after を横並びにした HTML を作って目視。CPU は中央値の差を plan のログに記録。有意に増えていれば `DropShadow` の都度生成や不要な repaint を疑う）
- [x] `mise run test` が green
- [x] ログに `debug.snapshot ok=1` が全 FX 分あること

### 動作確認 [人間👨‍💻]

- [ ] dev 版でノブを実際に回し、回し心地（感度・目盛りの点灯の追従）に違和感がないか
- [ ] 各 FX を渡り歩いて、色の見分け・トラック画面との境界・全体の好みを最終判断

## ログ
### 試したこと・わかったこと
- 2026-08-20 Phase 0 ベースライン（変更前・commit a06f8cb）: スナップショット 8 枚を scratchpad `baseline/` に保存（全て `debug.snapshot ok=1`、2200×1400）。撮影は再生 1.3 秒時点でクリップ開始前のため GR は 0 表示（レイアウト比較用）。**再生中 CPU（`--comp-editor --comp-demo --play`・起動 12 秒後から `top -l 7 -s 5` で 6 サンプル）: 53.2 / 58.4 / 58.5 / 58.7 / 59.3 / 60.2 → 中央値 58.6%**。`ps -o %cpu` は起動からの累積平均で単調増加するため瞬間値の比較には使えない（`top` を使う）。撮影ループは scratchpad `fx-snap.sh`（1 FX ごとに pkill → `open -g` → ログ `debug.snapshot.*ok=1` 待ち。`--comp-demo` で dirty になるので quit でなく pkill で終了）。`mise run test` green

### 試したこと・わかったこと（続き）
- Phase 1〜4 完了（2026-08-20）。変更後の再生中 CPU: 54.0 / 54.8 / 56.4 / 58.3 / 58.9 / 59.0 → 中央値 57.4%（変更前 58.6%・増加なし）。全 8 FX のスナップショット `debug.snapshot ok=1`、`mise run test` green。撮影スクリプトは `tools/fx-snapshots.sh` としてコミット対象に
- hover の2分割は「LED側（左）＝電源・右＝エディタ」で LED の意味と一致するのでそのまま

### 方針変更
- 2026-08-20 人間確認後: ラックのピルは金属風グラデをやめ **フラットな暗い面＋細い暗縁＋LED**（モック `rack-mock.html` 案B）に。SENDS の小ノブは **26px・フラット円盤・太アーク**（案4。行高 22→28）— 20px では円盤の描き込みが潰れて読めなかった
- EQ: ±24dB の点がグラフ最上端に乗り、見えている下半分（4px）を外すとタイトル行にクリックが逃げて「掴めない」問題を、縦軸両端の余白（`halfPlotHeight`）で解消（変更前からの潜在バグ）
- Phase 2: `FxDetailView` のタイトル先頭に固有色の■は置かない（縛りどおり）。hover 中のピルは地を固有色で塗らず、白 8% の被せだけにした
- Phase 4: Delay の Time ボタン・Ping-pong・EQ の HIGHPASS ピルは固有色を使わず `hwButtonOn/Off` の明度差で ON を示す
- Phase 1: `Theme::sendArcGreen` は Sends ノブがバス固有色になったため削除（`panArcGreen` は Pan ノブ用に残す）。ミキサーの `MixerStrip::setupKnob` は Pan ノブ（logicKnob）専用なので触らない
