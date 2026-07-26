# 下部エリアの表示トグルと履歴ナビゲーション（E / [ / ]）

## 概要・やりたいこと

画面下部のエリアは「1つの枠を複数の中身が奪い合う」場所になっている。今そこに入るのは2つだが、
**枠は2つでも中身は別物**で、行き来の単位としては別画面として数える必要がある。

| 中身 | 単位 | 状態 |
|---|---|---|
| ピアノロール | トラック × リージョン | 実装済み |
| サンプル音源エディタ（`InstrumentDetailView`） | トラック（サンプラー使用時） | 実装済み |
| EQ | チャンネル（トラック / バス3本 / Master） | 将来 |
| Comp | 同上 | 将来 |
| Ext | 同上 | 将来 |

つまり行き来の候補は「2種類」ではなく **チャンネル数 × スロット数 ＋ リージョン数** まで広がる。
今でも「Pianoトラックのピアノロール → 808トラックのサンプル調整 → またPianoのピアノロール」
のような3点の行き来が起きうる。

### 現状の3つの不満

1. **キーボードで閉じられない** — `×` クリックのみ。Shortcuts.h に該当エントリがない
2. **閉じると何を見ていたか忘れる** — 開き直すには元の導線（リージョンをダブルクリック／`I` → スロットクリック）を辿り直す
3. **往復が毎回2〜3操作** — 打ち込み ⇄ 音色調整の行き来が支配的なのに、切り替えが重い

### 目指す状態

```
E    下部エリアの表示/非表示（閉じても中身は記憶し、E で復元）
[    履歴を戻る
]    履歴を進む

履歴の例:
 [PR: Piano 小節2] → [サンプル: 808_quality_C] → [PR: 808 小節5]  ← いまここ
                  [ で1つ前へ、] で戻った先へ進む
```

## 前提・わかっていること

### 現状の実装（確認済み）

| 項目 | 現状 | 場所 |
|---|---|---|
| 下部の枠 | `pianoRoll` と `fxDetail` の排他・後勝ち | `MainComponent.cpp:2657` |
| ピアノロールを開く | リージョンのダブルクリック → `openPianoRoll(trackIndex, regionIndex)` | `MainComponent.cpp:1094` |
| ピアノロールを閉じる | 同じリージョン再ダブルクリック／`×` → `closePianoRoll()` | 同 1114 |
| FX詳細を開く | 左FXパネルのスロットクリック → `toggleFxDetailSlot(slot)` | 同 1191 |
| FX詳細を閉じる | 同じスロット再クリック／`×` → `closeFxDetail()` | 同 1209 |
| FX詳細の中身 | Instrumentスロットのとき `fxDetail.setBody(&instrumentDetail)` | 同 1267 |
| 高さ | 両パネル共通・セッション内保持（`bottomPanelHeight = 320`） | `MainComponent.h:178` |
| 表示対象の識別 | `fxDetailSlot`（スロット番号）＋ `fxDetailKey`（`"track"`/`"bus0..2"`/`"master"`） | `MainComponent.h:158-159` |
| FXパネルの表示トラック | `fxEditor.shownTrack()`（選択トラックとは独立） | `FxEditorView.h:46` |
| ピアノロールの表示対象 | `currentTrackId()` はある。**`currentRegionId()` は無い**（`shownRegionId` はprivate） | `PianoRollView.h:34` |
| ID | `Track::id` / `MidiRegion::id` ともに `juce::uint64`・永続化される | `Project.h:136,64` |
| 履歴クラスの前例 | `AudioBrowserNavigation::History`（`visit`/`canMove`/`move`のブラウザ型） | `shared/AudioBrowserNavigation.h` |
| テスト基盤 | `Tests/TestsMain.cpp` ＋ CTest。`ui/` のヘッダオンリークラスもテスト済み | `CMakeLists.txt:209-244` |

### 既存の open 関数がすでにやっていること（復元処理で書き直さない）

復元を独自に書くと処理が二重管理になるため、**既存の `openPianoRoll()` / `toggleFxDetailSlot()` を
再利用する**。両者は下記を漏れなく行っている。

