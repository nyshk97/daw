# サンプル音源トラック（外部ワンショットの割り当て）

## 概要・やりたいこと

MIDIトラックの音源を、macOS内蔵GM音源（DLSMusicDevice）だけでなく**外部の単発オーディオファイル**からも選べるようにする。

- 主用途は「サンプルパックをDLしてキックだけ差し替える」。素材はドラム・808などのワンショットで、単発ファイル単位で扱う
- 右パネルのファイルブラウザ（自動試聴つき）で聴き比べ → トラックへドラッグ、で完結させる
- 副次的に808ベースのような音程ものも扱えるよう、トラックごとに「音程固定/追従」を切り替えられるようにする

**スコープ外**（意図的に作らない）:
- Keyscape等のマルチサンプル音源＝**AUプラグインホスティング**。これは別テーマ（CLAUDE.mdでTier 1のスコープ外に明記済み）。1サンプルのピッチシフトでピアノを作るのは原理的に無理があり、代替にはしない
- 1トラックに複数サンプルのキーマッピング（ドラムキット風）。既存GMの「1トラック＝1打楽器」（Kick/Snare/Closed Hi-Hat）と同じ思想を保つ
- 逆再生・ADSR・フィルター等の音作り機能
- ミキサーオーバーレイ側のストリップへのInstrumentスロット追加（FXパネルのみに置く。必要になったら別途）

## 前提・わかっていること

### /dig（2026-07-25）での決定事項

| 論点 | 決定 |
|---|---|
| 割り当て単位 | 1トラック＝1サンプル |
| 保存 | プロジェクトフォルダにコピー（自己完結。同じ音を複数プロジェクトで使うと重複コピーになるのは許容） |
| 割り当て入口 | ファイルブラウザから**トラックヘッダー or MIDIトラックの行**へD&D |
| 空白へのドロップ | 従来どおり新規オーディオトラック＋クリップ（変更なし） |
| 楽器プルダウン | GM 13項目の下にサンプル名が並ぶ。GM楽器を選び直せば内蔵音源に戻る（別途「戻す」操作は作らない） |
| 音程 | トラックごとに固定/追従。**落とした直後は常に「固定」** |
| ルート音 | 追従モード時のみ有効。ドロップダウンで選ぶ（既定 C3=60） |
| 発音長 | 固定＝ノート長を無視して最後まで（One Shot）/ 追従＝ノートの長さで止まる |
| 連打 | 重ねて鳴らす（ボイスを奪わない） |
| 設定UI | FXパネルに `Instrument` 枠を新設。内蔵音源時は `Ext` と同じグレー表示（クリック不可）、サンプル割当時のみ点灯してクリック可 |
| 下部エディタ | 波形＋サンプル名／音程モード／ルート音／サンプル音量／頭の無音カット。波形クリックで試聴 |
| 頭の無音カット | 取り込み時に自動検出して初期値を入れ、波形上のドラッグで微調整 |
| トラック名 | 「トラック 5」等の初期名のときだけサンプル名へ自動リネーム。手動命名済みなら触らない |
| ⌘Z | サンプル音量・頭カットも取り消し対象 |

### コード調査でわかっていること

**現状のMIDI音源経路**
- `Track` は `gmProgram` / `drums` / `drumPitch` の3値だけを持つ（`Project.h:129-131`）
- `SynthBank::sync()` がトラックごとに DLSMusicDevice の `AudioPluginInstance` を1個生成し（`SynthBank.cpp:94`）、`SynthInstance` に包んで `PlaybackSnapshot` へ `shared_ptr` で載せる。破棄は退役スナップショット解放後（メッセージスレッド）
- `PlaybackEngine::renderMidiTracks`（`PlaybackEngine.cpp:475`）が MidiBuffer を組み立てて `synth->plugin->processBlock (block, midiScratch)` を呼ぶ。**MidiBuffer を作る部分（activeNotes 追跡・境界マスク・プレビュー発音・消音）は音源の種類に依存しない**ので、差し替えるのは最後の processBlock だけで足りる
- バウンスは `SynthBank::createIndependent()` で共有インスタンスと独立した音源を作る（`MainComponent.cpp:1297,1476`）。ノートのフラット化規則は `Project::buildSnapshot` と `BounceRenderer.cpp:55` に二重にある（固定ピッチ置換も両方に）
- ノートオンには velocity が載っている（`PlaybackEngine.cpp:584` 他）。サンプラーが無視すると**GMから切り替えた瞬間に強弱が消える**

**消音と再発音（One Shot設計の前提。レビュー指摘で調査）**
- 消音には2段階ある（`PlaybackEngine.cpp:501`）
  - `silenceAll`（停止・シーク・サイクル折返し）: 追跡中のノートへ noteOff ＋ **allNotesOff（CC123）**
  - `silenceAll` でない（スナップショット差し替え＝再生中の編集・トラック追加）: noteOff のみ。プレビュー発音（`endPpq == -1`）は温存し CC123 も送らない
- その直後に `resound`（`PlaybackEngine.cpp:573`）が「再生位置を跨いでいるノートを offset 0 で再発音」する
- → **One Shot が noteOff を一律無視すると、消えていないボイスの上に resound がもう1発重ねて二重発音になる**。「noteOff を無視する」だけでは仕様として不足で、消音の2段階と resound の両方に規則が要る（下記「設計判断」で確定）

**WAV読込がSRを捨てている**
- `Project::loadWav`（`Project.cpp:174`）はバッファだけを返し、リーダーの `sampleRate` を捨てる。クリップはプロジェクトSRへ変換済みなので今まで不要だった
- サンプルは元SRのまま保存する方針なので、**再生比率の計算に元SRが要る**。読込経路の拡張が必須（`sampleSourceRate` が0のままでは再生レートが決まらない）

**undo の仕組み**
- `UndoStack` は `project.tracks` の構造コピーを積む方式（`UndoStack.h:20`）。**Track のフィールドに持たせれば自動的にundo対象**になる
- 逆に `TrackParams`（atomic）は shared_ptr 共有なのでundo対象外 ＝ 音量・ミュートが戻らないのはこの構造による
- → サンプル音量・頭カットは **Track のフィールドに持つ**（undo対象にするため）。ただしオーディオスレッドへ即反映させたいので、`SynthInstance` 側に atomic のミラーを置き、真実の源は Track とする
- `UndoStack::referencedWavs()` は `clip.fileName` しか集めていない（`UndoStack.h:55`）→ サンプルファイルも保護対象に加える必要がある

