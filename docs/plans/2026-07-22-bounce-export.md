# バウンス（書き出し）＋メニューバー導入

## 概要・やりたいこと

- 完成した曲をWAVファイルとして書き出す（バウンス）機能を作る
- 起動導線としてネイティブのメニューバー（Fileメニュー）を導入する（`File > 書き出し… ⌘B`。Logic準拠）
- ヘッダーの常設ボタンは増やさない（バウンスは低頻度操作。ヘッダーは現状の5ボタン構成を維持）

## 前提・わかっていること

### 仕様（会話で決定済み）

| 項目 | 決定内容 |
|---|---|
| フォーマット | WAVのみ（ステレオ・24bit・プロジェクトのsampleRate）。mp3はJUCE非対応＋afconvertもエンコード不可のため見送り |
| 内容 | 「聞こえているまま」（what-you-hear）: mute/solo/gainを再生時と同じロジックで反映。ソロ中はソロトラックだけになる。**注: ミキサー基盤（2026-07-22-mixer-foundation.md）が先に入っている場合は pan・send・バス・Master も反映すること** |
| 含めないもの | メトロノーム・カウントイン・プレビュー発音 |
| クリッピング対策 | floatでミックスし、ピークが1.0を超えたときだけ全体をスケールダウン（Logicの「オーバーロード保護のみ」相当。普段は音量に触らない） |
| 範囲 | 全曲固定: 曲頭〜最後のクリップ終端/MIDIリージョン終端。MIDIトラックがあるときは終端後もシンセ余韻をレンダリング（-60dB無音検出・上限5秒） |
| 起動フロー | ⌘B（またはメニュー）→ 保存ダイアログ（デフォルト名=プロジェクト名・前回の保存場所を記憶）→ オフラインレンダリング → 進捗表示 |
| 再生中の⌘B | 再生を停止してから開始（状態を単純に保つ） |
| メニュー構成 | File: 保存 ⌘S / 書き出し… ⌘B / （区切り）/ プロジェクトを閉じる（既存のopenChooser）。Editメニュー等は今回作らない |

### スコープ外（後続フェーズ・別plan）

- サイクル範囲選択（ルーラードラッグ＋ループ再生）とバウンス範囲の連動。レンダラーは最初から範囲（startSample〜endSample）を引数に取る設計にしておき、サイクル実装後に数行で接続できるようにする
- リージョン単体のdry書き出し（リージョン右クリックメニューに追加予定）
- mp3書き出し（必要になったらHomebrewのlame＋`LAMEEncoderAudioFormat`で追加可能）

### 技術的な前提（コード調査済み）

