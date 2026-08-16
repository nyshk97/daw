# FXバッチ1: 基盤整備（音を変えないリファクタ）

## 概要・やりたいこと

[fx-roadmap.md](../design/fx-roadmap.md) バッチ1。サチュレーション以降の新トラックFXを足す前に、FX処理の重複を畳んで「コピー箇所と事故リスク」を減らす。**出力音は1サンプルも変えない**リファクタで、検証はバウンス出力のビット一致＋既存テストで行う。

対象は4点:

1. エンジンの6経路コピー（モノ/ステレオ/MIDI × RT/バウンス）を `processTrackFx()` 1関数に集約
2. Biquad構造体の共通ヘッダ抽出（TrackEq/TrackCompの完全重複を解消）
3. スロット構成の二重定義（FxEditorView / MixerOverlay）の一元化
4. 残タスク回収: EQサムネイル（StripParts.h）をプレースホルダから実カーブ描画へ

**やらないこと**: FxBase（DSP共通状態機械）の抽象化。TrackEq/TrackComp に同型の骨格（`resetSmootherRates` / `settled` / serial番兵 / `chainMix`）があるのは把握済みだが、3例目=サチュレーションの実物を見てから切る（抽象化のしすぎ注意）。

## 前提・わかっていること

調査結果（2026-08-16、コード実地確認済み）:

### 6経路の現在地

| # | 経路 | 場所 |
|---|---|---|
| 1 | RT × モノ | `PlaybackEngine.cpp:315-411`（FX適用 376-411）`processSegment` |
| 2 | RT × ステレオ | `PlaybackEngine.cpp:414-533`（FX適用 481-533）同関数 else 節 |
| 3 | RT × MIDI | `PlaybackEngine.cpp:846-911`（FX適用 854-889）`renderMidiTracks` |
| 4 | バウンス × モノ/ステレオ | `BounceRenderer.cpp:305-398`（FX適用 374-397）`renderPass` |
| 5 | バウンス × MIDI | `BounceRenderer.cpp:640-670` `renderSynthInto` |
| 6 | バウンス × EQテール | `BounceRenderer.cpp:439-481` `renderPass` テールループ |

全経路で処理順は同一仕様: 入力合算 → EQ → [RTのみ analyzerTap] → Comp → トラックgain → pan → メーター → send → mix。「gainをFX後段へ移す」仕掛け（`accumGain`/`postGain` ⇔ `clipTrackGain`/`outGain`）も同型の手書き。

**経路間の差分（共通化時の要注意点）**:
- モノはFXを1ch（`right=nullptr`）でpan前に掛け `Pan::monoGains`、ステレオは2chで `Pan::stereoGains`。ステレオは `prePanFx`（全クリップがステレオのときだけ）panを後段へ移す
- active判定: RTは `needsActivePath`（OFF後のフェードアウト＝`settled` 状態を見る）、バウンスは素の `enabled && !isNeutral`
- RT固有の付帯物5点: `analyzerTap` / `canProcess` 縮退フォールバック / `compGrDb`・`compDetectorPeak` の書き戻し / `timelineJumped`+serial / `tapOnly`
- MIDI経路: シンセ出力にインプレース、`srcR = jmin(1, numChannels-1)`、panは常にバランス型、RT側は `timelineJumped` 常に false（余韻を切らない）、ミュート時もレンダリングして gain 0（オーディオ経路は skip）
- **経路#6（EQテール）の条件は `eqEnabled && !isNeutral` のみで Comp を見ていない**。ただし Comp は無音入力から音を生成しないため、これは実害のある欠落ではない。統合時は `producesTail` 契約として明示する（Phase 4 参照）

**ビット一致契約**: `PlaybackEngine.cpp:317-324` / `BounceRenderer.cpp:305-308` にコメントで明文化。高速パス（activeFx=false）は既存式を完全素通しで維持する。

**ただし既存の守りは不十分**（レビューで判明・2026-08-16）:
- `testMonoRenderRegressionHash`（`Tests/TestsMain.cpp:8301`）は FNV-1a ハッシュを **stdout に出すだけで assertion がない**（期待値をハードコードしない設計＝同一環境での変更前後の目視比較用）。しかも fixture は EQ 中立・Comp OFF なので **Phase 4 で抽出する active FX 経路を通らない**
- RT/バウンス一致テスト群の許容誤差は 1e-4 でビット一致ではない
- → **リファクタ着手前（Phase 0）**に FX ON の全経路ハッシュを採取し、Phase ごと・経路差し替えごとに自動比較する。Phase 1（Biquad抽出）もDSPに触るため、基準はそれより前に採る

**GOTCHAS.md:515-521**: 非線形FX（Comp）の掛け算位置ズレは track-comp 実装で実際に3経路で起きた。RT/バウンス一致テストは両側が同じ誤りだと通るため、`testEngineCompPrePanDetection` 型の仕様テストは共通化後も残す。

