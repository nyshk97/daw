# リファレンス分析（analyze）の並列化による高速化

## 概要・やりたいこと

「リファレンスとして分析」の analyze パイプライン（tools/reference/analyze.sh）を、**精度を一切変えずに**約4分45秒 → 約2分弱へ短縮する。

- 実測（楽園ベイベー・本編4分35秒）で、ボトルネックは demucs（GPU・計48秒）ではなく librosa 系 CPU ステップ（計約230秒）が直列に並んでいることだった
- GPU 実行中に CPU が遊び、CPU 実行中に GPU が遊んでいる。依存グラフに沿って並走させるだけで2倍超の短縮になる
- 各ステップの計算内容・パラメータは不変。出力 JSON/PNG は現状と同一（demucs の MPS 非決定性は既存の性質で悪化しない）

## 前提・わかっていること

### ステップ別の実測（直列合計 約4分45秒）

| ステップ | 実測 | 依存 |
|---|---|---|
| overview.py | 4秒 | track.wav |
| demucs htdemucs (MPS) | 24秒 | track.wav |
| demucs htdemucs_6s (MPS) | 24秒 | track.wav（GPU競合を避け htdemucs の後に直列） |
| basics.py | 35秒 | track.wav |
| stems.py --label 4s | 30秒 | demucs4s + basics.json |
| stems.py --label 6s | 45秒 | demucs6s + basics.json |
| arrange.py | 0.4秒 | stems-6s.json |
| groove.py | 68秒 | demucs4s + basics.json（内訳: ドラム解析＋bass の librosa.pyin） |
| topline.py --label other | 13秒 | demucs4s (other/bass) + basics.json |
| topline.py --label 6s-{piano,guitar,other} | 13秒×3 | demucs6s + demucs4s (bass) + basics.json |
| gates.py / excerpts.py / card.py | 計約1.5秒 | 全 analysis 出力（直列の尻尾のまま） |

### /dig-lite での決定事項

- **並列の組み方**: Python DAG オーケストレータ（`analyze.py` 新設）。前段完了次第すぐ次を起動（3段バリア方式より約40秒速い）。`analyze.sh` は互換のための薄い入口として残す（LaLa の `ReferenceTools.h::analyzeScript()`・`analyze-url.sh` の呼び出し先は変えない）
- **進捗表示**: オーケストレータが「実行中: ○・○・○」の集約行を状態変化のたびに stdout へ出力。ダイアログ（ReferenceAnalysisOverlay）は UI 構造無変更で「目安 約4分」→「約2分」の文言だけ直す
- **失敗時**: fail-fast。1ステップ失敗で残りの実行中ステップを停止して非ゼロ終了（現状の `set -e` と同じ意味論）。失敗ステップ名と出力 tail は **stderr** に出す（詳細は「実装上の注意」）
- **ステップ内並列（②）**: `stems.py` はステム単位（`analyze()` が完全独立）、`groove.py` はドラム解析／ベース解析（pyin **2種**: JSON 用 hop=128 と描画用 hop=512）の2分割を multiprocessing で並列化。計算内容不変なので出力同一
- **demucs の順序**: htdemucs → htdemucs_6s の直列（GPU の取り合いになるだけなので並べない）。その間 CPU ステップを並走させる
- **CPU 同時実行の上限**: 依存充足順に無制限に起動しない。この環境は論理10コア（うち P コア4）で、無制限だと stems-6s（最大6ワーカー）＋topline×3＋groove などが重なり NumPy 内部スレッドも加わって、単体実測の合算による「約2分」の前提が崩れる。上限は**トップレベルのプロセス数ではなくステップごとの重み（≒内部ワーカー数）で数える**: 各 DAG ステップに重み（stems=Executor のワーカー数・groove=2・topline/basics/overview=1、demucs は GPU 主体なので 0 扱い）を持たせ、実行中の重み合計が予算（初期値: 4）以下のときだけ起動する。Phase 2 の Executor の max_workers はこの重みと一致させる。**Phase 1 完了時に実測してから Phase 2 の並列度（重みと予算）を決める**

### 実装上の注意

