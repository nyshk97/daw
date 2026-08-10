# 好きなビート23曲の横断分析 — 共通核・タイプ分け・素材調達への接続

## 概要・やりたいこと

[`docs/labs/reference-beat-picks.md`](../labs/reference-beat-picks.md) に集めた23曲を、
「好きな日本のビート」の**正例コーパス**として全曲分析する。上モノだけでなく、ドラム・ベース・
構成を含むビート全体から、本人がまだ言語化できていない共通点と複数の好みの型を見つける。

今回の出発点は、市販ループのコード／メロディーが「洋物っぽすぎる」という実使用上の不一致。
23曲を無理に1つへ平均せず、データ上まとまりがあるなら複数グループとして残し、最終的に
各タイプへ合う上モノ素材の調達方法（完成ループ・サンプリング用コンポジション・自作／演奏依頼）へ
接続する。

このplanの出口は**好みの地図と素材調達方針**。既存のおすすめ5やLaLa本体の推薦ロジック変更は
結果を耳で確認した後の別planにする。仮説の段階でアルゴリズムへ焼き込まない。

## 前提・わかっていること

### /dig で確定した分析範囲

- 入力は `reference-beat-picks.md` の23曲すべて。全曲が「作りたい側」の正例で、中心／隣接／洋物の
  区別は付けない
- 対象は**ボーカルを除いたビート全体**:
  - 上モノ: ハーモニー、音高運動、楽器／奏法、密度、質感
  - ドラム: **配置・グルーヴ**と**音響（ヒットのキャラクター／プロダクション）**を別軸で分析
  - ベース: 音数、音域、音価、配置、上モノとのハーモニー関係
  - 構成: パーツの抜き差し、ループ周期、代表区間
- 各曲の好きな時刻を人が先に23曲ぶん指定する方法は採らない。機械側で代表区間を2〜3個抽出し、
  グルーピング後に代表曲と境界曲だけ本人が耳で確認する
- クラスタ数は先に決めない。「全曲に共通する核」（後述の `measured=23/23`・`support=23/23` を
  満たすものだけ）と「一部だけに現れる型」を分離し、連続体なら無理にクラスタへ切らない。
  グループ数・名前は分析結果から決める
- 結論は「日本固有」と断定せず、**この23曲に共通する本人の好み**として書く。正例だけでは
  日本／海外の因果を分離できないため。現行Cymatics系は素材調達上の対照として上モノだけ比較する
- グループ確定後、代表曲だけクレジット／インタビュー／サンプル情報を調べる。音声だけから
  自演・演奏収録・既存曲サンプリング・市販ループを断定しない

### 既存パイプラインで再利用できるもの

- `tools/reference/analyze-url.sh` / `analyze.py`: YouTube取得、自動トリム、4分割・6分割Demucs、
  BPM、キー、小節頭、ステム、構成、グルーヴ、コード骨格、確認用クリップ
- `groove.py`: ドラムステムを low / mid / high に分けた16分プロファイル、マイクロタイミング、
  スウィング、ベース統計。ただし low はキック＋808のアタック、high はハット＋クラップのアタック
- `stems.py` / `topline.py`: 帯域配分、重心、ステレオ幅、HPSS比、上モノのアタック／減衰、
  コードループ骨格
- `scikit-learn 1.7.2` は既存venvに導入済み。新しいML依存を増やさずクラスタリングできる
- 既存の個別レポート生成は1曲5〜10分のClaude Code処理を含む。23本生成する必要はなく、
  機械JSONから横断レポートを1本だけ作る

### 現行分析の限界（横断分析でも隠さない）

- 現行 `topline.py` は全曲を1つのループに畳むため、途中で進行が変わる曲では実在しない平均を作る。
  横断分析は**代表区間単位**でコード骨格を取り直す
- コードの細かい種別（maj9 / maj7 / sus等）は音源分離とchromaのにじみで揺れる。個別名の一致を
  クラスタの主根拠にせず、ルート移動・三度の有無・テンション傾向・帰着先等の粗い性格を使う
- basic-pitchは持続音を刻むため譜面として使えない。メロディーは正確な採譜ではなく、音域・
  音高変化量・反復・音数等の統計として扱う
- ドラムの音響はキック／スネア／ハットの完全分離を前提にしない。ヒット周辺から取る
  **音のキャラクター**と、区間全体へ現れる残響・帯域バランス等の**プロダクション**を区別する。
  後者を純粋なサンプル音色とは呼ばない
- テンポ揺れ・上モノ無し等のfeature gate落ちで曲自体を除外せず、測れない軸を欠損として残す。
  一方、3拍子・6/8は既存4/4固定パイプラインでは**ゲート落ちとして検知できずサイレントに壊れる**ため、
  Phase 2.5で拍子／小節グリッドを別契約として確認する。未確認または非対応拍子はmeter-dependent viewへ
  理由付きで不参加とする。どの場合もペアごとにfeature集合を変えず、viewの固定参加条件を満たす曲だけで
  完全な距離表を作る。欠損を0や平均値へ黙って埋めない

### 入力・保存・容量

- `docs/labs/reference-beat-picks.md` を入力の単一の真実の源にする。箇条書きの最後のYouTube URLを
  識別子として読み、表や厳格な書式をユーザーへ要求しない
- 大きな音声成果物はrepoへ入れず、`~/Music/daw/reference-beat-corpus/` に置く。このフォルダは
  `project.json` を持たない研究コーパスで、LaLaのプロジェクト一覧には出ない。アプリで使う
  `<project>/references/` の共有ライブラリではない
- YouTube video IDは既存成果物の**候補発見**にだけ使う。同じ分析として自動再利用するには、
  後述する段階別fingerprintのうち必要なstage chainが一致した一意な候補であることを要求する。
  manifestは各曲の実フォルダ・stageごとの来歴・どこから再計算が必要かを指せるようにする
- manifestで成果物の所有元を `corpus` / `external` に分ける。コーパス外の既存分析フォルダは
  **read-only**とし、同期・分析・後始末のどの経路からも書き換え／削除しない。一致する既存成果物が
  無い場合は、コーパス所有フォルダへ別途取得・分析する
- 既存実測は1曲約600〜750MB。23曲で約14〜17GB、現在の空き65GB。トリム・分析・Phase 2.5の
  trim/grid監査成功後に
  **コーパス所有フォルダの** `source.wav` だけを削除し、`track.wav`・4/6分割ステム・分析JSON・
  確認クリップは保持する。外部成果物の `source.wav` は安い再トリム用なので残す
- Demucsは同じ入力でもバイト一致しないため、ステムを「再生成可能な一時物」として消さない
- リスト追加時は追加分だけ取得・分析し、既存曲の結果を再利用する。URL削除時も音声を自動削除せず、
  manifestで孤児として申告する（大量データの自動削除を同期処理に混ぜない）

