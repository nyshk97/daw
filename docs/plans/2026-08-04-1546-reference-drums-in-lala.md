# ドラムの分析→ガチャを LaLa からシームレスに（.midインポート / 右クリック分析 / ガチャUI）

## 概要・やりたいこと

Phase 2 で作ったドラムガチャ CLI（`tools/gacha/drums.py`）と分析パイプライン（`tools/reference/`）を、LaLa の中から使えるようにする。ベース生成より先にここをやる — 生成物を LaLa で鳴らして手直しする往復が閉じていないと、CLI で何を作っても曲にならないため。

3ステップで、各ステップが単体で価値を出す順に架ける:

1. **`.mid` インポート** — ガチャ→LaLa の橋。CLI ガチャの候補を本番音色（Drum Kit）で鳴らして手直しできるようになる
2. **リージョン右クリック「リファレンスとして分析」** — LaLa→分析の橋。トリム判断が人手から消える（リージョン範囲がそのままトリム）
3. **ガチャUI（右パネル第3タブ）** — 振り直し・ロック・仮配置試聴・採用を LaLa 内で完結

## 前提・わかっていること

### 決定事項（/dig 2026-08-04）

- **ツール群のパス解決は `~/daw` 固定**（yt-dlp の既知絶対パス方式と同じ発想）。リポジトリ不在・venv 未構築ならメニュー/タブを無効化して理由をツールチップ等で表示。別マシンでは clone 先を `~/daw` に揃えれば動く
- **レポート系（report.sh / レポートを開く / このリファレンスについて聞く）は今回スコープ外**。当面 CLI（`ref:report`）で
- **Step 1**: Finder / ファイルブラウザ→タイムラインの D&D（オーディオと同じ導線）＋ファイルブラウザでの表示・取り込み対応。**ch10 のノートは Drum Kit（drums=true）トラック、他チャンネルのノートは GM トラックへ分離**（混在 SMF は2トラックになる。レビュー指摘で「ch10があればDrum Kit＋全マージ」の矛盾を解消した仕様）。PPQ 換算（ファイルの ticks_per_beat → LaLa の 960）はどの経路でも実施
- **`.mid` は AudioFileTypes に足さない**: ファイルブラウザの表示・ダブルクリック・D&D・試聴・Fileメニューは全て `AudioFileTypes::isSupported()` を共用しており、単純に足すと「MIDIを音声として試聴」「MIDIトラックへのドロップがサンプル音源割り当てに化ける」「AudioImporter に流れる」の誤動作になる。**`MidiFileTypes` を別に設け、全入口で MIDI 分岐 → 専用の取り込み経路へ**。MIDI 取り込みは試聴対象外・サンプル音源割り当ての対象外・**プロジェクトのサンプルレートを確定させない**
- **Step 2**: クリップの参照範囲（offset〜offset+length）を**素のままコピー**（リージョンゲイン・フェード不適用。ループは1周分。分析対象は原曲であって LaLa 上の加工結果ではない）→ `<プロジェクト>/references/<名前>/track.wav`。名前は**クリップ名から自動サニタイズ・衝突は連番**（入力UIなし）。実行は**モーダル進捗オーバーレイ**（design doc どおり。約4分・キャンセル可・低頻度なので許容）。完了ダイアログで「**BPM をプロジェクトに設定**」を提案（1回きりコピー・undo可。押さなければ何もしない）。キーはプロジェクトに概念が無いので対象外
- **BPM は現状 undo 非対応**（`applyBpmText()` は直接更新のみ・`UndoStack::State` に bpm が無い）。「undo 可」を成立させるには **UndoStack への BPM 追加が前提作業**になる（begin/undo/redo/State・undo/redo 後の transport.bpm / LCD / タイムライン同期。既存の LCD からの BPM 変更も同時に undo 対応になる）
- **キャンセル・失敗時は今回作成したリファレンスフォルダを丸ごと削除する**: `analyze.sh` はステム出力ディレクトリが**存在するだけ**で分離処理をスキップするため（`[ -d ... ] ||`）、不完全な残骸が次回実行を壊す。さらに名前は衝突連番方式なので、track.wav だけ残しても**再実行時は別フォルダが作られて再利用されない**＝大きな孤児ファイルになるだけ。コピー元リージョンは残っているので全削除が最も単純（マーカー検証・再利用規則より税金が小さい）
- **Step 3**: 右パネルに「ガチャ」を追加。**現在の右パネルは Mode { notes, files } のボタン切替でタブは存在しない** — 実装は `Mode::gacha` の追加＋ヘッダーに第3ボタン（ショートカットは付けない。ボタンのみ）。カード選択はプロジェクト内 `references/*/card.json` のコンボボックス（無ければ「リージョンを右クリック→分析」への案内文）。**8候補一覧＋kick/snare/hat のロックトグル＋「振り直す」**（CLI と同じモデル。裏は drums.py を SpawnedProcess で叩くだけ・一瞬）。**全レーンをロックしたら「振り直す」を無効化**（CLI は全ロック時に重複排除して1件しか生成しない — 8件前提の一覧と矛盾するため、そもそも振れないことを UI で示す）。**候補選択＝対象トラックへ MIDI リージョンを仮配置**し、普段の再生で曲と一緒に鳴らす（単体プレビュー経路は作らない）。対象トラックは選択中の Drum Kit トラック、無ければ「Drums」トラックを自動作成。配置は**再生ヘッドの小節頭**（録音開始位置と同じ丸め）に4小節
- **仮配置と undo の方式（レビューを受けて確定）**: `UndoStack::begin()` は「編集**前**の状態を保存する」方式なので、「Project 直接変更→残す時点で積む」は成立しない（残した後の undo が no-op になる）。プレビュー層（Project 外で再生・描画）は再生スナップショット・タイムライン描画・ヒットテストの全部に特殊経路が要るため不採用。採用する形:
  - GachaSession が**仮配置開始時点の状態**（配置前の tracks 等）を保持し、候補切り替えは Project 直接書き換え＋スナップショット再push（UndoStack 非経由）
  - 「残す」で UndoStack に**保存済みの配置前状態を before として1件積む API**（`pushCommitted(beforeState)` 相当）を追加して確定。**pushCommitted は redo 履歴を破棄し、Keep でプロジェクトを dirty 化する**（両方 CTest で固定）
  - **中央の `cancelGachaPreview()` を1つ用意し、モデルに触る全入口から呼ぶ**。begin のフックだけでは足りない（undo/redo は `undoStack.undo/redo()` を直接呼び begin を通らない。バウンス・リージョン書き出しも begin を通らないため未確定候補が成果物に混入する）。呼び元として最低限: ①編集開始（begin フック）②**Undo / Redo の直前** ③**保存・バウンス・リージョン書き出しの直前** ④モードを離れる・カード変更・プロジェクトを閉じる ⑤**Copy（`copySelectedItem()`）** — Copy はモデルを変更せず begin も通らないため、仮リージョンを ⌘C → 閉じる → ⌘V で「残す」を迂回して確定できてしまう
  - **仮リージョン・自動作成トラック自体への操作は「撤去して中止」**: ダブルクリック（ピアノロール）・分割・複製・削除・**Copy** 等の対象が仮オブジェクトだったら、撤去した上でその操作自体を続行しない（撤去後の古い index で開くとクラッシュ・別対象への誤操作になる）
  - キャンセル時は仮リージョンに加え、**このセッションで自動作成した Drums トラックもトラックごと撤去**する
