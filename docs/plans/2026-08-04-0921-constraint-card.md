# 制約カード（card.json）のスキーマ確定と生成器 card.py

## 概要・やりたいこと

リファレンス分析（[reference-beat](../design/reference-beat.md)）の Phase 1 のうち**制約カード側**。
分析パイプラインが吐く `analysis/*.json` 群から「**ガチャの生成器が読む値だけ**」を厳選した
1枚の `card.json` を作る。Phase 2（MIDI生成ガチャCLI）の入力になる。

- 分析→生成のつなぎ込みの土台。カードは「その曲の雰囲気のレシピ」＝パターンそのものではなく、
  パターンが従う数値的な性格だけを運ぶ
- スキーマは **v1 最小で入れて Phase 2 の生成器の要求で育てる**（会話で確定済み）

## 前提・わかっていること

2026-08-04 の壁打ちで確定した設計判断:

- **制約は2分類**: (a) BPM・キーはプロジェクトへ**1回きりのコピー**で書き込む（リンクしない）。
  (b) それ以外（グルーヴ統計・密度・編成・構成）はプロジェクトの状態に昇格させず、
  **ガチャ実行時にカードを指すだけ**。二重管理を作らない
- **複数リファレンスはパーツ別参照で対応**: ドラムは曲A・コードは曲B、のようにパーツごとに
  別カードを指せる形にする。**複数カードのブレンド（平均）は禁止**（実在しない性格が出る。
  dB算術平均バグと同型の失敗）。カード側の対応は「パーツ別スライスに区切る」ことだけ
- **コード進行そのものは載せない**（ユーザー確認済み）。借りるのは性格
  （ループ長・動きの少なさ・動きの方向）のみ。進行の実物は `analysis/` とレポートに残る
- **キーの競合は「生成は常にプロジェクトのキー」で解決する**: カードの `global.key` を生成時に
  読むことはない（読むとプロジェクトキーと chords カードのキーが食い違ったとき生成先が不定になる）。
  カードのキーが使われるのは**プロジェクトへの1回コピーのときだけ**。chords スライスが運ぶのは
  動きの性格のみで、「明暗」を借りたければその曲のキーをプロジェクトへコピーする、が唯一の経路
- **loop_bars は chords スライスにだけ置く**（global と重複させない）。出どころが harmony 分析
  なので、harmony 落ちでスライスごと消える整合も自然になる。パーツ別参照でループ長がカード間で
  食い違う場合にどれを採用するかは、ガチャ側（Phase 2/3）の決め（既定は chords カードの値）
- **ファイル名は `card.json`**、置き場は `references/<名前>/card.json`（`report.md` の兄弟。
  design doc の仮名 `analysis.json` は `analysis/` フォルダと紛らわしいので改名）
- 載せる項目の採用基準は labs の「制約カードに載せてよいもの/いけないもの」リスト
  （[labs/reference-beat.md](../labs/reference-beat.md) 2026-08-03 エントリ）
- **ゲート落ちの項目はスライス（またはフィールド）ごと省略**し、省略した事実と理由だけ
  `meta.excluded` に残す。生成器は「無いなら自分のデフォルト」と解釈する
- **拍子は測定値ではなく4/4前提**（現行分析は3拍子・6/8を検出できずサイレントに壊れる —
  README「既知の壊れ方」）。カードでは `global` でなく `meta.assumptions` に置き、
  「測った値」と「決め打ち」を構造で区別する

### レビューで見つかった前提のずれ（2026-08-04 反映済み）

- **gates.py の harmony 判定が設計と食い違う（実バグ）**: `gates.py` は4分割 `other` を含む
  全 `topline-*.json` から「usable かつ最大音量」を source に選ぶが、設計は「上モノは6分割」
  （4分割 other は合議できず、rau で実際にループ長を誤答した実績あり。`chord_estimate_usable`
  の -25dB 閾値も6分割の実測で引いた線）。実測: kzm は 6s 3ステム全部 usable=false なのに
  4分割 other（-20.18dB）で harmony.ok=true になっている。→ **本planで gates.py を修正**し、
  harmony の ok / source_stem は 6s ステムのみから決める（usable=None は false 扱い。
  4分割の値は参考として stems リストに残す）。修正後に3曲の gates.json を再生成すると、
  kzm が harmony 落ちになり excluded 経路の実テストケースに戻る