### 横断分析の見方

1つの総合距離だけで分けると、ドラムの近さが上モノの違いを隠す（または逆）が起きる。以下を
**別々のview**として距離・グループを出し、最後に総合する。

1. 上モノ／ハーモニー
2. ドラム配置・グルーヴ
3. ドラム音響（ヒットのキャラクター＋プロダクション）
4. ベース／ハーモニー
5. 曲全体の構成
6. 総合ビート

同じ2曲が「ドラムは近いが上モノは遠い」こと自体を結果として残す。総合グループの説明には、
どのviewが近さを作ったかを必ず添える。

## 実装計画

### Phase 1: コーパスの入力契約と差分同期CLI [AI🤖]

- [x] `tools/taste/` を新設し、READMEに入力・保存先・再実行・成果物・既知の限界を書く
- [x] `corpus.py sync` を実装:
  - `reference-beat-picks.md` の箇条書きから表示名＋最後のYouTube URLを読む
  - video IDを曲エントリの安定IDにする。重複URL、URL無し、同一IDの別名、空行を明示的に診断
  - `~/Music/daw/reference-beat-corpus/manifest.json` を原子的に更新
  - 既存の `~/Music/daw/**/references/*/source.info.json` も走査し、同一video IDの分析済みフォルダを
    **再利用候補**として列挙する。video IDだけで採用せず、用途に必要なstage fingerprint chainが
    完全一致する一意な候補だけを `ownership: external` のread-only成果物として参照する
  - legacy成果物のようにstage provenance不足の候補、または同一IDでfingerprint chainが食い違う
    複数候補は `reuse_conflict` として全候補を申告し、黙って1つを選ばない。必要ならcorpus所有先で再分析する
  - コーパス内で新規作成する成果物は `ownership: corpus` とする。絶対パスとownershipはローカルmanifest
    だけに持ち、repoの派生データへ書かない
  - 追加・変更・分析済み・失敗・リストから外れた孤児を一覧表示。syncでは削除しない
- [x] provenanceを単一pipeline hashでなく、親fingerprintを持つ成果物DAGとして定義する:
  1. `acquire`: canonical URL／video ID、取得設定・取得コード、記録済み `source.wav` hash、materialization状態
  2. `trim`: acquire fingerprint、正確なtrim範囲・trimコード／設定、`track.wav` hash・尺／SR／channels
  3. `demucs`: trim fingerprint、モデル名／版・分離wrapper／設定、生成stemの整合情報
  4. `reference_analysis`: 必要なtrim／demucs fingerprint、既存個別分析コード・gate閾値・schema、主要JSON
  5. `taste_features`: 必要なupstream fingerprint、grid／segment契約、taste featureコード・schema、派生data
  6. `distance_cluster`: taste feature fingerprint、distance／weight／clusterコード・schema、距離表／群結果
- [x] 曲フォルダの `analysis-provenance.json` はacquire〜reference_analysis、corpus rootの
  `taste-provenance.json` はgrid監査・taste_features・distance_cluster・comparison sensitivityを持つ。
  各stageは `parent_fingerprints`、設定snapshot、出力hash、実際に使うPython／shell／requirements／schemaを
  相対パス順の `path + SHA-256` 一覧にした `stage_source_hash` を記録する。git commit／dirty状態は説明用
  メタデータに留め、同一性判定へ単独では使わない
- [x] source hash対象一覧をstage別schemaとして管理し、対象ファイルの追加漏れを診断する。同一commit上で
  1 stageの関連ファイルだけを別々に変更したfixtureはそのstageと子孫だけ異なるfingerprintになり、
  親stageは一致し、元へ戻すと全hashが戻ることをテストする
- [x] acquireの `materialization` は `present`／`dispose_pending`／`disposed_after_verified`／
  `missing_unexpected` に固定し、artifactの保管状態としてfingerprint本体から分離する。通常検証は:
  - `present`: `source.wav` が存在し、記録済みhashと一致すればvalid
  - `disposed_after_verified`: source不在でも、記録済みsource hash、同じacquire fingerprintをparentとして
    記録し、source bytesを再参照せず設定／コード／`track.wav` hashを検証できるtrim stage、cleanup記録が
    揃えばacquire chainをvalidのまま扱う。親valid→子validの循環判定にはせず、記録済み親fingerprintとの
    一致を先に検査する
  - `dispose_pending`: sourceがあれば`present`へrollbackし、不在なら上記trim／cleanup条件を検査して
    `disposed_after_verified`へfinalizeする。どちらも通常の再取得を起動しない
  - `missing_unexpected`: source不在かつ検証済みdispose条件が無ければinvalid。再トリムが必要になりvalidな
    trim子stageを失った場合も、ここから初めてsourceを再取得／externalから再copyする
- [x] invalidation規則を固定する。取得／trim変更は全子孫、Demucs変更はdemucs以降、個別分析変更は
  reference_analysis以降、taste feature変更はtaste_features以降、distance weight／linkage変更は
  distance_clusterだけを再計算する。Phase 3の追加でPhase 2成果物を自己失効させない
- [x] Markdownの表記揺れ（全角空白、曲名とURLの区切り欠損、記号を含む曲名）をfixtureで固定
- [x] `taste:sync` / `taste:test` を `.mise.toml` に追加（日本語description、usage方式）

### Phase 2: 再開可能な23曲バッチ取得・分析 [AI🤖]

- [x] `corpus.py analyze` を実装。曲単位は直列（1曲内の`analyze.py`がCPU/MPSを並列利用するため）、
  manifestとstage DAGの状態から最初のinvalid stage以降だけ中断再開可能にする
- [x] 各曲は既存 `analyze-url.sh` / `analyze.sh` を呼ぶだけにし、BPM・コード等の分析ロジックを
  コーパス側へ複製しない。stdout/stderrログを曲ごとに保存し、1曲の失敗で残りを止めない
- [x] `ownership: external` のフォルダには分析再実行・provenance追記・一時ファイル作成・cleanupを
  一切行わない。必要なstage chainが合わず再分析が必要なら `ownership: corpus` の新規フォルダを作る
- [ ] externalのstage prefixだけ一致する場合は、検証済みupstream出力をcorpus新規フォルダへcopyし、
  `imported_from_external_path` とhashを記録して最初のinvalid stageから再開する。外部ファイルへのhard linkや
  in-place更新には依存せず、corpus側が以後の成果物を所有する
- [x] 成功判定を「process exit 0」だけにしない:
  - `track.wav` が非無音・妥当な尺
  - `analysis/gates.json` と主要JSONが読める
  - 4/6分割ステムが揃う
  - `analysis-provenance.json` の完了stageが実ファイル、またはacquireの検証済みmaterialization契約と一致し、
    各stage fingerprintを再計算できる
  - ゲート落ちは失敗でなくfeature欠損として記録