- **drums.py に機械可読出力を追加**: 人間向け stdout の文字列解析で候補リストを作ると連携契約が脆い。`--porcelain` を CLI に追加し、LaLa はそれを読む（回帰テスト付き）。仕様: **stdout は JSON Lines のみ**（候補ごとに1行: ファイル名・レーン seed・生成/スキップ）、人間向けの進捗・補完申告は **stderr** へ、成否は **exit code**。「JSON でない行を読み飛ばす」形の文字列依存を残さない
- 細部の決め打ち: フォルダ名衝突は連番 / LaLa 経由のガチャも3点セット（.mid/.wav/.json）生成のまま（gacha/ の肥大は気にしない。ステム 636MB に比べ誤差）/ ラボ置き場（`~/Music/daw/references/`）の既存カードは一覧に**含めない** — 試すときは手でプロジェクトの `references/` へコピー

### 調査で確認済み（既存実装）

- **右クリックメニュー**: `TimelineView.cpp` 〜1920 に itemID 方式の PopupMenu。項目追加は1行＋コールバック分岐
- **Clip モデル**: `fileName`（プロジェクト相対）＋`offsetSamples`＋`lengthSamples` の非破壊参照 → track.wav はソース WAV の範囲コピーで作れる
- **MIDI モデル**: `MidiRegion`（startPpq 絶対）/`MidiNote`（startPpq リージョン相対・velocity 1..127）。**LaLa の PPQ は 960**（`Ppq::ticksPerQuarter`）。ガチャの .mid は 480 なので換算必須
- **SpawnedProcess**: posix_spawn＋プロセスグループ管理・行単位読み取り・キャンセル対応の汎用機構が `shared/` にある（yt-dlp で実戦済み）。**argv[0] は絶対パス必須**
- **進捗オーバーレイ**: `UrlImportOverlay` ＋ `UrlDownloader`（juce::Thread）の形をそのまま踏襲できる
- **D&D 受け口**: `TrackHeadersView` が Finder（FileDragAndDropTarget）とファイルブラウザ（DragAndDropTarget）の両方を受けている。オーディオ/instrument の振り分けが既にある
- **ノート単発プレビュー**: `PianoRollView::onPreviewNote` → PlaybackEngine の経路あり（候補一覧のクリック音などに流用可能）
- **ガチャ CLI**: `drums.py` は card.json（またはフォルダ）を受けて `gacha/` に3点セットを出す。stdout に生成ファイル名を1行ずつ申告。レーン seed はファイル名とサイドカーの両方にある。`--lock kick=HEX8,...` で固定
- **daw_tests は UI の .cpp を含まない** → 判断ロジック（.mid→ノート列変換・ch10判定・PPQ換算・リージョン範囲の書き出し計算・候補仮配置の差し替え）は `shared/` に置いてテストで固定する（CLAUDE.md「判断ロジックを ui/ に書かない」）

