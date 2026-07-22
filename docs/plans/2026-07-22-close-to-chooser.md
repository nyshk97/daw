# プロジェクトを閉じたら選択画面に戻る（⌘O対応）

## 概要・やりたいこと

- 現状はプロジェクトウィンドウのバツ＝アプリ終了で、別プロジェクトに移るにはアプリ再起動しかない。これが毎日の痛点
- シングルウィンドウ構成は維持したまま、「閉じる→プロジェクト選択画面に戻る」＋「⌘Oでいつでも選択画面へ」にする
- **マルチウィンドウ化（複数プロジェクト同時オープン）は見送り**。年数回しか使わないニーズのためにクロスウィンドウ協調（再生調停・SR競合・非同期ダイアログチェーン）という恒常的な複雑さを抱えるのは、引き算方針に反すると判断（経緯はログ参照）

## 前提・わかっていること

- `Main.cpp` の `MainWindow` は1枚で、`showChooser()` / `openProject()` により中身（`ProjectChooserComponent` ⇄ `MainComponent`）を `setContentOwned` で差し替える構造。**選択画面に戻る仕組み自体は既にあり、導線が未配線なだけ**
- バツ（`closeButtonPressed`）と ⌘Q（`systemRequestedQuit`）はどちらも `attemptQuit()` → `JUCEApplication::quit()` に直結している（`Main.cpp:97-129`）
- 未保存確認は `NativeMessageBox::showAsync`（非同期3択: 保存して終了・保存せず終了・キャンセル）
- `trySave()` は録音中だと**アラートなしで false** を返す（`MainComponent.cpp:715`）。閉じるフローでは先に `finishRecording()` を完了させる必要がある
- 既存コードの `onProjectOpened` コールバック内で `setContentOwned` すると、実行中の `ProjectChooserComponent`（`openRow()` がスタック上）を自己破棄する。**逆方向も同じ**: ⌘O（`MainComponent::keyPressed` 発）で未保存確認なしに即 `showChooser()` すると、実行中の `MainComponent` を破棄する。どちらも `MessageManager::callAsync` での遅延が必要
- 未保存ダイアログ（`showAsync`）のコールバックは現在 `[this]`（`MainWindow`）を生捕捉している（`Main.cpp:114`）。終了要求やshutdownとダイアログ完了が競合すると破棄済みウィンドウを参照しうる → `Component::SafePointer` で生存確認が必要
- `finishRecording()` は private（`MainComponent.h:49`）。閉じるフローから呼ぶには公開が必要だが、汎用の外部停止APIには広げない（Tier 1の引き算方針）
- ショートカットは `Source/ui/Shortcuts.h` のテーブルが単一の真実の源。⌘O追加はここに1行足す
- `MainComponent` の破棄で `AudioAppComponent` がオーディオを閉じるので、選択画面に戻ればデバイスは解放される（オーディオ周りの改修は不要）

### 決定済みの仕様

| 項目 | 仕様 |
|---|---|
| ウィンドウ構成 | 従来どおりシングルウィンドウ・中身差し替え方式 |
| プロジェクト画面のバツ | 未保存確認 → プロジェクトを閉じて**選択画面に戻る**（アプリは生き続ける） |
| ⌘O（プロジェクト画面で） | バツと同じ（未保存確認 → 選択画面へ）。ここから別プロジェクトを開く |
| 選択画面のバツ | アプリ終了（従来どおり） |
| ⌘Q | 未保存確認 → アプリ終了（従来どおり） |
| 録音中に閉じる/終了 | 先に `finishRecording()` を完了させて（録音をクリップ化して）から未保存確認 |
| 保存失敗時 | 「保存して終了」で `trySave()` 失敗なら閉じない/終了しない（現挙動を維持） |
| 未保存確認の文言 | 「〜して終了」は閉じる文脈に合わせて調整（例: 保存して閉じる） |
| 複数プロジェクト同時オープン | 対応しない。将来必要になったら `moreThanOneInstanceAllowed() = true` ＋プロジェクトディレクトリのロックファイルで**2プロセス方式**により実現する（アプリ内マルチウィンドウはやらない） |

## 実装計画

### Phase 1: 閉じる挙動の変更 [AI🤖]

- [x] `MainWindow::attemptQuit()` を「未保存確認 → 続きの処理」を受け取る形に一般化する（例: `attemptCloseProject (std::function<void()> onClosed)`）
  - 録音中なら先に録音を確定してから確認する。`MainComponent` には意図を限定した公開メソッド（例: `finishRecordingForClose()`）を追加し、汎用の外部停止APIは作らない
  - 「保存して〜」で `trySave()` 失敗なら続きを実行しない（現挙動維持）
  - ダイアログ文言は文脈（閉じる/終了）に合わせて切り替える
  - **ダイアログのコールバックは `juce::Component::SafePointer<MainWindow>` で捕捉**し、コールバック冒頭で生存確認してから処理する（`[this]` 生捕捉をやめる。終了要求・shutdownとの競合対策）