**ファイルとGC**
- `Project::save()` は `clip-*.wav` にマッチするファイルのうち未参照のものを削除する（`Project.cpp:328`）。サンプルは `instr-NNN.wav` とし、**同じGC規則を別パターンで追加**する（参照元は `track.sampleFile` ＋ undo履歴）
- 取り込みは `AudioImporter`（専用スレッド。デコード→リサンプル→24bit WAV）＋「一時名 `.import-<uuid>.wav.tmp` へ書き、完了時にメッセージスレッドで最終名へリネーム」パターン（`MainComponent.cpp:1576`）。**この流儀をそのまま使う**
- ただしサンプルは**プロジェクトSRへ変換せず、元のSRのまま保存する**（再生時に `sourceRate / deviceRate` の比で読み進めるため変換不要。デバイスSR変更にも追従できる）。`AudioImporter` に「SR変換しない」経路を足す

**D&D の現状**
- ブラウザ行は `getDragSourceDescription` でファイルパスを返す（`AudioFileBrowserView.h:65`）。受け側は `TimelineView::LaneContent` が `FileDragAndDropTarget`（Finder用）と `DragAndDropTarget`（ブラウザ用）の両方を実装（`TimelineView.cpp:220`）
- MIDIトラックの行は現在 `rejected = true` で不受理表示（`TimelineView.cpp:718`）。ここを割り当てに転用する
- `TrackHeadersView` は現在ドロップターゲットではない → 追加する

**下部エディタの現状**
- `FxDetailView` は「タイトル＋空の角丸パネル」だけの箱で、中身は未実装（`FxDetailView.h:64` のコメント「各FXのUIは後続スライスでここに載る」）。**Instrumentエディタが下部エディタの最初の中身になる**ため、中身を差し込む仕組み（子コンポーネントの切替）も本計画で作る
- 下部エディタはピアノロールと排他（後勝ち。`MainComponent.cpp:2377`）。ルート音を調整しながらノートを打つことはできないので、**波形クリック試聴が音を確かめる唯一の手段**になる

**ピアノロール**
- 固定ピッチ打楽器は `forcedPitch()`（`PianoRollView.cpp:353`）で**編集ピッチが1行に固定される**（表示は常に128鍵。行のハイライトと初期スクロールもこの値を使う）。サンプルの固定モードも同じ判定に乗せる
- 鍵盤クリックのプレビューは `PreviewFifo` 経由（`MainComponent.cpp:222`）。固定ピッチトラックはピッチを置換して送っている

### 設計判断（本計画で確定させるもの）

**モデル**: `Track` に音源種別を足す。GM用の既存3値はそのまま残し、サンプル用フィールドを併置する（GMへ戻したときに元の楽器が復元される）。

```cpp
enum class InstrumentKind { gm, sample };
// type == midi のとき
InstrumentKind instrument = InstrumentKind::gm;
// --- sample のとき ---
juce::String sampleFile;        // プロジェクトフォルダ相対（instr-NNN.wav）
juce::String sampleName;        // 表示名（元ファイル名・拡張子なし）
bool samplePitchFollow = false; // false=固定（One Shot）/ true=音程追従（ノート長ゲート）
int sampleRootNote = 60;        // 追従時の基準ノート
float sampleGain = 1.0f;        // サンプル音量
juce::int64 sampleStartOffset = 0;  // 頭の無音カット位置
std::shared_ptr<juce::AudioBuffer<float>> sampleAudio; // メモリ常駐（Clip::audio と同じ寿命規則）
std::vector<float> samplePeakCache;  // 波形描画用（Clip と同じ samplesPerPeak 単位）
double sampleSourceRate = 0.0;  // ファイル自体のSR（再生比率の計算に使う）
```

**音源の抽象化**: `SynthInstance` に `std::unique_ptr<SamplerEngine> sampler` を足し、`plugin`（AU）と排他で持つ。`renderMidiTracks` の末尾だけ分岐する。AUプラグイン化（`AudioPluginInstance` 継承）は純粋仮想17個のスタブが必要なわりに得るものがないので採らない。

**SamplerEngine の制約**（オーディオスレッドで走る）: ヒープ確保・ロック・ログ禁止。固定長ボイス配列（16）を事前確保し、サンプルバッファは `shared_ptr<const AudioBuffer<float>>` で共有所有する（`ClipPlayback::audio` と同じ規則）。

**発音・消音・再発音の規則**（MIDIメッセージにはノート個体のIDがないため、曖昧さが残らないよう明文化する）

| 入力 | 固定モード（One Shot） | 追従モード |
|---|---|---|
| noteOn | 空きボイスへ割り当て（満杯なら最古を奪う）。ゲイン＝`sampleGain × velocity/127` | 同左。再生レートに `2^((pitch-rootNote)/12)` を掛ける |
| noteOff | **無視**（最後まで鳴らす） | **同ピッチの未リリースボイスのうち最古**を5msフェードでリリース（同ピッチ連打で新しい発音まで切らないため） |
| allNotesOff（CC123） | 全ボイスを5msフェードで停止 | 同左 |

- **Mono（新しい打点で前の音を切る）**: トラックごとのトグル（既定OFF＝重ねる）。ONのとき noteOn の
  たびに既存の全ボイスを5msフェードで切ってから鳴らす（Logicの Quick Sampler「Polyphony: 1」相当＝
  リトリガーあり。実機TR-808と同じ挙動）。
  必要になった理由: 1.7秒の808を16分で連打すると最大10発が重なり、①合計が0dBFSを超えてクリップ
  ②音程のあるサイン波どうしが干渉してうなる（実測: 合計ピーク1.176・+1.4dB超え）。
  短い打楽器（0.05秒のハイハット等）はそもそも重ならないので、既定は「重ねる」のまま
