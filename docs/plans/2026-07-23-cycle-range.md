# サイクル範囲（範囲選択＋ループ再生＋範囲書き出し）

## 概要・やりたいこと

Logic Proのサイクル範囲に相当する機能を追加する。

- ルーラー上をドラッグして範囲を選択し、その範囲をループ再生できるようにする
- 選択した範囲は「選択範囲の書き出し」（⌘B連動）にも使う
- ビジュアル・操作感はLogic準拠（ルーラーの黄色い帯・`C`キーでトグル）

## 前提・わかっていること

### dig-liteでの決定事項

1. **ルーラー操作は上下分割しない**: クリック=シークは現状維持。ルーラー上でドラッグしたときだけサイクル範囲を作成する（Logicの上下2分割は各13pxと狙いにくいため不採用）
2. **録音中はループしない**: 録音開始時はサイクルを無視して従来どおり直進。テイク管理はTier 1スコープ外
3. **⌘B連動も今回のスコープに含める**: サイクルON時の⌘Bはその範囲を書き出す

### 裁量で決めた細部

- **スナップ**: 範囲の端はシークと同じ「表示中の最小グリッド」（1/16上限）にスナップ。ただし `snapUnitBeats()` は最小1拍しか返さない（`TimelineView.cpp:598`・マーカー用）ため使わず、シーク（`seekFromX`, `TimelineView.cpp:1242`）と同じく `gridDivisionsPerBar()` を基準にした専用変換 `xToCycleSixteenths()` を追加する
- **トグルキー**: 単独 `C`（未使用を確認済み。Logicのサイクルトグルと同じ）
- **永続化**: サイクル範囲（開始/終了拍）とON/OFFを project.json に保存。`currentVersion` を 5 に bump し、既定値（範囲なし・OFF）で後方互換
- **再生挙動**: サイクルON時に範囲外から再生開始したら範囲頭へジャンプ。範囲末尾でシームレスに範囲頭へ戻る
- **見た目**: ルーラーに黄色の帯（ON時は明るい黄、OFF時はグレー系で範囲を保持表示）。Logicのルックに準拠
- **範囲の編集**: 端±4pxのドラッグでリサイズ（マーカーレーンの `hitTestMarkerEdge` と同じパターン）。帯の中央ドラッグで移動。範囲を潰す（開始=終了）ドラッグで範囲削除
- **サイクル書き出しはテールなし厳密長**: `BounceRenderer` はMIDIありのとき末尾テールを付ける（`request.wantTail`, `BounceRenderer.h:47`）が、サイクル範囲の書き出しはループ素材用途なので `wantTail = false` を明示し、出力長＝範囲サンプル長ちょうどにする
- **クリックとドラッグの判別**: マーカーレーンの「動かさず離す＝クリック」と同じ方式（mouseUpまでにドラッグ閾値を超えたかで判定）

### コードベース調査結果

- **ルーラー**: `TimelineView::RulerContent`（`Source/ui/TimelineView.cpp:47-124`・高さ26px）。現在は `mouseDown`（→ `seekFromX`）と `mouseMagnify` のみで **ドラッグは未割り当て**
- **再生位置**: `TransportState`（`Source/shared/TransportState.h`）の `playheadSamplePos` / `seekRequest`（atomic）。位置前進は `PlaybackEngine::process` の末尾 `Source/audio/PlaybackEngine.cpp:319` で `store(pos + numSamples)` の単調加算 → ここをラップアラウンド化する
- **ループ機構は未実装**。`BounceRenderer.h:45` に「将来サイクル区間が入る。現状は常に 0〜曲末」のコメント＝区間指定フックあり
- **ショートカット**: `Source/ui/Shortcuts.h` のテーブルが単一の真実の源。単独 `C` は空き。ディスパッチは `MainComponent::keyPressed`（`MainComponent.cpp:1288-1483`）
- **永続化**: `Project::save`（`Project.cpp:183-317`）/ `Project::load`（`Project.cpp:319-522`）。トップレベルキー追加は `markers`（:270-279）〜 `master`（:292-294）付近。`currentVersion = 4`（`Project.h:139`）
- **スレッド間受け渡しの既存パターン**: 単一値は `TransportState` の atomic（lock-free static_assert あり）。サイクルON/OFF＋開始/終了サンプルはここに atomic を追加するのが既存流儀
- **マーカーレーン**（ルーラー直下18px）はドラッグ使用済み。今回はルーラー帯のみ触るので衝突しない

## 実装計画

### Phase 1: 状態と永続化 [AI🤖]