```
openPianoRoll()       closeFxDetail()（排他）→ openRegion → resized() → applyPendingScroll()
                      MainComponent.cpp:1109-1112
toggleFxDetailSlot()  closePianoRoll()（排他）→ fxDetailSlot/fxDetailKey 設定 → fxDetail.show()
                      → fxEditor.setActiveSlot() → updateFxDetailBody() → resized()
                      MainComponent.cpp:1198-1206
```

`closeFxDetail()` は `fxDetailSlot = -1` / `fxDetailKey.clear()` / `setActiveSlot(-1)` まで消す
（同 1216-1218）ので、復元でこれらを設定し忘れると `updateFxDetailBody()` の Instrument 判定が
`slot == -1` で落ち、本文もハイライトも戻らない。既存関数を通せばこの穴は自動的に塞がる。

### 決定事項（この会話で確定）

- **`E` = 下部エリアの表示/非表示**
  - Logic Pro のデフォルト `E` = Show/Hide Editor に合わせる。既存の `X`（ミキサー）`I`（インスペクタ）
    `F`（ブラウザ）と同じ「表示トグルは修飾なし1文字」の系列
- **`[` / `]`（修飾なし） = 履歴を戻る/進む**
  - Logic には「エディタの履歴を戻る」概念自体がない（Logicは下部エディタをタブ切り替え）ため、
    ここは Logic 準拠が使えず LaLa 独自に決める部分
  - すでに `⌘[` / `⌘]` がファイルパネルの戻る/進む（`Shortcuts::ID::browserHistory`）なので、
    「`[]` ＝行き来」の意味がアプリ内で既に立っている。⌘の有無で対象が分かれるだけ
- **履歴はブラウザ型**（戻った状態で新しいものを開くと、進む側は破棄）
- **上限16件・セッション内のみ**（project.json には書かない）
- **消えた対象は自動スキップ** — 削除されたリージョン・解除されたサンプル・削除されたトラック
- **左FXパネルは追従させる** — 履歴でFX詳細へ移動したら表示対象チャンネルとアクティブスロットも合わせ、
  パネルが閉じていたら開く。ピアノロールへ移動したときは左パネルに触らない（今どおり共存）
- **トラック選択（`selectedTrack`）は変えない** — 履歴移動で選択が動くと録音対象トラック（`R`）まで
  変わって事故になる。FXパネルは `shownTrack` を選択と別に持っているので表示だけ動かせる
- **画面上のボタンは追加しない** — `×` のツールチップに `(E)` を出す程度。⌘? 一覧はテーブル走査で自動生成

### わかっている引っかかり

- `E` は修飾なし1文字なので、**右パネルのプロジェクトメモにフォーカスがあると "e" が入力されてトグルできない**。
  既存の `F`（ファイルパネル）と同じ制約で、`⌘N`（メモ）だけが修飾付きになっているのと同じ理由。許容する
- `[` / `]` の matcher は `detail::noCmdCtrlAlt` を使う。`⌘[`（browserHistory）とは ⌘ の有無で排他になる。
  Shift併用時は文字が `{` `}` になるので `getTextCharacter()` 比較なら誤爆しない
- `InstrumentDetailView` に TextEditor はない（Label と自前 Component のみ）ので、下部エリアに
  フォーカスがあっても `E` / `[` / `]` は吸われない

## 実装計画

### Phase 1: 履歴の土台 [AI🤖]