- **ボイスは発音時のモード（固定/追従）を自分で保持する**。モードは atomic で非同期に変わるため、noteOff の扱いを「トラックの現在モード」で判断するとブロック境界で不整合が出る
- **モード変更（固定⇄追従）の瞬間は、全ボイスを5msフェードで停止する**
  - **フラグを立てるのは `SynthBank::sync()`**（UIハンドラではない）。undo/redo は `afterHistoryRestore()`（`MainComponent.cpp:948`）→ `pushSnapshot()` → `sync()` の経路で復元され、**UIハンドラを通らない**。UI側で呼ぶと undo での切り替わりを取りこぼす。`sync()` が前回の `pitchFollow` を `Entry` に保持してモデル値と比較し、変化していれば `requestStopAll()` を立てる（GMの `gmProgram` / `drums` 比較と同じ構造）
  - **UI側は「モデル更新 → `pushSnapshot()`」とする**（`sync()` を直接呼ばない）。`resound` は `snapshot != lastSeenSnapshot` の**ポインタ比較でしか立たない**（`PlaybackEngine.cpp:112,187`）ため、`sync()` だけでは「旧ボイスは止まったが跨いでいる追従ノートが復元されない」状態になる。`pushSnapshot()` 内で `sync()` が走る既存構造に乗せれば、手動操作もundo/redoも同じ一本の経路になる
  - **SamplerEngine は停止要求をブロック先頭・MidiBufferのnoteOff/noteOnより先に処理する**。固定→追従では「旧ボイスをリリース化 → その後で resound の新ノートオンを受ける」順序になり、二重発音が構造的に起きない
  - 両方向とも筋が通る:
  - 固定→追従: 全ボイス停止 → 次のスナップショットで resound が走り、跨いでいるノートが持続音として復元される（追従モードとして正しい）
  - 追従→固定: 全ボイス停止 → resound はスキップされ何も鳴らない（ワンショットが途中から鳴り出さない＝固定モードとして正しい）
  - 停止を挟まないと、旧モードのボイスが残ったまま新モードの resound が走り、二重発音や不自然な打ち切りになる
  - ルート音・音量・頭カットの変更では停止しない（発音中のボイスは発音時の値のまま鳴り切る＝自然な挙動）
- **音量・ルート音・頭カットの変更ではスナップショットを再pushしない**（atomicのミラー更新だけ）。
  再pushすると `snapshotChanged` 検出で「全ノートオフ→跨ぎノート再発音」が走り、追従モードで
  鳴っている音が頭から鳴り直す。UIの確定通知は「値の変更（再pushなし）」と「音程モードの変更（再pushあり）」で
  2本に分ける
  - **undo/redoも同じ扱いにする**: `UndoStack` に編集種別（`EditKind::sampleValue` / `structure`）を持たせ、
    サンプル値だけの復元では `afterHistoryRestore` が再pushしない（⌘Zでも鳴っている音を切らない）
- **サンプルの差し替えは即時打ち切り**（フェードしない）。`sampleFile` が変わると SamplerEngine ごと交換され、旧インスタンスは retired へ移って**もうレンダリングされない**（`PlaybackSnapshot.h:163`）ため、旧ボイスのフェードは原理的に出力できない。同一インスタンス内でRT安全にバッファを差し替える設計は寿命管理が増えるので今回は採らない（差し替えは音を止めてから行う操作で実害が小さい）。この非対称性は `requestStopAll()` のコメントに明記する
- `resound`（`PlaybackEngine.cpp:573`）は、**固定モードのサンプラートラックではスキップする**。One Shot は noteOff で消えていないので復元が不要であり、再発音すると二重になる。判定用に `SynthInstance` へ `std::atomic<bool> oneShot` を持たせ、PlaybackEngine が読む
  - 副作用として、シーク先で「跨いでいるドラム音」が途中から鳴り出すことがなくなる。ワンショットは「叩かれた瞬間の音」なので、途中から鳴り出さない方が正しい
  - 追従モードは noteOff で止まるので resound の対象に残す（AU音源と同じ挙動）
- 停止中のプレビュー発音は固定発音長0.5秒で noteOff が送られる（`PlaybackEngine.cpp:520`）。固定モードはこれを無視するので、0.5秒より長いワンショットも全長試聴できる（ピアノロールを閉じたときの allNotesOff では止まる）

**velocity**: `sampleGain × (velocity / 127)` の線形。カーブは付けない（GM音源から切り替えたときに強弱が消えないことが目的で、音作りの機能ではない）。

**固定モードのノートピッチの扱い**: GMの固定ピッチ打楽器は「再生時にノートのピッチを `drumPitch` へ置換」してGMのドラムマップへ送っている（`Project.cpp:672` / `BounceRenderer.cpp:55`）。**サンプラーの固定モードは置換しない** — サンプラーはピッチを見ずに等速で鳴らすので置換が無意味であり、規則を増やすだけになる。`fixedPitch` 置換は**GM専用の規則として残す**（両方の箇所にその旨をコメントで明記）。

- `PianoRollView::forcedPitch()` は、サンプル固定モードでは **`sampleRootNote` を返す**（既定60=C3）
- **`forcedPitch()` は「1レーン表示」ではない**（レビュー指摘で確認）。ピアノロールのコンテンツ高さは常に `128 * rowHeight`（`PianoRollView.cpp:565`）で、この値がするのは①ノート作成・移動・トランスポーズをそのピッチへ拘束（`PianoRollView.cpp:419,474,717,784`）②その行の背景をハイライト（同107）③開いたときそこへスクロール（同316）の3つ。**128鍵表示のまま編集ピッチが1行に固定される**という挙動で、GMの固定ピッチ打楽器と同じ
- 理由: 固定モードで打ったノートは全部 `sampleRootNote` になるので、あとで追従モードへ切り替えたときに**全ノートがルート音＝等速**で鳴り、音が飛ばない
- プレビュー発音のピッチ置換（`MainComponent.cpp:222`）も同じく `sampleRootNote` を使う。ピアノロール・プレビュー・通常再生・リージョン書き出しの4経路すべてでこの規則を揃える

## 実装計画

### Phase 1: サンプラーエンジン（音を出す土台） [AI🤖]

UIもモデルも触らず、テストで音が正しいことだけ固める。