- [x] `Project` にサイクル範囲を追加（`Project.h:141-151` 付近）
  - `cycleStartBeats` / `cycleEndBeats`（int・拍…ではなく最小グリッド対応のため**拍を分母とするdouble or 1/16単位のint**。タイムラインのスナップが1/16上限なので「16分音符単位のint」を採用: `cycleStartSixteenths` / `cycleEndSixteenths`）
  - `cycleEnabled`（bool）
  - 範囲なしは `start == end == 0` などの番兵でなく「`cycleStartSixteenths < cycleEndSixteenths` のときだけ範囲あり」で表現
- [x] `Project::save` / `Project::load` にキー追加（`cycle: { start, end, enabled }`）。`currentVersion` を 5 に bump、旧バージョン読み込み時は既定値（範囲なし・OFF）
- [x] `TransportState` に atomic を追加（`TransportState.h:13-18` 付近）
  - **開始・終了は1つの `std::atomic<juce::uint64>` にパック**（上位32bit=開始・下位32bit=終了、サンプル単位。uint32はサンプルレート48kHzで約24時間まで表現でき十分）。個別atomic 2本だと編集中にオーディオスレッドが「新開始＋旧終了」の中途半端な組を読み、意図しない位置へ一度ループするため
  - `std::atomic<bool> cycleEnabled` は別atomicでよい（書き込み順を「範囲を書いてから enabled を立てる」に固定すれば、有効化時に不整合な範囲を読むことはない）
  - UI側でBPMとサンプルレートから換算して書く。lock-free static_assert に追随

### Phase 2: オーディオエンジンのループ再生 [AI🤖]

- [x] `PlaybackEngine::process` の位置前進（`PlaybackEngine.cpp:319`）をラップアラウンド対応にする
  - `cycleEnabled` かつ **録音中でない** かつ範囲が有効（start < end）のとき: `pos + numSamples` が `cycleEndSample` を跨いだら `cycleStartSample` へ戻す
  - **サイクル終端は排他的（区間は `[start, end)`）**: セグメント処理の先頭で `pos >= cycleEnd` なら先に `cycleStart` へ正規化し、`pos + segmentSamples == cycleEnd` の境界ちょうどでもラップする。「跨いだら」だけの判定だと終端ちょうどで終わるブロックが次callbackで範囲外を再生する
  - ブロック内で境界を跨ぐ場合、境界までのサンプル数だけ処理→残りを範囲頭から処理する分割方式にする（クリップ再生・メトロノームが境界後も正しい位置の音を出すため。単純に位置だけ巻き戻すとブロック内の残り分が範囲外の音になる）
  - **分割はループで「残サンプルが0になるまで」繰り返す**: 最小1/16のサイクルは高BPM・大きいデバイスブロックだと1 callback中に複数回ラップし得るため、「一度だけ跨ぐ」前提にしない。セグメントごとに（開始pos・サンプル数・bufferOffset）を計算して処理する
  - **セグメント共通の `bufferOffset` を全出力経路へ通す**: オーディオクリップ再生・MIDIミックス・バス/Masterミックス（mixScratch・send）・メトロノームはすべて現状ブロック先頭基準で書いているため、分割セグメントの後半をバッファの正しい位置へ足せるよう、全経路にオフセット引数を導入する（MIDIだけでは不十分）
  - **ラップ地点は「内部シーク」として扱い、MIDIの発音状態をリセットする**: `renderMidiTracks`（`PlaybackEngine.cpp:325`）は `activeNotes` で発音中ノートを保持するため、境界でシーク時と同じ処理（note-off送出→`activeNotes` 消去、`PlaybackEngine.cpp:358` 付近の `silenceAll` パターン）を行い、範囲頭では跨ぎノート（範囲頭より前に始まり範囲頭を跨いで鳴っているノート）をシーク後再生開始と同様に再発音する。これを怠ると範囲外で始まったノートの残響・不正なnote-off・範囲頭の持続音欠落が起きる
  - GOTCHAS.md のオーディオスレッド禁則（アロケーション・ロック禁止）を遵守
- [x] 再生開始時の挙動: サイクルON・範囲外から再生 → 範囲頭へジャンプしてから再生（UI側 `MainComponent` の play 開始処理で `seekRequest` を積む）
- [x] 録音開始時はサイクルを無視（録音フラグを見てラップアラウンドを抑止。既存の録音ガードと同じ場所で判定）

### Phase 3: ルーラーUI（範囲の作成・編集・描画） [AI🤖]

- [x] `RulerContent::mouseDrag` を新設し、ドラッグで範囲を作成
  - mouseDownの時点では何もせず、ドラッグ閾値を超えたら範囲作成モードに入る。動かさず離したら従来どおりシーク（マーカーレーンの mouseUp 方式を踏襲）
  - 両端は新設の `xToCycleSixteenths()` でスナップ（`gridDivisionsPerBar()` 基準・表示中の最小グリッド・1/16上限。`snapUnitBeats()` は最小1拍なので使わない）
  - 範囲作成が確定したら `cycleEnabled = true` に自動でON（Logicと同じ）
