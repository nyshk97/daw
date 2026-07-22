# ショートカットキー表示（ツールチップ・メニュー横・⌘?一覧）

## 概要・やりたいこと

割り当て済みのショートカットキーがUI上どこにも表示されておらず、覚えていないと使えない。以下の3箇所に表示を追加し、今後ショートカットを追加するときも自動で表示に反映される仕組みにする。

1. **ボタンのホバーツールチップ**: 「録音 (R)」のように機能名＋ショートカットを表示
2. **右クリックメニュー**: 項目の横にショートカット表記（ミュート ⌃M・分割 ⌘T など）
3. **⌘? でショートカット一覧オーバーレイ**: ジャンルごとにグルーピングして全ショートカットを表示

「今後追加するときも同じルールを守る」ため、定義を一元テーブル（Shortcuts.h）に集約し、CLAUDE.md にルールを明文化する。

## 前提・わかっていること

### 現状（調査済み）

- ショートカットは `MainComponent::keyPressed()`（`Source/ui/MainComponent.cpp:760-880`）に全20個が直接ハンドリングで散在。`ApplicationCommandManager` 未使用。「キー↔説明」の対応表は存在しない
- フォーカス設計: サブビューは `setWantsKeyboardFocus(false)` で、キーは `MainComponent` に集約されている。サブビュー独自の `keyPressed` は無い
- ツールチップ基盤は稼働済み: `MainComponent.h:99` に `juce::TooltipWindow`。`addTrackButton`・`settingsButton` には `setTooltip` 済み、`playButton`・`recordButton`・`clickButton` は未設定。`IconButton` は `juce::Button` 基底の `SettableTooltipClient` で対応済み
- 右クリックメニューは3箇所とも手動 `addItem`、ショートカット表記なし:
  - クリップ/リージョン: `TimelineView.cpp:796-818`（ミュート・複製・分割・削除）
  - トラックヘッダ: `TrackHeadersView.cpp:214-226`（トラック削除のみ）
  - （ComboBox のポップアップは対象外）
- オーバーレイの前例: `AddTrackOverlay.h`（全面 Component＋自前 paint＋パネル外クリック/Escで閉じる）。⌘?一覧はこのパターンを流用する

### /dig-lite での決定事項

- **一元テーブル方式**: `Shortcuts.h` に「ID・KeyPress・説明・ジャンル」を定義し単一の真実の源にする。`keyPressed` のキー判定はテーブルの KeyPress を参照（ハンドラ本体は手書きのまま）。ApplicationCommandManager への全面移行はしない（メニューバーの無いこのアプリには過剰）
- **⌘?一覧のジャンルは6分類**: トランスポート／編集／トラック／ピアノロール／表示・ズーム／プロジェクト
- 表記は Mac 流の記号（⌘⇧⌥⌃）。JUCE の `KeyPress::getTextDescriptionWithIcons()` が使える見込み（macOS では記号表記を返す）

### 現在の全ショートカット（テーブル化の元ネタ、modifier は実装現物を正とする）

| ジャンル | キー | 機能 |
|---|---|---|
| トランスポート | Space | 再生/停止 |
| トランスポート | R | 録音トグル |
| トランスポート | , / . | 1拍シーク |
| トランスポート | < / >（Shift+,/.） | 1小節シーク |
| 編集 | ⌘Z / ⇧⌘Z | Undo / Redo |
| 編集 | ⌘T | 再生ヘッド位置で分割 |
| 編集 | ⌃M | リージョン/クリップのミュート切替 |
| 編集 | Delete/Backspace | 選択クリップ/リージョン削除 |
| トラック | ⌘⌥A | オーディオトラック追加 |
| トラック | ⌘⌥S | ソフト音源トラック追加 |
| トラック | ⌘Delete | 選択トラック削除 |
| トラック | M | 選択トラックのミュート切替 |
| ピアノロール | ↑/↓ | ノート半音移動 |
| ピアノロール | ⌥↑/⌥↓ | ノートオクターブ移動 |
| ピアノロール | ⌘C/⌘V | ノートのコピー/ペースト |
| 表示・ズーム | ⌘←/⌘→ | 横ズームアウト/イン |
| 表示・ズーム | ⌘? | ショートカット一覧（今回新設） |
| プロジェクト | ⌘S | 保存 |
| プロジェクト | ⌘, | オーディオ設定 |

※ Esc（オーバーレイを閉じる）はコンテキスト操作なので一覧には載せない（載せるかは実装時に判断）

## 実装計画

### Phase 1: Shortcuts.h 一元テーブル [AI🤖]