### 設計上の注意（実装時に確認・対処すること）

- **`analyze.sh` が cwd 非依存で動くか**を最初に確認する（`.venv` や兄弟スクリプトを相対パスで参照していれば、LaLa からの起動用に自身の位置基準に直す）
- **juce::MidiFile** で読む。変換仕様（レビューを受けて確定）:
  - type 0/1 両対応。**SMF トラックはチャンネル群（ch10 / それ以外）の中でマージ**する（「全トラックを1リージョンに」ではない — ch10 分離と両立させる）
  - **SMPTE 形式は明示的に非対応エラー**（`getTimeFormat()` が負値。ticks_per_beat として換算すると位置が壊れる）
  - note_on/note_off の対応付けキーは**（MIDIチャンネル, pitch）**とし、その中で最も古い未クローズ note_on と組にする（pitch だけだと ch1 と ch2 の同音で他チャンネルの off が誤って閉じる。重複 note_on は各々を独立ノートに）。孤立 note_off は無視、クローズされない note_on はファイル末尾（最終イベント時刻）で閉じる。velocity 0 の note_on は note_off 扱い。リージョン格納時にチャンネル情報を捨てるのは問題ない（対応付けにだけ使う）
  - `MidiNote::lengthPpq` = off−on の換算値（最低 1）。`MidiRegion::lengthPpq` = 最終ノート終端を**小節単位に切り上げ**
  - **drums.py 側の前提修正が1つ要る**: 生成器は note_on を小節末 1 tick 前まで許し note_off を +60 tick に置くため、スウィング＋正方向ジッターが重なった最終ハットの note_off が小節境界を越え、**CLI の4小節候補が取り込みで5小節リージョンになり得る**。drums.py で note_off を `bars × TICKS_BAR` にクランプし、「全 note_off ≤ bars×TICKS_BAR」を `gacha:test` で固定する。**出力が変わりうる変更なので GENERATOR_VERSION を 2 に上げ、ゴールデンを採り直す**（「同名で違う音」を作らない原則）
  - テンポトラックは**無視**（プロジェクト BPM が真実。PPQ ベースなので相対配置は保たれる）