- **リアルタイムエンジンとAUは共有できない**: `SynthInstance`（DLSMusicDevice）はオーディオスレッドが毎ブロック触っている。バウンスは専用のAUインスタンスをSynthBankから新規生成し、`Project::buildSnapshot()`で作った専用スナップショットで独立レンダリングする。オーディオデバイスには一切触れない
- **`buildSnapshot()`はsynthを埋めない**（`Project.cpp:572`。通常再生では`MainComponent::pushSnapshot`がSynthBankから注入している）。バウンス専用スナップショットにも専用synthを明示的に注入する手順が必要
- **`TrackParams`は共有atomicなので、そのまま参照すると「開始時の状態を固定したバウンス」にならない**（バウンス中にユーザーがフェーダーを触ると結果が変わる）。開始時にmute/solo/gainをプレーン値へコピーしてワーカーに渡す
- **メモリに全曲を蓄積しない**: ステレオfloat全保持は1時間/48kHzで約1.3GB。ブロックごとにレンダリングしながら一時WAV（32bit float）へストリーム書き出し＋ランニングピーク計測 → 完了後にピーク>1.0なら係数を掛けつつ、そうでなければそのまま24bit WAVへ変換する2パス構成にする
- **出力先を直接壊さない**: 一時ファイルへ書き、成功時のみ最終パスへ置換（rename）。キャンセル・失敗時は一時ファイルを削除して出力先の既存ファイルには触れない
- **ワーカーの寿命管理**: バウンスワーカーは専用スナップショット・専用synth・固定済みパラメータを自己所有し、MainComponentの生存に依存しない。完了通知は`Component::SafePointer`等で生存確認してから届ける。バウンス中はFileメニューの保存/閉じる/書き出しを無効化し、アプリ終了時はキャンセル→join後に破棄する
- **ノートスケジューリングは書き下ろす**: `PlaybackEngine::renderMidiTracks` はシーク・プレビュー・resound・スナップショット差し替え対応で複雑だが、オフラインは位置0からの直線再生なのでオン/オフの単純なループで済む。共通化は狙わない（クリップのミックスは20行程度）
- **`Project::sampleRate == 0`（未録音・MIDIのみ）の場合**: 現在のデバイスレートで書き出す
- **リージョンmuted除外は`buildSnapshot()`が既に行っている**
- **ネイティブメニューの⌘B表記は`ApplicationCommandManager`経由の登録が必要な見込み**（`PopupMenu::Item::shortcutKeyDescription`はNSMenuのkeyEquivalentに変換されない可能性が高い。要実機検証）
- **キーイベントの経路が変わる点に注意**: NSMenuのkeyEquivalentは`keyPressed`より先にイベントを取る。メニューに載せた項目（⌘S/⌘B等）はメニュー経由で発火するようになり、`MainComponent::keyPressed`側の同判定は実質デッドパスになる（フォールバックとして残すのは無害）。ハンドラは1つの関数に集約し、メニューとkeyPressedの両方から同じ関数を呼ぶこと
- Shortcuts.hのテーブルが単一の真実の源。`bounce`エントリを追加し、メニュー表示用に`menuKeyPress`（juce::KeyPress）的なフィールドをメニュー掲載項目にだけ持たせる案。matcherラムダとの二重管理にならない形をPhase 1で設計する

## 実装計画

### Phase 1: メニューバー導入 [AI🤖]

- [x] `Shortcuts.h` に `bounce`（カテゴリ: project、keyLabel: ⌘B）を追加
- [x] メニュー掲載項目がネイティブkeyEquivalent用の `juce::KeyPress` を提供できる仕組みをテーブルに追加（`Entry::menuKey`。表示・判定・メニューの3系統がテーブル1箇所から出る）
- [x] `MenuBarModel` を実装し `setMacMainMenu` でFileメニューを表示（保存 / 書き出し… / プロジェクトを閉じる）
  - 既存の`AppMenuModel`（Sparkle用・DawApplication所有・shutdownで`setMacMainMenu(nullptr)`済み）を拡張した
  - コマンド実体は`DawApplication::perform`が`MainWindow::currentMainComponent()`経由で現在のMainComponentへ委譲
  - プロジェクト未オープン（選択画面）ではFile項目をdisabled（実機で確認済み）
- [x] ⌘B・⌘Sのキー表記がネイティブメニューに出るか検証 → JUCEソース照合の上ApplicationCommandManager橋渡しを実装。実機のAXMenuItemCmdCharで S/B/O 表示を確認
- [x] 保存・プロジェクトを閉じるの既存ハンドラをメニューから呼べる形に集約（menuItemSelectedは空・実行はperform経由の一本道で二重発火なし）
- [x] ~~書き出しはこの時点ではプレースホルダ~~ Phase 2-3と一括で実装した

### Phase 2: オフラインレンダラー＋WAV書き出し [AI🤖]