- [x] `audio/SamplerEngine.h/.cpp`: 固定16ボイス。`processBlock (AudioBuffer<float>&, MidiBuffer&)` で `plugin->processBlock` と同じ形にする
  - 発音・消音の規則は上表のとおり（noteOn / noteOff / CC123 の3種すべてを実装する。**noteOff無視とCC123停止を必ず分ける**）
  - ボイスゲイン＝`sampleGain × (velocity / 127)`。velocity は noteOn 時にボイスへ焼き込む（後から `sampleGain` を変えても発音中のボイスには影響させない）
  - 再生レート: 固定＝`sourceRate / deviceRate`、追従＝それに `2^((pitch - rootNote)/12)` を掛ける
  - 開始位置は `startOffset`。リリースは5msフェード（急に切るとクリックが出る）
  - 補間は線形（ワンショットのピッチシフト用途では十分。品質が問題になったら4点Hermiteへ）
  - ソース1ch→2ch複製 / 2ch→そのまま。出力へ加算
  - `std::atomic` で `gain`(float) / `startOffset`(int64) / `pitchFollow`(bool) / `rootNote`(int) / 停止要求(bool) を持ち、メッセージスレッドから即時反映できるようにする（発音中のボイスには影響させず、次の発音から反映）
  - **新設する全atomicに `static_assert (std::atomic<T>::is_always_lock_free)` を書く**（GOTCHAS.md の規約。特に `startOffset` の64bit atomicは環境によってロックへフォールバックしうるので明示的に保証する。`TrackParams`（`PlaybackSnapshot.h:39`）と同じ流儀）
  - **各ボイスは発音時のモード（固定/追従）を保持**し、noteOff の扱いはボイス自身の値で判断する（トラックの現在モードで判断しない）
  - **`requestStopAll()`（atomicフラグ）を持ち、次のブロックで全ボイスを5msフェード停止する**。**処理はブロック先頭・MidiBufferのイベントより先**（固定→追従で旧ボイスのリリースが resound の新ノートオンより先に来る順序を保証するため）。フラグを立てるのは `SynthBank::sync()`（Phase 2）
- [x] `SynthInstance` に `std::unique_ptr<SamplerEngine> sampler` と `std::atomic<bool> oneShot` を追加（`sampler` は `plugin` と排他）。`PlaybackEngine::renderMidiTracks` の末尾を分岐（`plugin ? plugin->processBlock : sampler->processBlock`）
  - レート/ブロックサイズ不一致のスキップ判定（`PlaybackEngine.cpp:490`）はサンプラーにも同じ基準で効かせる
  - `totalOutputChannels` はサンプラーでは常に2
  - **`resound`（`PlaybackEngine.cpp:573`）を `oneShot == true` のトラックでスキップ**する（二重発音の防止。理由はコードコメントに明記）
- [x] Tests: サンプラー単体の音響検証
  - 固定モード: 8分音符のノート長でも1秒のサンプルが最後まで鳴ること／noteOff でレベルが落ちないこと／**CC123では止まること**
  - 追従モード: ノート長でフェードアウトすること／rootNote+12 のノートで再生長がおよそ半分になること／rootNote ちょうどなら元のサンプルとサンプル単位で一致すること
  - **同ピッチ連打の noteOff 対象**: 同じピッチで2回オン→1回オフしたとき、**先の発音だけが止まり後の発音は鳴り続ける**こと（最古リリース規則）
  - 連打: 前のボイスが生きたまま新しいボイスが加算されること（重ねて鳴らす仕様）
  - 17音同時（ボイス上限超え）で落ちない・最古が奪われること
  - `startOffset` の分だけ先頭が飛ぶこと／`gain` が線形に効くこと／**velocity 64 が velocity 127 のおよそ半分の振幅になること**
  - デバイスSR≠ソースSR（44.1k素材を48kで再生）で再生長が比率どおりになること
- [x] Tests: エンジン結合での消音・再発音（`PlaybackEngine` 経由）
  - **停止**・**シーク**・**サイクル折返し**で固定モードのボイスが止まること（CC123経路）
  - **再生中のスナップショット差し替え**（ノート追加相当）で固定モードのワンショットが**二重に鳴らない**こと（resound スキップの検証。振幅が2倍にならないことで判定）
  - 追従モードは従来どおり resound で持続音が復元されること
  - **再生中のモード切替**: 固定ボイスの発音中に追従へ切り替えて差し替えたとき、旧ボイスが残って**二重発音にならない**こと／追従の持続音中に固定へ切り替えたとき、鳴り残りも不自然な打ち切りも起きないこと（どちらも「全ボイス停止」を経ること）

### Phase 2: 割り当て導線（落とせば鳴るところまで） [AI🤖]

このPhase完了時点で「ブラウザからキックを落として打ち込む」が成立する。設定は全部デフォルト（固定・One Shot・音量1.0・自動検出の頭カット）。

- [x] `Track` にサンプル用フィールドを追加（上記「設計判断」のとおり）
- [x] **`Project::loadWav` を「バッファ＋元SR」を返す形へ拡張**（`Project.cpp:174`。現状はリーダーの `sampleRate` を捨てている）。クリップ読込の既存呼び出しはSRを使わないので、出力引数を省略できる形にする
  - `sampleSourceRate` はここで復元する。**SRが取れない（0以下）ファイルは読込失敗として扱う**（警告を出してそのトラックは無音。ファイル欠損時と同じ扱い）。正常なWAVならリーダーから必ず取得できるので、デバイスSRで代用するフォールバックは置かない — 代用する場合その責務はデバイスSRを知る `SynthBank` 側になり、経路が増えるだけで得がない
- [x] `Project` の保存・読込を v7→v8 に上げる
  - 保存: `instrument` / `sampleFile` / `sampleName` / `samplePitchFollow` / `sampleRootNote` / `sampleGain` / `sampleStartOffset` / `sampleSourceRate`
  - 読込: 旧版は `instrument = gm` として読む。`sampleFile` の実体を拡張版 `loadWav` でメモリへ載せ、**元SRを `sampleSourceRate` に復元**し、ピークキャッシュを作る。**ファイルが欠けていたら警告を出してそのトラックは無音**（勝手にGM音源へ戻すと音が変わって混乱するため）
  - `sampleStartOffset` はバッファ長でクランプ、`sampleRootNote` は 0..127、`sampleGain` は 0..2 でクランプ
  - なお `sampleSourceRate` は**JSONにも書く**（WAVから復元できるが、ファイル欠損時のログ・将来の診断に使えるため。読込時はWAV実体の値を優先する）
- [x] `Project::save()` のGC: `instr-*.wav` パターンを追加し、参照元に `track.sampleFile` を加える。`UndoStack::referencedWavs()` にも `sampleFile` を追加（undo/redoでの復元に備える）
- [x] `AudioImporter` に「SR変換しない」経路を追加（`targetSampleRate <= 0` を「元のSRを保つ」と定義）。デコードと24bit WAV書き出しはそのまま流用
  - **実装注意**: `targetSampleRate` を0のまま既存の計算へ流すと、出力フレーム数（`llround(inputFrames × targetSR / sourceSR)`）もwriterのSRも0になる。デコード後に `effectiveTargetRate = (targetSampleRate > 0 ? targetSampleRate : sourceRate)` を決め、**リサンプル要否の判定・出力長・writer生成のすべてでこの値を使う**
