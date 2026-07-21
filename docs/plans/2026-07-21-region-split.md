# リージョン分割（カット）機能

## 概要・やりたいこと

選択中のリージョン（オーディオクリップ / MIDIリージョン）を再生ヘッド位置で2つに分割できるようにする。
Logic Proの `⌘T`（Edit > Split > Regions at Playhead）準拠。

- Tier 1スコープの「まだ作らないもの: クリップの分割」を更新する（CLAUDE.mdの該当行も修正）
- ハサミツール・マーキー選択・ロケーター分割・Option等分割は作らない
- オーディオは非破壊（WAVは書き換えない）。この構造は将来のトリム・リサイズにそのまま流用できる

## 前提・わかっていること

### Logic仕様の調査結果（採用する範囲）
- `⌘T` = 選択中のリージョンのみを再生ヘッド位置で分割。再生ヘッド直下でも非選択リージョンは切れない
- オーディオ分割は非破壊（リージョン＝ファイルへの参照＋オフセット＋長さ）
- MIDIの分割点をまたぐノートは「Keep」を採用（ダイアログなし）: 左リージョンにフル長のまま残す

### /dig-lite での決定事項
1. **非破壊参照方式**: `Clip` に `offsetSamples` / `lengthSamples` を追加し、分割後の左右クリップが同じWAV（同じ `shared_ptr<AudioBuffer>`）を共有参照する。project.json v2 → v3
2. **操作系**: `⌘T` ＋ 既存右クリックメニュー（ミュート/複製/削除）への「再生ヘッド位置で分割」追加のみ
3. **MIDIノートまたぎ**: Keep（データ加工ゼロ。再生は既存のリージョン境界マスクが止める。リージョンを後で伸ばせば尻尾が復活）

### コードベースの事実
- `Clip`（`Source/shared/Project.h:13`）: `fileName` + `startSample` + メモリ常駐バッファ。ファイル内オフセットなし。長さは `lengthSamples()` がバッファ全長を返す
- `ClipPlayback`（`Source/shared/PlaybackSnapshot.h:29`）: `audio` + `startSample` のみ。`PlaybackEngine.cpp:137` 付近がバッファ全長を再生長として使っている
- `Project::save/load`（`Source/shared/Project.cpp`）: `currentVersion = 2`。クリップは `file` / `startSample` を書き出し。保存時GC（未参照WAV削除）は `fileName` 単位の参照集合なので、複数クリップが同一WAVを共有しても自然に保護される
- **`Project::load` はクリップごとに `loadWavMono()` を呼ぶ**（`Project.cpp:195`）。分割で同一WAVを複数クリップが参照するようになると、再読込時にバッファが共有されず全量コピーが増える → ロードキャッシュが必要（Phase 1）
- テスト基盤あり: `Tests/TestsMain.cpp` ＋ CTest（`add_test(NAME daw_tests ...)`）。永続化・スナップショット・エンジン読み出しの回帰はここに足す
- 選択は単一選択（`ClipSelection` / `RegionSelection`）。Logicの「複数選択を全部切る」は考慮不要
- 再生ヘッドはシーク時に表示グリッドへスナップ済み → 分割は「再生ヘッド位置そのまま」でよい。追加スナップ・ゼロクロス吸着は作らない
- `MidiNote.startPpq` はリージョン相対。リージョン端を越えるノートは許容済み（再生時マスク）→ Keepは追加コストゼロ
- リージョン単位操作は `TimelineView::toggleMuteAt / duplicateAt`（対象を引数で明示するスタイル）＋ `showItemMenu`（`TimelineView.cpp:770`）に前例あり
- undoは `onWillEditModel`（= `undoStack.begin`）→ モデル編集 → `onModelEdited`（= pushSnapshot・dirty）の既存パターン
- `⌘T` は既存ショートカット（`MainComponent.cpp:725` 付近）と衝突なし
- クリップ/リージョンに名前はないので、Logicの `.1`/`.2` 命名は考慮不要