- [x] 曲の終端計算（可聴トラックのクリップ終端と非ミュートMIDIリージョン終端のmax。PPQ→サンプル変換はPpqヘルパー）
- [x] `SynthBank` にバウンス専用の独立`SynthInstance`生成API（`createIndependent`。`setNonRealtime(true)`も設定）を追加
- [x] オフラインレンダラー（`Source/audio/BounceRenderer.h/cpp`。スレッド前提をヘッダーコメントに明記）
  - 開始時に専用スナップショット構築＋専用synth注入＋mute/solo/gainのプレーン値コピーを行い、以降ワーカーが自己所有する（前提セクション参照）
  - 範囲（startSample〜endSample）を引数に取る（今回は常に0〜終端）
  - クリップ: 再生時と同じ重なり加算＋gain
  - MIDI: フラット化済みnotesを直線走査してnoteOn/noteOffをブロックごとに`processBlock`
  - mute/solo判定は`PlaybackEngine::process`と同じ規則（固定済みプレーン値を使用）
  - テール: 終端後、出力ピークが-60dBを下回るまで延長（上限5秒）。**mute/solo判定後に可聴なMIDIトラックがある場合のみ**（ミュート済みMIDIしかないのに最大5秒の無音を足さない）
- [x] 2パス書き出し: パス1=ブロックごとに一時WAV（32bit float）へストリーム書き出し＋ランニングピーク計測 → パス2=ピーク>1.0ならスケール係数を掛けて24bit WAVへ変換、以下ならそのまま変換（GOTCHAS.mdの`createWriterFor`／`AudioFormatWriterOptions`版を使用）
- [x] 一時ファイル→成功時のみ最終パスへ原子的置換。キャンセル・レンダリング失敗時は一時ファイルを削除し既存の出力先ファイルには触れない
  - 一時ファイルは**出力先と同一ディレクトリ**に作る（同一ボリューム＝原子的renameの前提。隠しファイル名 `.<name>.wav.tmp` 等）
  - 置換は `juce::File::replaceFileIn()` またはPOSIX `rename()` を使う。**`moveFileTo()` は禁止**（既存の出力先を先に削除してから移動するため、移動失敗時に「既存ファイルに触れない」要件を破る）
  - 最終置換に失敗した場合は一時ファイルを**削除せず残し**、エラーメッセージにそのパスを含める（レンダリング結果を救出できるようにする）
- [x] DLSMusicDeviceがオフライン（実時間より速い`processBlock`連打）で正常にレンダリングできるか検証 → 既存の`testDlsMusicDeviceRendersAudio`が既に実証済みだった（`setNonRealtime(true)`使用）。加えて新規`testBounceRendererMidiTail`で本レンダラー経由でも確認

### Phase 3: UIフロー統合 [AI🤖]

- [x] ⌘B/メニュー → 再生中なら停止 → `FileChooser`（拡張子.wav・デフォルト名=プロジェクト名・前回の保存場所を記憶（セッション内static・初回はデスクトップ））
- [x] レンダリングをバックグラウンドスレッドで実行。完了通知はpush型でなく既存Timer（30Hz）のポーリング（`pollBounce`。GOTCHAS.mdのpull型パターンに合わせ、SafePointerより単純な形にした）
- [x] 進捗オーバーレイ（進捗バー＋キャンセル＋Escキャンセル）。進捗はパス1=0〜85%・テール=85〜90%・パス2=90〜100%
- [x] 完了・失敗の表示とログ出力（完了はオーバーレイに約1.3秒「書き出しが完了しました」を出して自動クローズ。失敗はダイアログ＋`bounce.failed`）
- [x] バウンス中の操作ガード: オーバーレイがモーダルに塞ぐ＋keyPressed全消費＋Fileメニューdisabled＋perform側でも再ガード
- [x] アプリ終了/閉じる時にバウンス中なら`cancelBounceForClose()`（キャンセル→join→一時ファイル削除）してから進む（`confirmCloseProject`冒頭。メニューはdisabledだがバツボタン・⌘Qの経路をカバー）

### 動作確認 [AI🤖]