- [x] Tests: `AudioImporter` のSR保持経路を直接検証（44.1kHz入力＋`targetSampleRate = 0` → 出力WAVのSRが44.1kHz・フレーム数が入力と一致・リサンプルがバイパスされること）
- [x] `MainComponent` にサンプル取り込みフローを追加（既存 `startImport` と同じ一時ファイル→リネーム方式。最終名は `instr-NNN.wav`）
  - 完了時にメッセージスレッドで一続きに: リネーム → `undoStack.begin()` → 頭の無音カット自動検出 → Track へ反映（`instrument = sample`・初期モードは固定）→ トラック名が初期名なら `sampleName` へリネーム → `synthBank.sync()` → スナップショット再push → 保存
  - 頭の無音カット自動検出: ピーク絶対値の 1%（かつ下限 -60dBFS）を最初に超える位置の 1ms 手前。全編が閾値未満なら 0
  - 取り込み中の排他は既存の `importActive` を共用（録音・バウンス・クリップ取り込みと同時に走らせない）
- [x] `SynthBank::sync()` をサンプル対応に拡張
  - `instrument == sample` のトラックは `SamplerEngine` を作る。差し替え判定は `sampleFile` の変更とデバイスSR変更（GM側の判定と同じ構造）
  - サンプル音量・頭カット・音程モード・ルート音の変更は**インスタンスを作り直さず atomic の更新だけ**で反映する（作り直すと発音中の音が切れる）
  - **`Entry` に前回の `pitchFollow` を保持し、変化を検出したら `requestStopAll()` を立てる**（`SynthInstance::oneShot` の更新もここで行う）。UIハンドラではなく `sync()` に置くのは、**undo/redo が `afterHistoryRestore()`（`MainComponent.cpp:948`）→ `pushSnapshot()` → `sync()` の経路で復元されUIを通らない**ため。この理由をコードコメントに明記する
  - `createIndependent()` のサンプル版（バウンス用）
- [x] D&D受け口
  - `TrackHeadersView` を `FileDragAndDropTarget` ＋ `DragAndDropTarget` 化。MIDIトラックのヘッダーのみ受理し、ドラッグ中は対象ヘッダーをハイライト。オーディオトラックのヘッダーは不受理表示
  - `TimelineView::updateFileDrop`（`TimelineView.cpp:718`）: MIDIトラック行を「不受理」から「音源割り当て」に変更。インジケータは**縦線（配置位置）ではなく行全体のハイライト**にして、クリップ配置と意味が違うことを見た目で区別する
  - 複数ファイルドロップは先頭のみ（既存の流儀と同じ）
- [x] トラックヘッダーの楽器プルダウン: GM 13項目の下に区切り＋サンプル名を追加し、それを選択状態にする
  - GM楽器を選ぶと `instrument = gm` に戻る。このとき `sampleFile` 等は**保持したままで、プルダウンのサンプル項目も残す**
  - **残ったサンプル項目を選び直せば、D&Dし直さずにサンプル音源へ戻せる**（選べるのに反応しない項目を作らない）
  - サンプルを持たないトラックではサンプル項目自体を出さない
- [x] ピアノロール: サンプルの固定モードも**編集ピッチの固定**に乗せる。`PianoRollView::forcedPitch()`（`PianoRollView.cpp:353`）が**サンプル固定モードでは `sampleRootNote` を返す**ようにする（GMは従来どおり `drumPitch`）。表示は従来どおり128鍵のままで、変わるのは編集の拘束・行のハイライト・初期スクロール位置。ハイライト行の鍵盤ラベルはサンプル名（GMドラムの `gmDrumName` に相当）
- [x] プレビュー発音（`MainComponent.cpp:222`）: 固定モードのサンプルトラックは**`sampleRootNote` へ置換**して送る（ピアノロールのレーンピッチと一致させる）
- [x] `Project::buildSnapshot` の固定ピッチ置換（`Project.cpp:672`）は**GM専用のまま**にし、サンプラーのトラックでは置換しない旨をコメントで明記する（サンプラーは固定モードでピッチを見ないため置換が無意味）
- [x] Tests: `SynthBank::sync()` が `pitchFollow` の変化を検出して `requestStopAll()` を立てること。**UIを介さずモデルを直接書き換えてから sync() を呼ぶ**形で検証する（undo/redo と同じ経路になる）
- [x] ログ: `instrument.assign` / `instrument.load_fail` / `instrument.revert_gm` を記録

### Phase 3前: UIモックで方向確定 [AI🤖 → 人間👨‍💻]

下部エディタは**このプロジェクト初の「下部エディタの中身」**であり、波形・トリム線・トグル・ルート音セレクタはいずれも新規のビジュアル。直接JUCEに書くと手戻りが大きいので、先にモックで確定させる（グローバルルールの「UIの見た目の相談」に従う）。

- [x] [AI🤖] scratchpadに単一HTMLのモックを作る。**複数案をカードで並べ**、擬似データで波形を描き、トリム線はドラッグ可能・音量スライダーは可動にする。案の軸は「波形の大きさと設定群の配置（横並び / 波形メイン＋右に設定 / 上下分割）」
  - FXパネルの Instrument スロット（点灯時・グレー時）も同じHTMLに並べる
  - 既存の見た目（`Theme.h` の色・`AppLookAndFeel` のピル/ノブ）に寄せる。スクショ（本セッションで取得済み）を参照する
- [x] [AI🤖] `open` で開き、**返答に絶対パスを明記**する
- [x] [人間👨‍💻] ブラウザで見て案を確定（**案A: 上下分割** に決定。2026-07-25）

### Phase 3: FXパネル Instrument枠＋下部エディタ [AI🤖]

確定したモックをJUCEへ移植する。

- [x] `FxEditorView` に Instrument スロットを追加（MIDIトラック選択時のみ表示。EQサムネイルの下・EQスロットの上）
  - 内蔵音源時: GM楽器名をグレー表示・クリック不可（`Ext` と同じ `configure(name, nullptr, true)`）
  - サンプル時: サンプル名を点灯表示・クリックで下部エディタを開く
  - `slotPills` の配列長を 3→4 に拡張し、`slotNames` / `numSlots()` / `slotName()` の索引を通す
  - パネル幅156pxに収まらない名前は末尾省略（`Fonts::forText` でCJK補正を通す）
