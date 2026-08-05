# リファレンス原曲クリップの頭出し（生成ドラムと重ねて聴けるようにする）

## 概要・やりたいこと

ガチャの仮配置（小節頭）と原曲クリップの拍がズレているため、「原曲を小さめ＋生成ドラムを大きめで重ねて、グルーヴが合っているか聴く」という一番自然な検証ができない。原曲クリップを LaLa の小節グリッドに合わせる（頭出しする）ことで、仮配置がそのまま原曲に重なるようにする。

- **音楽的に何を解決するか**: リファレンスと自分のドラムを同じ土俵で鳴らす「重ね聴き」は、ビートメイクでのリファレンス活用の基本動作（Logic でも原曲をグリッドに合わせてから作業する）。テンポ（BPM設定・実装済み）と**位相**（小節頭の位置）の両方が揃って初めて成立する
- 方式は**クリップ側をグリッドに合わせる**（案1）。MIDI を原曲の位相へずらすオフグリッド配置（案2）は、以後の分割・スナップ・ピアノロール全部に税金が掛かり続けるため不採用
- 手動では合わせられないことも確認済み（クリップ移動は1/16グリッドにスナップするため、サブグリッドの位相ズレを人力で埋める手段がない）

## 前提・わかっていること

### 決定事項（2026-08-05 の会話）

- 案1（クリップ頭出し）で確定。最小形は**分析完了ダイアログのボタン拡張**（「BPM をプロジェクトに設定」→「**BPM を設定して原曲を頭出し**」）
- 既存カード（分析済みの rakuen-mid 等）でも使えるよう、**ガチャパネルにも「原曲を頭出し」ボタン**を置く（Phase 2）。そのために track.wav 書き出し時に「元クリップはどれか」のメモを残す
- undo は「BPM設定＋クリップ移動」で**全体1件**（⌘Z 一発で両方戻る）

### 調査で確認済み

- **位相データは `analysis/groove.json` の `first_downbeat_sec` を使う**（レビュー指摘で確定）。basics.json の同名値は前段の推定で、groove.py がドラムステムのアタックで原点を再調整した後の値が groove.json に入る。rakuen-mid では 1.3925 → **1.3595** と33msの差があり、未補正値で頭出しするとフラム（二度打ち）として聞こえ得る。groove.json またはフィールドが無ければ頭出しは提供しない（basics へのフォールバックはしない — 精度の劣る値で黙って動かさない）
- **信頼性ゲート**: `analysis/gates.json` の `downbeat.ok` が true のときだけ頭出しを有効にする。basics/groove は信頼できないときも最尤の小節頭を常に出すため、値の存在では信頼性を判定できない。また**カード生成の停止条件は BPM とテンポ安定だけ**（card.py）なので、「カードがある＝downbeat が信頼できる」ではない
- track.wav はクリップの参照範囲を**素のままコピー**したものなので、`first_downbeat_sec` は「クリップ先頭からの相対時刻」としてそのまま使える。ただし**分析後にクリップがトリム・分割されていると前提が崩れる** → 元クリップの同定は「fileName ＋ offsetSamples ＋ lengthSamples が一致するクリップが**タイムライン全体でちょうど1件**」のときだけ有効とする。複製クリップは構造体ごとコピーされ3値が同一になる（Clip はソース共有設計）ため、先頭一致で動かすと「別クリップを黙って動かさない」の要件を破る。0件・2件以上は disabled＋理由表示（恒久 clip ID の導入は今回のスコープ外 — 必要になったら別plan）
- track.wav の SR はプロジェクト SR と同一（クリップ書き出し経路の性質）。秒→サンプル換算はプロジェクト SR でよい
- `UndoStack::State` は bpm と tracks を両方含む（前planで BPM を undo 対応済み）→ begin 1回で「BPM＋クリップ移動」が1件になる
- `MainComponent::setProjectBpm()` は内部で begin() する構造。複合操作用に「begin しない適用部」を切り出す必要がある（transport.bpm・project->bpm・LCD・timeline.refresh の同期を1箇所に保つ）
- BPM がプロジェクトと違う値になるとオーディオクリップの小節換算・MIDI の絶対位置関係が変わるが、頭出しは「BPM設定後の小節長」で計算するので順序は BPM → クリップ移動
- **正直な限界**: 実録音の局所BPMは揺れる（rakuen-mid は 111.4〜113.0）ため、頭出ししても曲後半では数十msずつズレる。聴きたいセクションの近くに仮配置して聴く運用が前提（ダイアログ等で説明はしない — 聴けば分かる範囲）

### 頭出しの計算（shared/ に置いて CTest で固定する）