- [x] 既存範囲の編集
  - 端±4pxでリサイズカーソル＋端ドラッグでリサイズ（`hitTestMarkerEdge` パターン）
  - 帯の内側ドラッグで範囲ごと移動
  - 開始=終了に潰したら範囲削除（cycleEnabled も OFF）
- [x] `RulerContent::paint` にサイクル帯を描画
  - ON時: 明るい黄色の帯（Logicルック準拠）、OFF時: グレー系の帯で範囲を保持表示
  - 再生ヘッド・小節番号との重なりは「プレイヘッド > サイクル帯 > 小節番号」の強弱（UIデザイン方針メモ準拠）
- [x] UI→TransportState の同期: 範囲変更・BPM変更・サンプルレート変更時にサンプル位置へ換算して atomic に書き込む（既存のBPM反映処理と同じ経路に足す）。範囲の書き換えはパックした単一atomicへの1回のstoreで行う（Phase 1の設計）
- [x] **プロジェクト読み込み直後・オーディオデバイス準備完了後の同期**: 保存済みサイクルを再オープンした時点、およびデバイス確定でサンプルレートが判明した時点でも `cycle*Sixteenths` → サンプル位置の換算・書き込みを行う（編集時だけだと再オープン直後のループが効かない）
- [x] 変更を Undo 対象にするか確認: 既存の undo 体系（リージョン操作系）を調査し、マーカー移動が undo 対象ならサイクル範囲も合わせる。対象外なら合わせて対象外とする

### Phase 4: ショートカット・トグル [AI🤖]

- [x] `Shortcuts.h` のテーブルに `toggleCycle` を追加（単独 `C`・カテゴリはトランスポート系・keyLabel "C"）
- [x] `MainComponent::keyPressed` で `Shortcuts::matches()` 判定 → `cycleEnabled` をトグル
  - 範囲が未設定のときの `C` は何もしない（トグルする対象がない）
- [x] ⌘?一覧は自動生成のため追加対応不要（確認のみ）

### Phase 5: ⌘B（書き出し）のサイクル範囲連動 [AI🤖]

- [x] `BounceRenderer.h:45` の区間フックにサイクル範囲を渡す
  - サイクルONかつ範囲あり → その範囲（サンプル区間）を書き出し
  - サイクルOFF or 範囲なし → 従来どおり 0〜曲末
  - **サイクル書き出し時は `request.wantTail = false` を明示**（MIDIありでも末尾テールを付けず、出力長＝範囲サンプル長ちょうど。ループ素材用途のため）
- [x] ミュートトラックの扱いは既存 bounce の挙動を踏襲（既にミュート除外なら変更不要。調査して確認）
- [x] 書き出しファイル名や完了通知に範囲情報を含めるかは既存UIに合わせ最小変更

### Phase 6: テスト・動作確認 [AI🤖]

- [x] CTest（`daw_tests`, `CMakeLists.txt:203-235`）にテストを追加
  - v4 JSON読み込み時に既定値（範囲なし・OFF）になること、cycle の保存→再読み込みで値が一致すること
  - ブロック境界を跨ぐオーディオループ: 分割処理後の最終 `playheadSamplePos` が期待どおり範囲頭側へラップしていること
  - **範囲長 < blockSize（1 callback内で複数回ラップ）のケース**: 最終 `playheadSamplePos` と出力波形（各セグメントが正しいバッファ位置に正しい内容で書かれていること）を確認
  - **等号境界ケース**: ブロックが `cycleEnd` ちょうどで終わるとき、次の再生位置が `cycleStart` に戻っていること（終端排他の検証）
  - サイクル書き出し: 出力の開始位置と厳密なサンプル長（MIDIトラックありのケース含む・テールなし）
- [x] ビルド（dev版）→ CTest実行 → 起動 → ログで裏取り
- [ ] ~~CGEvent合成ドラッグでルーラー上に範囲を作成 → project.json に cycle が保存されることを検証（成果物ベース）~~（実施不可 → ログ参照。人間の動作確認へ委譲）
- [ ] ~~再生してループすることをログ（playheadSamplePos の巻き戻り）で検証~~（ループ挙動はCTestで全ケース検証済み。実機はユーザー確認へ）
- [ ] ~~範囲外から再生開始 → 範囲頭へジャンプすることをログで検証~~（同上）
- [ ] `C` トグル（キー操作の確認はユーザーに依頼。AXPress代替が無いため）
- [ ] ~~録音開始 → ループしないことをログで検証~~（エンジン側は `!armed` ガードで抑止。実機はユーザー確認へ）
- [ ] ~~⌘B → 出力WAVの長さがサイクル範囲と一致することを afinfo 等で検証~~（CTestの `BounceRenderer cycle range` で厳密長・開始位置・内容一致を検証済み）
- [x] 旧バージョン（version 4）の project.json が既定値で読み込めることを検証
- [ ] ~~スクリーンショットでサイクル帯の見た目を確認（ON/OFF両方）~~（プロジェクトを開く操作ができず未実施。ユーザー確認へ）