- [x] Phase 2では `source.wav` を削除せずmaterialization=`present`、cleanup_status=`pending_grid_audit` とする。
  Phase 2.5でtrim／gridを確認する前に安い再トリム元を失わないため。外部フォルダは全Phaseを通して
  `source.wav` を含め全ファイルを保持する
- [x] `--only <video-id>` / `--retry-failed` / `--dry-run` を用意し、トリム修正や個別再分析を
  全曲再実行なしで行えるようにする
- [ ] テストは偽の子プロセスと一時ディレクトリで、差分実行・失敗継続・再開・成功前非削除・
  Phase 2完了後pending_grid_audit・grid確定後source削除・needs_review／unknown時source保持・
  external全ファイル不変・
  stage chain完全一致時だけ再利用・複数候補競合・孤児非削除を固定。ネットワーク／実曲はCIテストへ
  入れない
- [x] 23 URLのメタデータを確認し、削除済み・年齢制限・動画タイトル取り違えをmanifestに申告する
- [x] 全23曲を実行。各曲のBPM/キー/ゲート結果/容量/所要時間をサマリーへ出す

### Phase 2.5: トリム・拍子・小節グリッド監査 [AI🤖 → 問題曲のみ人間👨‍💻]

- [x] 既存パイプラインの4/4決め打ち結果をそのまま信用せず、`grid-audit.py` で全23曲をトリアージする。
  3/4・4/4・6/8のaccent周期仮説と、曲の序盤／中盤／終盤でのclick-grid整合を比較し、trim境界・tempo・
  first downbeat・bar gridの候補と証拠を出す。既存のtempo/downbeat gateだけで4/4確定にはしない
- [x] 自動判定のmargin／整合条件は合成3/4・4/4・6/8 fixtureで分析前に固定する。条件を満たす曲は
  `auto_verified_4_4`、仮説が割れる・途中でgridがずれる・trim境界が不自然な曲だけ `needs_review` とし、
  先頭／中盤／終盤のclick付き短音源を `review/grid/` に生成する
- [x] 人間は `needs_review` の曲だけ、trim範囲・拍子（4/4、3/4、6/8、other、unknown）・click位置を
  `review/grid/README.md` で確認する。AI側が回答をmanifestへ取り込み、再監査後にPhase 3へ進む。
  未回答を暗黙の4/4として扱わない
- [ ] `ownership: external` の曲でtrim修正が必要になった場合は、外部フォルダを更新せず次の遷移を行う:
  1. 外部に読める `source.wav` があればhashを記録してcorpus新規フォルダへコピーし、無い／不整合なら
     canonical URLから再取得する
  2. `ownership: corpus` の新規フォルダでacquire〜reference_analysisの全per-song stageを実行し、
     provenanceとgridを再監査する
  3. 全per-song stageとgrid監査が成功した時だけmanifestの `active_artifact_path` を新規corpus先へ
     原子的に切り替え、`supersedes_external_path` と移行理由を残す。失敗時は参照先を切り替えず、
     外部候補と失敗中間物を申告する
  4. 切替後はその曲を親に持つtaste_featuresとdistance_clusterだけをinvalid化して再計算する
  外部元は移行成功後もread-onlyの候補として残し、自動削除しない
- [x] trim修正時は保持中の `source.wav` から `track.wav` を作り直し、分析・provenance・grid監査を
  再実行する。trimが確定しgrid_statusが `auto_verified_4_4`／`human_verified_4_4`／
  `human_verified_non_4_4` のいずれかになったコーパス所有曲だけ、cleanup intentと記録済みsource hashを
  原子的に保存してmaterializationを `dispose_pending` にする。その後 `source.wav` を削除し、
  `disposed_after_verified` とcleanupログを原子的に確定する。needs_review／unknown曲は `present` のまま
  再確認用に保持する
- [x] manifestへ `trim_start/end`、`meter_numerator/denominator`、`tempo_bpm`、`first_downbeat_s`、
  `bar_duration_s`、`grid_status`、判定証拠、schema revision、確認日時を保存する。外部成果物には書かず
  local manifestだけを更新する
- [x] `grid_status` は `needs_review`／`auto_verified_4_4`／`human_verified_4_4`／
  `human_verified_non_4_4`／`unknown` に固定する。4/4の2状態だけをmeter-dependentな
  上モノ／ハーモニー・ドラム配置・
  ベース／ハーモニー・曲全体の構成viewへ参加可能とする。confirmed 3/4／6/8／otherとunknownは、
  既存4/4分析値を使わず `unsupported_meter`／`unverified_grid` の理由付き不参加にする。ドラム音響viewは
  bar位置を使わないhit／activity窓で必要featureを満たせる場合だけ参加可とする

### Phase 3: 代表区間の抽出と横断用特徴量 [AI🤖]

- [x] `segments.py` を実装。確認済み4/4曲では小節頭に揃った4または8小節窓の**共通候補プール**を作る。
  grid非対応曲のドラム音響viewだけは、拍位置へ依存しない固定時間のhit／activity窓を別に作る。この段階では
  ドラム・ベース・上モノの同時存在を要求せず、ボーカルは評価音から除外する。自動トリムや小節頭が
  信頼できない曲を後段で黙って補正せず、Phase 2.5へ戻すか関連viewを理由付き不参加にする
- [x] 共通候補プールから、曲ごと・viewごとに2〜3区間を独立選定する:
  - 上モノ／ハーモニー: guitar/piano/otherの対象実音と十分なharmony信頼度がある窓
  - ドラム配置／ドラム音響: drumsの対象実音がある窓。ドラムだけのブレイクも候補に残す
  - ベース／ハーモニー: bassの対象実音と、相対関係を測れるコード推定がある窓
  - 各view内で反復度と代表性を優先しつつ、同じ特徴の窓を3つ選ばず、曲内の多様性枠を1つ残す。
    1回だけのSEやintro/outroを代表枠にはしないが、そのviewの曲内variantを示す場合は多様性枠で残す
  - 曲全体の構成viewは代表窓を使わず、全タイムラインから直接featureを作る
- [x] 選定理由（view・必要なactive stems・反復度・小節範囲・代表／多様性枠）をJSONへ残す。
  同じ窓が複数viewへ採用されることは許す。閾値・重みはコードの定数にし、23曲に合わせて場当たり的に
  手編集しない。変更理由はplanログとlabsへ残す
- [x] クラスタリングの標本単位は**曲**に固定する。各曲はviewごとに2〜3個の代表区間ベクトルの集合を持ち、
  区間を平均して「実在しない代表区間」を作らず、区間を独立標本にして3区間の曲を重くもしない。
  viewごとの曲間距離は、Aの各区間からBの最近傍区間への距離とBからAへの距離を両方向で平均する
  **対称最近傍集合距離**とする。各曲の最終的な重みは1とする