- 検証素材は分析済み3曲だが、**リポジトリ外かつ再分析で値が変動する**（demucs の非決定性）ので、
  回帰テストはリポジトリ内の最小 fixture で行う
- **対象外**: design doc の Phase 1 に書いてある `report.py` はこの plan に含めない
  （`report.sh` が3曲とも人手介入なしで完走しており、`report.py` が今も必要かは別途判断）

### スキーマ v1（確定・rau-def-freeze の実値。2026-08-04 に現物から算出済み）

```json
{
  "card_version": 1,
  "meta": {
    "reference": "rau-def-freeze",
    "generated_at": "2026-08-04T09:00:00+09:00",
    "assumptions": { "time_signature": "4/4" },
    "excluded": []
  },
  "global": {
    "bpm": 97.6,
    "key": { "root": "C#", "mode": "major" }
  },
  "drums": {
    "swing_ratio": 0.499,
    "quantize_dev_ms": 5.0,
    "kick_profile_by_beat": [1.0, 0.605, 0.522, 0.248],
    "snare_profile_by_16th": [0.753, 0.035, 0.104, "…(実配列16件)"],
    "hat_profile_by_16th": [0.68, 0.018, 0.405, "…(実配列16件)"]
  },
  "bass": {
    "notes_per_bar": 2.13,
    "core_range_semitones": 18,
    "pitch_center": "C2",
    "note_length_16ths": 2.27,
    "onset_rate_by_16th": [0.258, 0.258, 0.086, "…(実配列16件)"],
    "quantize_dev_ms": 38.0
  },
  "chords": {
    "loop_bars": 4,
    "has_7th_or_more": true,
    "changes_per_loop": 7,
    "root_motion": "mixed"
  },
  "arrangement": {
    "n_bars": 92,
    "instrumentation": ["bass", "drums", "guitar", "other", "piano", "vocals"],
    "sections": [
      { "bars": [1, 8], "active": ["piano", "vocals"], "rms_db": -14.2 }
    ]
  }
}
```

プロファイル配列の `"…(実配列16件)"` は plan の紙面上の省略で、実装は `groove.json` の
該当配列（16件の数値）をそのまま転記する。カード内に文字列プレースホルダは存在しない。

### 値の出どころ（転記元）と導出規則

| カードの値 | 転記元 | 導出 |
|---|---|---|
| global.bpm / key | `gates.json` bpm.value / key.value | 転記のみ |
| chords.loop_bars | harmony.source_stem が指す `topline-*.json` の `loop_progression.loop_bars` | 転記のみ（global には置かない） |
| drums.swing_ratio | `groove.json` swing.ratio | 転記のみ |
| drums.quantize_dev_ms | `groove.json` drums.mid / drums.high の `dev_ms_by_16th` | **非Noneスロットの絶対値平均**（rau 実測 5.0ms, n=18） |
| drums.kick_profile_by_beat | `groove.json` drums.low.profile_by_16th | **4スロットずつ max で拍単位に畳み、最大=1.0 に正規化**（rau 実測 [1.0, 0.605, 0.522, 0.248]。16分は低域の時間分解能不足で信用しない） |
| drums.snare/hat_profile_by_16th | `groove.json` drums.mid / drums.high の profile_by_16th | 転記のみ |
| bass.* | `groove.json` bass | core_range = 5-95%区間・他は転記 |
| chords.* | source_stem の `loop_progression.progression` | **下記「chords の導出規則」** |
| arrangement.n_bars / sections | `arrangement.json` | rms_db はミックス実測（修正済みの値）。bars は [bar_start, bar_end] |
| arrangement.instrumentation | `arrangement.json` sections | **全セクションの active の和集合をソート**（rau 実測 6楽器） |

### chords の導出規則（一意に実装できるレベルまで固定）

入力は `loop_progression.progression` のスロット列（半小節単位。4小節ループなら8スロット）。