- [x] `Source/ui/Shortcuts.h` を新規作成
  - `enum class ShortcutID`・`enum class ShortcutCategory`（6分類）
  - **「表示用のキー表記」と「入力マッチ条件」を分離した設計にする**（単一の `juce::KeyPress` 等値比較では現行挙動を再現できないため）:
    - `struct Shortcut { ShortcutID id; const char* name; ShortcutCategory category; const char* keyLabel; Matcher matcher; }` のように、表示は固定文字列 `keyLabel`（例: `"⌘T"` `"Delete"` `", / ."`）、判定は matcher（関数ポインタ/ラムダ or 複数 KeyPress 候補の配列）で持つ
    - matcher で表現が必要な現行の特殊ケース（`MainComponent.cpp:760-880` 現物準拠）:
      - `,`/`.` シーク: `getTextCharacter()` ベース＋「⌘/⌃/⌥が押されていない」ガード。Shift 有無で1拍/1小節が変わり、レイアウトによっては Shift+,/. が `<`/`>` の文字として届く
      - `Delete`/`Backspace`: 2つのキーコードを同一機能として受ける
      - `m`/`r`: 同じく getTextCharacter＋修飾なしガード
      - `⌘?`（新設）: `/` キー＋⌘で、Shift 有無どちらでも発火
  - ヘルパー: `matches(const juce::KeyPress&, ShortcutID)`、`keyText(ShortcutID)`（keyLabel を返す）、`tooltipText(ShortcutID)`（「録音 (R)」形式）
  - **文字化け防止**: `name`/`keyLabel` は日本語・⌘ 等の非ASCIIを含むため `u8"..."` リテラルで保持し、UIへ渡すヘルパー（`keyText`/`tooltipText` 等）の内部で必ず `juce::String::fromUTF8()` を通す（既存コードの `jp()` ヘルパーと同じ流儀。`MainComponent.cpp:10` 参照）
  - keyLabel は Mac 記号表記で統一。`getTextDescriptionWithIcons()` は matcher と表示の分離後は補助程度に留め、テーブルの keyLabel を正とする（レイアウト依存キーで自動生成が実態とズレるため）
- [x] `MainComponent::keyPressed()` の各 `if` 判定をテーブル参照（`matches(key, ShortcutID::xxx)`）に置き換える。ハンドラ本体・ピアノロール表示中のみ等の条件分岐は手書きのまま維持
- [x] ビルドが通り、既存ショートカットが全て従来どおり動くこと（挙動変更ゼロのリファクタ）

### Phase 2: ツールチップとメニュー横表示 [AI🤖]

- [x] ボタンのツールチップにショートカット併記
  - `playButton`: 「再生/停止 (Space)」、`recordButton`: 「録音 (R)」を新規 `setTooltip`
  - `clickButton`: ショートカット無しなので「メトロノーム」のみ
  - 既存の `addTrackButton`・`settingsButton` のツールチップは維持（該当ショートカットがあれば併記: 設定→⌘,）
  - 表記はすべて `Shortcuts.h` のヘルパーから生成（文字列の手書き禁止）
- [x] 右クリックメニューにショートカット表記
  - JUCE 8.0.9 では `PopupMenu::Item` の公開フィールド `shortcutKeyDescription`（`juce_PopupMenu.h:172`）に代入する方式（setter は無い）:
    ```cpp
    juce::PopupMenu::Item item ("ミュート");
    item.itemID = 1;
    item.shortcutKeyDescription = Shortcuts::keyText (ShortcutID::muteRegion);
    menu.addItem (item);
    ```
  - `TimelineView.cpp:796-818` のクリップメニュー: ⌃M・⌘T・Delete を横に表示。複製はショートカット無しのまま
  - `TrackHeadersView.cpp:214-226` のトラック削除メニュー: ⌘Delete を表示
  - 表記文字列は `Shortcuts.h` から取得

### Phase 3: ⌘? ショートカット一覧オーバーレイ [AI🤖]

- [x] `Source/ui/ShortcutListOverlay.h` を新規作成（`AddTrackOverlay` のパターン踏襲: 全面 Component＋半透明背景＋自前 paint、パネル外クリック/Esc/再度⌘? で閉じる）
  - `Shortcuts.h` のテーブルから6分類ごとにセクション描画（カテゴリ見出し＋「キー表記　機能名」の行）。項目はテーブル走査で自動生成し、オーバーレイ側にショートカットのハードコードを持たない
  - フォントは `Fonts.h`、配色は既存トーンに合わせる。キー表記は `Fonts::mono()` を検討
