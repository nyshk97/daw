# Apple/Logic Pro風フォント統一

## 概要・やりたいこと

- UIのフォント（タイプフェイス・サイズ）をApple / Logic Proらしい見た目に寄せる
- 対応言語は日本語と英語。Apple製アプリと同じく「欧文・数字 = SF Pro、かな・漢字 = ヒラギノ角ゴシック」の組み合わせを目指す
- 現状はタイプフェイス指定なし（JUCE 8.0.9/macOSのデフォルトsans-serif = Lucida Grande）で、サイズも 8〜20px が各ファイルに直書きでバラバラ

## 前提・わかっていること

- macOS限定・個人用アプリなので、システムインストール済みフォントをそのまま使う（SF Proはライセンス上同梱不可だが、同梱の必要自体がない）
- macOSのシステムUIフォントはCoreTextの内部名 `.AppleSystemUIFont` で取得できる。JUCEでは `LookAndFeel::getTypefaceForFont` をoverrideして差し替えるのが定番
- 日本語グリフはSF Proに含まれないが、JUCE 8はフォントフォールバック対応済みで、CoreText経由でヒラギノに自動フォールバックするはず。**8.0.9で実際に効くかは要ビルド確認（本計画の唯一の技術リスク）**
- 等幅数字: BPM・小節番号・タイムコードは可変幅だと再生中にガタつく。システム等幅UIフォント `.AppleSystemUIFontMonospaced` を数字表示にだけ使う
- サイズはApple HIGのmacOSスケールに寄せる:
  - 小ラベル（ルーラー数字・鍵盤名）: 10–11px（現状の8–9pxは詰めすぎ。11pxを下限にする）
  - 標準（トラック名・ボタン・キャプション）: 13px
  - セクションタイトル: 15px semibold
  - 大タイトル（プロジェクト選択画面）: 20px bold
- Logicの密度感は「フォントを小さくする」ではなく「13px基準で行間・余白を詰める」で作られている
- 現状のフォント指定箇所:
  - `Source/ui/TimelineView.cpp:66` — ルーラー数字 11px
  - `Source/ui/PianoRollView.cpp:51,57` — 鍵盤名 8px / 9px
  - `Source/ui/PianoRollView.cpp:221,224` — タイトル13px・キャプション12px
  - `Source/ui/ProjectChooserComponent.cpp:21,77` — タイトル20px bold・リスト15px

## 実装計画

### Phase 1: システムフォント化とフォールバック確認 [AI🤖]
- [x] カスタムLookAndFeel（`ui/AppLookAndFeel.h/.cpp` 等）を作成し、`getTypefaceForFont` を override:
  - **デフォルトsans-serif名の場合のみ** `.AppleSystemUIFont` のTypefaceを返し、それ以外（`.AppleSystemUIFontMonospaced` 等の明示指定）は基底実装へ委譲する（無条件置換だとPhase 3の等幅フォントまで上書きされる）
- [x] LookAndFeelインスタンスは `DawApplication` のメンバーとして保持し、`initialise` で `setDefaultLookAndFeel` を呼ぶ。`shutdown` では `mainWindow.reset()` → `setDefaultLookAndFeel(nullptr)` → インスタンス破棄の順にする（ダングリングポインタ防止）
- [x] ビルドして起動し、スクリーンショットで確認:
  - 欧文・数字がSF Proになっているか（HelveticaとSFは数字の「1」やRの脚で判別可能）
  - 日本語（トラック名に日本語を入力）がヒラギノで描画されるか（豆腐・崩れが出ないか）
- ~~[ ] フォールバックが効かない場合の代替案を検証: `Font::findSuitableFontForText` / フォント名を直接「Hiragino Sans」にする等~~

### Phase 2: サイズの一元管理 [AI🤖]
- [x] `ui/Fonts.h` を作成し、名前付き定数＋ヘルパーを定義（例: `small()` 11px / `body()` 13px / `title()` 15px semibold / `largeTitle()` 20px bold / `mono()` 等幅）
  - `title()` のsemiboldは `Font::bold` で済ませず、System UI fontのSemiboldスタイルを明示指定できるか確認する（boldとsemiboldは密度が異なる。`.AppleSystemUIFontSemiBold` 等の内部名や `withStyle("Semibold")` を検証）
- [x] 各ビューの直書きサイズを置換:
  - TimelineView ルーラー数字 → small (11px)
  - PianoRollView 鍵盤名 8/9px → small系に引き上げ。**現在の行高は12px（`PianoRollView.h:21` の rowHeight）で11pxフォントは縦に詰まる／切れる恐れがあるため、rowHeight・keyboardWidth・鍵盤ラベルの左右余白もセットで調整対象にする**（特にドラム名の縦詰まりを確認）
  - PianoRollView タイトル/キャプション → body (13px)
  - ProjectChooserComponent → largeTitle (20px) / リスト行は body〜15px