1. **正規化**: 各コード名を「ルート音程クラス＋基本クオリティ」に落とす。
   クオリティは min（`m` 始まりで `maj` 以外）/ dim（`dim`・`ø` を含む）/ それ以外は maj
   （sus・5・テンションは maj 族に吸収 — コードの細部は信用しない、の方針どおり骨格だけ見る）
2. **has_7th_or_more**: 正規化**前**のラベルに `7/9/11/13` のいずれかを含むスロットが1つでもあれば true
3. **changes_per_loop**: 正規化後のスロットを**循環**（末尾→先頭を含む）で隣接比較し、
   (ルート, クオリティ) が変わった回数
4. **root_motion**: 変化のたびにルートの移動を半音数 **(-6, +6]**（最短方向・トライトーンは+6）で取り、
   **0（クオリティのみの変化）は方向の集計から除外**。非ゼロの移動が無ければ static。
   changes_per_loop ≤ 1 も static。それ以外は上行/下行の多数が**2/3以上**なら up / down、どちらでもなければ mixed

**source の選択規則（確定）**: usable **かつ進行が抽出できた**6分割ステムのうち最大音量。
進行の抽出可否を条件に入れるのは、曲が短くループ2周ぶん取れないと topline が
`loop_progression: {}` を出し、usable だけで選ぶと下流が KeyError で落ちるため。
複数ステムの進行が食い違うときの**合議は v1 では入れない**（性格は骨格レベルまで落として
読むので揺れが小さい。必要になったら Phase 2 の生成器の要求で決める）。

**3曲の期待値**（全ステムを現行コードで再分析した状態に対する値）:

| 曲 | source | changes_per_loop | root_motion | has_7th |
|---|---|---|---|---|
| rau-def-freeze | 6s-other（-15.97dB で最大） | 7 | mixed（moves [-1,5,5,-3,1,-3,-4] → 上行3/下行4） | true |
| my-way | 6s-guitar | 14 | mixed（上行6/下行8 = 8/14 < 2/3） | true |
| kzm-doshaburi | —（harmony 落ち） | chords スライスごと省略 | — | — |

> 注: 実曲の値は再分析で動く（旧 topline が残っていた plan 作成時点の rau は 6s-piano しか
> 候補が無く {6, up} だった）ため、回帰の固定は fixture テストで行う（この plan の前提どおり）。

### ゲート→省略の対応

| ゲート | 落ちたときの扱い |
|---|---|
| `bpm.octave_ok` false / `tempo_stable.ok` false | **カードを生成しない**（全分析が無意味）。既存の card.json があれば**削除**する（古いカードが正常品の顔で残るのを防ぐ）。想定内の結果なので exit 0 |
| `downbeat.ok` false | 小節内の位置情報を全部省略: drums の3プロファイル・bass.onset_rate_by_16th・chords スライス・arrangement.sections。**残す**: bpm・key・swing_ratio（拍相対で小節位相に依存しない）・bass の密度/音域/音価・instrumentation |
| `harmony.ok` false（6s のみで判定・修正後） | `chords` スライスを省略（loop_bars も chords 内なので一緒に消える） |
| `swing.ok` false | `drums.swing_ratio` と `hat_profile_by_16th` を省略（同じ壊れた高域を見ているため）。kick/snare は残す |
| `key.confidence` 低 | `global.key` を省略 |

### card.py の書き込み・失敗時の扱い

- **メモリ上で組み立て→検証→一時ファイル→`os.replace`**（atomic）。不正なファイルを一瞬も公開しない
- `meta.excluded` の要素は**単一形式に固定**: `{"path": "chords" | "drums.swing_ratio", "gate": "harmony.ok", "reason": "…"}`。
  path はドット記法（スライス省略はトップレベル名のみ）。自己検算と将来の表示側が同じ形を読める
