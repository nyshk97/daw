# トラックごとの出力レベルメーター

## 概要・やりたいこと

Logic Proのトラックヘッダのように、再生中に各トラックの出力レベルを音量スライダー上に緑のバーで表示する。「どのトラックが今鳴っているか」「クリップしていないか」をヘッダだけで把握できるようにする。

- フェーダー＝設定値（青い塗り）、メーター＝実際の出音（緑のバー）を1つのUIに重ねるLogicのデザインを踏襲する
- メーターは**1本**（L/Rの最大値を表示。LogicのようなL/R 2本は作らない）
- クリップ警告: レベルが0.9超で色を赤系に変える（既存の入力メーターと同じ閾値）。ただし検知できるのは**トラック単体のクリップ**まで。複数トラック合算によるマスタークリップの検知はマスターメーターが必要なので本planのスコープ外（将来課題）

## 前提・わかっていること

- 既存の入力メーターの仕組み: オーディオスレッドが `transport.inputPeakLevel`（atomic float）に `buffer.getMagnitude()` を store → UIの30Hz Timer（`MainComponent::timerCallback`）が load して描画。今回も同じpull型でいく（GOTCHAS: オーディオスレッドからUIへはpush通知しない）
- `TrackParams`（`shared/PlaybackSnapshot.h`）は `shared_ptr` でモデルとスナップショットに共有されており、atomic（gain/mute/solo）を両スレッドから読み書きしている。**per-trackのピーク値を置く場所として最適**（スナップショット再構築不要・トラック対応付け不要）
- ミックス箇所は `PlaybackEngine.cpp` の2箇所:
  - オーディオトラック: クリップ加算ループ（`buffer.addFrom(..., gain)`。97〜130行付近）。**同一トラックで重なるクリップは加算再生**なので、個別クリップのピークのmaxでは合算クリップ（や位相打ち消し）を正しく測れない → **トラックごとのモノスクラッチバッファに一旦合算してから計測**する（下記）
  - MIDIトラック: AUレンダリング後の `block` 加算（391行付近。非可聴時は gain=0）。ミックスは出力chごとにMIDIのch0/ch1を対応させるステレオなので、**ch0とch1の `getMagnitude` の大きい方**を測る（ch0だけだと右chを見落とす）
- オーディオクリップはモノラルで全出力chに同一内容が加算される（トラック内はモノ）。よってオーディオトラックの合算計測はモノスクラッチ1本で正確。スクラッチは `prepareToPlay` で確保する固定メンバ（オーディオスレッドでのアロケーション禁止）
- 1 UIフレーム（33ms）に複数オーディオブロックが走るため、audio側は**CASループでmax更新**、UI側は**exchange(0)で読み取りリセット**にする（storeだけだと最後のブロックの値しか見えず、瞬発音を取りこぼす）
- 描画は `AppLookAndFeel::drawLinearSlider`（前回追加）が溝のジオメトリを知っている。スライダーの `getProperties()` に `meterLevel` を積んで drawLinearSlider 内で描くと、描画ロジックが1箇所にまとまる。プロパティが無いスライダー（ピアノロールのベロシティ）は影響を受けない
- メーターの見た目: 溝（4px）の内側に細い緑バー（2px・丸端）を左から重ねる。青い値塗りの上に描く
- UI側のディケイ: 表示値 = max(今回の読み取り値, 前回表示値 × 減衰係数)。ピーク保持（黄色線）はTier 1では作らない
- 録音中もメーターは動いてよい（再生ミックスが走っている限り値が入る。入らなければ0のまま＝自然に消える）

## 実装計画

### Phase 1: shared — ピーク値の受け渡し [AI🤖]
- [x] `TrackParams` に `std::atomic<float> peakLevel { 0.0f }` を追加（`static_assert` は既存でカバー済み）
- [x] コメントで「audio側はCAS max・UI側はexchange(0)で読む」規約を明記