### 分割の計算（オーディオ）
分割位置 `pos`（絶対サンプル）が `startSample < pos < startSample + lengthSamples` のときのみ実行:
- 左: `offsetSamples` そのまま / `lengthSamples = pos - startSample`
- 右: `startSample = pos` / `offsetSamples += 左のlength` / `lengthSamples = 元length - 左length`
- `fileName`・`audio`（shared_ptr）・`muted` は両方にコピー。peakCacheは左右とも再構築
- 境界ちょうど・範囲外は no-op

### 分割の計算（MIDI）
再生ヘッドのサンプル位置をPPQへ換算（最近傍tickへ丸め）し、`startPpq < splitPpq < startPpq + lengthPpq` のときのみ実行:
- 左: `lengthPpq = splitPpq - startPpq`。ノートは全て残す（またぎノートもフル長のまま = Keep）
- 右: 新規ID採番（`allocateId`）/ `startPpq = splitPpq` / `lengthPpq = 残り`。`startPpq >= splitPpq` のノートを相対シフトして移動
- `muted` は両方にコピー

## 実装計画

### Phase 1: Clipの非破壊参照化（offset/length導入）[AI🤖]
- [x] `Clip` に `offsetSamples` / `lengthSamples` フィールドを追加（既存メソッド `lengthSamples()` と名前が衝突するので、メソッドを廃止してフィールド参照へ置換。呼び出し箇所をgrepで洗う）
- [x] `Clip::buildPeakCache()` を `[offsetSamples, offsetSamples + lengthSamples)` のスライス対象に変更
- [x] `ClipPlayback` に `offsetSamples` / `lengthSamples` を追加し、`buildSnapshot()` と `PlaybackEngine`（クリップ長・読み出し位置の計算）を対応
- [x] `Project::save`: `currentVersion = 3` に上げ、クリップに `offsetSamples` / `lengthSamples` を書き出し
- [x] `Project::load` に `fileName` 単位のロードキャッシュを置き、同一WAVを参照するクリップは同じ `shared_ptr` を再利用する（分割済みプロジェクトの再読込でバッファが複製されるのを防ぐ）
- [x] `Project::load`: v2以前は `offsetSamples = 0` / `lengthSamples = バッファ全長`。v3は不正値をクランプ（不正値・WAV差し替え対策）。**クランプ順序**: 先に `offsetSamples` を `0..bufferLength` に収め、次に `lengthSamples` を `0..bufferLength - offsetSamples` に収める（`offset + length` を先に計算すると手編集JSONの極端な整数値でオーバーフローし得るため）
- [x] クリップ描画（LaneContent）と `duplicateAt` のオーディオ複製が offset/length に追従していることを確認
- [x] CTest（`Tests/TestsMain.cpp`）に回帰テストを追加:
  - v2読込時に `offsetSamples = 0` / `lengthSamples = WAV全長` になること
  - v3の不正 offset/length がクランプされること（オーバーフローを誘発する極端値を含む）
  - 同一WAVを参照する2クリップの保存 → 再読込で `audio` の `shared_ptr` が共有されること
  - `buildSnapshot()` が offset/length を `ClipPlayback` に伝播すること
  - 再生エンジンが offset付きの左右クリップを「連続した元音源」として読むこと（現状はバッファ全長をクリップ長として扱っているため直接検証する。`PlaybackEngine.cpp:136` 付近）
- [x] ビルド＋`ctest` が通り、既存プロジェクト（v2）が警告なしで開けて再生できることを確認