- 検証内容: 数値フィールドが有限値・プロファイル配列の件数（4/16）・excluded の path が本体に存在しないこと・excluded が上記形式であること
- **検証失敗（= card.py のバグ）**: 非ゼロ終了。既存の card.json には触らない（tmp 方式なので部分書き込みも起きない）
- **ゲート落ちで生成しない（= 想定内）**: 上表のとおり既存カードを削除して exit 0
- **analyze.sh は冒頭で card.json を削除する**: 再分析で `analysis/*.json` が順次書き換わった後に
  途中のステップ（excerpts 等）が失敗すると、旧分析由来のカードが新JSONの横に残ってしまう。
  分析を始めた時点で旧カードは無効なので先に消す（`ref:card` でいつでも再生成できるため安い）

## 実装計画

### Phase 1: gates.py 修正と card.py 実装 [AI🤖]

- [x] `gates.py` の harmony 判定を修正: ok / source_stem は **6s ステム（6s-piano / 6s-guitar / 6s-other）のみ**から決める。usable=None（古い分析で欄が無い）は false 扱い。4分割の値は stems リストに参考として残す。**source 選択は関数に切り出し、最小の topline 断片を渡す単体テストを `tests/test_card.py` に1件入れる**（「4s が usable=true でも 6s 全 false なら harmony=false」を固定。生成済み gates.json を fixture にすると選択ロジック自体の回帰が効かないため）
- [x] `excerpts.py` の同型バグを修正: コード確認クリップ（`02-chords-check` / `03-loop-only`）の出どころを、全 `topline-*.json` からの独自選択（`excerpts.py:129` 付近）でなく **`gates.json` の harmony.ok / source_stem を唯一の判断元**にする（harmony.ok=false ならコード系クリップを作らない）。カード・レポートは「harmony なし」なのに listen/ だけ4分割 other 由来の進行を鳴らす、という不整合を塞ぐ。analyze.sh は gates → excerpts の順なので実行順の変更は不要
- [x] 3曲の `gates.json` を再生成（gates.py 単体実行。demucs 再実行は不要）し、kzm が harmony.ok=false になることを確認
- [x] kzm の既存 `report.md` を確認: 旧ゲート（4分割 other 由来）でコード進行の節を書いていたら、その旨をユーザーに報告する（レポートの書き直しは本 plan のスコープ外）
- [x] `tools/reference/card.py` を作成: `analysis/*.json` + `gates.json` を読み、上記スキーマ・導出規則・省略対応・atomic 書き込みを実装して `<フォルダ>/card.json` を書く
- [x] **fixture 回帰テスト**: `tools/reference/tests/fixtures/` に最小の analysis JSON セットを2つ（正常系 / harmony 落ち系）コミットし、`tests/test_card.py`（プレーン assert・pytest 不要）で「期待スライスの有無・chords 3値・kick 畳み・excluded の中身と形式」を検証する。**残りのゲート分岐は fixture を増やさず、正常系 fixture の gates.json をテスト内で書き換えて網羅する**: bpm/tempo 落ち（カード自体を削除・exit 0）・downbeat 落ち（位置情報フィールドの省略）・swing 落ち・key 低
- [x] 既存3曲に対して card.py を実行し、`card.json` を生成する

### Phase 2: パイプライン組み込みとドキュメント [AI🤖]

- [x] `analyze.sh` に card.py を組み込む: **冒頭で既存 card.json を削除**し、最後（excerpts の後）に生成する
- [x] `.mise.toml` にタスク追加: `ref:card <フォルダ>`（カード再生成）・`ref:test`（fixture テスト実行）。description は日本語
- [x] `tools/reference/README.md` 更新: スクリプト表に card.py / tests を追加・card.json の説明（何を載せ、何を載せないか・拍子は4/4前提であること・ゲート落ちの表現・ゲート落ちで旧カードが消えること）
- [x] `docs/design/reference-beat.md` 更新:
  - [x] 「決まったこと」に追記: 制約の2分類（BPM/キーは1回コピー・他はガチャ時参照）／パーツ別リファレンス参照・ブレンド禁止／コード進行は載せず性格のみ／**生成は常にプロジェクトのキーで行い、カードのキーはプロジェクトへのコピー時にだけ使う**（明暗を借りる経路はキーのコピー）／loop_bars は chords スライスのみ／カードの名称は `card.json`（本文中の `analysis.json` 表記を差し替え）／harmony ゲートは6分割のみで判定
  - [x] スキーマ v1 の置き場所を明記（真実の源は card.py のコード＋README。design doc には方針のみ）
  - [x] Phase 1 チェックボックスの制約カード側を進捗反映（report.py の要否は未決のまま残す）