- [x] ビルド＋dev版起動、メニューのAXPress（`click menu item`）でバウンスを駆動（`bounce.start`→`bounce.done`をログで確認。4.7秒の曲が57msでレンダリング）
- [x] 成果物WAVを`afinfo`で検証（2ch/24bit/48kHz/226816サンプル=endSample 192000＋テール34816）
- [x] パターン検証: オーディオのみ（unit test）/ MIDI混在（実機）/ ミュートあり（実機: MutedLoudトラックがtracks数・endSample・波形から除外確認）/ ソロあり → **実機検証はユーザー操作検知で中断**（コードパスはmuteと同一式。下の人間確認に委譲）
- [x] テール検証: 実機波形で末尾1024サンプルのピーク0.0006（<-60dB）まで減衰してから終端。unit testでも上限5秒以内を確認
- [x] クリッピング検証: unit test（0.8×2枚重ね=1.6 → 出力ピーク0.999へ正規化・`scaled`フラグ・スケール前ピーク記録を確認）
- [ ] キャンセル検証: バウンス中にキャンセルし、出力先の既存WAVが不変・一時ファイルが残っていないこと（実バウンスが速すぎて実機で狙えず未検証。ワーカーのキャンセル分岐は一時ファイル削除を通るコードパス）
- [ ] バウンス中ガード検証: バウンス中のメニューdisabled・quit安全完了（同上の理由で実機未検証。perform側の再ガードあり）

### 動作確認 [人間👨‍💻]

- [ ] ⌘B（実キーボード）でメニュー経由と同じフローが走ること
- [ ] 書き出したWAVを再生し、アプリ内再生と同じバランスに聞こえること（聴感確認。`~/Desktop/0-0-bounce-test.wav` にソロなし版あり）
- [ ] ソロ反映: `0-0-bounce-test`（Pianoにソロ設定済み）を書き出し、サイン波（Vocal）が入らずピアノだけになること
- [ ] Escまたはキャンセルボタンでのキャンセル（曲が短いと一瞬で終わるので長めの曲で）

## ログ

### 試したこと・わかったこと

- ネイティブメニューのkeyEquivalentは`ApplicationCommandManager`のKeyPressMappings経由でのみ設定される（`juce_MainMenu_mac.mm:311`で確認。`shortcutKeyDescription`は使われない）。橋渡し実装後、実機のAXMenuItemCmdCharで⌘S/⌘B/⌘O表示を確認
- DLSのオフラインレンダリングは既存テスト`testDlsMusicDeviceRendersAudio`が`setNonRealtime(true)`込みで実証済みだった
- NSSavePanelはリモートビュー（openAndSavePanelService）でAXからボタンが見えない。ただしネイティブパネルなのでキー合成（Return=Save）は効く。上書き確認の「Replace」は破壊的ボタンでReturnでは押せない
- JUCEウィンドウへのキー合成は不可（既知）。選択画面のプロジェクトオープンはCGEvent合成マウスクリック（ヒーローカードはmouseUpで開く）で自動化した
- 実測: 4.7秒・3トラック（うちDLS 1）のバウンスが57ms
- ~/DesktopはTCCでsandbox・素のシェルから読めない → 成果物検証はFinder AppleScriptでコピーしてから行う（VERIFY.mdに追記済み）
- レビュー指摘の修正: 終端を「最後のノート終端」で切っていたのを仕様どおり「MIDIリージョン終端」（非ミュート・モデル側の`midiRegions`から算出）に修正。最後のノート後の余白が範囲に含まれ、ノートなしリージョンだけのプロジェクトもリージョン終端までの無音として書き出される

### 方針変更

- 完了通知はSafePointer+callAsyncでなく既存Timer（30Hz）のポーリングにした（GOTCHAS.mdのpull型パターンに合わせて単純化。安全性は同等以上）
- 完了表示はダイアログでなくオーバーレイの自動クローズ表示（約1.3秒）にした（毎回OKを押させない）
- 保存ダイアログの初回デフォルト保存先はデスクトップ（以降はセッション内で前回場所を記憶）
- メニュー文言は「プロジェクトを閉じる」（Shortcuts表の「プロジェクトを閉じて選択画面へ」はメニューには長すぎるため。keyLabelの手書き禁止ルールとは別物）