- LaLa は `ReferenceAnalyzer`（audio/ReferenceAnalyzer.h）が analyze.sh を外部プロセスで起動し、キャンセル時に **pgid ごと kill**（TERM → 1秒 → KILL、`SpawnedProcess.cpp` の termGraceMs=1000）する。~~ステップごとに別プロセスグループを作ってはいけない~~ → **実装レビューで方針転換**: ステップは各自のプロセスグループに入れ（`process_group=0`）、orchestrator が SIGTERM を例外に変換して全ステップグループへ TERM → 0.5秒 → KILL を伝播する（ログ > 方針変更 参照）
- **子プロセスの stdout/stderr は起動直後から継続的に drain する**: 各スクリプトは大きな JSON を stdout に出す（例: groove.py:281）。継承すると集約進捗行が即座に流され、PIPE にして完了まで読まないとバッファ満杯でデッドロックする。drain スレッドで読み続け、オーケストレータの stdout へ流すのは「集約行」と card.py の申告行（`ReferenceAnalyzer` が stdout から拾う）だけ。残りはステップごとの有界 tail としてメモリ保持し、失敗時の報告に使う
- **失敗の詳細は stderr へ出す**: `ReferenceAnalyzer` は失敗理由を stderr からのみ抽出する（ReferenceAnalyzer.cpp:53,79）。進捗＝stdout・エラー＝stderr で分離する
- **内部 fail-fast の子孫停止**: ~~ppid 再帰列挙 leaf-first 方式~~ → **実装レビューで欠陥が判明し killpg 方式へ変更**。ppid 方式は「失敗したステップ自身の子孫は親の死後 reparent 済みで列挙できない」「TERM 無視の孫が reparent すると次の列挙から消える」の2パターンで取りこぼす（レビューが最小再現で実証）。各ステップを自グループのリーダーにし、停止は `killpg`（TERM → 猶予 → KILL の一斉エスカレーション）で行う。グループ所属は reparent で変わらないため取りこぼしがない。全停止後に失敗要約を stderr に出して非ゼロ終了する
- groove.py には bass の pyin が **2回**ある: JSON 用（hop=128, `bass_notes()`）と描画用（hop=512, groove.py:258）。ベース側ワーカーは**両方**を担当させる（パラメータ統合は精度不変の条件を破るので2種類を維持）
- demucs は MPS で非決定的（GOTCHAS.md）。出力一致の検証は **stems キャッシュを固定して** analysis/ の成果物を比較する
- analyze.sh 冒頭の `rm -f card.json`（旧カード無効化）と、stems 出力存在チェックによる demucs スキップは analyze.py に引き継ぐ
- analyze.sh の各ステップに書かれた「なぜ4分割と6分割の両方か」等のコメントは貴重なドキュメント → analyze.py の DAG 定義に移設する
- 出力比較は **analyze.sh が所有する成果物の manifest を固定して**行う: analysis/ には analyze-url.sh だけが生成する `source-overview.json/png`（analyze-url.sh:55）もあり、丸ごと退避→analyze.sh のみ再実行だとこの2ファイルの不在が偽 fail になる

## 実装計画

### Phase 1: analyze.py オーケストレータ（DAG 並列化） [AI🤖]

- [x] `tools/reference/analyze.py` を新設
  - ステップを（名前・コマンド・依存・表示名）の表で定義し、依存充足したものから subprocess で起動
  - 各子の stdout/stderr を PIPE ＋ drain スレッドで常時読み取り。stdout へは集約行と card.py 申告行のみ転送、他はステップ別の有界 tail に保持
  - 「==> 実行中: ステム分離(6s)・グルーヴ・上モノ」形式の集約行を実行集合の変化ごとに出力（ダイアログの最新行表示がそのまま意味を持つ）
  - fail-fast: 失敗検知で実行中ステップを停止（各ステップは自グループのリーダー。killpg で TERM → 猶予 → KILL の一斉エスカレーション。※当初の ppid 列挙方式はレビューで欠陥判明 → ログ参照）し、失敗ステップ名＋出力 tail を **stderr へ**出力して非ゼロ終了
  - ステップごとの重み（stems=ワーカー数・groove=2・topline 等=1・demucs=0）の合計が予算（初期値4）以下のときだけ起動するスケジューリングでグローバルに並列度を制限
  - `rm card.json`・demucs スキップ判定・完了メッセージ（`open '$REF/listen'` 案内）を移植
  - 各ステップの解説コメントを analyze.sh から移設
- [x] `analyze.sh` を「`.venv/bin/python analyze.py "$REF"` を exec するだけ」の薄い入口に書き換え
- [x] `analyze-url.sh` は無変更で通ることを確認（`./analyze.sh "$REF"` 呼び出しのまま）
- [x] **恒久回帰テスト** `tools/reference/tests/test_analyze.py` を新設（`ref:test` から実行される）: 実曲を使わずダミーステップの DAG で、依存順序どおりの起動・キャッシュ skip・大量 stdout/stderr でも詰まらない・失敗ステップの依存先を起動しない・非ゼロ終了、を固定する（実曲の2分テストは将来の回帰確認には重すぎるため）

### Phase 2: ステップ内並列 [AI🤖]

