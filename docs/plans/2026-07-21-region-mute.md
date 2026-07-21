# リージョン単位のミュート

## 概要・やりたいこと

- タイムライン上のリージョン（オーディオクリップ・MIDIリージョン）を個別にミュートできるようにする
- 操作は2系統:
  - リージョンを右クリック → メニューから「ミュート/ミュート解除」「複製」「削除」
  - 選択中のリージョンに `Ctrl+M`（Logic デフォルトの「リージョンをミュート/ミュート解除」準拠）
- トラックミュート（`m` キー）はトラック全体、こちらはリージョン単体という使い分け

## 前提・わかっていること

- リージョンは2種類: オーディオトラックの `Clip`（`Source/shared/Project.h`）と MIDIトラックの `MidiRegion`。**両方ミュート対象**
- 選択状態は `TimelineView::ClipSelection` / `RegionSelection`（単一選択のみ）。複数選択は存在しないので今回のスコープ外
- タイムラインに右クリックメニューは現状存在しない（新設）。`TrackHeadersView.cpp:214` に `PopupMenu::showMenuAsync` の先行例あり
- ミュート状態は `muted` フラグとしてモデルに持たせ **project.json に永続化**。デフォルト false の後方互換読み込みなのでプロジェクトバージョンの bump は不要
- 再生への反映は既存の `buildSnapshot()` 再構築パターン（ミュート中のクリップ/ノートをスナップショットから除外）。`TrackParams` の atomic 方式は使わない
- **Undo対象にする**（ユーザー決定）: `onWillEditModel`（= `undoStack.begin`）→ 編集 → `onModelEdited`（= `pushSnapshot` + dirty）の既存パターンに乗せる。undoスナップショットはプロジェクト全体なのでフラグ追加だけで乗る
- 複製の配置先は**元リージョンの終端直後**（Logic のリピート ⌘R 相当）
  - MIDIリージョン: ⌥ドラッグ複製の既存ロジックを流用。リージョン・ノートのIDは `allocateId()` で再採番
  - オーディオクリップ: 複製は新規実装。`fileName` と `audio`（shared_ptr）は元クリップと共有できるが、**`peakCache` は `std::vector<float>` なので共有不可、値コピーする**（表示用データのみなので現スコープで共有ポインタ化はしない）。save() の WAV GC は参照中ファイルを保護するので同一ファイルを2クリップが参照しても消えない
- `showMenuAsync` のコールバックは後から呼ばれるため、`this` や選択状態を直接参照すると TimelineView／プロジェクト破棄後に危険。`TrackHeadersView.cpp:218` と同様に `Component::SafePointer` で寿命確認する。オーディオクリップは永続IDを持たない（複製後は同一 `fileName` のクリップが並ぶ）ため、コールバック内では右クリック時に捕捉した track/clip インデックスを範囲チェックしてから操作する。非同期削除の厳密な対象同定（Clip への永続ID追加）は現スコープでは不要と判断
- 表示は Logic 同様、ミュート中リージョンをグレー減光
- `MainComponent::keyPressed` の1文字ショートカット switch は Cmd/Ctrl/Alt を除外しているので、`Ctrl+M` はその手前で `juce::KeyPress ('m', ModifierKeys::ctrlModifier, 0)` を明示判定する
- 削除は既存の `deleteSelectedRegion()` / `requestDeleteSelectedClip()`（MainComponent側）があるので、メニューの「削除」はコールバックで委譲する

## 実装計画

### Phase 1: モデル・永続化・再生反映 [AI🤖]
- [ ] `Clip` / `MidiRegion` に `bool muted = false` を追加（`Source/shared/Project.h`）
- [ ] `Project::save` / `load` で `muted` を読み書き（欠落時 false）
- [ ] `buildSnapshot()` でミュート中のクリップ・リージョンのノートを除外
- [ ] ビルドが通ることを確認

### Phase 2: Ctrl+M ショートカット・減光表示 [AI🤖]
- [ ] `TimelineView` に**対象を引数で受ける**ミュートトグル API を追加（例: `toggleMuteAt (track, index, type)`。`onWillEditModel` → フラグ反転 → `onModelEdited` を通す）。「現在の選択」を暗黙に読む形にはしない
- [ ] `MainComponent::keyPressed` に `Ctrl+M` を追加（クリップ選択 or リージョン選択の対象を取り出して `toggleMuteAt` に渡す。選択なしなら何もしない）
- [ ] `LaneContent::paint` でミュート中のクリップ・リージョンをグレー減光表示（波形・ノートプレビュー含む）
- [ ] Undo/Redo でミュート状態が戻ることを確認

### Phase 3: 右クリックメニュー [AI🤖]
- [ ] `handleLaneMouseDown` で `e.mods.isPopupMenu()` を判定。リージョン/クリップ上ならまず選択（ドラッグ開始はしない）→ `PopupMenu::showMenuAsync` で「ミュート/ミュート解除」「複製」「削除」を表示。リージョン外の右クリックはメニューを出さない
- [ ] メニューコールバックの寿命・対象の安全確保: `Component::SafePointer<TimelineView>` で生存確認し、右クリック時点の対象（track/clip または track/region のインデックス＋種別）をコールバックに捕捉。実行時に project 非null＋インデックス範囲チェックしてから、**捕捉した対象を引数で各操作 API に渡す**（「現在の選択」経由では操作しない）
  - 範囲チェックは対象同一性までは保証しないが、**メニュー表示中はモーダルで他の編集操作が発生せずインデックスは変化しない**、という前提を許容する（PopupMenu 表示中はマウス・キー入力がメニューに奪われるため）
- [ ] ミュート: 捕捉対象を `toggleMuteAt`（Phase 2）に渡す
- [ ] 複製: 対象を引数で受ける `duplicateAt (track, index, type)` を追加し、元の終端直後に配置。MIDIリージョンは⌥ドラッグ複製ロジックを流用しID再採番、オーディオクリップは `fileName`/`audio` を共有・`peakCache` を値コピーするエントリ追加で新規実装。両方 Undo 対象
- [ ] 削除: 対象を引数で受けるコールバックで MainComponent に委譲し、**対象指定版の削除 API を追加する**（`requestDeleteClipAt (track, clip)` / `deleteRegionAt (track, region)`）。既存の `requestDeleteSelectedClip()` / `deleteSelectedRegion()` は選択を読んで対象指定版を呼ぶ薄いラッパーに書き換え、「現在の選択を読む」経路と「捕捉対象を渡す」経路を混在させない。オーディオクリップの削除確認ダイアログのコールバックにも同じ track/clip を捕捉し、実行時に範囲チェックする
- [ ] ビルドが通ることを確認

### 動作確認 [AI🤖]
- [ ] ビルド・起動（旧インスタンスの終了確認を含む。VERIFY.md 参照）
- [ ] CGEvent合成マウスで右クリック → メニュー項目クリック（PopupMenu は AXPress 不可のため。操作間1秒以上空ける）
- [ ] ミュート後に ⌘S 保存 → project.json の `muted` フラグを裏取り。複製・削除も project.json の値で検証
- [ ] スクショで減光表示を確認
- [ ] 旧 project.json（`muted` なし）が警告なく読めることを確認

### 動作確認 [人間👨‍💻]
- [ ] `Ctrl+M` でミュートがトグルされるか（合成キー送信は不確実なためキー操作は人間が確認）
- [ ] 再生してミュート中リージョンが鳴らないこと・解除で鳴ることを耳で確認

## ログ
### 試したこと・わかったこと
（実装中に随時追記）

### 方針変更
（実装中に随時追記）
