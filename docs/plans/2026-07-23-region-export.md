# 選択リージョンの書き出し（⌘E・右クリックメニュー）

## 概要・やりたいこと

選択中のリージョン（オーディオクリップ / MIDIリージョン）を単体でWAVに書き出せるようにする。
Logicの「リージョンをオーディオファイルとして書き出す」（⌘E）に相当する操作。

- 用途: 録ったテイクや打ち込んだフレーズを、そのリージョン区間だけ切り出して他で使う
- 既存の⌘B（全体/サイクル範囲バウンス）の基盤（`BounceRenderer`・`BounceOverlay`・pollBounce）をそのまま流用する

## 前提・わかっていること

### /dig-lite での決定事項

| 論点 | 決定 |
|---|---|
| 音の内容 | **聞こえたまま**。そのリージョンだけをソロにしたミックス相当＝**現在可聴な経路（シンセ音源・gain・pan・sends→バス・Master）を反映**。素の素材切り出しはやらない。固定ストリップFX（EQ/Comp）は現状DSP未実装で再生にもバウンスにも影響しない（PlaybackSnapshot.h:27）ため今回の対象外。FXのDSPが実装される際は `BounceRenderer` 側（⌘Bと共通の経路）に入るので、リージョン書き出しにも自動で追従する |
| 起動方法 | リージョン右クリックメニュー「書き出し…」＋ショートカット **⌘E**（Logic準拠） |
| 長さ | **リージョン厳密長**（開始〜終了ぴったり・`wantTail = false`。⌘Bのサイクル範囲書き出しと同じ規則） |

### コード調査でわかっていること

- 選択の概念は既にある: `TimelineView::ClipSelection`（audio）/ `RegionSelection`（MIDI）の単一選択。
  `MainComponent::keyPressed` の `deleteItem` 処理が「regionSelection優先→clipSelection」の先例（MainComponent.cpp:1399-1408）
- `BounceRenderer::Request` は任意範囲（`startSample`〜`endSample`）・任意トラック集合を受ける設計で、
  サイクル書き出しが既に `startSample > 0` で動いている実績あり（beginBounce末尾）
- 右クリックメニューは `TimelineView::showItemMenu`（TimelineView.cpp:1134あたり）が (trackIndex, itemIndex) を
  捕捉して項目を出す。削除は `onDeleteItemRequested (track, item)` コールバックでMainComponentへ委譲する先例あり
- ネイティブFileメニュー（Main.cpp の MenuCommands）はNSMenuのkeyEquivalentがkeyPressedより先にキーを取るが、
  **今回はFileメニューには追加しない**（選択依存のenable管理が増えるだけ。右クリック＋keyPressedの⌘Eで完結させる）
- Shortcuts.h テーブルに `'e'` の割り当てはなく⌘Eは空いている
- `buildSnapshot()` はミュートリージョンをスキップし、ノートを絶対PPQへフラット化＋リージョン境界マスクする
  （Project.cpp:622-684）。**選択リージョン1個だけ**が要るので、スナップショットは使わず同じ規則で
  モデルから直接 `TrackRender` を組む

### 設計判断（dig-liteで聞かなかった細部）

- **トラックのmute/solo・リージョン自身のmutedは無視して書き出す**（明示選択が優先。ミュート中だからと無音を書き出しても無意味）。
  gain/pan/sends/バス/Masterは開始時の値をそのまま適用（=聞こえたまま）
- ノートが1つもないMIDIリージョンは入口で弾く（「書き出す内容がありません」アラート。⌘Bの空プロジェクト弾きと同じ流儀）
- 保存ダイアログは⌘Bと共通（`lastBounceDirectory` 記憶を共用）。デフォルトファイル名は `<プロジェクト名>-<トラック名>.wav`
- 書き出し中UI・完了/失敗通知・24bit WAV・ピーク超過時スケールは既存バウンスと完全共通（pollBounceがそのまま面倒を見る）
- undo対象外（モデルを変更しない）

## 実装計画

### Phase 1: ショートカット定義 [AI🤖]

- [x] `Shortcuts.h` のテーブルに `ID::exportRegion` を追加（編集カテゴリ・説明「リージョン/クリップを書き出す」・keyLabel `⌘E`・matcher `KeyPress('e', commandModifier)`）

### Phase 2: MainComponent に書き出しフロー実装 [AI🤖]

- [x] `startRegionExportFlow (int trackIndex, int itemIndex)` を追加
  - 入口ガード: `bounceActive` / 録音中 / index範囲外 / SR未確定（⌘Bと同じ）/ ノート空のMIDIリージョン → アラートで弾く
  - 再生中なら停止（⌘Bと同じ）
  - FileChooser（saveMode・warnAboutOverwriting・`lastBounceDirectory` 共用・デフォルト名 `<プロジェクト名>-<トラック名>.wav`）