### Biquad重複

- `TrackEq.h:55-69` と `TrackComp.h:62-76` の `Biquad` 構造体は**1文字たがわず完全一致**（Direct Form II transposed）
- 周辺も重複: 「`ArrayCoefficients::make*` を a0 正規化して詰める」5行（`TrackEq.cpp:68-75` / `TrackComp.cpp:43-56`）、無名namespaceの `snapToZero`（`TrackEq.cpp:13-17` / `TrackComp.cpp:14-18`）

### スロット構成の二重定義

- `FxEditorView.cpp:252-291`（`rebind`）と `MixerOverlay.cpp:139-147`（`MixerStrip::bind`）が同じ `[EQ][Comp][Ext]` を別々に `configure`
- スロット番号 0=EQ/1=Comp/2=Ext は**暗黙の番号契約**（`FxEditorView.h:52-54` にコメント）で、`onOpenSlot(0)=EQ` が `MixerOverlay.cpp:258` / `FxEditorView.cpp:433` に別々にハードコード。GR表示のピルindex `slotPills[1]` も両方に散在
- バス/Masterのスロット名（Limiter/Delay/Reverb）: FxEditorView は自前で組み立て、MixerStrip はコンストラクタ引数で受け取る＝命名規則が2箇所
- 共有されているのは描画部品 `SlotPill` のみ。構成データは非共有

### EQサムネイル

- プレースホルダ: `StripParts.h:12-28` `drawEqThumbnail`（水平直線のみ・パラメータを受け取らないシグネチャ）。呼び出しは `FxEditorView.cpp:405-411` / `MixerOverlay.cpp:173-174` の2箇所
- 実カーブ描画は `EqEditorView.cpp:254-318` に既存: `IIR::Coefficients::make*` → `getMagnitudeForFrequencyArray`（200点・対数周波数）→ Path化。この計算部を純関数に切り出せば再利用できる
- `Coefficients::make*` は内部で `new` するためオーディオスレッド不可 → UI描画専用なら問題なし

### テスト・ビルド

- `daw_tests` は `Tests/TestsMain.cpp` 1本 + `Source/audio/` `Source/shared/` の .cpp のみ。**`Source/ui/` は含まれない** → UI側の共通化はヘッダオンリー or UIに閉じる。EQカーブ計算を `Source/shared/` ヘッダオンリーに切り出せばテスト可能になる
- 守りのテスト（既存）: `testMonoRenderRegressionHash`（ハッシュを stdout に出すのみ・FX 経路は不通過＝上記のとおり単体ではゲートにならない）・`testEngineEqBounceConsistency` / `testEngineCompBounceConsistency` / `testBounceEqTail` / `testEngineBounceStereoConsistency`（RT/バウンス一致・許容誤差 1e-4）・`testEngineCompPrePanDetection`（仕様固定）・`testTrackEqResponse` / `testTrackCompDynamics`（DSP単体）

## 実装計画

Phase間は独立性が高い順に「小さく安全なもの → 大物」で進める。各Phase完了ごとに `mise run test` を通す。

### Phase 0: ビット一致ゲートの整備（リファクタ着手前） [AI🤖]

DSPに触る最初の変更（Phase 1 の Biquad 抽出）より**前**に基準を採取する。後で採ると「変更後の音」が基準になり、それ以前の破壊を検出できない。

- [x] FX ON の回帰ハッシュテストを追加: `testTrackFxRegressionHash`（Tests/TestsMain.cpp）。EQ 非中立・Comp ON・pan振り・send ありで**6経路すべてを通す**（RTモノ/RTステレオ/RT MIDI/バウンス本編/バウンスMIDI/バウンスEQテール）。MIDI はサンプラー音源。トラック別の「FXが実際に効いている」ガード付き（空一致防止）
- [x] FNV-1a を stdout へ出す（`hash-fx-engine` / `hash-fx-bounce` / `hash-fx-project-bounce` の3行）
- [x] 比較スクリプト `scripts/check-render-hashes.sh`（capture/compare）: 出力をファイル保存→終了コード確認→抽出→diff。ハッシュ行の本数・名前の完全一致も検査。ベースラインは `.render-hash-baseline/`（gitignore済み・build/外＝クリーンビルドで消えない）
- [x] Debug と Release の両方でベースライン採取済み（2026-08-16。両構成でハッシュが一致していた）。Release 実行手段として mise タスク `test:release` を追加
- [x] 実プロジェクト相当の基準: `hash-fx-project-bounce`（Project::save→load→バウンス）としてハッシュテスト内に組み込み。fixture 自体が冪等な seeder（毎回同一プロジェクトを再生成）

### Phase 1: Biquad共通ヘッダ抽出 [AI🤖]