- [x] ビルドしてレイアウト崩れ（ラベル見切れ・鍵盤名のはみ出し）がないか確認

### Phase 3: 数字表示の等幅化 [AI🤖]
- [x] 適用対象（判明分。他にあれば実装時に追加）:
  - 小節ルーラーの数字（`TimelineView.cpp:66`）
  - BPM編集値（`MainComponent.cpp:112` 付近。現状フォント明示指定なし → `Fonts::mono()` の適用が必要）
  - 再生位置ラベル（`MainComponent.cpp:862` 付近。同上）
- [x] `.AppleSystemUIFontMonospaced` を使った `Fonts::mono()` を上記に適用
- [x] 再生中に数字が横にガタつかないことを確認

### 動作確認（一次確認）[AI🤖]
- [x] `cmake --build build` でビルドし、アプリを起動
- [x] 全画面（プロジェクト選択・タイムライン・ピアノロール）を `screencapture` で取得し、レイアウト崩れ（ラベル見切れ・鍵盤名のはみ出し・豆腐文字）がないか確認

### 動作確認（最終判定）[人間👨‍💻]
- [ ] 見た目の好み・Logicらしさの最終判定を目視で確認
- [ ] 日本語トラック名の表示品質（フォールバックの見た目）に違和感がないか確認

## ログ
### 試したこと・わかったこと
- JUCEのmac実装（`juce_Fonts_mac.mm`）は「family名＋style名」でCTFontDescriptor解決する。`.AppleSystemUIFontDemi` のようなfont名指定では引けないため、semiboldは `FontOptions(".AppleSystemUIFont", "Semibold", 15.0f)` で指定（family内に "Semibold" styleが存在することをCoreTextの列挙で事前確認）
- LookAndFeelは `getTypefaceForFont` の手書きoverrideでなく、基底の `setDefaultSansSerifTypefaceName(".AppleSystemUIFont")` を使用（基底実装が「デフォルトsans名のときだけ差し替え、明示指定は素通し」そのもの）
- 日本語→ヒラギノのフォールバックは8.0.9で問題なく機能（起動スクショで豆腐・崩れなしを確認）。代替案検証ステップは不要になった
- ピアノロールを開いた直後の縦スクロール位置が最上部（pitch 127側）でドラム名の無い帯域だった。フォント起因ではない既存挙動（CGEventのscrollWheel合成でスクロールして鍵盤名を確認）

### 方針変更
- 【追加対応】「日本語がでかい」フィードバックを受け、コントロール類のフォントを13pxに統一。原因はJUCEのLookAndFeelデフォルト（TextButton/ComboBox = 高さ連動で最大16px、Label = 15px）で、全角を目一杯使う日本語で大きさが露呈していた。`AppLookAndFeel` に `getTextButtonFont` / `getComboBoxFont` / `getPopupMenuFont` のoverrideを追加し、ToggleButtonはフォントフックがないため `drawToggleButton` を基底コピーで13px化。`getLabelFont` は「Label自身のフォントを返す」実装のためoverrideせず（`Fonts::mono` 等を壊すので）、トラック名・BPMキャプション・SR警告・chooserのラベル/エディタは生成側で `setFont(Fonts::body())`
- 【追加対応2】13px統一後も「自由入力のトラック名で日本語と英語の大小差が気になる」とのことで、**自由入力テキストに限定してCJK光学補正を導入**。`Fonts::forText(base, text)` がCJK含有時に8%縮小（13→約12px）。適用箇所はトラック名Label（bind時とリネーム時）とプロジェクト一覧の行。固定UI文言はOS標準と同じ「同ポイント方式」を維持。トレードオフ: 混在文字列（例「ボーカル録音 take2」）は欧文部分も一緒に縮む／chooserの新規名入力欄はタイピング中の動的切替はせず13px固定
- プロジェクト選択のリスト行は `Fonts::body().withHeight(15.0f)`（planの「body〜15px」の範囲内。主要コンテンツなのでbodyより一回り大きく維持）
- `Fonts::title()`（15px semibold）は定義したが現時点で使用箇所なし。今後のセクションタイトル用
- keyboardWidth 56→80・rowHeight 12→14 に変更。「Low WoodBlock」等の長いドラム名は80pxでも末尾省略されるが許容（Logicも省略する）