- **入口ごとの配置位置**: タイムライン D&D＝ドロップ位置の小節頭 / Fileメニュー＝曲頭（小節1。オーディオと同じ）/ ファイルブラウザのダブルクリック・D&D＝D&D はドロップ位置の小節頭・ダブルクリックは**再生ヘッドの小節頭** / **トラックヘッダーへの .mid D&D＝再生ヘッドの小節頭**（ヘッダーには横座標が無いため）。混在 SMF の2トラックは**同じ開始位置**に置き、**全体で undo 1件**
- 仮配置リージョンの undo 非汚染: 候補切り替えは UndoStack を経由せず Project を直接書き換え＋スナップショット再push、「残す」時点の状態で1件積む。タブを閉じる/カードを変える/候補未確定でプロジェクトを閉じる時は仮リージョンを取り除く（取りこぼすと保存に混入する）
- ガチャ実行後の候補一覧は「**今回の実行で申告されたファイル名**」から作る（gacha/ フォルダの全列挙だと過去の振り直し分が混ざる）
- BPM 反映は既存の BPM 変更経路（undo 対応）を使う。カード BPM は小数（112.946）なので既存 UI の表示・丸めと衝突しないか確認

## 実装計画

### Phase 1: `.mid` インポート [AI🤖]

- [x] `drums.py` の note_off クランプ（前提の「drums.py 側の前提修正」）: 全 note_off ≤ bars×TICKS_BAR を保証し `gacha:test` で固定。**GENERATOR_VERSION を 2 に上げてゴールデンを採り直す**
- [x] `shared/MidiFileTypes.h`: `.mid` 判定を AudioFileTypes とは**別に**新設（AudioFileTypes には足さない — 試聴・サンプル音源割り当て・AudioImporter への誤流入を防ぐ）
- [x] `shared/MidiImport.h/.cpp`: juce::MidiFile → **ch10 のノート列とそれ以外のノート列に分離**して返す。PPQ 換算（ticks_per_beat → 960、丸めは round）・SMPTE 形式はエラー・note on/off 対応付け（キーは (channel, pitch)。重複/孤立/未クローズは上記仕様）・lengthPpq とリージョン長（小節切り上げ）の算出・type 0/1 両対応・不正/空ファイルはエラー返し
- [x] `shared/` に**適用ヘルパ**（MidiImport の結果 → Project への反映）: ch10 ノート→Drum Kit（drums=true）トラック・他チャンネル→GM トラック（混在 SMF は2トラック・**同じ開始位置**）・トラック名はファイル名由来・**プロジェクトのサンプルレートは確定させない**・undo は全体で1件。**daw_tests は UI の .cpp を含まないため、トラック生成・配置・undo の判断はここに置いて CTest で固定する**（UI は呼ぶだけ）（`MidiImport::apply`）
- [x] 入口の分岐: Finder D&D（`TrackHeadersView`/`TimelineView`）・ファイルブラウザ（表示・ダブルクリック・D&D。試聴は対象外のまま）・Fileメニュー（対象拡張子に追加）の全入口で MidiFileTypes 判定→ 適用ヘルパへ。配置位置は前提の「入口ごとの配置位置」どおり。**MIDIトラックへの .mid ドロップがサンプル音源割り当てに化けないこと**
- [x] Fileメニューの文言更新: `Shortcuts.h` のテーブル（真実の源）の「オーディオを読み込む」を「オーディオ/MIDIを読み込む」へ（ツールチップ・⌘?一覧は自動追従）
- [x] CTest: MidiImport の変換（PPQ 換算の数値・ch10/他chの分離・(channel,pitch) 対応付けの各ケース（別chの同音・重複・孤立・未クローズ）・リージョン長の小節切り上げ・SMPTE エラー・velocity クランプ・不正ファイル）と適用ヘルパ（SR が確定しないこと・混在2トラックが同位置・undo 1件）を fixture .mid（juce::MidiFile で生成）で固定