- [x] `Source/audio/BiquadFilter.h`（新規・ヘッダオンリー）に `Biquad` 構造体を移動。`snapToZero`（static メンバ化）と `setCoefficients`（a0正規化）も同居
- [x] `TrackEq.h/.cpp` と `TrackComp.h/.cpp` を新ヘッダ参照に書き換え、重複定義を削除。検波HPF用途のコメントは `TrackComp::hpf` メンバに残した
- [x] 全テスト green ＋ ハッシュ比較 Debug/Release とも全5ハッシュビット一致（2026-08-16）

### Phase 2: EQサムネイル実カーブ化 [AI🤖]

- [x] カーブ計算を `Source/ui/EqCurve.h`（`EqCurve::response`・SR引数つき）へ切り出し、EqEditorView を書き換え
- [x] `StripParts::drawEqThumbnail` に EQ値・有効フラグ・SRを追加し実カーブを縮小描画（48点・縦スケールはエディタと同じ±maxGainDb）。OFF・中立時は旧プレースホルダと同じフラット線
- [x] 呼び出し2箇所（FxEditorView / MixerStrip）に値とSR（`getSampleRate` コールバック・未確定は48k）を配線
- [x] 再描画配線: `eqDetail.onEdited` で FXパネル＋該当トラックの MixerStrip のサムネイル領域だけを repaint（`repaintEqThumbnail` / `repaintTrackEqThumbnail`）。電源ピルのトグルでも自ストリップのサムネイルを repaint
- [x] 陳腐化コメントを削除（drawEqThumbnail 書き換えに含む）
- [x] dev版で目視確認済み（検証フック `--open --eq-editor --snapshot`。FXパネルのサムネイルがエディタ大カーブと同形＝HPロールオフ・500Hzブースト・2kディップ・シェルフ上昇）

### Phase 3: スロット構成の一元化 [AI🤖]

- [x] `Source/ui/FxSlotLayout.h`（新規・ヘッダオンリー）を作成。意味ID `FxSlots::Id`（eq=0/comp=1/ext=2/instrument=3）・`trackBaseLayout`（ミキサーの3枠投影）・`trackPanelLayout`（FXパネル・MIDIはInstrument追加）・`busLayout`/`masterLayout`・`panelOrder`（Instrument先頭の表示順）・`grSlot`・`busFxName`/`masterFxName` を集約。gmInstrumentName もここへ移動
- [x] `FxEditorView::rebind` / `MixerStrip::bind` を新定義参照に書き換え。`onOpenSlot(0)` / GRピル `slotPills[1]` のハードコードを `FxSlots::eq` / `FxSlots::grSlot` に置換。`FxEditorView::maxSlots`/`instrumentSlot` も FxSlots 参照
- [x] MixerStrip にInstrumentスロットを表示しない現状は維持（`mixerSlots=3` の投影規則として明示）
- [x] ビルド＋dev版スクショで FXパネルのスロット表示（EQ/Comp点灯・Extグレー）と EQサムネイルクリック遷移相当（--eq-editor で fxdetail.open fx=EQ）を確認

### Phase 4: エンジン6経路の `processTrackFx()` 集約 [AI🤖]

一番の大物。ハッシュゲートは Phase 0 で整備済みの前提で、経路を1本ずつ差し替える。

- [x] **4a: 共通関数の設計と抽出**。`Source/audio/TrackFxChain.h`（namespace `TrackFx`）を新設:
  - `Settings`（プレーン値）＋ `loadSettings(TrackParams&)`（RTのatomic読み込み）
  - `evaluateActivity(Policy, ...)` — RT（`needsActivePath`）とバウンス（`enabled && !isNeutral`）の判定を policy で一元化
  - `process(...)` — EQ→アナライザタップ→Comp（＋GR書き戻し）の適用順を1箇所に。RT固有の付帯物は固定サイズの `Context` 構造体（`AnalyzerTap*`・trackId・GR atomic ポインタ）。全関数 確保なし・noexcept・std::function不使用
  - 高速パス（activeFx=false）は関数に入れず呼び出し側の既存式のまま（ビット一致契約の維持）
- [x] **4b: 経路を1本ずつ差し替え**（RTモノ → RTステレオ → RT MIDI → バウンス本編 → バウンスMIDI → バウンスEQテール）。RT3経路差し替え後・バウンス3経路差し替え後にそれぞれ全テスト＋Debugハッシュ一致を確認
- [x] **テール判定を `TrackFx::producesTail` 契約として明示**（現時点はEQのみtrue＝現挙動と同一）。将来の stateful FX の申告先をヘッダのコメントで固定
- [x] 補助判定の統合: `isMonoClipTrack`/`isStereoOnlyClipTrack` を TrackFx へ移動し、RT側のインライン `allClipsStereo` ループも置換（ビット一致を確認済み）
- [x] 全テスト green ＋ 全5ハッシュがベースライン一致（Debug/Release とも・2026-08-16）