- [x] `Source/ui/BottomPanelHistory.h` を新規作成
      - 置き場所は `ui/`。この履歴はUIスレッド専用で、`shared/` は**スレッド境界の受け渡し用**という
        ディレクトリ方針（CLAUDE.md）に従う。既存の `AudioBrowserNavigation.h` が `shared/` にあるが、
        あれもUIスレッド専用なので置き場所は踏襲しない（APIの形だけ倣う）
      - テストからの include は問題ない。`Tests/TestsMain.cpp` はすでに `ui/FileSortOrder.h` /
        `ui/PreviewPolicy.h` / `ui/AppLookAndFeel.h` を include している
  - `struct Entry { enum class Kind { pianoRoll, fxDetail }; Kind kind; juce::uint64 trackId; juce::uint64 regionId; juce::String channelKey; int slot; }`
    - ピアノロール: `trackId` + `regionId`
    - FX詳細: `channelKey`（`"track"`/`"bus0..2"`/`"master"`）+ `trackId`（channelKeyが`"track"`のとき）+ `slot`
  - `bool operator==(const Entry&)` — 「同じものを開き直した」判定用
  - `push(Entry)` — 現在位置より先を破棄してから積む。現在位置と同じなら何もしない。上限16件で古い方から捨てる
  - `replaceCurrent(Entry)` — カーソルを動かさず現在エントリの内容だけ差し替える（トラック追従用・下記 Phase 2）
  - `current()` / `hasCurrent()`
  - **`findValid(int direction, predicate) -> int`** — カーソルを動かさずに、direction方向で最初に
    predicate を満たす位置を返す（見つからなければ -1）
  - **`entryAt(int position)`** — 指定位置のエントリを読む（カーソルは動かさない）
  - **`commit(int position)`** — 指定位置へカーソルを移す
  - 破壊的な `stepTo()` は作らない。「端まで探索して見つからなかったら元の位置に戻す」を
    後始末で辻褄合わせするのではなく、**非破壊で探して、復元が成功したときだけ commit** する形に統一する
  - `clear()`
- [x] `PianoRollView` に `juce::uint64 currentRegionId() const` を追加（`currentTrackId()` の隣）

### Phase 2: 開く操作から履歴に積む [AI🤖]

- [x] `MainComponent` に `BottomPanelHistory bottomHistory;` と `bool suppressHistoryPush = false;` を追加
- [x] `openPianoRoll()` — 開いたあと（`suppressHistoryPush` でなければ）`bottomHistory.push({pianoRoll, track.id, region.id})`
  - 「同じリージョン再ダブルクリック＝閉じる」既存トグルはそのまま。閉じても履歴は消さない
- [x] `toggleFxDetailSlot()` — 開いたあと（同上）`bottomHistory.push({fxDetail, channelKey, trackId, slot})`
  - `channelKey` は `fxEditor.targetKey()`、`trackId` は `fxEditor.shownTrack()` から引いたトラックのID
- [x] `closePianoRoll()` / `closeFxDetail()` では履歴を触らない（`E` で戻せるようにするため）
- [x] **`syncFxDetail()` の追従では `push` ではなく `replaceCurrent()` を呼ぶ**
  - `syncFxDetail()` はトラック選択の変更に追従して FX詳細を**実際にトラックAからBへ載せ替える**
    （`MainComponent.cpp:1223-1238`）。ここで履歴を放置すると、履歴の現在地はAのまま実表示はBになり、
    `E` を2回押すとAが復元されて「同じ中身が戻る」を満たさない
  - かといって `push` すると、トラックをクリックして回るだけで履歴が伸びて汚れる
  - 追従は「ユーザーが下部エリアを開く操作をした」わけではないので**履歴を1件消費させず、
    現在エントリの内容だけをBへ差し替える**のが正しい

### Phase 3: 復元とショートカット [AI🤖]

- [x] `MainComponent::restoreBottomEntry(const Entry&) -> bool` を追加
  - **既存の open 関数を再利用する**（前提セクション参照。排他close・状態設定・resized・
    applyPendingScroll をすべて含んでいるため、独自に書き直さない）
  - 復元中は `suppressHistoryPush = true` にして push を止める（RAII か try/finally 相当で必ず戻す）
  - `pianoRoll`: `trackId`/`regionId` から index を引いて `openPianoRoll(trackIndex, regionIndex)`。
    すでに同じリージョンを表示中なら**呼ばない**（`openPianoRoll` は同一リージョンで閉じるトグルのため）
  - `fxDetail`: 左FXパネルが閉じていたら `openFxEditor()` → `channelKey` に応じて
    `fxEditor.showTrack(index)` / `showBus(i)` / `showMaster()` で**先に対象を合わせてから**
    `toggleFxDetailSlot(slot)`（そうしないと `fxDetailKey = fxEditor.targetKey()` が古い対象になる）。
    すでに同じチャンネル・同じスロットを表示中なら**呼ばない**（同上のトグル対策）
  - **`selectedTrack` は変更しない**