- [x] **テスト可能なヘルパー**として、選択アイテム1個から `TrackRender`（clips/notes・gain/pan/sends）と
  レンダリング範囲（startSample/endSample）を組み立てる純粋関数を切り出す
  （synth生成・FileChooser・レンダラー起動はヘルパーの外。置き場所は `BounceRenderer.h` の static か shared/ の自由関数）
  - 実装を `.cpp` に置く場合は CMakeLists.txt の `target_sources(daw ...)` と `target_sources(daw_tests ...)` の
    **両方**へ追加する（header-only なら不要）
  - gain/pan/sends は `track.params` から焼き込み（**mute/solo・リージョンのmutedは見ない**）
  - audioクリップ: 選択クリップ1個だけを `clips` に入れる（buildSnapshotと同じ `offsetSamples`/長さの範囲クランプを適用）。
    範囲 = `clip.startSample` 〜 `clip.startSample + clip.lengthSamples`
  - MIDIリージョン: 選択リージョン1個のノートだけを絶対PPQへフラット化＋境界マスクして `notes` に入れる
    （固定ピッチ打楽器の置き換えも buildSnapshot と同じ規則）。
    範囲 = `llround(startPpq / tps)` 〜 `llround((startPpq + lengthPpq) / tps)`（サイクル書き出しと同じ丸め）
- [x] `beginRegionBounce (target, trackIndex, itemIndex)` を追加
  - 上記ヘルパーで `Request` を構築し、バス/Masterの焼き込みは⌘Bと同じ。MIDIならsynthを `synthBank.createIndependent()` で専用生成
  - `wantTail = false`（厳密長）
  - レンダラー起動〜オーバーレイ表示は既存 `beginBounce` 末尾と共通。重複が大きければ小さなヘルパー（リクエスト起動部）を抽出する
  - ログ: 既存 `bounce.start` に `source=region track=N item=M` 相当の情報を足す
- [x] `keyPressed` に ⌘E 判定を追加: `regionSelection` 優先→`clipSelection`（deleteItemと同順）。どちらも無効なら何もしない

### Phase 3: 右クリックメニュー [AI🤖]

- [x] `TimelineView::showItemMenu` に「書き出し…」項目を追加（`itemWithKey` でショートカット表記 `⌘E` を併記）
- [x] `onExportItemRequested (int track, int item)` コールバックを追加し、MainComponent で `startRegionExportFlow` に接続（`onDeleteItemRequested` と同配線）

### Phase 4: ユニットテスト [AI🤖]

- [x] `Tests/TestsMain.cpp` に Phase 2 のヘルパーのテスト関数を追加し、**`main()` の実行列（TestsMain.cpp 末尾）へ呼び出しを登録する**（関数定義だけでは実行されない）。内容（既存の `expect` スタイルに合わせる）:
  - 異なる信号値の複数クリップを持つトラックから「**選択した1個だけ**」が `clips` に入り、範囲がそのクリップ区間になること
  - 複数MIDIリージョンから選択リージョンのノートだけがフラット化されること（他リージョンのノートが混ざらない）
  - トラックmute/solo・選択リージョン自身の `muted` を**無視**して組み立てられること
  - クリップの `offsetSamples`/長さがbuildSnapshotと同じ規則でクランプされること
  - MIDIの境界マスク（リージョン端をはみ出すノートの切り詰め）と、PPQ→サンプルの厳密長丸めが期待値どおりであること

### Phase 5: 動作確認 [AI🤖]

- [x] ビルド（警告ゼロ）＋ `daw_tests` 通過（新規テスト含む）
- [ ] dev版起動 → テストプロジェクトを開く → CGEvent合成マウスでリージョンを **Ctrl+左クリック** →
  ポップアップの「書き出し…」をクリック（CLAUDE.mdのJUCEポップアップ検証手順: 1プロセス内でmenuウィンドウのboundsを取得して項目クリック）
- [ ] NSSavePanel は合成Returnで保存（ネイティブパネルには合成キーが効く。別名保存で上書き確認を回避）
- [ ] 成果物WAVの裏取り:
  - `afinfo` でサンプル数 == 期待値（リージョン厳密長: `endSample - startSample`）・**2ch / 24bit**（既存バウンスと同一仕様。パン・send込みの出力なのでステレオ）・SR一致
  - アプリログに `bounce.start`（source=region・範囲サンプル値）→ 成功イベントが出ていること
- [ ] MIDIリージョン・オーディオクリップの両方で1回ずつ確認
- [ ] VERIFY.md に再利用可能な確認手順があれば追記

### 動作確認 [人間👨‍💻]

- [ ] リージョンを選択して ⌘E → 保存ダイアログ → 書き出し（キー合成はJUCEアプリに効かないため人間確認）
- [ ] 書き出したWAVを聴いて「聞こえたまま」（音量・パン・センド→バス・Master込み）の音になっているか確認

## ログ

### 試したこと・わかったこと
- Phase 1〜4 実装完了。ビルド通過（新規警告なし。ProjectThumbnails.h の shadow 警告と pollBounce の
  -Wswitch-enum 警告は変更前から存在する既存警告で、git stash して確認済み）。daw_tests 全通過
  （新規 testBuildItemRender 含む・FAILなし）
- GUI動作確認（Phase 5 の実機フロー）は保留: ユーザーの旧 LaLa-dev インスタンスが起動中で、
  ウィンドウタイトルが「0-0-shortcut-test ●」= 未保存変更あり。quit を送ると保存ダイアログで
  ユーザーの判断を代行することになるため中止（シングルインスタンスのため新ビルドも起動不可）。
  旧インスタンス終了後に再開する

### 方針変更
（実装中に随時追記）