- [x] `FxDetailView` に「中身を差し込む仕組み」を作る: 現在の空パネル領域に子コンポーネントを載せ替えられるようにする（`setBody (juce::Component*)` 相当）。これは後続のEQ/Compエディタでも使う共通土台
- [x] `ui/InstrumentDetailView.h/.cpp`（下部エディタの中身）
  - 波形表示（`samplePeakCache` を使う。Clipの波形描画と同じ流儀）
  - 頭の無音カット: 波形上の縦線をドラッグ。カット区間は減光表示（「痕跡が見える」＝`docs/design/region-settings.md` の可視性の原則）
  - 音程モードのトグル（固定 / 追従）。追従のときだけルート音ドロップダウンを表示（C-2〜G8 の128項目・既定C3）
  - モード変更時は**モデルを書き換えて `pushSnapshot()` を呼ぶ**（停止要求と `oneShot` の更新は、その中で走る `sync()` 側の責務。UIから `requestStopAll()` / `sync()` を直接呼ばない）。`resound` はスナップショットのポインタ変更でしか立たないため、`pushSnapshot()` を省くと追従へ切り替えたときに持続音が復元されない
  - あわせてピアノロールを再描画する（`forcedPitch()` が変わり、ハイライト行と編集の拘束ピッチが切り替わるため）
  - サンプル音量スライダー（ドラッグ中だけdBポップアップ。ヘッダーの音量スライダーと同じ流儀）
  - 波形クリックで試聴（`PreviewFifo` 経由。固定モードは元ピッチ、追従モードはルート音で鳴らす）
  - 変更は Track のフィールドへ書き、`SamplerEngine` の atomic へ即反映。`setDirty(true)` ＋ 必要なら `pushSnapshot()`
- [x] undo の粒度: **ドラッグ開始時に1回だけ `undoStack.begin()`**（`mouseDown` で積み、`mouseDrag` 中は積まない）。音程モード切替・ルート音変更は操作ごとに1回。既存の音量フェーダー（undo対象外）とは異なる扱いになるので、コードコメントで理由を明記する
- [x] Instrumentスロットのクリック → 下部エディタ表示のトグル挙動は既存スロットと同じ（`toggleFxDetailSlot`）。トラック選択が変わったときの追従も既存 `syncFxDetail` に乗せる

### Phase 4: バウンス・書き出し・仕上げ [AI🤖]

- [x] `BounceRenderer` / リージョン書き出しをサンプル音源に対応（`createIndependent` のサンプル版を使う）。固定ピッチ置換（`BounceRenderer.cpp:55`）は**GM専用のまま**とし、`Project::buildSnapshot` 側と同じ規則になっていることを確認する（サンプラーでは置換しない。2箇所に重複した規則があるので片方だけ直す事故を防ぐ）
- [x] **通常バウンス（全体）のレンダリング範囲を、固定モードのワンショットが鳴り切るまで延長する**
  - 現行のテールは**最大5秒**（`BounceRenderer.cpp:270`）で、かつ1ブロックでも-60dBを下回ると打ち切る（同305）。曲末に6秒の808を置いた場合や、途中に無音区間を含むサンプルは**途中で切れる**
  - 対策: 固定モードのサンプルトラックについて、各ノートの `noteStartSample + (bufferLength - startOffset) × deviceRate / sourceRate` を**終端候補**に加え、`rangeEnd` をサンプル末尾まで延長する（テールの制限に依存させない）
  - **サイクルバウンス・リージョン書き出しは従来どおり厳密な範囲で切る**（指定範囲を書き出すのが仕様なので延長しない）
  - あわせて、**固定モードでは範囲頭より前に始まるノートを読み飛ばす**。リアルタイム側は oneShot のとき
    resound しない（シーク先で途中から鳴り出さない）ので、跨ぎノートを範囲頭で鳴らすと再生結果と
    食い違い、叩いていない打点が増える
  - **ノートの位置判定はPPQ同士で比べずサンプル位置（`llround(ppq / tps)`）で行う**。`pos` は丸め済みの
    サンプル位置なのでPPQへ戻すと境界ちょうどのノートを取りこぼし、One Shotは resound もしないため消える
    （127BPM/44.1kHzで再現。GOTCHAS.mdに追記）
  - 追従モードは延長しない（ノート長で止まるので既存のテールで足りる）
- [x] **テールループのサンプラー対応**: `track.synth->plugin == nullptr` で `continue` している（`BounceRenderer.cpp:288`）ため、**このままではサンプラーがテールで一切レンダリングされない**。サンプラーも回るよう分岐を直す（範囲を跨ぐ追従ノートの余韻に必要）。テール先頭の noteOff 送出もサンプラー経路に通す
- [x] Tests: 曲末に**6秒の固定サンプル**を置き、**全体バウンスでは全長が含まれる**こと／**サイクル・リージョン書き出しでは指定範囲で切れる**こと
- [x] Tests: 再生とバウンスの出力一致（同一プロジェクトで PlaybackEngine と BounceRenderer のサンプル値が一致すること。既存のモノ経路テストと同じ流儀）
- [x] Tests: v8 の保存/読込ラウンドトリップ（サンプル欠損時の警告・クランプ含む）
- [x] Tests: **SR復元**（44.1kHz素材を48kHz環境で割り当て → 保存 → 再読込 → 再生長とピッチが保存前と一致すること）。`sampleSourceRate` が0のまま復元されると再生比率が壊れるため、ここを回帰の要にする
- [x] Tests: GCがサンプルファイルを誤って消さないこと（undo履歴からのみ参照されている `instr-*.wav` が保護されること）
- [ ] `VERIFY.md` に確認手順を追記（再利用可能なものだけ）

### 動作確認

- [x] [AI🤖] ビルド＋既存テスト（`ctest`）が通ること
- [x] [AI🤖] Phase 1 のサンプラー音響テスト（固定/追従・連打・ボイス奪取・SR比率）
- [x] [AI🤖] `~/Music/daw/<test>/` に `instr-001.wav` が作られ、project.json が version 8 でサンプル情報を持つこと。アプリ再起動後も同じ音が鳴ること（プロジェクト再読込のログで裏取り）
- [x] [AI🤖] バウンス出力にサンプルの音が含まれること（無音でないこと・再生とサンプル一致）／曲末の長いワンショットが全長書き出されること
- [ ] [人間👨‍💻] ブラウザでキックを試聴 → トラックヘッダーへドロップ → ピアノロールで打ち込み → 再生、の通しフロー
- [ ] [人間👨‍💻] 808を落として追従モードに切り替え、ルート音を合わせてベースラインが弾けるか（ルート音のドロップダウンで足りるかの操作感）
- [ ] [人間👨‍💻] 頭の無音カットの自動検出が実素材で妥当か（打点がずれないか）、波形ドラッグの操作感
- [ ] [人間👨‍💻] ⌘Zでサンプル音量・頭カットが期待どおり戻るか（ドラッグ1回＝undo 1回になっているか）
- [ ] [人間👨‍💻] **再生しながら音量・ルート音・頭カットを触っても、鳴っている音が頭から鳴り直さないか**（レビュー指摘の修正確認）
- [ ] [人間👨‍💻] **ヘッダーのプルダウンでGM⇄サンプルを往復したとき、FXパネルのInstrumentスロットの表示（名前・点灯/グレー）が追従し、開いていた下部エディタがGMへ戻した時点で閉じるか**（レビュー指摘の修正確認）
- [ ] [人間👨‍💻] 再生中にノートを打つ・トラックを追加したときに、鳴っているワンショットが二重に聞こえないか（resoundスキップの耳確認）
- [ ] [人間👨‍💻] 楽器プルダウンでGM→サンプル→GMと往復して、どちらも意図どおり切り替わるか（サンプル項目を選び直せば戻れるか）
- [ ] [人間👨‍💻] 再生しながら固定⇄追従を切り替えて、音が二重になったり不自然に途切れたりしないか。**⌘Zでモード変更を戻したときも同じか**（UIを通らない復元経路の確認）