### 動作確認 [AI🤖 + 人間👨‍💻]

- [x] [AI🤖] 全テスト green（RT/バウンス一致・pan前検波の仕様テスト含む）＋ Phase 0 のハッシュベースラインと全5ハッシュ一致（Debug/Release とも。`scripts/check-render-hashes.sh compare` exit 0）
- [x] [AI🤖] 実プロジェクト相当の二重チェック: `hash-fx-project-bounce`（Project::save→load→バウンス）がベースライン一致（ハッシュテストに組み込み済み＝上と同時に確認）
- [x] [AI🤖] dev版目視: EQ入りテストプロジェクトを検証フック（`--open --eq-editor --snapshot`）で起動し、FXパネルのサムネイル実カーブ（大カーブと同形）・スロット表示（EQ/Comp点灯・Extグレー）・EQエディタのカーブ描画をスクショで確認（ログ `project.open` / `fxdetail.open fx=EQ` / `debug.snapshot ok=1`）
- [ ] [人間👨‍💻] dev版で普段の操作感（FXパネル遷移・サムネイル見た目・EQ編集中のサムネイル追従・ミキサーのサムネイル）に違和感がないか一巡

### 仕上げ [AI🤖]

- [x] fx-roadmap.md のバッチ1チェックボックスを更新（成果物のファイル名も記録）
- [x] GOTCHAS.md: 「掛け算位置ズレ」セクションに TrackFxChain 集約の旨と「仕様テストは残す」注意を追記
- [x] VERIFY.md: ハッシュ回帰節を `scripts/check-render-hashes.sh` ベースの手順に更新（expected_names の保守・grep直結禁止の理由を明記）

## ログ

### 試したこと・わかったこと
- Debug と Release で全5ハッシュが**一致していた**（このコンパイラ・最適化設定では積和順序が変わらなかった）。基準は両構成とも保持し、比較も両方で回す
- 6経路の差し替えは「RT3経路 → Debugハッシュ比較 → バウンス3経路 → Debugハッシュ比較 → 最後に Debug/Release full compare」の粒度で実施。全段でビット一致
- dev版の目視は既存の検証フック（`--open --eq-editor --snapshot`）で完結（合成クリック不要・フォーカス奪取なし）。EQ入りテストプロジェクトは project.json 手書き（v16 の fx.eq.bands 形式）で用意し、確認後に削除
- 集約の到達点: FXチェーン（判定・適用順・テール）と Biquad・スロット構成・EQカーブ計算は一元化した。クリップ合算・pan法則（monoFx/prePanFx）・ミックス合流は経路ごとに本当に違うため呼び出し側に残した（次のサチュレーションで触るのは TrackFx の3点＋Settings/Activity への項目追加）

### 方針変更
- レビュー指摘（2026-08-16・実装後）: ①`wantTail` の本番判定（MainComponent::startBounce）が `eqEnabled && !isNeutral` の直書きのままで producesTail 契約が入口に効いていなかった → `TrackFx::producesTail` 呼び出しに置換し、ハッシュテストの `wantTail` も本番と同じ判定（ハードコードtrue をやめる）に変更。修正後も全5ハッシュがビット一致（Debug/Release） ②fx-roadmap のバッチ1チェックは人間の操作感確認が済むまで外した
- レビュー指摘2巡目（同日）: テストが wantTail 判定（TrackType分岐＋代入）を再実装しており、本番入口の呼び出し欠落を検出できない → `BounceRenderer::trackWantsTail(TrackRender)`（MIDI=synthあり or producesTail）と `Request::resolveWantTail()` に切り出し、本番・テストとも同じ入口を呼ぶ形に変更。テスト側は `expect(request.wantTail)` で共通判定がテールを要求することも主張。dev版の `--bounce` フックで実地確認: フェードあり曲は tail=0（applySongFadeToRange の正当な上書き・endSample=フェード終端と整合）、フェードなしEQ曲は tail=1・書き出し長がendSample+テール分（26048 > 24000）。全5ハッシュもビット一致のまま
- 実プロジェクトの基準バウンスは「アプリの⌘B＋cmp」でなく、ハッシュテスト内の `hash-fx-project-bounce`（Project::save→load→BounceRenderer）に組み込んだ。fixture 自体が冪等な seeder になり、比較も check-render-hashes.sh に一本化される（アプリGUI経由は自動化コストが高く、Request組み立て以外は同一経路のため）
- Phase 2（UI・daw_tests非依存）の着手を Phase 1 より先に始めた（Phase 0 の Release ベースライン採取ビルドと並行させるため。DSPに触る Phase 1 はベースライン確定後に実施＝順序の趣旨は維持）