### Phase 1 の動作確認

- [x] [AI🤖] ガチャ既出力の `.mid`（楽園ベイベー gacha/）をテストプロジェクトへ D&D 相当（CGEvent or Fileメニュー AXPress）で取り込み → project.json のノート数・startPpq が元 .mid と一致（480→960 換算済み）・トラックが drums=true であることを確認
- [ ] [人間👨‍💻] 取り込んだ候補を再生して Drum Kit の音で鳴ること・ピアノロールで手直しできることを確認

### Phase 2: リージョン右クリック「リファレンスとして分析」 [AI🤖]

- [x] **UndoStack へ BPM を追加**（State・begin/undo/redo・undo/redo 後の transport.bpm / LCD / タイムライン同期。`applyBpmText()` にも `begin()` を通し、既存の LCD からの BPM 変更も undo 対応にする）。CTest で「設定→undo→redo」を固定（BPM 変更は `setProjectBpm()` に一本化）
- [x] `analyze.sh` の cwd 非依存化（既に `cd "$(dirname "$0")"` で自身の位置基準・修正不要と確認）＋ツール群の存在チェック（`~/daw/tools/reference/analyze.sh` と `.venv/bin/python`）を `shared/ReferenceTools.h` に
- [x] `shared/` にリージョン範囲書き出し: ソース WAV の offset〜offset+length を `references/<名前>/track.wav` へコピー（名前サニタイズ・衝突連番込み）。CTest で範囲・命名を固定（`shared/ReferenceExport`）
- [x] 右クリックメニュー（オーディオリージョンのみ）に「リファレンスとして分析…」を追加。ツール不在時は disabled＋理由
- [x] 分析ワーカー（UrlDownloader と同じ juce::Thread＋SpawnedProcess 構成）＋モーダル進捗オーバーレイ（stdout 行を流す・キャンセルで terminate）（`audio/ReferenceAnalyzer`・`ui/ReferenceAnalysisOverlay`）
- [x] **キャンセル・失敗時は今回作成したリファレンスフォルダを丸ごと削除**する（前提の「キャンセル・失敗時は〜」参照。名前連番方式では track.wav を残しても再利用されず孤児になる）
- [x] 完了ダイアログ: 成功時「分析が完了しました（BPM 112.9 / D major）」＋「BPM をプロジェクトに設定」ボタン（上の undo 対応済み経路で）。**キーは card.json でゲート落ち省略されることがある** — 無ければ BPM だけ表示する。card.json 自体が生成されなかった場合（BPM/テンポのゲート落ち）はその旨を表示（analyze.sh の出力から拾う）
- [x] ログ: `reference.analyze.start/done/fail/cancel`（Log.h。名前・所要時間）

### Phase 2 の動作確認

- [x] [AI🤖] URL取り込み済みのテストプロジェクトでリージョンを右クリック→分析（Ctrl+左クリック合成＋メニュー項目クリック、1プロセス完結）→ オーバーレイ表示のスクショ → 完了後に `references/<名>/card.json` の実在と BPM 値、ログの `reference.analyze.done` で裏取り
- [x] [AI🤖] 分析中キャンセル（demucs 途中）→ プロセスグループが残らないこと（`pgrep -g`）・今回のリファレンスフォルダが丸ごと消えること・**同じリージョンで再実行して完走する**こと（pass 条件）
- [ ] [人間👨‍💻] BPM 設定ボタンを押して LCD の BPM 表示が変わり ⌘Z で戻ることを確認