## ログ

### 試したこと・わかったこと

- Phase 1・2・4 を実装（Phase 3は下部エディタのモック確定待ち）。テストは `daw_tests` に6件追加（sampler engine / sampler×PlaybackEngine / SynthBank sampler / v8ラウンドトリップ＋GC / AudioImporterのSR保持 / BounceRendererサンプラー）。全テストpass
- **テストが実際に落ちることを確認済み**: resoundスキップ・noteOff無視・最古ボイス奪取・テールのサンプラー対応の4箇所をわざと壊すと該当テストがFAILする
- **バウンス範囲延長の計算は `BounceRenderer::oneShotEndSample()`（純関数）に切り出した**。MainComponent（GUI）に埋めるとテスト対象外になるため。`buildItemRender` と同じ流儀
- **`onInstrumentChanged` にトラックindexを渡す形へ変更**（ログの `instrument.assign` / `instrument.revert_gm` にどのトラックか出すため）
- 実機確認: 44.1kHz素材＋48kHzデバイスの sampler-test プロジェクトで、①楽器プルダウンにサンプル名が出る ②再生でメーターが振れる ③全体バウンスの `endSample=384000`（=8.0秒 = 最終ノート2.0秒＋サンプル6.0秒。リージョン長2.67秒を超えて延長されている）④出力WAVの7.8〜7.99秒にも音が残っている（テール上限5秒で切れていない）

- Phase 3実装後の追加確認: FXパネルのInstrumentスロット（サンプル=点灯 / GM=グレー）と下部エディタを
  **オフスクリーン描画**（VERIFY.mdの手順・確認後に一時コード削除）＋**実機の合成クリック**の両方で確認。
  実機では「スロットクリック→下部エディタが開く（ログ `fxdetail.open fx=instr-001`）→ 追従へ切替でROOTが有効化
  → 保存でproject.jsonが `samplePitchFollow: true` になる → 固定へ戻して保存で false」まで通した
- 波形は**クリップ描画の1.4倍強調を掛けない**（大きい表示だと頭打ちになって減衰の形が読めない）
- 検証中断: 途中でウィンドウが別ディスプレイへ移動していた（ユーザー操作か表示構成の変化）。CLAUDE.mdの規約どおり
  合成入力を即中止した。**合成クリックの被覆チェックに穴があった**（クリック点にlayer=0のウィンドウが1つも
  無いとき＝デスクトップ上でも abort しない）→ VERIFY.md に注意として追記

- レビュー（4件）の反映:
  - **P1 サンプル設定変更で再生中ノートが再トリガー**: `InstrumentDetailView` の確定通知を
    `onValueEdited`（音量・ルート音・頭カット。**再pushせず atomic ミラー更新のみ**）と
    `onPitchModeEdited`（音程モード。停止要求＋resoundが必要なので pushSnapshot）に分割。計画の
    「ルート音・音量・頭カットの変更では停止しない」を実装が満たしていなかった
  - **P2 GM⇄サンプル切替でFXパネルが更新されない**: `headers.onInstrumentChanged` に
    `fxEditor.refreshFromModel()` / `syncFxDetail()` を追加（`onChanged` と同じ流儀）。
    GMへ戻すと `updateFxDetailBody` が中身のないInstrumentエディタを閉じる。あわせて対象トラックの
    解決を `selectedTrack` から `fxEditor.shownTrack()` / `instrumentDetail.shownTrack()` に変えて
    取り違えを防いだ
  - **P2 サイクル書き出しの範囲頭を跨ぐOne Shotが鳴る**: `BounceRenderer::renderPass` の
    読み飛ばし条件を oneShot のとき `startPpq < rangeStartPpq` に変更（テスト追加）
  - **P3 追従モードのレート上限64倍**: クランプを `[1.0e-6, 8192.0]` に拡張（MIDI全域 2^(±127/12) と
    SR比を通す幅）。7オクターブ上で再生長が理論値どおりになることをテストで固定
  - 機械検証できる2件（P2バウンス・P3レート）は**わざと戻すとテストがFAILする**ことを確認済み。
    残る2件はGUI配線なので実操作の確認項目に追加した

- 再レビュー（3件）の反映:
  - **P1 undo/redoでサンプル値を戻すと再トリガー**: `UndoStack` に `EditKind` を追加し、
    `begin(project, kind)` / `undo(project, kind)` / `redo(project, kind)` で往復とも同じ種別を返すようにした。
    `afterHistoryRestore(kind)` はサンプル値だけの復元では再pushせず、atomicミラー更新＋ピアノロール再描画に留める。
    `InstrumentDetailView::onWillEdit(bool valueOnly)` で編集の種別を宣言する
  - **P1 PPQ丸めで境界のOne Shotが消える**: `PlaybackEngine::renderMidiTracks` と
    `BounceRenderer`（読み飛ばし・`scheduleBlockMidi`）の位置判定を全てサンプル位置へ統一。
    ついでにオフセット計算の重複（llround→jlimit）も1箇所にまとまった
  - **P2 サイクル回帰テストが弱い**: 跨ぎノートの終端を範囲頭より後にし、127BPM（非整数サンプル境界）の
    ケースを再生側・書き出し側の両方に追加
  - 指摘のうち「前回のサイクルテストは修正前でも通る」は**実際には修正前だとFAILする**（120BPMでは
    `88200 * tps` が double で 3839.9999999999995 になり、旧条件 `endPpq <= rangeStartPpq` が偽になって
    除外されない）。ただしテストの意図が読み取りにくかったのは事実なので、上記のとおり強化した
  - 追加・強化したテストはいずれも**わざと壊すとFAILする**ことを確認（PPQ比較へ戻す／One Shotの読み飛ばしを外す／
    EditKindを捨てる の3パターン）
  - 実機再確認: 90BPMのプロジェクトで全体バウンスが変更前と同一（frames 385024・peak 0.537）で、
    4つの打点（0 / 0.667 / 1.333 / 2.0秒）と7.99秒の余韻が残っていること