- [x] Phase 1 完了時点でスクラッチ実行を計測し、CPU スロット数と Phase 2 の並列度（各 Executor の max_workers）を決める（→ 予算6・stems=3・groove=2）
- [x] `stems.py`: ステムごとの `analyze()` を `concurrent.futures.ProcessPoolExecutor` で並列化（JSON の出力順は現状維持・max_workers は上で決めた予算内）
- [x] `groove.py`: ドラム解析とベース解析を別プロセスで並走させ、main が両結果を合流して groove.json / groove.png を書く（出力は現状と同一）。ベース側ワーカーは `bass_notes()`（hop=128）と描画用 pyin（hop=512, groove.py:258）の**両方**を担当する（片方だけだと重い pyin が直列に残り短縮効果が出ない）→ 実装中に「2本の pyin を同一ワーカーに入れると直列のまま」と判明し、**pyin 1本＝1ワーカーの2ワーカー構成**に変更（ログ参照）

### Phase 3: LaLa 側の文言更新 [AI🤖]

- [x] `ReferenceAnalysisOverlay.h`: 「目安 約4分」→「約2分」、冒頭コメントの「demucs 分離が支配的・約4分」を実態（CPU 並列化済み・約2分）に更新
- [x] `ReferenceAnalyzer.h` / `MainComponent.h` の「約4分」コメントを更新
- [x] CLI 側の所要時間表記も更新: `tools/reference/README.md:22`・`.mise.toml:10`（ref:url）・`.mise.toml:21`（ref:analyze）・`docs/design/reference-beat.md:218-219`（「分析の実行時間」節の実測値と「どちらかに絞れば2分台」の記述を並列化後の実態に書き換え）

### 動作確認 [AI🤖]

- [x] **出力一致**: 既存リファレンス（rip-slyme-楽園ベイベー）の stems/ を残したまま analysis/ を退避 → 新 analyze.sh を実行 → **analyze.sh 所有の成果物 manifest を固定して**旧版と比較（demucs 再実行なしなので一致するはず。analyze-url.sh 所有の `source-overview.json/png` は比較対象外）
  - analysis/ の JSON: manifest の全ファイル diff → **全30ファイルがバイト一致**（PNG・WAV・MIDI 含む。バイト一致したためピクセル比較は不要だった）
  - `card.json`: `meta.generated_at` 除きの意味比較で一致
  - `listen/`: ファイル集合・中身とも一致
  - Phase 1（オーケストレータ）・Phase 2（ステップ内並列＋スレッドキャップ）の各時点で同じ比較を実施し、どちらも全一致
- [x] **恒久回帰テスト**: `mise run ref:test` で test_analyze.py（依存順序・キャッシュ skip・大量出力・fail-fast・非ゼロ終了）を含む全テストが通ることを確認（13 checks）
- [x] **時間計測**: スクラッチからフル実行 **2分07秒**（旧直列 4分45秒 → 2.2倍）。ただし実行間ブレが大きい（ログ参照）
- [x] **fail-fast**: stems-6s の入力 wav を破損させ ProcessPool ワーカー内で失敗させる方式で確認。パイプライン 8 プロセス稼働中に発火 → 非ゼロ終了・stderr に `ERROR: 「ステム表(6s)」が失敗…`・全プロセス消滅（ステップ別グループ化後は cmdline ベースの pgrep -f で残留ゼロを判定）。加えて回帰テストで「失敗ステップ自身の子」「reparent＋SIGTERM 無視の孫」の停止も固定
- [x] **キャンセル**: LaLa と同一手順（orchestrator のグループへ killpg TERM → 1秒 → killpg KILL）を再現し、8 プロセス稼働中から全プロセス消滅を確認（orchestrator の SIGTERM 伝播が 1秒の猶予内に全ステップグループを掃除）
- [x] 既存テスト（daw_tests・tools/reference/tests/・tools/gacha/tests/）を実行 → 全て pass
- [x] LaLa をビルド（Debug）成功。ダイアログは stdout 最新行を表示するだけの既存実装のため、集約行がそのまま出る（見え方の目視は人間確認へ）

### 動作確認 [人間👨‍💻]

- [ ] LaLa から実際に「リファレンスとして分析」を実行し、進捗表示の見え方に違和感がないか確認

## ログ

### 試したこと・わかったこと