### Phase 3 前の準備 [人間👨‍💻]

- [x] 楽園ベイベーのリファレンスをテスト用プロジェクトの `references/` へコピーする（またはAIが Bash でコピーしてよい）
  → Phase 2 の動作確認で `~/Music/daw/cli-ref-test/references/rakuen-mid/`（card.json つき）が生成済み。Phase 3 の確認はこれを使う

### Phase 3: ガチャUI（右パネル第3モード） [AI🤖]

- [x] `drums.py` に `--porcelain` を追加: **stdout は JSON Lines のみ**（候補ごとに1行: base・レーン seed・generated/regenerated/skipped）・人間向け出力は stderr・成否は exit code。`gacha:test` に回帰を追加
- [x] **UndoStack に確定用 API を追加**: 保存済みの「配置前状態」を before として1件積む（`pushCommitted(beforeTracks, project)`。**redo 履歴を破棄**）。あわせて **begin のフック**（`willBegin`。状態保存の**前**に呼ばれる）を設ける。CTest で「仮配置→他編集→undo で仮リージョンが復活しない」「残す→undo→redo」「pushCommitted が redo 履歴を消す」を固定。**dirty は MainComponent の private 状態なので CTest では `GachaSession::keep()` が「確定変更あり」を返すところまで**を固定し、実際の dirty 化はウィンドウタイトル末尾の「●」で GUI 確認（動作確認に含める）
- [x] `shared/` にガチャ候補管理（GachaSession）: 実行結果（候補ファイル名・レーン seed）の保持・仮配置開始時点の状態保持・選択候補の仮配置/差し替え/撤去・「残す」確定（上の API へ）。**このセッションで「Drums」トラックを自動作成した場合、キャンセル時はトラックごと撤去**。CTest で差し替え・撤去漏れなし（トラック含む）を固定
- [x] **中央 `cancelGachaPreview()` と呼び元の網羅**: ①begin フック（willBegin） ②Undo/Redo の直前 ③保存（trySave）・バウンス（startBounceFlow）・リージョン書き出し（startRegionExportFlow）・録音開始・ペーストの直前 ④モードを離れる・カード変更・プロジェクトを閉じる ⑤Copy（copySelectedItem の最終防衛線）。**仮リージョン・自動作成トラック自体への操作は撤去した上でその操作を中止**（タイムラインは mouseDown を単一のガード点にした — 選択・ドラッグ・右クリック・ダブルクリックの全ジェスチャーがここを通るため。加えて openPianoRoll・requestDeleteTrack・reorderTrack・ヘッダのリネーム/楽器変更（editBlocked）にも個別ガード）
- [x] drums.py 実行: `~/daw/tools/gacha/drums.py` を venv の python で spawn（`--count 8` `--porcelain`・ロックがあれば `--lock`）。ワーカーは設けず SpawnedProcess の同期読み切り（実測0.5秒・10秒タイムアウト付き）
- [x] 右パネルに `Mode::gacha` を追加（ヘッダーに第3ボタン「ガチャ」。ショートカットなし）: カードのコンボボックス（`references/*/card.json` 列挙・無ければ案内文）・候補8件の一覧・kick/snare/hat ロックトグル・「振り直す」「残す」ボタン。ツール不在時はパネル内に理由表示。**全レーンロック時は「振り直す」を無効化**（`ui/GachaPanelView`）
- [x] ガチャボタンのアイコン: IconButton に Icon::dice（サイコロの線画・3の目）を追加
- [x] 候補選択 → 対象トラック（選択中の drums MIDI トラック or「Drums」自動作成）の**再生ヘッド小節頭**に4小節の MIDI リージョンを仮配置（Phase 1 の MidiImport を流用）。別候補選択で差し替え（**位置は初回配置を維持**）
- [x] ロックの実装: 選択中候補の porcelain 出力（一覧に保持した seed）からロック対象レーンの seed を取り `--lock` へ
- [x] 仮配置の撤去経路を塞ぐ: モードを離れる・カード変更・プロジェクトを閉じる/保存前に未確定リージョン（＋自動作成トラック）を取り除く
- [x] ログ: `gacha.roll`（count・locks）/ `gacha.pick`（候補名）/ `gacha.keep` / `gacha.cancel_preview`