- [x] **多重起動ガード**: `MainWindow` にフラグ（`flowPending`）を持たせ、未保存確認〜画面遷移完了までの間の バツ/⌘O/⌘Q/chooserダブルクリック の再入を無視する。開始時に立て、キャンセル・保存失敗・遷移完了で戻す（確認と遷移は同時に走らないので1本のフラグで足りる）
- [x] バツ（`closeButtonPressed`）: プロジェクト表示中は `attemptCloseProject → showChooser()`、選択画面表示中は従来どおり `quit()`
- [x] ⌘Q（`systemRequestedQuit`）: 従来どおり `attemptCloseProject → quit()`
- [x] **`setContentOwned` による画面遷移（chooser→プロジェクト・プロジェクト→chooser の両方向）は、すべて `MessageManager::callAsync` で呼び出し元のコールスタックを抜けた後に実行する**。特に「未保存変更なしの⌘O」は確認ダイアログを経ずに `keyPressed` から直接遷移する経路なので、実行中の `MainComponent` の自己破棄を必ず遅延で避ける。**callAsync のラムダも `SafePointer<MainWindow>` を捕捉**し、キュー済み遷移が shutdown 後に実行される経路を防ぐ
- [x] 選択画面に戻ったとき、ウィンドウタイトルを初期状態（`daw-dev` 等）に戻し、プロジェクト一覧を最新化する

### Phase 2: ⌘O ショートカット [AI🤖]

- [x] `Shortcuts.h` のテーブルに「プロジェクトを閉じて選択画面へ」（⌘O）を追加（ID・カテゴリ・説明・keyLabel・matcher）。⌘?一覧には自動で載る
- [x] `MainComponent::keyPressed` で `Shortcuts::matches()` 判定 → コールバック（例: `onOpenChooserRequested`）→ `MainWindow` が `attemptCloseProject → showChooser()` を実行

### Phase 3: 自動での動作確認 [AI🤖]

- [x] ビルド（Debug）して起動し、ログ + AXPress で確認:
  - プロジェクトを開く → **変更なしのままバツ** → 確認ダイアログなしで直ちに選択画面に戻り、アプリが生きている（`pgrep` でプロセス生存 ＋ CGWindowList でウィンドウ実在を裏取り）。この即時遷移パスが callAsync 遅延の自己破棄対策を直接カバーする（⌘O も同じ経路を通る）
  - 選択画面から**別プロジェクト**を開ける → さらに戻って**元のプロジェクト**も開き直せる
  - 未保存変更あり（トラック追加等をAXPressで作る）→ バツ → 3択ダイアログが出る。「キャンセル」でプロジェクト画面に留まる。「保存して閉じる」で保存されて選択画面に戻る（project.json の更新時刻で裏取り）
  - 録音中にバツ → 録音がクリップ化されている（project.json / clip-NNN.wav で裏取り）
  - 選択画面でバツ → アプリ終了（`pgrep` が空）
- [x] VERIFY.md に再利用可能な確認手順を追記

### 動作確認 [人間👨‍💻]

- [ ] ⌘O・⌘Q の実キー操作（合成キーでは確認できないため）
- [ ] 「保存せず閉じる」を含むダイアログ3択の実操作フロー
- [ ] 閉じる→開き直すを繰り返したときの使用感（タイトル・一覧の更新・オーディオの復帰）

## ログ

### 試したこと・わかったこと
- 2026-07-22 動作確認はユーザーがdev版を操作中（未保存のBPM変更あり）だったため、Release版の並走検証で実施（VERIFY.mdの手順どおり）。全ケース合格: 変更なしバツ即時遷移・別プロジェクト乗り換え・キャンセル後の再入ガード解除・録音中バツのクリップ化（record.stop→ダイアログ→保存でproject.jsonにclip記録）・選択画面バツでsession.end終了
- 未保存ダイアログはプロジェクトウィンドウとは**別ウィンドウ**（約260×270）として出る。AXPressでボタン押下可・スクショはCGWindowListでIDを引いて撮る（VERIFY.mdに追記済み）
- 未保存変更の作成は「録音」で行った（トラック追加オーバーレイの項目はAXPress不可のため）。録音中バツのテストと兼ねられて一石二鳥

### 方針変更
- 2026-07-22 縮小版planへの再レビューを全件反映: ①`setContentOwned` の画面遷移は両方向とも `callAsync` 遅延と明確化（特に変更なし⌘Oの即時遷移パス） ②未保存ダイアログの `[this]` 生捕捉を `SafePointer` ＋生存確認に ③録音確定は意図限定の公開メソッド `finishRecordingForClose()` とし汎用外部停止APIは作らない ④自動確認に「変更なしバツ/⌘Oの即時遷移」を明記
- 2026-07-22 当初のマルチウィンドウ化plan（複数プロジェクト同時オープン）を破棄し、本planに縮小。理由: 2プロジェクト同時は年数回しか使わないのに対し、①今後の全機能（プラグインホスティング等）が複数ウィンドウ考慮を強いられる ②クロスウィンドウ協調（再生調停・SR競合・非同期ダイアログチェーン）がバグの温床 ③滅多に通らないパスに複雑さが集中しバグが眠る、と割に合わないため。実装前レビューで出た5指摘のうち、本planでも生きる2件（chooserコールバック内の自己破棄→callAsyncで遅延／録音中の閉じるは`finishRecording()`先行＋保存失敗で中断）は反映済み。残り3件（AudioAppComponent複数生成・ロード前二重オープン判定・再生開始通知）はマルチウィンドウ前提のため対象外。将来2プロジェクト同時が本当に欲しくなったら、アプリ内マルチウィンドウでなく複数インスタンス（`moreThanOneInstanceAllowed` + ロックファイル）で対応する