- [x] `MainComponent::keyPressed()` に ⌘? のハンドリング追加（`?` は Shift+`/` なので、⌘+`/`（Shift 有無どちらでも）でトグルにする。JIS/US 両配列で発火することを意識）
- [x] **一覧表示中のキー処理をモーダル化する**（AddTrackOverlay の流用だけでは Esc 以外が素通しになり、一覧を開いたまま Space で再生・R で録音が走ってしまう）:
  - `keyPressed()` の**冒頭**で `shortcutListOverlay.isVisible()` を最優先判定し、表示中は「Esc と ⌘?/⌘/（閉じる）だけを処理し、それ以外のキーはすべて消費（`return true`）」する
  - 既存の AddTrackOverlay の Esc 判定より前に置く
  - **AddTrackOverlay 表示中の ⌘? は消費して無視する**（オーバーレイの同時表示を避け、「オーバーレイは常に高々1枚」の単純な状態を保つ。AddTrackOverlay の Esc 判定と同じ場所で ⌘? を握りつぶす）

### Phase 4: ルールの明文化と動作確認 [AI🤖]

- [x] `CLAUDE.md` に運用ルールを追記:
  - ショートカット追加時は必ず `Shortcuts.h` のテーブルに追加し、`keyPressed` は `matches()` で判定する
  - ボタンのツールチップ・メニュー項目の表記は `Shortcuts.h` のヘルパーから生成する（表記の手書き禁止）
  - ⌘?一覧はテーブルから自動生成なので個別対応不要
- [ ] ビルド＋起動して自動確認（CGEvent/AXPress＋screencapture）:
  - ボタンホバーでツールチップにショートカットが出る
  - 右クリックメニューにショートカット表記が出る（Ctrl+左クリック合成でメニューを開き、メニューウィンドウをキャプチャ）
  - 一覧オーバーレイ表示中に Space/R を送ってもアプリログに再生/録音の発火が**出ない**こと（モーダル化の裏取り）
- [x] `VERIFY.md` に再利用可能な確認手順があれば追記

### 動作確認: 既存ショートカット全件の退行チェック [人間👨‍💻]

キー入力自体はログに残らず、自動確認だけでは網羅できないため、Phase 1（テーブル参照への置き換え）後に以下のマトリクスを手動で全件確認する。特に matcher 化で挙動が変わりやすい太字項目は必須。

| キー | 期待動作 | 確認 |
|---|---|---|
| Space | 再生/停止 | [ ] |
| R | 録音トグル | [ ] |
| **, / .** | 1拍シーク（後退/前進） | [ ] |
| **Shift+, / Shift+.（< / >）** | 1小節シーク | [ ] |
| ⌘Z / ⇧⌘Z | Undo / Redo | [ ] |
| **⌘T** | 再生ヘッド位置で分割 | [ ] |
| **⌃M** | リージョン/クリップのミュート切替 | [ ] |
| **Delete と Backspace の両方** | 選択クリップ/リージョン削除 | [ ] |
| **⌘Delete** | 選択トラック削除 | [ ] |
| ⌘⌥A / ⌘⌥S | オーディオ/ソフト音源トラック追加 | [ ] |
| M | 選択トラックのミュート切替 | [ ] |
| **↑/↓（ピアノロール表示中のみ）** | ノート半音移動（非表示時は無反応） | [ ] |
| **⌥↑/⌥↓（ピアノロール表示中のみ）** | ノートオクターブ移動 | [ ] |
| **⌘C/⌘V（ピアノロール表示中のみ）** | ノートのコピー/ペースト | [ ] |
| **⌘←/⌘→** | 横ズームアウト/イン | [ ] |
| ⌘S | 保存 | [ ] |
| ⌘, | オーディオ設定ダイアログ | [ ] |
| ⌘?（Shift 有無両方） | 一覧の開閉 | [ ] |
| Esc | 一覧/AddTrackオーバーレイを閉じる | [ ] |

### 動作確認: 表示まわり [人間👨‍💻]

- [ ] ⌘? で一覧が開閉すること（JIS 配列の実キーボードでの発火確認は合成イベントでは不完全なため）
- [ ] 各ボタンのホバーツールチップと右クリックメニューの見た目確認

## ログ

### 試したこと・わかったこと
- 2026-07-22 実装完了。ビルド成功・daw_tests全パス。JUCE警告のみ（自作コード由来の警告なし）
- Phase 4 の GUI 自動確認（ツールチップ・右クリックメニューの合成マウス確認）は、キャリブレーション用スクショに前面で作業中の別アプリ（ChatGPT）が写ったため中止（グローバルルール: ユーザー実機操作中の合成操作禁止）。表示確認は人間の動作確認へ委譲
- 「挙動変更ゼロ」の裏取りは退行チェックマトリクス（人間）待ち。matcher はコード上、旧 keyPressed の判定と1対1対応させた

### 方針変更
（実装中に随時追記）