### 動作確認 [AI🤖]

- [x] `ref:test`（fixture）が通ること
- [x] 3曲の card.json の値を転記元 JSON と突き合わせ（bpm・swing・kick 畳み・chords 3値の期待値表と一致すること）
- [x] kzm-doshaburi: `chords` スライスと `loop_bars` が省略され、`meta.excluded` に理由が入っていること
- [x] kzm-doshaburi: gates 再生成後に excerpts.py を再実行し、`listen/` に `02-chords-check` / `03-loop-only` が**生成されない**こと（旧クリップが残っていれば消えること）
- [x] rau-def-freeze: chords が期待値（changes 7 / mixed / has_7th true。source=6s-other）どおりで、実際の進行（F#maj9 系・4小節ループ）と矛盾しないこと
- [x] `analyze.sh` 通しで card.json まで落ちること（既存曲の再分析 `ref:analyze` で確認。1曲だけでよい）

## ログ

### 試したこと・わかったこと

- gates 修正後、kzm は想定どおり harmony NG。ただし kzm の既存 `report.md` はハーモニー節を
  旧ゲート（4分割 other）で書いていた。内容は「使えたのは4分割 other だけ」と明記した上で
  ベース音名分布（F 58%）と独立に裏取りした慎重な書き方で、結論（Fペダル・進行なし）は
  修正後も成立している。書き直しはスコープ外なので据え置き（ユーザーへ報告済み）
- my-way のコード確認クリップ（listen/02）の出どころが other（4分割）→ 6s-guitar に変わった。
  excerpts.py は起動時に listen/ を全消しするので旧クリップの残留はない
- 3曲の card.json の実測値は plan の期待値表と完全一致（rau: chords {4, true, 6, up} /
  kick [1.0, 0.605, 0.522, 0.248] / qdev 5.0）
- analyze.sh 通し（rau）も card.json 生成まで完走。ただし再分析で rau の chords が
  {6, up} → {7, mixed} に変わった。原因は旧 topline JSON（usable 欄の無い古いコードの出力）が
  現行コードで再計算され、usable な 6s が3本に増えて source が 6s-piano → 6s-other
  （-15.97dB で最大）に変わったこと。ゲート仕様どおりの動きで、4分割 other（-9.41dB で最大音量）が
  正しく無視されていることも確認できた。実曲の値のドリフトは想定内（回帰は fixture で固定）

- レビュー3周目で2件の実バグ相当を追加修正: ① usable でも `loop_progression: {}`
  （曲が短くループ2周取れない）のステムを source に選ぶと card.py が KeyError で落ちる
  → pick_harmony_source の候補条件に「進行が非空」を追加 ② 自己検算が float の有限性しか
  見ておらず、bpm が文字列でも素通りしていた → FIELD_TYPES による型検査＋root_motion の
  値域＋プロファイル配列の要素型を追加（検算失敗で旧カード保持のテストも追加、計33件）
- レビュー4周目（中程度3件）: ① 進行判定を `any()` に（[None,None] の全スロット None を弾く）
  ② 検算に MISSING sentinel を導入して「欠損（正当な省略）」と「値が None（バグ）」を区別。
  card_version / meta / sections の構造（bars・active・rms_db）・instrumentation も検査対象に
  ③ 動作確認欄の旧期待値（6/up）を確定値（7/mixed）に更新。テストは計40件

### 方針変更

- **source 選択は「usable かつ進行あり の6分割で最大音量」を正として確定**（レビュー指摘）。
  通し再分析で rau の source が 6s-piano → 6s-other に変わり chords が {6, up} → {7, mixed} に
  なったのは、この規則どおりの動き。合議は v1 では入れず、plan の期待値表・スキーマ例を
  現行の実値（7, mixed）に更新した