### Phase 3 の動作確認

- [x] [AI🤖] ガチャモードを開きカード選択 → 「振り直す」→ 一覧に8件（`references/<名>/gacha/` に3点セット×8）→ 候補クリックで project 内の仮リージョン差し替わり（保存前の project 状態をログ/AXで裏取り）→「残す」→ ウィンドウタイトル末尾に「●」（dirty 化）→ 保存後 project.json に確定リージョンとノートが残る
- [x] [AI🤖] 仮リージョンを ⌘C → 撤去され Copy が中止されること（後で ⌘V しても仮候補が貼られない）
  → JUCE アプリには合成キーが届かないため GUI では「仮リージョンをクリック→即撤去（選択自体が成立しない＝⌘C の対象になれない）」を実機確認。copySelectedItem 側の最終防衛線はコードガード
- [x] [AI🤖] ロック1レーン→振り直し → 新候補のファイル名の該当レーン seed が固定されていることを確認（8/8件 k=35ae22ed）。全レーンロックで「振り直す」が無効化されることを確認
- [x] [AI🤖] 仮配置中に別の編集（リージョンミュート）→ ログ順で `gacha.cancel_preview` → `region.mute` を確認（begin フックで先に撤去される）。undo 非復活は CTest（willBegin フック）で固定
- [x] [AI🤖] 仮配置中の ⌘Z / ⌘B → 合成キー不可のため GUI 確認は不可。undo 直前の撤去は performUndo/performRedo 冒頭の cancelGachaPreview()＋CTest、バウンス非混入は startBounceFlow 冒頭の cancelGachaPreview() で担保（コード経路）
- [x] [AI🤖] 仮配置のまま候補未確定でプロジェクトを閉じる → project.close dirty=0・project.json は確定分の1リージョンのみ（仮リージョン非混入）
- [ ] [人間👨‍💻] 曲を再生しながら候補を切り替えて「差し替わって鳴る」体感・ロックして振り直す操作感・「残す」後にピアノロールで手直し、の一連を確認（ガチャとして楽しいかの判定）

## ログ

### 試したこと・わかったこと
- Phase 1 動作確認（2026-08-04）: Fileメニュー AXPress → NSOpenPanel の ⌘⇧G でパス入力 → 取り込み成功。project.json のノート72件が元 .mid と (startPpq×2, lengthPpq, pitch, velocity) 全一致・drums=true・リージョン4小節（15360）・sampleRate 0.0 のまま・ログ `midi_import.done tracks=1 drumNotes=72`
- NSOpenPanel の ⌘⇧G シートに日本語入りパスを `keystroke` で打つと Return が効かない（パスが不可視に壊れる）。ASCII パスへコピーしてから打つと通る
- drums.py の note_off クランプでは既存ゴールデンの .mid/.wav バイトは不変（fixture ではクランプ非発動）。GENERATOR_VERSION 2 でファイル名の cfg ハッシュ部だけ変わった
- Phase 2 動作確認（2026-08-04）: cli-ref-test（楽園ベイベー track.wav の 30s〜150s 抜粋クリップ）で右クリック→分析。①キャンセル（demucs 途中）: `pgrep -g` 0件・references/rakuen-mid 丸ごと削除 ②同じリージョンで再実行→129秒で完走・card.json 実在（BPM 112.938 / D major）・完了ダイアログに「BPM をプロジェクトに設定」/「閉じる」表示 ③閉じるでは BPM 100 のまま（無変更）
- JUCE の右クリックメニューは CGWindowList で「クリック後に現れた owner の新窓」として特定でき、高さ÷項目数の等分クリックで項目選択できる（refmenu.swift・1プロセス完結）