- 3rdレビュー（P3 2件）の反映:
  - **One Shotのバウンス終端が1サンプル短い**: `oneShotEndSample` を `llround` → `ceil` に。
    SamplerEngineは「読み出し位置が末尾に達するまで」出力するので必要長は `ceil(remaining / rate)`。
    helperの値とエンジンの実出力サンプル数が一致することをテストで突き合わせた
    （実害は小さい: 全体バウンスは常にテール付きなので欠けた1サンプルはテール側で鳴る。ただし helper の
    契約が「鳴り切る終端」なので正しい式に直した）
  - **欠損時に `sampleSourceRate` がJSONから復元されない**: 読込でJSONの記録値を先に入れ、WAVが読めたときだけ
    実体の値で上書きするようにした。計画の「診断用にJSONへ書く」意図が、欠損状態で開いて保存し直すと
    0で上書きされて失われていた（`hasSample()` は `sampleAudio != nullptr` も見るので音の経路は不変）

- 実使用でのフィードバック（固定モードのピアノロールが「ノートを追加・編集できない」と感じる）:
  - 原因は仕様どおりのピッチ拘束（どこを押しても固定行にノートができる／縦に動かせない）。
    壊れてはいないが**画面がそれを伝えていない**（他の行も普通の128鍵に見え、固定行が画面外だと
    「押したのに何も出ない」ように見える）
  - 対応（ユーザー判断: 拘束は残して見せ方を直す）:
    - **固定ピッチのトラックは、置ける行以外をグリッド上で減光**（ノートより後に描くので、他の行に
      残っている既存ノートも減光されて「編集対象ではない」と分かる）。GMの固定ピッチ打楽器にも同じく効く
    - **ノート作成時に固定行が画面外なら、その行が見える位置へスクロール**（`ensurePitchVisible`）。
      音源の切り替え・ルート音の変更で固定行が変わったときも同様（変化したときだけ動かす）
    - **リージョン終端より右（編集範囲外）を減光＋境界線**。ピアノロールはリージョン単位の編集なので、
      終端より右は編集範囲外。グリッド線が途切れるだけでは「まだ置ける」ように見えるため、地を沈めて
      境界を示す（サンプラーとは無関係の既存挙動だが、固定行のハイライトが全幅に伸びるぶん誤解しやすい）
    - **終端より右のダブルクリックはリージョンを伸ばして、押した場所にノートを作る**（ユーザー判断）。
      従来は `clampNote` が開始位置を終端の1tick手前へ丸めていたため、「押した場所と違うところに
      打点が増える」うえ One Shot ではその1tickでもサンプルが最後まで鳴っていた。
      伸ばす単位は「元の長さが小節ちょうどなら小節単位・半端な長さ（分割後等）ならノート終端ちょうど」。
      長さの変更も同じ操作のundoに含まれる（`onWillEditModel` は作成前に積まれている）

- 実使用でのフィードバック（808を固定モードで16分連打すると「ブーン」と別の音になる）:
  - 原因は仕様どおりの「連打は重ねる」。実測で切り分け済み（合計ピーク1.176でクリップ＋うなり。
    ハイハット0.05秒は重ならず全打点0.56で正常）。単発/重ね/音量半分/モノ の4パターンを書き出して
    耳でも確認できるようにした
  - 他DAWの調査: Logicの Quick Sampler は AMP セクションに Polyphony（Mono / 1 / 2…）があり、
    808は「1」（リトリガーあり）にするのが定番。実機TR-808も各音色1ボイス。Ableton Simplerも Voices 設定。
    つまり「デフォルトは重ねる・必要なら1ボイスにする」がどのDAWでも標準
  - 対応（ユーザー判断）: **`sampleMono` トグルを1個追加**（下部エディタの VOICE 欄。既定OFF）。
    値の変更なのでスナップショット再pushは不要（`onValueEdited` 経路）＝発音中の音を切らない。
    保存はv8のJSONへ追加（キー欠損=OFF。バージョンは据え置き）

- Monoトグルのレビュー（3件）の反映:
  - **P1 追従＋Monoで古いノートのオフが新しいボイスを止める**: MIDIのnoteOffにはノート個体のIDが無く、
    Monoが先に切ったノートAのオフが後から届くと「同ピッチの未リリース最古」＝いま鳴っているBを
    リリースしてしまう（Bが早切れ）。**ピッチごとの「飲むべきオフの数」`pendingNoteOffs[128]`** を
    オーディオスレッド専用状態として持ち、Mono由来で切った追従ボイスの分だけ加算して1件ずつ無効化する。
    停止・シーク・サイクル折返し（CC123）と停止要求でクリアするので、取りこぼしても後を引かない
  - **P2 「最大2ボイス」は5ms以上間隔がある場合しか成立しない**: **一度も出力していないボイスは
    フェードせず即停止**するようにした（同じ位置に複数のnoteOnが来る＝和音や同時刻イベントのケースで
    尾が積み上がらない。未出力ボイスのフェードは振幅を足すだけで音楽的な中身がない）。
    残るのは「リリース長より短い間隔での連打」だけで、ボイス上限16で有界。この不変条件をヘッダに明記した
  - **P3 v8の `sampleMono` キー欠損互換が未テスト**: 保存JSONからキーを削除して読み込み、
    OFFになることをテストで固定した
  - 3件とも**わざと戻すとテストがFAIL**することを確認済み（計4件）

### 方針変更

- **`AudioImporter` の一時ファイル起動を `beginImportWorker()` に共通化**した（クリップ取り込みとサンプル割り当てで一時名・オーバーレイ・排他が同じため）。当初計画は「startImportと同じ方式」だけだったが、SR確定の有無が違うので入口を2本に分けた
- **サンプル欠損時のログは `SynthBank::createSampler()` から出す**ことにした（読込時の警告と二重になるが、undo/redoでの復元やGM⇄サンプル往復でも無音の理由が追えるようにするため）
