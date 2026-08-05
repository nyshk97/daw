# レポート生成の高速化・品質改善・モデル固定

## 概要・やりたいこと

リファレンス分析レポートの生成（`tools/reference/report.sh` = Claude Code ヘッドレス実行）が10〜15分かかっている。時間の大半は「書く量」ではなく **コールドスタートの読み込みターン**（テンプレ15KB＋見本18KB＋analysis/ のJSON約10個を毎回ゼロから読む＝15〜25ターン×往復レイテンシ＋thinking）にある。/dig（2026-08-05）で /plot 済みの plan と同規模の文書（plan は 13〜32KB、report.md 実物は 17.7KB）が1〜2分で書けている事実から、読み工程の圧縮が本丸と特定した。

質と速度の両方を上げる。トレードオフにならない施策（計測・プロンプト改善・【機】の機械充填・入力ダイジェスト）を優先し、トレードオフになる部分（effort）は A/B 実験で決める。

- 0: 計測 — 生 stream-json の保存（ターン・時刻・トークン・コストが全部残る）
- A: プロンプト改善 — 読みターンの圧縮・ヘッダ幻覚対策
- B: `report.py` で【機】を機械充填（design doc の Phase 1 構想を「やる」に倒す。転記ミスゼロ・判定語の線引きをコードで保証）
- C: 入力ダイジェスト — AI に「使うな」と指示している生データを渡さない
- D: モデルを **Opus 5 に固定**＋effort を low vs high の A/B で決定

効果の推定: A〜C で 15分 → 5分前後、D が当たればさらに短縮（0 の計測で答え合わせする）。

## 前提・わかっていること

/dig（2026-08-05）での決定事項と調査結果。

### 実施順序

- **report-in-app（`docs/plans/2026-08-05-1804-report-in-app.md`）の実装が先。本件はその後** → 先行2plan（並列分析・report-in-app）は **2026-08-06 実装完了を確認済み**。report.sh 現物にトランザクション・lockf・妥当性検査（サイズ下限＋見出し。完成検査の挿入位置としてコメントで本 plan が予告済み）・`CLAUDE_BIN` env 上書き（fake claude テスト用）が入っており、`tests/test_report_sh.sh` も存在する。本 plan のシェルテストはこの方式に乗る
- `./report.sh <フォルダ>` のコマンドインターフェースは変えない（in-app 側の起動・ポーリング・成否判定＝exit 0 に影響なし）
- 現行 report.sh はプロンプトを sed で `report.md`→`report.md.next` に置換して出力先を差し替えている。Phase 2 のプロンプト全面改訂でこの sed ハックは**廃止**する（新プロンプトは最初から `report.md.next` を対象に書く）
- in-app 側のボタン文言「10〜15分」は本件の実測後に更新する（本 plan の最終 Phase）

### 現状の構造（調査済み）