- [x] 選んだ区間について既存ステムから以下のfeature viewを生成:
  - **上モノ／ハーモニー**: 相対キー上のルート移動、帰着度、コード変化数、三度を伏せる比率、
    7th/add/sus系の粗い比率（信頼度付き）、ループ長、音高変化量、反復、オンセット密度、
    piano/guitar/otherの在不在、帯域・アタック・余韻・幅
  - **ドラム配置**: low/mid/highの16分プロファイル、拍単位の骨格、8分／16分密度、
    スウィング、マイクロタイミング、フィル／抜き差し量
  - **ドラム音響**:
    - low/mid/high別に検出したヒット周辺を短く切り、ヒットごとに音量正規化してから、帯域包絡・
      重心・rolloff・アタック／減衰等を中央値とばらつきで集約する。別ヒットとの重なりが大きい窓は
      除外または低信頼度とし、これは「ヒットのキャラクター」と表記する
    - 区間全体のband balance・HPSS比・ステレオ幅・残響は、配置／密度にも影響される
      「ドラムのプロダクション」として別feature群にする。純粋なサンプル音色とは表記しない
  - **ベース／ハーモニー**: 音数/小節、主要音域、音価、16分配置、ドラムとの同時率に加え、
    推定コードルートに対するroot／3rd／5th／non-chord比率、コード変化との同期、次コードへの
    アプローチ傾向を信頼度付きで持つ
- [x] 代表区間とは別に、**曲全体の構成feature**を生成する。active stemの時間占有率、セクション数・
  長さ・遷移、intro/body/outro比、セクション再登場、抜き差しの位置と継続時間、energy／帯域／幅の
  推移、セクション列の反復を持ち、4/8小節窓だけで順序や再登場を失わないようにする
- [ ] `topline.py` / `groove.py` / `stems.py` の純粋関数を必要最小限refactorして共有し、同じDSPの
  二重実装を避ける。既存個別分析の出力が変わらないことを既存テスト＋fixtureで固定
- [x] viewごとに固定の `required_features`・最小信頼度・参加条件をschemaとして定義する。
  必須featureが揃った曲だけをそのviewへ参加させ、全参加曲へ同じfeature集合と型別distance schemaを使う。
  optional featureは説明だけに使い、距離へは入れない。欠損を補完せず、ペアごとのfeature共通部分で
  距離を測らず、クラスタリングへは必ず完全な距離表を渡す。不参加曲と理由も結果へ残す
- [x] version管理したdistance schemaでfeature群ごとの符号化・距離・重みを固定する。個々のfeature数で
  暗黙に重みが変わらないよう、まず各feature群を0〜1距離へ集約し、schemaに書いた群weightで合成する:
  - 正値の連続量は必要に応じてlog変換後、robust center／scaleしてclipped L1距離
  - 0〜1の比率は変換せず絶対差、16分profileやband balance等の分布はJensen–Shannon距離
    （両方zero-massなら0、片方だけzero-massなら1とする）
  - 相対pitch class／root intervalの要素間は12半音の循環距離。root／コードの**ループ列**同士は、
    一方の全循環シフトについて正規化DTWを計算し、その最小値を採る。これにより
    `Am–F–C–G` と `F–C–G–Am` は同じ循環として扱うが、逆順までは同一視しない
  - 曲全体のsection列はintroからoutroへの順序自体がfeatureなので、循環シフトせず通常の正規化DTWを使う。
    楽器在不在等のmulti-hotはJaccard距離（両方空集合なら0）
  - schemaの型と異なる値は黙って連続量として扱わずvalidation errorにする。weight変更はrevisionを上げ、
    変更理由をplanログへ残してdistance_clusterだけ全曲分を再計算し、upstream stageは再利用する
- [x] 総合view v1のmandatoryは、上モノ／ハーモニー・ドラム配置・ドラム音響・ベース／ハーモニー・
  曲全体の構成の**5 base viewすべて**と分析前に固定する。「総合ビート」が一部軸だけの近さへ変質するのを
  防ぐためで、結果を見てmandatoryを減らさない。5 viewすべてへ参加できる曲だけで総合viewを作り、
  参加曲が少なすぎて安定した群を作れない場合は総合グループを出さず、各viewの地図と不参加理由を
  正式な結果にする。mandatoryを変える場合は別schema revision／別実験として全結果を併記する
- [x] 小さい派生データ `docs/labs/reference-beat-taste-data.json` を生成。video ID・表示名・
  区間・ゲート・正規化前featureのみを持ち、音声・絶対パス・YouTube取得メタデータは含めない
- [ ] 合成音と小型JSON fixtureで、区間選定の決定性・異なる編成の多様性枠・イントロ除外・
  view別の実音条件・ドラムだけのbreak採用・ヒット単位正規化・曲全体の構成順序・ベースのコード相対・
  固定feature参加条件・コードループ開始位相不変／section順序保持・分布／multi-hot距離・欠損非補完・
  既存DSPと同値をテスト

### Phase 4: multi-viewグルーピングと機械ドラフト [AI🤖]

- [x] `cluster.py` を実装。上モノ／ドラム配置／ドラム音響／ベースハーモニー／曲全体の構成の
  5 base viewと、それらを合わせた総合viewを出す:
  - distance schemaに従って型別距離を計算し、feature群の次元数が多いだけで勝たないよう
    feature→群→viewの段階で正規化・固定weight合成する
  - 代表区間を使う4 viewは、固定feature集合で作った区間距離から対称最近傍集合距離を計算する。
    曲全体の構成viewは1曲1ベクトルを直接比較する。いずれも**曲×曲の完全な距離表**を作る
  - 総合viewはschemaでmandatoryとした全base viewに参加できる曲だけを対象に、その全距離表を
    view等重みで合成する。曲やペアごとに合成viewを変えず、feature数の多いviewや代表区間数の多い曲を
    重くしない
  - 参加条件を満たす曲だけで階層クラスタリングする（結果が連続体なら切らない）。NaNを含む
    不完全距離表は渡さず、view不参加曲は理由付きで別欄へ残す
  - 非ユークリッドなprecomputed距離を受け取れるaverage linkageを主契約にする
    （`AgglomerativeClustering(metric="precomputed", linkage="average")`）。Wardは使わない。
    1つの遠い区間だけで曲群全体を切るcomplete linkageより、曲内variantを別途flagする今回の設計に
    合うためaverageを選ぶ。silhouetteも同じprecomputed距離で計算する
  - view参加曲数をMとし、`M < 3` なら距離地図・近傍・不参加理由だけを出してクラスタリング／silhouetteを
    呼ばない。`M >= 3` なら `k = 2..min(6, M-1)` だけを候補にする
  - 各候補は全cluster sizeが2以上の場合だけsilhouette・少数feature除外時の安定性を評価する。
    singletonを含む分割は群候補として棄却し、その曲は距離地図上の外れ値候補としてだけ表示する。
    この条件で候補が0件なら、Mが3以上でも「群なし／距離地図のみ」を正式結果にする
  - seed／入力順に依存しない決定性