- Phase 3 動作確認（2026-08-04）: cli-ref-test で一連を実機確認。振り直す→8候補（gacha/ に24ファイル・`gacha.roll count=8`）→候補クリックで「Drums」自動作成＋4小節仮リージョン→別候補で差し替え（リージョン増えず・project.json 未変更）→kick ロック振り直しで8/8件 seed 固定→全ロックで「振り直す」無効→仮リージョンクリックで撤去（トラックごと）→別編集で begin フック撤去→「残す」→●→保存で project.json に76ノート確定→仮配置のまま閉じても非混入
- ミスクリックがウィンドウ外（デスクトップ）に落ちると Finder にフォーカスが移り、以後の JUCE ボタンクリックが不発になる（activate で復帰）。-abs クリックは必ずウィンドウ矩形内か確認してから撃つこと（グローバル CLAUDE.md ⑩の実例）

### レビュー対応（2026-08-04）

- [P1] 仮トラックの空エリアのダブルクリック（onWillEditModel の begin フックが仮トラックを撤去 → 失効参照へ書き込み）→ `handleLaneDoubleClick` の空エリア作成前に `onPreviewObjectGesture(track.id, 0)` で「撤去して中止」。実機確認: ダブルクリックで cancel_preview のみ・リージョン非作成・クラッシュなし
- [P1] 仮トラックへの音源D&D（finishInstrumentImport の begin 後に trackIndex が範囲外）→ `startInstrumentImport` 冒頭で仮トラック宛は「撤去して中止」。他トラック宛は仮オブジェクトが末尾に居る不変条件により index 不変で安全
- [P2] カード変更で前カードの候補が残る → `cardBox.onChange` で候補一覧・選択・ロックをクリア（MainComponent 側の GachaSession も同期）
- [P2] ロック表示と生成条件の食い違い → ロックを「トグルON時に選択中候補から seed を確保する」方式に変更。未選択時はONにできず（disabled）、振り直しで選択が外れてもONの間は確保済み seed が `--lock` に渡る。実機確認: 選択なしの2回目の振り直しでも locks=kick=f24e120b が維持
- [P2] 分析中に再生を止められない → `startReferenceAnalysis` でバウンスと同じ `stopPlaybackForBounce()` を呼び、開始時に再生を停止

### 方針変更
- **タイムラインの仮オブジェクトガードは mouseDown を単一チョークポイントにした**: 選択・ドラッグ・右クリックメニュー・ダブルクリックの全ジェスチャーが `handleLaneMouseDown` を通るため、ここで「撤去して中止」すれば下流の操作（分割・複製・削除・⌘C・ピアノロール）に仮オブジェクトが渡ることが構造的にない。個別操作へのガードは最終防衛線として残置
- **cancelGachaPreview のヘッダ rebuild は非同期**（begin フック経由でヘッダ自身のコールバック内から呼ばれると同期 rebuild が実行中のコンポーネントを破壊するため）。撤去直後は `TrackHeadersView::unbindAll()` で 30Hz timer のダングリング参照を防ぐ。あわせてヘッダのリネーム/楽器変更は `editBlocked` コールバックで仮トラックへの書き込み自体を止める
- **drums.py の実行はワーカーを設けず同期**（SpawnedProcess 読み切り・実測0.5秒・10秒タイムアウト）。plan の「実行ワーカー」より単純な形で足りた
- **候補差し替えの位置は初回配置を維持**（「差し替え」の自然な解釈。別の場所で聴きたいときは一度撤去して仮配置し直す）
- **porcelain の status は generated / regenerated / skipped の3値**とし、実行内重複（全レーンロック）は行を出さない（受け手の一覧が「ユニークな候補」になる）