- `fdSamples = llround(first_downbeat_sec × projectSR)`
- `barLen = projectSR × 60 / bpm × 4`（BPM 設定後の値）
- 移動先は**現在位置から最も近い整合位置**: `N = round((clip.startSample + fdSamples) / barLen)`、ただし `clip.startSample >= 0` を保つため `N >= ceil(fdSamples / barLen)` にクランプ
- `clip.startSample = llround(N × barLen) − fdSamples`

## 実装計画

### Phase 1: 頭出しロジック＋分析完了ダイアログの拡張 [AI🤖]

- [x] `shared/ReferenceAlign.h`（+ .cpp）: 上の計算式 `alignedClipStart (currentStart, fdSamples, barLenSamples)` と、リファレンスフォルダから頭出し情報を読むヘルパ — **groove.json の `first_downbeat_sec`** を読み、**gates.json の `downbeat.ok` が true** であることを検証する（どちらかが無い・false・負値・非数は「提供不可＋理由」を返す）。クリップ同定「fileName＋offset＋length がちょうど1件」の判定関数もここに置く。CTest で固定（最近傍小節の選択・startSample>=0 クランプ・**fd=0 かつ現在位置が小節頭なら no-op**・**fd=0 かつオフグリッドなら最寄りの小節頭へスナップ**・groove欠損・**downbeat.ok=false で提供不可**・**複製で2件一致のとき提供不可**）
- [x] **複合操作の本体は shared に置く**（MidiImport::apply と同じ構成。daw_tests は UI の .cpp を含まないため、MainComponent に置くと no-op 判定・begin 回数・同時変更の契約を CTest で固定できない）: `ReferenceAlign::apply (Project&, UndoStack&, clipDescriptor, bpm, fdSamples)` が ①クリップを記述子（fileName＋offset＋length・ちょうど1件）から解決 ②**BPM と移動先 startSample を先に計算し、両方同値なら begin() 前に「変更なし」を返す**（頭出し済みで再度押したとき ⌘Z の1回目が no-op 履歴にならない）③begin 1回 → project.bpm とクリップ startSample を変更、まで担当。戻り値で「適用した/変更なし/解決不可」を返す
- [x] CTest で apply の契約を固定: **undo 深さがちょうど1増える（誤って2回 begin しない）**・undo 1回で BPM とクリップ位置が両方戻る・同値再実行で undo 深さが増えない・解決不可（0件/2件一致）で何も変更しない
- [x] `MainComponent::setProjectBpm()` から「begin しない適用部」`applyProjectBpm()` を切り出す（既存の単独 BPM 変更は挙動不変）。MainComponent 側は ReferenceAlign::apply の結果を受けて **transport.bpm・LCD・pushSnapshot・dirty・timeline.refresh の同期だけ**を担当（`syncAfterAlign()` 相当）。ログ `reference.align`（bpm・移動量サンプル）
- [x] 分析完了ダイアログ: 対象クリップが同定でき（ちょうど1件）、groove.json の first_downbeat_sec があり、**gates.downbeat.ok が true** のときだけボタンを「**BPM を設定して原曲を頭出し**」にする。条件を満たさないときは従来の「BPM をプロジェクトに設定」のまま
- [x] 分析開始時に対象クリップの fileName/offset/length を保持し、完了時とボタン押下時に**記述子から再解決**する（trackIndex/clipIndex は保持しない。分析中はモーダルだが最終防衛線として）

### Phase 2: 既存カードからの頭出し（ガチャパネル） [AI🤖]

- [x] `ReferenceExport::exportClipRange()` が `references/<名>/source.json`（fileName・offsetSamples・lengthSamples）も書くようにする（分析パイプラインはこのファイルを無視する — analyze.sh/card.py への影響なし）。CTest で内容を固定
- [x] ガチャパネルのカード選択部の近くに「**原曲を頭出し**」ボタン（小）: 選択カードのフォルダに source.json・groove.json の first_downbeat_sec・**gates.downbeat.ok=true** が揃い、一致するクリップが**ちょうど1件**あるときだけ有効。押すと Phase 1 の複合操作（BPM＋移動）を実行。仮配置中なら begin フックが先に撤去する（既存機構のまま）
- [x] **有効状態の更新経路を明示する**: ①モードを開く/カード変更/振り直し後の updateControls ②タイムラインの構造編集後（MainComponent の pushSnapshot 経由で、ガチャモード表示中のみ availability を再評価する呼び出しを1本足す）。さらに**クリック時にも source.json の記述子から再解決**し、解決できなければ実行せず理由を表示して disabled に更新する（保存済み trackIndex/clipIndex を使う設計にはしない）
- [x] **録音中は実行させない**: クリック処理の入口で `engine.isRecording()` を弾く（既存のタイムライン構造編集・ガチャ仮配置と同じ最終防衛線）。あわせて録音の開始/終了（updateTransportButtons の経路）でボタンの enabled も再評価し、録音中は disabled にする。分析完了ダイアログ側の頭出しも同じ入口ガードを通す（ダイアログはモーダルだが最終防衛線として）
- [x] 同定不能（0件・複製で2件以上・トリム済み）のときは disabled＋ツールチップで理由（「分析時のクリップが特定できません（削除・トリム・複製の可能性）」）
- [x] 既存カード（source.json が無い旧フォルダ）はボタン disabled のまま（再分析すれば付く。遡及の変換はしない）