- [x] 各曲は各viewで主所属を最大1群だけ持つ。同じ曲の代表区間が互いに遠く別タイプをまたぐ場合も、
  曲を複数群へ複製せず `internal_variant` と副タイプ候補を記録してレビュー対象にする
- [x] クラスタ数はsilhouette最大を自動採用して終わらせない。以下を満たす切り方を機械ドラフトに
  候補として最大2案出し、代表音声の耳確認で最終決定する:
  - 各群を特徴量の差で説明できる
  - 1曲だけの群は出さず、外れ値候補として理由と近傍距離を示す
  - viewを1つ抜いただけで全所属が崩れる切り方は不安定として落とす
- [ ] 各viewの参加曲／不参加理由、近傍曲、総合グループ、境界曲、外れ値、曲内variant、共通核候補を
  JSON＋Markdownへ出す。
  「近い理由」はfeature寄与上位を音楽語へ変換し、内部統計名だけで終わらせない
- [x] 「23曲すべての共通核」は、同じ定義で `measured=23/23` かつ明示した条件を
  `support=23/23` が満たすfeatureだけに使う。数値が全曲で測れただけなら「23曲の分布」と表記する。
  欠損を含むview内の傾向は「view参加曲の共通点（support=N/M、eligible=M/23）」、グループ内は
  `support=N/M` と必ず母数を併記し、参加曲だけの結果を23曲共通へ昇格させない
- [x] 同一アーティスト名をfeatureへ入れず、結果説明時にだけ表示する。アーティストが同じだから
  同群になる循環を防ぐ
- [x] 現行ループライブラリとの比較は**上モノの共有subsetだけ**実施する。現行 `index.py` の値だけでは
  ハーモニーを比較できないため、そのまま上モノview全体との距離には使わない
- [x] referenceの上モノ合算区間とCymatics系ループ原音を、同じsample rate・mono化・bar整列・
  loudness条件、同じversionの共有抽出器へ通す。共有subsetは、両方で同じ定義にできる相対root移動・
  root変化数・三度の明瞭／曖昧比・音高運動／反復・onset密度・band balance・centroid・rolloff・
  HPSS比に限定する。referenceだけで得られるinstrument stem在不在・stereo幅等は比較距離へ入れない
- [x] Demucs入力条件の差を測る `comparison-sensitivity.py` を先に実行する。既存indexのcentroid高低×
  onset密度高低の4領域から各2本、計8本のループを選ぶ（音響的な端を少数で覆うため）。各ループを
  drums（contaminant/upper `-6dB`）、drums+bass（`0dB`）、drums+bass+vocals（`+6dB`）の3条件で
  混ぜる。上モノ優勢・同等・他パート優勢の実用範囲を段階的にstressするための値とし、referenceと同じ
  Demucsモデルを通したguitar+piano+other合算と、分離前のクリーンループで共有featureを比較する
- [x] 感度テストの採用基準は23曲の群をみる前に固定する。scalarはSpearman `rho >= 0.8`（順位が強く
  保たれる水準）かつmedian absolute errorが元ループ群IQRの`0.25以下`（群内幅の1/4以下）、分布／列は
  schema距離のmedian `<= 0.15`・90th percentile `<= 0.35`（典型時だけでなく悪条件でも差が支配しない
  水準）を両方満たすfeatureだけ `comparison_required_features` に採用する。落ちたfeatureと誤差は隠さず
  sensitivity JSONへ残す。harmony・pitch motion/repetition・onset/rhythm・spectrum/textureの4群中、
  安定featureを持つ群が3群未満なら「Cymatics比較は根拠不足」として距離比較を出さない
- [x] 共有subsetにも固定required featureとdistance schemaを適用し、同じ抽出器を通過した項目だけで
  corpus各群とCymatics系の中央値／IQR・効果量・近傍を出す。比較参加数を両側とも明記し、欠損時に
  feature集合をペアごとに変えない。corpus側は代表区間数で重みを変えず1曲1票で集約する。
  ドラムの洋物対照曲は今回無いため、ドラム共通点を「日本固有」とは書かない
- [ ] 合成コーパスで「上物だけ2群」「ドラムだけ2群」「区間数2対3」「曲内variant」「view欠損曲」
  「連続体」「外れ値」を作り、曲の等重み・完全距離表・multi-view分離・決定性・無理なクラスタ非採用を
  テスト

### Phase 5前の確認素材生成 [AI🤖]

- [x] 機械ドラフトの**各base view**・各グループ案から、代表2曲＋境界1曲を選ぶ。同じ曲が複数viewで
  選ばれた場合も曲名は集約するが、そのviewを判断できる素材は省略しない
  - 群候補が成立しなかったview／総合viewは、無理に群を作らず、距離地図の中心1曲＋最遠の両端2曲を
    生成して「連続体／外れ値でよいか」を確認する
- [x] `~/Music/daw/reference-beat-corpus/review/` にview別の短い確認素材を生成:
  - 上モノ／ハーモニー: 上モノのみ
  - ドラム配置／ドラム音響: ドラムのみ（音量差だけで音響群を判断しないよう試聴用loudnessを揃える）
  - ベース／ハーモニー: 上モノ＋ベース、およびベースのみ
  - 曲全体の構成: 主要セクションと遷移を時系列順に並べた短いダイジェスト。各境界の前後を残し、
    0.5秒の無音とREADMEの元時刻対応で編集点と原曲の遷移を区別できるようにする
  - 総合グループ候補: ボーカル抜きのビート全体
  - ファイル名にview・案・グループ・曲名・小節／元時刻範囲を入れる
- [x] `review/README.md` にviewごとの「何を比べるか」、元時刻、チェック欄を用意。全23曲×全ステムは
  出さず各viewの代表／境界だけに絞る。必要なら誤分類群だけ追加生成できるCLIを用意
- [x] 各WAVの尺・RMS・peakを自己検査し、無音や切り出し範囲外が1本でもあれば非ゼロ終了

### Phase 5: グループと共通核の耳確認 [人間👨‍💻]