- `report.sh` は `claude -p` に report-prompt.md を渡し、AI が gates.json → report-template.md → report-example.md → analysis/*.json を読んで report.md を Write する
- テンプレートは【機】（JSONキーのパス指定つき転記）と【判】（因果の説明・定石と癖の切り分け・総合節）に色分け済み。判定語の閾値表はテンプレ末尾の HTML コメントにある
- `stream_progress.py` が経過時間つきの1行ログを stdout に流している（保存はしていない）
- labs の記録: 初版は「判断材料を渡すと判断をやり直す」で28ターン迷走 → gates.json に判定を移して解決した経緯がある。今回のゲート焼き込みはこの延長
- `claude` CLI v2.1.222 で `--model claude-opus-5` と `--effort low|medium|high|xhigh|max` が使えることを確認済み
- design doc（`docs/design/reference-beat.md`）に「Phase 1 で report.py を作り【機】を埋める。【判】は Claude Code が埋める」の構想が既にあり、「3曲完走したから要否未決」で保留中だった → 本件で「やる」に確定
- A/B 用の完走済み分析: `~/Music/daw/references/rip-slyme-楽園ベイベー/`（2026-08-05 に並列版 analyze.py で分析）

### 新しい report.sh の流れ（確定設計）

**先行 plan（report-in-app Phase 4）の生成トランザクション契約をそのまま維持し、その上に差分で載せる**: `report.md.next` への出力 → 妥当性検査 → atomic rename ＋ report.html 削除、lockf（fd 形式）によるフォルダ単位の排他、EXIT/シグナル trap での `.next` 回収、失敗・中断時の旧 report.md / report.html 維持、ロック取得後の stale `.next` 掃除 — これらは先行 plan の実装・シェルテストを変更しない。

1. **ゲート短絡**: gates.json の BPM ゲート（`bpm.octave_ok` / `tempo_stable.ok`）落ちなら claude を起動せず**理由を stderr に出して非ゼロ終了**（トークンゼロ・`.next` を作らない・旧 md/html 維持。**アプリのトーストは stderr 末尾行を表示する実装**（ReferenceReportGenerator.cpp）なので理由行は stderr 必須。exit code は既存の 65=妥当性・75=ロックと使い分ける）。先行 plan の「トランザクション完了＝成功」契約と整合させる（exit 0 だと UI が成功トーストを出しうる）。なお UI 経由ではガチャパネルが card.json のあるフォルダしか列挙せず、BPM ゲート落ち曲は card 自体が無いので構造的に到達しない — この短絡は CLI 手実行向けの防御
2. **report.py（新設）がドラフト生成**: ロック取得後に **`report.md.next` へ直接出力**（別名の draft ファイルは作らない。先行 plan の trap / stale 掃除がそのまま効く）
3. **claude 起動**: `--model claude-opus-5 --effort <確定値>`（env `REPORT_EFFORT` で上書き可）。AI の仕事は「`report.md.next` を Read → 【判】をセクション別 Edit → 3点報告」だけ
4. **検査 → rename**: 先行 plan の妥当性検査（存在・見出し・非自明サイズ）に**完成検査を追加**し、通過したときだけ atomic rename ＋ report.html 削除。検査項目: ①`{{...}}` と可視プレースホルダ（`<結論を見出しに>`・表インライン型の未置換プレースホルダ等）の残存ゼロ ②必須の各論見出しが存在 ③**本文型【判】**: 各領域の開始・終了マーカー（HTML コメント。閲覧時は不可視なので最終 report.md に残ってよい）の**内側**に実質的な文章段落があるか。見出し・表・箇条書き・HTML コメント・空行に加え、**単独の強調ラベル行**（`**定石通りのところ**` 等、テンプレに機械充填前から存在する定型行）**は文章段落に数えない** ④**表・インライン型【判】**: セル内マーカー対の**間**が空・空白のみでないこと（機の数値がセルに残っていても判の削除を検出できる）。Claude が見出しだけ直して【判】を書かないケースを機械検出（質の担保を兼ねる）
5. **計測**: `claude -p` の生 stream-json を `tee` で `<ref>/logs/report-<タイムスタンプ>.jsonl` に保存（stream_progress.py への表示はそのまま）

### report.py の設計（確定）

- **テンプレが真実の源**: report-template.md の【機】箇所に `{{basics.tempo.bpm}}` 形式の**置換マーカーを直接書き**、report.py は置換するだけ。雛形の編集で report.py の修正が原則不要
- **判定語の閾値は report.py のコードに移す**（「ハネ無し < 0.52」等）。テンプレ末尾の閾値表は削除し「線引きは report.py。変更時は labs に記録」のポインタに置換。card.py/gates.py と同じ型（テストで固定できるロジック）
- **ゲートの焼き込み**: BPM 以外のゲート落ち（swing.ok / downbeat.ok / harmony.ok / key.confidence）は該当節に「測れなかったと書け」の指示コメントとしてドラフトに埋め込む。**AI は gates.json を読まない**（「判断材料を渡すと判断をやり直す」の根を完全に断つ）
- **ヘッダの幻覚対策**: `source.info.json`（yt-dlp メタデータ）は**任意入力**。あれば動画公開日・チャンネル・URL を report.py が充填（`upload_date` は楽曲の発売日ではないので表記は「**動画公開日**」に限定）。**無ければ**（LaLa 内からの書き出しは `track.wav`＋クリップ同定用 `source.json` しか作らない — `Source/shared/ReferenceExport.cpp`）フォルダ名から曲名だけ埋め、日付・チャンネル・URL の行は省略する。Prod./レーベルは不明なら書かない（プロンプトにも「作文しない」を明記）
- **【判】は2種類に分ける**（表セル内の判と本文の判は検査方法が違う）:
  - **本文型**: 開始・終了マーカー（HTML コメント）で囲まれた領域。検査は内側の文章段落の有無
  - **表・インライン型**（楽器編成表の「エレピ系」等、機と判が同じセルに混在する箇所）: **セル内に不可視のインライン開始・終了マーカー対**（HTML コメント）を置き、間に可視プレースホルダを挟む（例: `| 上モノA | <!-- 判i:instrument-a:start -->〈呼び名〉<!-- 判i:instrument-a:end --> / 重心 {{...}}Hz |`）。検査は「マーカー間が空・空白のみでない」こと — セル全体の非空判定だと、判だけ消しても機の数値が残って通ってしまう。インラインの HTML コメントは同一行内なので Markdown の表を分断しない
- **キー・テンポ等の最終判定は gates.py に集約し、report.py は再実装しない**: card.py は既に `gates.key.value` を採用しており、report.py が独自にキーを再確定すると同じ分析からカードとレポートで食い違う恐れがある。テンプレの「キー確定手順」のうち機械化できる部分は gates.py 側に寄せ（必要なら gates.py を拡張）、report.py は gates の結論をレンダリングするだけ。長短どちらに寄るか等の解釈は【判】に残す
- **【判】用ダイジェストも同時生成**: 判の執筆に必要な値（groove の profile/dev/swing・bass 統計・band_balance・centroid_median・stereo_width・timbre・loop_progression の chord/conf/stability・repetitions_folded・arrangement.sections・小節ごとのミックスRMS）だけを1つの JSON に。**使用禁止の生データ（`chords[]` の区間ごと推定・similarity 全曲線等）は含めない**

### 新しい report-prompt.md（確定方針）

- 見本（report-example.md）とダイジェストは **report.sh がプロンプトに直接同梱**する（AI の読みターンをほぼゼロに）
- AI のファイル操作は「`report.md.next` を Read → 【判】をセクション別 Edit」だけ。テンプレも gates.json も読まない
- 逃げ道: ダイジェストで足りない時だけ analysis/ を読んでよい。ただし再導出・分析スクリプト読みは引き続き禁止
- 最後の3点報告（採用した BPM・キー・ループ長／ゲートの扱い／耳で確かめてほしい箇所）は維持

### D: effort A/B 実験の設計（確定）

- モデルは **Opus 5 固定**（`--model claude-opus-5`）
- 楽園ベイベーの分析済みフォルダで、**同じドラフトを入力に effort low と high の2本**を走らせる
- **成果物とログの退避**（2本目が1本目を上書きしないように）: 各ラン完了後に `report.md` を `report-low.md` / `report-high.md` へ **cp** し、正規の `report.md` は維持する（mv だと high 実行前に正規レポートが消え、high 失敗時にトランザクションが守るべき旧レポート自体が無くなる）。ログ名にも effort を含める（`logs/report-<effort>-<タイムスタンプ>.jsonl`）
- **ドラフト同一性の担保**: 各ランの claude 起動直前に `report.md.next` のコピーを `logs/draft-<effort>.md` に退避し、両者を diff で突き合わせる（report.py は決定的なので一致するはず。不一致なら実験を中止して原因を見る）
- 速度・ターン数・トークン・コストは logs/ の JSONL から機械的に比較。質はユーザーが【判】の節（特に総合節）を読み比べ
- **1回ずつの比較は「予備比較」と位置づける**（生成揺らぎが混ざるため）。差が微妙なら追加ランで確かめ、明確なら確定してよい
- 差がなければ low（大幅な高速化）、差があれば high を report.sh のデフォルトに固定。結果は `docs/labs/reference-beat.md` に記録

## 実装計画

### Phase 0: 計測とモデル固定（先行で仕込む・効果測定の土台） [AI🤖]

- [ ] `report.sh` に `--model claude-opus-5` と `--effort ${REPORT_EFFORT:-high}` を追加（A/B 決着までの暫定デフォルトは high）
- [ ] `claude -p` の生 stream-json を `tee` で `<ref>/logs/report-<タイムスタンプ>.jsonl` に保存（logs/ が無ければ作成。stream_progress.py の表示は現状維持）
- [ ] 既存フォルダで1本走らせ、現行方式のベースライン（所要時間・ターン数・トークン）を JSONL から集計して本 plan のログに記録する

### Phase 1: report.py — 【機】充填とダイジェスト生成 [AI🤖]

- [ ] `report-template.md` の【機】箇所に `{{...}}` 置換マーカーを埋め込む。**本文型【判】は開始・終了マーカー（HTML コメント）で囲み、表・インライン型【判】はセル内にインラインマーカー対＋間に可視プレースホルダを置く**（report.py はどちらもそのまま通すだけ — マーカー・プレースホルダの真実の源もテンプレ）。末尾の閾値表を削除し report.py へのポインタに置換
- [ ] 必要なら `gates.py` を拡張（テンプレの「キー確定手順」のうち機械化できる部分を gates 側に寄せる。**report.py では再実装しない** — card.py と真実の源を共有する）
- [ ] `tools/reference/report.py` 新設:
  - analysis/*.json を読み、マーカーを充填したドラフトを**引数で渡された出力パス**へ書く（report.sh からは `report.md.next` を渡す）
  - `source.info.json` は**任意**: あれば「動画公開日」・チャンネル・URL を充填、無ければフォルダ名から曲名だけ埋めて該当行を省略
  - **判定の所有を明文化**: gates.py が「測定可否・確度」（テンポ安定・小節頭・スウィング測定可否・キー確度・ハーモニー可否）を持ち、report.py は**ゲート通過後の表現分類だけ**（ハネの程度・クオンタイズ・ループ長/コード確信度・ステム分離・basic-pitch の線引き）をコードで実装。gates が持つ判定は report.py で再実装しない
  - ゲート落ちの節別焼き込み（「測れなかったと書け」指示コメント）
  - **【判】のマーカー（本文型・インライン型とも）はテンプレから素通しで出力**（完成検査の対象。プロンプトでも「本文はマーカーの内側に書く・表セルはマーカー間のプレースホルダを判断語で置き換える・マーカーはどちらも消さない」を指示する）
  - BPM ゲート落ちなら非ゼロ終了（ドラフトを出さない。report.sh の短絡が主で、こちらは防御）
  - 【判】用ダイジェスト JSON の生成
- [ ] `tests/test_report.py` 追加（test_card.py と同じ fixture 方式）: 機充填・判定語の境界値・ゲート焼き込み・BPM ゲートでの非生成・**source.info.json 無しでも生成できること**・ダイジェストに禁止データが含まれないこと・**カードとレポートでキーが一致すること**
- [ ] 楽園ベイベーの実データでドラフトを生成し、既存 report.md（他曲）の【機】値と目視突き合わせ

### Phase 2: report.sh と report-prompt.md の書き換え [AI🤖]

- [ ] `report.sh` を新フロー（ゲート短絡 → report.py が `report.md.next` へ → claude → 検査 → rename）に書き換え。インターフェースは `./report.sh <フォルダ>` のまま。**先行 plan のロック（lockf fd 形式）・trap・stale `.next` 掃除・旧 md/html 維持・成功時の report.html 削除はそのまま維持**し、既存シェルテストが通ることを確認
- [ ] `--allowedTools` を **`Read Edit` のみに変更**（現行の Write/Glob/Grep/Bash を外す。ドラフトの Edit と analysis/ の不足時 Read だけで足り、逸脱の余地も減る）
- [ ] `report-prompt.md` を全面改訂: ドラフト（`report.md.next`）を Read → 【判】をセクション別 Edit、見本とダイジェストはプロンプト同梱、gates.json・テンプレは読まない、ヘッダの作文禁止、analysis/ は不足時のみ、3点報告は維持
- [ ] 完成検査の実装: 先行 plan の妥当性検査に加え、①`{{...}}`・可視プレースホルダ（表インライン型のマーカー間の未置換分を含む）の残存ゼロ ②必須の各論見出しの存在 ③**本文型【判】**: 各領域マーカーの内側に実質的な文章段落がある（見出し・表・箇条書き・HTML コメント・空行・`**定石通りのところ**` 等の単独強調ラベル行は数えない） ④**表・インライン型【判】**: セル内マーカー対の間が空・空白のみでない、を検査し、不合格なら rename せずエラー報告（旧 md/html は維持される）
- [ ] シェルテスト追加（fake claude 方式）: **現在の実テンプレートと fixture に対する実際の report.py 出力**を入力に「見出しだけ変更して【判】の文章を書かない」fake claude を通し、検査が失敗すること（空の簡易ドラフトで代用しない — 【機】の表・単独強調ラベル行が存在する状態でも検出できることの検証が目的）。**表インライン型のマーカー間を空にした（プレースホルダを消しただけの）ケースでも失敗すること**（機の数値がセルに残っていても検出できることの検証）／BPM ゲート落ちフォルダ（fixture でよい）で **claude 未起動・非ゼロ終了・旧 md/html 維持**となること

### Phase 3: 通し実行と effort A/B [AI🤖 + 人間👨‍💻]

- [ ] [AI🤖] 楽園ベイベーで `REPORT_EFFORT=low` → `REPORT_EFFORT=high` の順に通し実行。各ランで claude 起動直前の `report.md.next` を `logs/draft-<effort>.md` に退避し、完了後の `report.md` を `report-low.md` / `report-high.md` へ **cp**（正規 `report.md` は維持）。ログ名は `logs/report-<effort>-<タイムスタンプ>.jsonl`
- [ ] [AI🤖] 2つの退避ドラフトを diff し完全一致を確認（不一致なら実験中止・原因調査）
- [ ] [AI🤖] logs/ の JSONL から所要時間・ターン数・トークン・コストを集計し、Phase 0 のベースラインと比較して本 plan のログに記録
- [ ] [人間👨‍💻] report-low.md / report-high.md の【判】（特に総合節）を読み比べ、low で足りるか判定（1回ずつは予備比較。差が微妙なら追加ランを依頼する）
- [ ] [AI🤖] 勝者を `report.sh` のデフォルト effort に固定し、勝者の退避ファイルを **cp** で `report.md` に確定してから `report-low.md` / `report-high.md` を削除。実験結果を `docs/labs/reference-beat.md` に記録

### Phase 4: 後始末 [AI🤖]

- [ ] `docs/design/reference-beat.md` の Phase 1 項（report.py 要否未決）を完了に更新
- [ ] `tools/reference/README.md` のスクリプト表・所要時間の記述を更新
- [ ] in-app 側の所要時間文言「10〜15分」を実測値に更新（`grep -rn "10〜15"` で洗える。GachaPanelView.cpp のボタン・右クリックメニュー・ツールチップ／MainComponent.cpp の書き直し確認ダイアログ／Main.cpp・MainComponent.h のコメント／report.sh の echo とヘッダコメント）
- [ ] `VERIFY.md` に再利用可能な確認手順があれば追記

### 動作確認 [人間👨‍💻]

- [ ] 改善後の report.md を1本通読し、旧方式（既存3曲）と比べて質が落ちていないか確認
- [ ] listen/ のクリップで数値の耳検算（従来どおり）

## ログ

### 試したこと・わかったこと
（実装中に随時追記）

### 方針変更
（実装中に随時追記）