- [x] `MainComponent::bottomEntryIsValid(const Entry&) const -> bool` を追加
  - `pianoRoll`: そのトラックIDが存在し、そのリージョンIDも存在する
  - `fxDetail`: `channelKey=="track"` ならトラックIDが存在する。Instrumentスロットなら `usesSampler()` も要る
- [x] **カーソル操作の順序を全経路で統一する**: `findValid()` で候補位置を非破壊に探す →
      `entryAt(position)` を `restoreBottomEntry()` に渡す → **戻り値が true のときだけ `commit(position)`**。
      先に commit すると、復元に失敗したとき「表示は前のまま・カーソルだけ進む」というズレが残る
- [x] `MainComponent::toggleBottomPanel()` — `E`
  - 開いている → 閉じる（`closePianoRoll()` / `closeFxDetail()` のうち開いている方）
  - 閉じている → まず `current()` を復元。成功すればそこで終わり（カーソルは元から現在地なので commit 不要）
  - `current()` が無効／復元に失敗したら `findValid(-1, ...)` で後ろ方向に探し、
    上記の「復元 → 成功したら commit」の順で辿る（復元したものが現在地になるのが自然）。
    見つからなければ **カーソルを動かさず** no-op
- [x] `MainComponent::navigateBottomHistory(int direction)` — `[` / `]`
  - `findValid(direction, bottomEntryIsValid)` で非破壊に探す → 復元 → 成功したら `commit()`
  - 見つからない／復元に失敗した場合は**カーソルを動かさず** no-op（開いているものはそのまま）
  - 閉じている状態で押したら、開いた上で移動する
- [x] `Shortcuts.h` にエントリを2行追加（カテゴリは `view`）
  - `ID::toggleBottomPanel` — `u8"エディタ（下部）を表示/隠す"` / `u8"E"` / `noCmdCtrlAlt && textCharacter=='e'`
  - `ID::bottomHistory` — `u8"エディタ（下部）の戻る/進む"` / `u8"[ / ]"` / `noCmdCtrlAlt && (textCharacter=='[' || ']')`
- [x] `MainComponent::keyPressed` に分岐を追加（`toggleFiles` の直後・`browserHistory` の前後どちらでも可）
  - `AddTrackOverlay` 表示中の無視リスト（`MainComponent.cpp:2127`）に
    **`toggleBottomPanel` と `bottomHistory` の両方**を加える（履歴キーを抑止しないと、
    トラック追加オーバーレイの背後で `[` / `]` による画面遷移が起きる）
- [x] `Log::info` を入れる（`bottom.toggle` / `bottom.history` に方向と復元先を出す。不具合調査の裏取り用）

### Phase 4: テストと仕上げ [AI🤖]

- [x] `Tests/TestsMain.cpp` に `BottomPanelHistory` のテストを追加
      （`AudioBrowserNavigation::History` のテスト（`TestsMain.cpp:96`）に倣う）
  - 上限16件で古い方から捨てられる
  - 同じエントリの連続 push で伸びない
  - 戻った状態で push すると進む側が破棄される
  - `replaceCurrent()` がカーソルを動かさず内容だけ差し替える
  - `findValid()` が非破壊（呼んでもカーソルが動かない）
  - 有効な候補が1つも無いとき `findValid()` が -1 を返し、カーソルが維持される
  - `commit(position)` 後は `current()` がその位置になり、次の `findValid()` がそこ起点で探す
- [x] `FxDetailView` / `PianoRollView` の `×` ボタンに `Shortcuts::tooltipText(ID::toggleBottomPanel)` をセット
- [x] `VERIFY.md` に確認手順を追記（再利用可能な手順のみ）

### 動作確認 [AI🤖]