- [x] 各base viewの代表曲・境界曲を、そのview用素材で試聴して `review/phase5-human-review.md` に記録:
  - 上モノ／ハーモニー: 同じコード／メロディーのタイプに感じ、共通説明が合っているか
  - ドラム配置／音響: 配置のノリと、ヒットのキャラクター／プロダクションの説明が合っているか
  - ベース／ハーモニー: ベース単体の動きと、上モノに対する音の選び方が同じタイプに感じるか
  - 曲全体の構成: セクション順序・長さ・抜き差し・再登場の説明が同じタイプに感じるか
  - 各viewの境界曲はどちら寄りか／別タイプか。総合候補はボーカル抜きビートで納得できるか
- [x] 機械案が体感と違う箇所だけ追加クリップを依頼。全曲総当たりへ戻らない

### Phase 6: 耳確認の反映と代表曲の制作方法調査 [AI🤖]

- [x] 人間の判定を「正解ラベル」として盲目的にクラスタへ強制せず、ズレの原因をfeature不足・
  区間選定ミス・連続体・本当に別型、に切り分ける。妥当なものだけ区間／feature／切り方へ反映し、
  変更前後をログへ残す
- [x] 確定した各グループの代表曲について、制作クレジット・制作者インタビュー・公式情報を調査:
  - 自作演奏／セッション収録／既存曲サンプリング／サンプリング用素材のどれか
  - 断定できないものは「不明」とし、音から制作方法を推測して事実扱いしない
  - Web情報は一次情報・公式媒体を優先し、URLと確認日を記録
- [x] 制作方法と音響特徴を結びつけ、グループごとに素材調達の第一候補・フォールバックを書く。
  実装コストでなく、得られる聴こえ方と作業速度のトレードオフを説明する

### Phase 7: 横断レポート・設計への反映 [AI🤖]

- [x] `docs/labs/reference-beat-taste-analysis.md` を作成:
  - 対象23曲と分析条件
  - `measured=23/23`・`support=23/23` を満たす全曲共通核（無ければ「全曲共通は無い」と明記）
  - view／グループ内の傾向は `support=N/M`・`eligible=M/23` の母数付きで別記
  - 確定したグループ、代表曲、境界曲、外れ値
  - 各グループの上物／ドラム配置／ドラム音響／ベースとハーモニーの関係／曲全体の構成
  - グループ間で何が違うか
  - Cymatics系上モノ素材との違い
  - 代表曲の制作方法調査
  - グループ別の素材調達方針
  - 測定限界・断定できないこと
- [x] `docs/labs/reference-beat.md` に実験の短い要約と上記レポートへのリンクを日付見出しで追記
  （既存エントリは書き換えない）
- [x] 結果によって現在の「上モノは完成ループを主経路」とする設計が変わる場合のみ、
  `docs/design/reference-beat.md` の**仕様本文**を更新する。実験ログだけに方針変更を置かない
- [x] 次の実装候補（推薦feature変更・コンポジション窓検索・自作上物生成等）は結論から列挙するが、
  このplanでは実装しない。必要なものだけ別途 `/dig` → `/plot`

### 動作確認 [AI🤖]

- [x] `mise run taste:test`、既存 `mise run ref:test`、`mise run lib:test` を実行。既存個別分析・
  おすすめ5への回帰がないことを確認
- [x] `taste:sync --dry-run` を2回実行し、2回目が変更なし。曲を1件追加したfixtureでは追加分だけ、
  削除では自動削除せず孤児申告になることを確認
- [x] 既存分析済み1曲＋新規1曲で中断再開smoke。失敗後の再実行で完了曲を飛ばし、失敗曲だけ再開
- [ ] 同一video IDの候補について、用途に必要なstage fingerprint chainが完全一致する一意な候補だけが
  再利用され、track hash・trim stage・Demucs stage等が食い違う候補および複数候補競合は
  `reuse_conflict` になることをfixtureと実データ候補一覧で確認
- [x] 同一git commitのまま1 stageのsourceを2通りに未コミット変更し、異なる `stage_source_hash` と
  fingerprintになること、親stageは一致して子孫だけinvalidになり、元へ戻すとhashも戻ることを確認。
  distance weightだけの変更ではacquire〜taste_featuresを再実行しないことも確認
- [ ] external曲のtrim修正fixtureで、外部sourceをread-only入力としてcorpus新規フォルダに全per-song
  stageを生成し、成功時だけ `active_artifact_path` が切り替わることを確認。途中失敗では参照先と
  external全hash／mtimeが不変であること、external source無しでは再取得へfallbackすることを確認
- [x] 全曲実行後、manifest上で23/23件が成功または理由付き欠損状態になり、無言の除外が0件
- [x] 各成功曲にtrack・両Demucsモデル・主要JSON・provenanceがあることを確認。`ownership: corpus` は
  3つのverified grid_statusだけmaterialization=`disposed_after_verified`かつ `source.wav` 不在、
  needs_review／unknown曲は`present`かつsource保持であること、`ownership: external` は分析前後で
  全ファイルのhash／mtimeが不変であることを確認
  （外部のsource有無は合否条件にしない）
- [x] cleanup済みcorpus曲で `corpus.py analyze` を2回再実行し、acquire／download子プロセス呼出回数が0、
  stage chainがvalidのままであることを確認。`dispose_pending`＋source不在のcrash fixtureも再取得せずfinalizeし、
  cleanup記録無しのsource消失だけが `missing_unexpected` となることを確認
- [x] viewごとに全参加曲が同じrequired feature集合を持ち、距離表にNaNが無いことを検査。不参加曲が
  理由付きで出力され、総合viewの参加条件も各mandatory viewと一致することを確認
- [ ] 合成3/4・4/4・6/8と途中gridずれfixtureで `grid-audit.py` を実行し、4/4以外／曖昧曲が
  auto_verified_4_4にならないことを確認。manifestがunknownの曲をmeter-dependent viewへ渡すと
  非ゼロ終了または理由付き不参加になり、総合v1のmandatoryが5 base viewすべてであることを検査
- [ ] 同じ曲から上モノ無し／ドラムだけ／ベース＋上モノのfixture窓を作り、viewごとに異なる区間が
  採用され、上モノview欠損でもドラムviewには参加できることを確認
- [x] distance schemaの単体テストで、隣接pitch classが近いこと、16分profileはJensen–Shannon、
  multi-hotはJaccardになることを確認。回転した同一コードループは距離0、逆順ループと回転したsection列は
  距離0にならないことを固定する。クラスタがprecomputed＋average linkageで、Wardを受け付けない契約も
  固定する
- [ ] 参加数M=0／1／2ではcluster／silhouetteが呼ばれず距離地図だけ、M=3では合法kが2でもsingleton分割が
  棄却され群なし、M=7ではk=2〜6だけが評価されることをspy付きfixtureで確認する。どのMでも例外や
  singleton groupを出さない