- **groove の実体はほぼ pyin 2本**: ドラム側（onset_env×4＋slot_profile＋density）は全部で約2秒。bass の pyin が hop=128 で52秒・hop=512 で13秒。分割の設計を「ドラム vs ベース」から「pyin 1本＝1ワーカー」に直して初めて効いた
- **BLAS スレッドが最大の敵だった**: pyin 同士を並走させると 52→76秒に劣化（sys time 爆発）。`OMP/OPENBLAS/VECLIB/MKL_NUM_THREADS=1` で干渉が消え、単独実行すら速くなる。**数値結果はスレッド数によらずバイト一致**を確認。demucs（torch）もスレッドプールのスピンで並走ステップを轢いており `OMP_NUM_THREADS=2` で解消（basics 73→39秒）。詳細は GOTCHAS.md「librosa 系プロセスの並走は BLAS スレッドを 1 に絞らないと轢き合う」
- **実行時間の実測はブレる**: 同一構成でスクラッチ 2:07〜3:08。犯人は `mediaanalysisd`（macOS のメディア解析デーモン）等の外部負荷。計測時は `ps -Ao pcpu,comm -r | head` を先に見る
- ステップ別実測（スクラッチ・並走時）: demucs 25+30 / basics 39 / stems 29-31 / groove 87 / topline 17-23。クリティカルパスは basics → groove（pyin 52秒が下限）
- **macOS の `killpg(pgid, 0)` は生死判定に使えない**: グループ内に終了処理中のメンバーが1つでもいると EPERM を返す（man kill: 「グループのいずれかのメンバーへ送れないと EPERM」）。存在確認は `pgrep -g`、送信側は EPERM も握りつぶす（配送自体は行われる）
- **失敗ステップの drain スレッドを kill 前に join してはいけない**: 生き残りの子孫がパイプの write 端を握っていると EOF が来ず、join(5秒)×2 で fail-fast が10秒遅れる。join は stop_all の kill 後に行う

### 方針変更

- **キャンセル後の後続起動と ValueError 時のハンドラ復元**（実装レビュー3巡目）: ①`in_launch` がループ全体を覆っていたため、キャンセル検知後も同じ ready 集合の後続ステップを起動していた → フラグ区間を各 `launch(s)` の前後だけに絞り、起動ごとにチェック。テストを独立3ステップに拡張し「起動数=1」を固定（なお検証中、テストのフック（subprocess.Popen 差し替え）が stop_all の pgrep（subprocess.run 内部の Popen）まで誤カウントする罠を踏んだ — ステップ起動は `process_group=0` の有無で見分ける）②依存検証の ValueError がハンドラ設定後・try/finally 外で起きるとハンドラが復元されない → 検証を設定前へ移動し、復元の回帰テストを追加
- **シグナル変換の競合と SIGHUP**（実装レビュー2巡目）: ①Popen（子グループ作成）→ running 登録の窓で SIGTERM が即 raise すると登録前の子が孤児化する → ハンドラは launch 区間中フラグだけ立て、登録完了直後のチェックで raise する方式に（sigmask で塞ぐ案は子がマスクを継承して TERM が効かなくなるため不採用）。回帰テストに「launch 中の SIGTERM」（Popen フックで窓のど真ん中に撃つ）を追加 ②ターミナルを閉じたときの SIGHUP は別グループのステップに届かない → SIGTERM と同じ停止経路に接続し、orchestrator 単体への SIGHUP で全8プロセスが消滅することを実プロセスで確認
- **子孫プロセスの停止方式を ppid 列挙 → ステップ別プロセスグループ＋killpg に変更**（実装レビューの P1 指摘）: ppid 方式は「失敗ステップ自身の子孫（親の死後に reparent 済み）」「TERM 無視の孫の reparent」で取りこぼすことがレビューの最小再現で実証された。各ステップを `process_group=0` で自グループのリーダーにし、停止は killpg の TERM → 猶予 → KILL。LaLa のキャンセル（orchestrator への TERM → 1秒後 KILL）は orchestrator が SIGTERM を例外に変換して 1秒以内に全グループへ伝播する。計画時の「別グループ禁止」はこの伝播とセットで解消。回帰テストに「失敗ステップ自身の子」「reparent＋TERM 無視の孫」の2ケースを追加し、実プロセス検証も LaLa と同一のシグナル手順で再実施済み
- **groove.py のワーカー分割単位**: 計画の「ドラム解析／ベース解析の2分割」では効果ゼロ（ベース側に pyin 2本が直列で残る）と実測で判明 → 「pyin hop=128」「pyin hop=512」を別ワーカーにする2ワーカー構成へ変更（ドラム側は約2秒しかないので main が持つ）
- **スレッドキャップの追加**（計画外の要素）: オーケストレータが全 CPU ステップに BLAS 系スレッド上限=1、demucs に OMP=2 を環境変数で注入する。出力のバイト一致を確認済みなので「精度を変えない」の範囲内
- **taskpolicy -c utility（QoS で E コアへ寄せる案）は実測で効果なしのため不採用**（外部負荷によるブレの方が大きく、むしろ悪化して見えた）
- ステップ完了時に「==> 完了: ラベル（N秒）」を stdout に出す計測行を追加（計画外・チューニングと将来の退行検知に有用）