- [x] `cmake --build build` が通る
- [x] `ctest --test-dir build --output-on-failure` が通る（Phase 4 の履歴テストを含む）
- [x] dev版を `open -g` でバックグラウンド起動し、クラッシュせず `session.start` が出ることを確認
- [x] ⌘?一覧のレイアウトをオフスクリーン描画で確認（実機不要。ログ参照）
- [ ] dev版を起動し、リージョンのダブルクリック → サンプルスロットクリック → 別リージョン、と
      CGEvent合成マウスで操作してログに `fxdetail.open` / ピアノロール開閉が期待順で並ぶことを確認
      → **未実施**。ユーザーが操作中（HIDIdleTime=0）だったため合成マウスを撃たなかった
- [ ] キー合成（`E` / `[` / `]`）が JUCE アプリに届くか1回試す。届かなければ人間の確認に委ねる
      （AppleScript keystroke は JUCE 製アプリに効かない実績あり）
      → **未実施**。同上

### 動作確認 [人間👨‍💻]

- [ ] `E` で下部エリアが閉じ、もう一度 `E` で同じ中身が戻る
- [ ] **FX詳細を開いたまま別トラックを選択して追従させ、`E` を2回押す** → 追従後（＝いま見えている方）の
      内容が戻る（Phase 2 の `replaceCurrent` が効いているかの確認）
- [ ] ピアノロール → サンプルエディタ → 別のピアノロール、と3点開いてから `[` `[` で最初まで戻り、`]` `]` で戻った先まで進める
- [ ] 履歴で FX詳細に戻ったとき、左FXパネルの表示チャンネルとハイライトされたスロットが一致し、
      本文（サンプル波形・GAIN等）も表示されている
- [ ] 履歴移動でトラック選択（ヘッダーのハイライト・`R` の録音対象）が変わらない
- [ ] 履歴にあるリージョンを削除してから `[` で辿ると、そのエントリを飛ばして次の有効なものに行く
- [ ] トラック追加オーバーレイ表示中に `E` / `[` / `]` を押しても背後の下部エリアが動かない
- [ ] `⌘?` の一覧に `E` と `[ / ]` が「表示・ズーム」カテゴリに出る
- [ ] `⌘[` / `⌘]`（ファイルパネルの履歴）が従来どおり動き、`[` / `]` と混線しない

## ログ

### 試したこと・わかったこと

- **`FxEditorView::instrumentSlot` が private だったので public に移した**。有効性判定に既存の
  `isInstrumentSlot(slot)` は使えない（あれは「いま表示中のチャンネル」基準で、バス表示中は常にfalse）。
  履歴は表示対象を切り替えずに判定する必要があるので、スロット番号の定数そのものを見る形にした
- **⌘?一覧のレイアウトは実機なしで確認できた**（VERIFY.md「アプリを起動せずに描画を確認する」）。
  `ShortcutListOverlay` を2xでオフスクリーン描画してPNG目視。`I`（FXパネル）の直後に `E` と `[ / ]` が入り、
  `⌘[ / ⌘]`（ファイルパネルの履歴）と縦に並んで対応関係が読める配置になっていること、2行増でも
  パネル高が破綻しないことを確認。確認後に一時コードは削除済み
- **キー衝突チェックはテーブル全走査で恒久テスト化した**。VERIFY.md は一時コードでの確認を勧めているが、
  `E`⇄`⌘E`（書き出し）・`[`⇄`⌘[`（ファイルパネル履歴）は1文字違いで今後も踏みやすいので、
  「そのKeyPressにマッチする項目がちょうど1件」をテストに残した（今後キーを足したときも守られる）
- 実機での結合確認（キー操作・復元の見た目）は未実施。ユーザーが操作中（HIDIdleTime=0）だったため、
  合成キー・合成マウスは撃たずに人間の確認へ委ねた

### 方針変更

- Phase 4 のテストに `testBottomPanelShortcuts`（キーマッチャの誤爆・衝突チェック）を追加した。
  plan には履歴クラスのテストしか書いていなかったが、修飾なし1文字を2つ増やす変更なので
  マッチャ側の退行も押さえておくべきと判断した