### Phase 2: 分割ロジック＋操作系 [AI🤖]
- [x] 分割計算は純粋関数としてモデル層（`shared/`）に置く（例: `splitClip (const Clip&, int64 pos)` / `splitMidiRegion (const MidiRegion&, int64 splitPpq, ...)` が左右ペア or 失敗を返す）。UIから切り離してユニットテスト可能にする
- [x] `TimelineView::splitAtPlayhead (int trackIndex, int itemIndex)` を追加（`toggleMuteAt` / `duplicateAt` と同列。undoパターン: `onWillEditModel` → 編集 → `onModelEdited`）。分割後の選択は左側（indexそのまま）を維持
- [x] CTestに分割計算のテストを追加: オーディオ左右の offset/length/startSample、境界ちょうど・範囲外の no-op、MIDIのまたぎノートKeep・右リージョンへの相対シフト移動
- [x] `MainComponent::keyPressed` に `⌘T` を追加: 選択中のクリップ or MIDIリージョンを分割。選択なし・範囲外は no-op
- [x] `showItemMenu` に「再生ヘッド位置で分割」を追加（再生ヘッドが対象リージョンの内側にないときは disabled 表示）
- [x] ログイベントを追加（`region.split track=.. item=.. pos=..`。既存の `region.mute` / `region.duplicate` の命名に合わせた）

### Phase 3: 動作確認 [AI🤖]
- [x] `ctest` 全件パス（Phase 1・2で追加した回帰テスト含む。全20テスト・FAILなし）
- [x] ビルド＋旧インスタンス終了→起動（VERIFY.md手順）
- [x] テストプロジェクトで右クリックメニュー経由の分割を実行（右クリック合成は不可と判明 → Ctrl+左クリックで代替。詳細はログ参照）
- [x] 成果物検証: 保存後の project.json で「同一 `file` を参照するクリップが2個・`offsetSamples`/`lengthSamples` が連続・`startSample` が分割点」を確認。アプリログで `region.split` の着弾も裏取り
- [x] v2プロジェクトの読込互換（v2形式のテストプロジェクトを開いて分割・保存 → v3で再読込し分割状態が復元されること）
- [x] MIDIリージョン分割: またぎノートが左リージョンに残り、右リージョンのノートが相対シフトされていることを project.json で確認
- [x] CLAUDE.md の Tier 1「まだ作らないもの」から分割を外す。VERIFY.md に再利用可能な確認手順があれば追記

### 動作確認 [人間👨‍💻]
- [ ] `⌘T` での分割（合成キーストロークはJUCEアプリに効かないため人間が確認）
- [ ] undo/redo: 分割 → `⌘Z` で元の1クリップ/リージョンに戻ること（モデル層のundoは既存CTestでカバー済み。実機のキー操作のみ確認）
- [ ] 分割点をまたいで再生し、クリックノイズ・音の欠落がないか耳で確認
- [ ] 分割後の右クリップをミュート/複製して違和感がないか

## ログ

### 試したこと・わかったこと
- CGEventの `.rightMouseDown` 合成ではJUCEの右クリックメニューが開かない（選択はされるがポップアップトリガー扱いにならない。clickState/buttonNumber明示・hid/sessionタップとも不可）。**Ctrl+左クリック**（flagsに `.maskControl` を直接セット・`.cgSessionEventTap` へpost）で開けた
- メニュー表示中に別プロセスのAppKitリンクCLIを起動するとメニューが閉じる → 「開く→CGWindowListでmenuウィンドウ検出→項目クリック」を1プロセス内で完結させて解決（手順はVERIFY.mdへ記載）
- 実機確認の結果: 分割後のproject.jsonは同一WAV参照2クリップ（offset 0/88200で連続）・MIDIまたぎノートKeep・右リージョン新ID採番、いずれも期待どおり。再読込でも復元・WAVのGC誤削除なし

### 方針変更
- ログイベント名はplanの例 `edit.split` でなく既存の `region.mute`/`region.duplicate` に合わせて `region.split` にした
- 「undo/redoの実機確認」は合成キーがJUCEに届かないため人間確認へ移動（モデル層のundoは既存CTestのUndoStackテストでカバー）
- CLAUDE.mdの「まだ作らないもの」から、分割と合わせて「MIDI編集」も外した（ピアノロール実装済みでリストが実態と乖離していたため）