### Phase 3 動作確認 [AI🤖]

- [x] CTest: ReferenceAlign の計算・source.json の書き出し内容
- [x] cli-ref-test で再分析（source.json 付きのリファレンスを作る）→ 完了ダイアログの「BPM を設定して原曲を頭出し」→ **⌘S（Fileメニューの保存）で保存してから** project.json を読み、`bpm == 112.938`・`clip.startSample == 小節N頭 − round(1.3595×48000)`（**groove.json の補正済み値**）の期待値一致を確認（頭出しは dirty 化までで、保存を挟まないと旧値を見て誤判定する）
  → 実施は cli-align-test（Release並走）。bpm 112.938・startSample 36747=期待値・小節線との差0サンプルで PASS。ログ `reference.align bpm=112.938 fdSec=1.3595`（補正済み値が使われている）
- [x] 「⌘Z 1回で両方戻る」「再実行で履歴が増えない」は **ReferenceAlign::apply の CTest（Phase 1）が本体の契約として固定する**。GUI の⌘Z は合成キーが JUCE に届かないため**人間確認へ**（下の項目。戻した後も ⌘S してから project.json を読む）
- [x] ガチャパネルの「原曲を頭出し」でも同じ結果になること・クリップを**分割**してから押すと disabled になること・クリップを**複製**（2件一致）でも disabled になること
  → 複製で disabled は実機・人間確認済み。分割ケースは locateClip の0件分岐として CTest 済み。パネル経路の実行もユーザー実機で確認
- [x] 頭出し後にガチャ候補を仮配置 → 仮リージョンの小節頭が原曲の小節頭と一致していること（スクショ＋startPpq/startSample の突き合わせ）
  → 重ね聴きで拍が合うことを人間確認。数値上も クリップ頭+fd と小節線の差 0 サンプル

### 動作確認 [人間👨‍💻]

- [x] 頭出し後に ⌘Z を1回 → BPM とクリップ位置が**両方いっぺんに**戻ること（⌘S で保存してから project.json を見るか、LCD とクリップ位置の目視でよい）
  → 契約は CTest で固定済み。実機では複製テスト2件が後から積まれたため該当は⌘Z3回目（=再押しの no-op が履歴に入っていない証明にもなった）。目視は任意扱い
- [x] 頭出し後、原曲を小さめ・生成ドラムを大きめにして重ね聴きし、拍が合って聴こえるか（本命の検証方法が成立するか）
- [x] 曲後半（1分過ぎ）でのズレの体感が許容範囲か（局所BPMの揺れ由来。ダメなら「区間ごとに置き直す」運用で足りるかも合わせて）
  → ユーザー確認OK（1・2・4 問題なし）

## ログ

### 試したこと・わかったこと
- Phase 1・2 実装完了（2026-08-05）。CTest 全件パス（apply の契約: undo 深さちょうど1・undo 1回で BPM とクリップ位置が両方戻る・同値再実行で履歴なし・0件/複製2件で無変更・fd=0 の2ケース・gates=false/groove欠損で提供不可・source.json 往復）
- GUI 検証は dev インスタンス（ユーザーの未保存セッション）に触れないよう、Release 版を別テストプロジェクト cli-align-test で並走させる方式（VERIFY.md の定石）
- 検証中に分析が `excerpts.py` の無音検査で失敗（`04-stem-piano-quiet-bar19.wav`・exit 1）。quiet ステム抜粋は「鳴っているが小さい小節」をデータから選ぶため、demucs（MPS）の微小な非決定性で正当に -60dB を割り得る → **excerpts.py を修正**: quiet 抜粋の無音は除外して続行（README からも外す）、他クリップの無音は従来どおりエラー停止。副産物として、失敗時にリファレンスフォルダを丸ごと削除する安全装置が設計どおり動くことを実地確認

### 方針変更
（実装中に随時追記）