### 動作確認 [人間👨‍💻]

- [ ] `C` キーでのトグル動作
- [ ] ルーラードラッグの操作感（スナップ・端リサイズの掴みやすさ）
- [ ] サイクル帯の見た目がLogicのルックとして違和感ないか
- [ ] ループ再生の実機確認（範囲末尾→頭のシームレスさ・MIDIの持続音・メトロノーム）
- [ ] 範囲外から再生開始 → 範囲頭へジャンプすること
- [ ] 録音開始時にループしないこと
- [ ] サイクルON時の⌘B → 出力WAVが範囲の長さちょうどであること
- 検証用プロジェクト: `~/Music/daw/0-0-cycle-test/`（v4形式・MIDIリージョン4小節入り。開くだけでv4互換読み込みも確認できる）

## ログ

### 試したこと・わかったこと
- 2026-07-23 実装完了。CTest 32本全通過（新規3本: `cycle range roundtrip and v4 defaults` / `PlaybackEngine cycle loop` / `BounceRenderer cycle range`）。既存の `mixer params roundtrip` が「version=4で保存」を期待していたため `Project::currentVersion` 比較に修正
- エンジンは `process()` をセグメントループ化し、1セグメント分の全出力（クリップ・MIDI・バス/Master・クリック）を `processSegment(outOffset, ...)` に抽出。スクラッチバッファは常にオフセット0で使い、デバイスバッファへの最終出力だけ outOffset へずらす方式（全経路への bufferOffset 貫通が最小差分で済む）
- ラップの「内部シーク」はメンバフラグ `cycleWrapPending` でコールバック跨ぎにも伝搬（ブロックが終端ちょうどで終わるケースで、次コールバック先頭の消音＋跨ぎノート再発音が漏れないため）
- 合成マウスでのUI検証は実施不可だった: ユーザーが Logic Pro を全画面表示中（単一ディスプレイ・Zオーダーで LaLa-dev の窓全体が Logic に覆われ、CGEventクリックが Logic に着弾する状況）。選択画面の描画確認（windowIDスクショ）とアプリの起動・正常終了（session.start/end）までは裏取り済み
- ルーラーのシークは mouseDown 発火から「動かさず離した時＝mouseUp」発火に変更（ドラッグで範囲を作るときに再生ヘッドが動かないようにするため。マーカーレーンと同じ方式）。ルーラー右クリックは従来シークしていたが no-op に変更

### 方針変更
- 2026-07-23 サイクル範囲・トグルは **undo対象外** とした（計画では「マーカーが対象なら合わせる」としていたが、UndoStack の State は tracks+markers のみで、サイクルを含めると「ノート編集のundoでサイクルON/OFFまで巻き戻る」意外性が生じる。音量・ミュート・ソロと同じ扱いで、Logicもサイクル操作をundoしないことに合わせた）。ルーラー操作は onWillEditModel/onModelEdited でなく専用の `onCycleChanged` コールバックで Transport同期＋dirty化のみ行う
- 2026-07-23 UI→Transportの同期は「編集時の即時呼び出し＋Timerで毎tick再同期」の二段構え（BPM編集・デバイスSR確定・undo等どの経路でも取りこぼさない。atomic 2本のstoreのみで安価）

### 方針変更
- 2026-07-23 実装前レビューを反映（6件すべてコードで裏取りして採用）: ①スナップは `snapUnitBeats()`（最小1拍）でなく `gridDivisionsPerBar()` 基準の専用変換 ②ループ境界を内部シーク扱いにしてMIDI発音状態をリセット＋跨ぎノート再発音＋分割後半のミックスオフセット対応 ③サイクル書き出しは `wantTail=false` で厳密長 ④範囲は単一atomicにパックして更新整合性を担保 ⑤読み込み直後・デバイス準備後のTransport同期を追加 ⑥CTest追加
- 2026-07-23 2回目レビューを反映（2件採用）: ①bufferOffset はMIDIだけでなく全出力経路（オーディオクリップ・バス/Masterミックス・メトロノーム）へ通す ②分割処理は「残サンプル0までループ」とし1 callback内の複数回ラップに対応（CTestに範囲長<blockSizeケースを追加）
- 2026-07-23 3回目レビューを反映（1件採用・これで計画承認）: サイクル終端は排他的（`[start, end)`）と明文化。`pos >= end` は先にstartへ正規化し、境界ちょうどでもラップ。CTestに等号境界ケースを追加