- [ ] 2区間曲と3区間曲を混ぜたfixtureで各曲の重みが1であること、曲が同一viewの複数群へ重複所属せず、
  曲内差が大きい場合は `internal_variant` になることを確認
- [ ] 同じ短い音源をreference上モノ形式とlibrary loop形式の両方から共有抽出器へ渡し、共有subsetが
  完全一致することを確認。比較出力に両側の参加数・固定feature集合・schema revisionがあることを確認
- [ ] Demucs感度fixtureで安定基準を満たすfeatureだけが `comparison_required_features` に入り、閾値を
  外れるfeatureと安定feature群3未満のケースでは距離比較が生成されないことを確認
- [ ] 共通核fixtureで22/23しか測れないfeatureが「全曲共通」へ入らず、`support=N/M`・`eligible=M/23`
  と出ることを確認
- [ ] `reference-beat-taste-data.json` から音声無しで距離表・クラスタ・Markdownドラフトを再生成し、
  同一結果になることを確認
- [x] review WAV全件の尺・RMS・peakを検査。各base viewの代表／境界に、そのview用素材が最低1組あり、
  構成ダイジェストの元時刻対応が単調増加であることを確認。生成本数と選出理由を報告
- [x] 実行コマンド、出力抜粋、pass/fail根拠をplanログと最終報告に残す
- [x] 成功した再利用可能なコーパス分析手順が `VERIFY.md` に無ければ、既存構造に合わせて追記

## 未決事項

- 実際のグループ数・グループ名・代表曲・共通核は分析結果そのものなので未決。事前に固定しない
- 代表区間の抽出重みとクラスタの切り方は、合成fixtureでの妥当性→23曲の機械診断→耳確認の順で
  決める。本人の確認前に「確定」と扱わない
- ドラムについて「日本／海外の差」を断定したくなった場合は、本人が洋物と感じる曲の対照コーパスが
  別途必要。今回は23曲の好みの共通点まで

## ログ

### 試したこと・わかったこと

- 2026-08-10 23 URLを曲単位直列で取得・分析。23/23 `analyzed`、両Demucs stem・主要JSON・曲別
  provenanceをhash検証済み。既存4候補はlegacy provenance不足のためexternalを更新せずcorpus側で再分析した
- 取得中の2件を回収。EXPLAINは主要分析後の無音確認clipだけが失敗したためwarning付き成功、My Wayは
  WebM format 251の403に対してAAC format 140 fallbackで成功した
- 人間確認前のgrid監査は15曲 `auto_verified_4_4`、8曲 `needs_review`。前者だけsourceを検証後cleanupし、後者は
  sourceと先頭／中盤／終盤のclick素材24本を保持した。閾値を実曲結果に合わせて緩めていない
- 人間確認前の機械案は上モノ・ベース・構成でk=2が成立。ドラム配置・ドラム音響・総合はsingleton拒否条件により
  群なし／距離地図のみ。grid依存4viewは15曲、drum audioは23曲が参加した
- Cymatics感度は8 loop×3混入条件。4群中3群（音高運動／反復、onset／rhythm、spectrum／texture）が
  固定基準を通過し、harmonyは比較距離から除外。corpus 15曲対library 8本の比較draftを生成した
- Phase 5前素材は33 WAV。群ありviewは代表2＋境界1、群なしviewは地図中心＋両端。全件1秒以上・非無音・
  peak 0dBFS以下を自己検査した。古いgrid review 45本はTrashへ移動済み（回復可能）
- 検証: `taste:test` 15件、`ref:test`、`lib:test`がexit 0。sync 2回は変更なし、analyze dry-runは
  23/23 `unchanged`。距離表は全viewで正方・対称・対角0・finite、16分profile質量は0.999997〜1.000003
- 2026-08-10 人間grid確認は3曲OK、2曲NG、3曲click不明。元READMEを保持したまま、曲を-7 dB・clickを
  約+7.6 dBにした再確認素材を5曲33本生成した。NG曲は高い小節頭accentだけを1拍ずつ回したA〜D、
  不明曲はlouder版。`taste:test` 16件がexit 0、全WAV 6.00〜9.98秒・非無音・peak 0.974976以下、
  NGのA〜Dが異なる波形であることを確認した
- 再確認でシグナル=C、タクシードライバー=Bに確定。louder版でもNGだったお嫁においで・Made my day・
  YourSongの3曲だけをA〜Dへ昇格し、第2回素材36本を生成した。`taste:test` 17件がexit 0、全WAV
  6.00〜9.98秒・非無音・peak 0.974976以下、3曲ともA〜Dが異なる波形であることを確認した
- 第2回確認はお嫁においで=`drift`、Made my day=C、YourSong=D。全回答をmanifestへ反映し、最終gridは
  15曲 `auto_verified_4_4`＋7曲 `human_verified_4_4`＋お嫁においで1曲 `unknown`。確認済み7曲はsourceを
  hash検証後cleanupし、unknown曲だけsourceを保持した。全23曲のanalyze dry-runは`unchanged`
- 22曲版の特徴量再計算は約26分。最終機械案は上モノ・ドラム配置・構成でaccepted k=2、ベース・
  ドラム音響・総合は群なし。拍位置依存viewは22曲、drum audioは23曲、library比較は22曲対8 loops。
  Phase 5素材は30 WAVへ更新した
- drum audioの候補はstability gateで棄却済みなのにreview生成が先頭候補へfallbackしていたため、
  accepted候補だけを群として扱うよう修正。棄却時は距離地図の中心＋両端へ戻る回帰テストを追加した。
  最終検証は`taste:test` 18件pass、profile質量0.999997〜1.000003、全距離表がfinite・対称・対角0、
  review 30 WAVが1秒以上・非無音・0 dBFS以下、root provenance全output hash一致
- Phase 5既存30素材を一問一答で耳確認。overallの群なしは支持、ドラム配置・上モノの2群は耳では差が弱く
  連続体、ドラム音響は機械が棄却したMade my day型を耳では別型、ベースは機械が群なしとした中に
  「動く支え」と「反復する土台」の2型を感じた。構成の2群は支持せず複数パターンがあり、FALL IN LOVEは
  2ブロックだけで素材不足。全回答を`review/phase5-human-review.md`へ記録した
- Phase 5追加確認は全曲総当たりへ戻さず14 WAVへ限定。FALL IN LOVEは等間隔8地点×2小節の固定timeline、
  上モノは重複boundaryの代替One、drum audioはMade／memory各側の機械近傍2曲ずつ、ベースは両anchorに
  近い曖昧2曲＋反復側近傍2曲をbass／top-bassで生成。全件12.00〜45.70秒・非無音・peak 0.980011以下、
  `taste:test` 19件pass、元Phase 5のprovenance output hash不変を確認した