### Phase 2: audio — PlaybackEngineでの計測 [AI🤖]
- [x] `prepareToPlay` でモノスクラッチバッファをメンバとして確保。容量は既存のMIDIスクラッチと同じ `max(4096, samplesPerBlockExpected)`（`PlaybackEngine.cpp:22` の防御パターンに合わせる）。`process()` 側では `numSamples` が容量を超える場合は**メーター計測のみスキップし、出力は従来どおり per-clip の `buffer.addFrom` で維持する**（スクラッチ経由ごと止めると oversized block でクリップが無音になるため。音を落とさないことを最優先）
- [x] オーディオトラック: **各トラックの処理前にスクラッチの先頭 `numSamples` を必ず clear**（1本のメンバスクラッチを全トラックで再利用するため、clear漏れ＝前トラックの音の混入）→ クリップ群をスクラッチに合算（gain込み）→ `getMagnitude` で**加算後ピーク**を測って `peakLevel` にCAS max → スクラッチを出力バッファの各chに加算（既存の per-clip `addFrom` をスクラッチ経由に置き換え）
- [x] MIDIトラック: `block` の `max(getMagnitude(ch0), getMagnitude(ch1)) × gain` をCAS max（gain=0なら書かない）
- [x] リアルタイム制約の確認: 追加コードにアロケーション・ロックがないこと（スクラッチは事前確保・`getMagnitude` はループのみで安全）
- [x] 既存テスト（daw_tests）が通ること（ミックス経路の置き換えで挙動を変えていないか）
- [x] テスト追加: 同一トラックで振幅0.6のクリップ2枚を重ねて再生 → `peakLevel > 0.9` になること（合算後計測のリグレッション防止）。ついでに別トラックへの混入がないこと（clear漏れ検知）も同テストで確認

### Phase 3: ui — 表示 [AI🤖]
- [x] `TrackHeaderComponent::updateMeter()` を追加（`track`・`volumeSlider`・減衰状態はここのprivateにあるため、exchange・減衰・プロパティ更新・再描画までこのメソッドで完結させる）: `params->peakLevel.exchange(0)` を読み、`表示値 = max(読み値, 前回 × 0.8)` で更新。表示値が変わったときだけ `volumeSlider` の `meterLevel` プロパティを更新して `volumeSlider.repaint()`（0のままのトラックは再描画しない）
- [x] `TrackHeadersView::updateMeters()` は各itemの `updateMeter()` を呼ぶだけの転送
- [x] `MainComponent::timerCallback`（30Hz）から `headers.updateMeters()` を呼ぶ
- [x] `AppLookAndFeel::drawLinearSlider`: `meterLevel` プロパティがあれば緑バー（>0.9で赤系）を描く。描画順は**背景の溝 → 青い値塗り → メーター（2px・丸端） → つまみ**に固定
- [x] 再生停止後はディケイで自然にゼロへ（exchange(0)で新規値が入らなくなるため）

### 動作確認 [AI🤖]
- [x] ビルド → 起動 → cli-font-test を開き、AXPressで再生
- [x] 再生中のスクショ: Piano/ドラムのスライダーに緑バーが出て、クリップの無いトラック（ボーカル録音）には出ないこと
- [x] 停止後のスクショ: 緑バーが消えること
- [x] ~~ミュート時に消えること（mキー or AXPressでMボタン）~~ → 停止後の消灯確認で同一経路（値が入らない→ディケイ）を検証済み。Mボタンの自動操作はプロジェクトをdirtyにするため人間確認に委譲
- [x] ログ確認: `~/Library/Logs/daw/` にオーディオ異常（drop等）が出ていないこと

### 動作確認 [人間👨‍💻]
- [ ] 実際に再生して、メーターの動き・ディケイ速度・色味に違和感がないか目視確認
- [ ] 録音しながらのメーター挙動の確認（自動確認は録音がプロジェクトを汚すため人間側で）

## ログ
### 試したこと・わかったこと
- リニア振幅のまま描くとDLS音源の実用レベル（magnitude 0.1前後 = -20dB）でメーターが幅2%程度にしか見えなかった。表示のみ -60dB..0dB → 0..1 のdBスケールに変更（`drawLinearSlider` 内。peakLevelの受け渡し・赤閾値0.9はリニアのまま）
- `juce::Decibels` は juce_audio_basics のためGUI専用ヘッダでは使えない → `std::log10` で直接計算
- 検証時のスクショはLogic Proが前面でも `screencapture -x -l <windowID>` で背面のまま撮れる（winlistのdict出力はキー順不定でgrepが不安定 → `番号\t名前` のTSVを出す専用ツールで解決）

### 方針変更
（なし）