- Phase 5追加14素材も一問一答で確認完了。FALL IN LOVEは実際に変化量が小さいがEXPLAINと同じ大きな型、
  Oneはコードだけ近く質感／楽器／刻みが異なるため上モノ2群は不支持。drum audioは0℃の日曜／Hugが
  Made側、YourSongがmemory側、タクシードライバーが中間となり機械近傍と逆転。bassはHugが動く支え、
  ゆれる／salt water taffy／Summer Situationが反復する土台となり、耳の2型が追加曲でも再現した。
  詳細を`review/phase5-human-review.md`へ追記し、Phase 5を完了した
- Phase 6のズレを切り分けた。上モノ・ドラム配置・overallは耳でも離散群より連続体、構成は単一軸の2群でなく
  複数の変化パターン、FALL IN LOVEは初回の区間不足だった。drum audioは耳の差を現行band proxyが安定再現
  できず、23曲だけへ過適合する弱いfeature追加は見送った。bassだけは追加曲でも「動く支え／反復する土台」が
  再現し、低音のpitch motion／repetition不足というfeature gapに特定した
- bass schemaをv2へ上げ、既存の選定区間を変えず66区間へpitch motion／repetitionを差分計算した。人間確認7曲の
  最近傍同型率は6/7から7/7へ改善。全組平均は同型0.210855→0.164738、異型0.185412→0.147801でまだ逆転して
  おり、音域・配置・密度を含むbass view全体を人間の2型へ強制分割しない。k=2候補も20対2かつstability不足で
  rejectedのままとした
- schema revisionを距離成果物へ引き継ぐ漏れも修正。最終検証は`taste:test` 22件pass、schema v2のbass motion
  66/66区間がfinite、5距離表すべてfinite・対称・対角0、Phase 5の30 WAVが非無音・peak 0.981以下、root
  provenance全output hash一致、analyze dry-run 23/23 `unchanged`、人間回答ファイル保持を確認した
- bass 2タイプの代表4曲を公式／本人情報で調査。シグナルはLIBROが全曲sound produceし元晴のsax演奏参加、
  HugはRhymeTube、LOVEはjjj、Summer SituationはSTUTSの制作まで確認した。bass音源が自演奏・既存曲sample・
  素材loopのどれかは公開根拠がなく不明とした。制作経路とbass型の対応は立証できないため、LaLaでは両型とも
  上モノloop追従MIDI＋pitch付きone-shotを第一候補、演奏loop／奏者依頼を動く型のfallback、長い808/subを
  反復型のfallbackとした。URL・確認日・音楽的／速度トレードオフは
  `docs/labs/reference-beat-taste-production-research.md`に記録した
- Phase 7の横断レポートを`docs/labs/reference-beat-taste-analysis.md`へ確定した。23曲共通の音楽的核は無し、
  overall 22曲はchill hiphop周辺の連続体、bass 2極だけを生成用の連続軸として採用。Cymatics 8 loopsは
  corpus 22曲とpitch motion／centroidが近い一方、repetitionが0.923対0.771と強く、完成loop主経路は維持し
  切替・休符・短い追加要素で変化を補う結論。設計本文、追記専用labs索引、次の実装候補も更新した

### 方針変更

- 2026-08-10 生成READMEはreview_materials stageのhash対象で`phase5/`自体も再生成時に置換されるため、
  人間回答を`review/phase5-human-review.md`へ分離した。再生成可能な機械成果物と、再生成で失っては
  いけない人間判断をディレクトリ境界でも分ける
- 2026-08-10 群候補なしのviewを試聴素材なしにすると「群なし」の耳確認ができないため、群を捏造せず
  距離地図の中心1曲＋最遠両端2曲を生成する仕様をPhase 5前へ追加した
- 2026-08-10 `analyze.py`末尾の任意clipだけが失敗した曲は、track・両stem・主要JSON・provenanceの
  成功契約を満たす場合にwarning付き成功とする。process exit 0だけを成功判定にしない既存方針の具体化
- 2026-08-10 bassだけは耳確認が追加曲へ再現したため、低音の動き／反復を必須群へ加えてschema v2とした。
  weightはplacement 0.25・pitch relation 0.30・density 0.15・motion 0.30。耳ラベルへの完全一致を目的にせず、
  音楽的に独立して説明できる不足軸として採用し、既存区間を再選定しない差分更新にした
- 2026-08-10 上モノ・ドラム配置・構成は閾値を調整して耳の回答へ合わせず、連続体／複数軸として扱う。
  drum audioも現行抽出から確かな代理特徴を作れないため保留し、Made型を出すためだけの閾値変更はしない

- 2026-08-09 planレビューを反映。既存分析フォルダをread-onlyの `external` とし、`source.wav` 削除を
  `corpus` 所有先だけに限定した。再利用判定はvideo ID一致からprovenance stage chain一致へ変更
- 欠損ペアごとの可変feature距離を廃止し、viewごとの固定feature／参加条件と完全距離表へ変更。
  クラスタリング単位を曲、代表区間を平均しない対称最近傍集合距離とした
- ドラム音色をヒット単位の音のキャラクターと区間全体のプロダクションへ分離。ベースとコードの
  関係、および代表窓から独立した曲全体の構成featureを追加した
- 2026-08-09 追加レビューを反映。代表区間をview別選定へ変更し、全base viewの試聴素材を追加。
  Cymatics比較は同一抽出器の共有subsetだけに限定した
- 全曲共通核を `measured=23/23`・`support=23/23` に限定。混合型featureは型別距離＋固定weight、
  クラスタリングはprecomputed距離対応のaverage linkageへ固定した
- 2026-08-09 追加レビューを反映。4/4固定パイプラインの拍子問題をfeature gateから分離し、Phase 2.5で
  トリム／拍子／gridを監査。未確認・非4/4はmeter-dependent viewへ入れない契約にした
- provenanceへ分析source実内容のhashを追加。Cymatics比較前にDemucs感度試験を置き、
  安定featureだけを共有subsetへ採用する。総合view v1のmandatoryは5 base viewすべてに固定した
- 2026-08-09 追加レビューを反映。externalの再トリムはcorpus所有先へcopy／再取得し、全per-song stage
  成功後だけmanifestを切り替える。provenanceを段階別DAGへ分割し、変更stage以降だけ再計算する契約にした
- コード／rootループ距離は全循環シフトの最小DTW、曲全体のsection列は通常DTWとし、ループ開始位相だけを
  距離から除外した
- 2026-08-09 追加レビューを反映。acquireにmaterialization状態を設け、検証済みsource cleanup後もvalidな
  trim子stageがあれば再取得しない契約にした
- クラスタ候補kを参加数Mに対する `2..min(6, M-1)` へ制限。M<3とsingletonしか作れない場合は
  クラスタ／silhouetteを出さず距離地図だけにする
