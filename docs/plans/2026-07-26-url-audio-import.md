# URLから音声を取り込む（yt-dlp連携）

## 概要・やりたいこと

YouTube等のURLを入力すると、その動画の音声をダウンロードして新規オーディオトラックのリージョンとして配置する機能を作る。

現状は https://wvw-y2mate.com/ja1/youtube-to-mp3/ のような外部サイトでmp3を書き出し、それを手動でプロジェクトに取り込んでいる。この「ブラウザでURLを貼る → mp3をダウンロード → DAWで読み込む」の3手を、「メニューを選ぶ → URLを貼って確定」の1手に畳む。

用途は参考音源・ビートの持ち込み。曲の頭で数回使う程度の低頻度操作。

## 前提・わかっていること

### 既存経路がほぼそのまま使える

`MainComponent::startImport(file, -1, 0)`（`Source/ui/MainComponent.cpp:1932`）にローカルの音声ファイルを渡すと、以下が全部走る:

- `AudioImporter` スレッドでデコード → サンプルレート変換（WindowedSinc 200タップ）→ 24bit WAV書き出し
- `Project::nextClipFile()` で `clip-NNN.wav` に採番してリネーム
- 新規オーディオトラック作成 → 位置0にリージョン配置 → `pushSnapshot()` → `trySave()`

つまり**新規に作るのは「URL → ローカルの音声ファイル」の部分だけ**。`Project` のスキーマ変更も不要（`Clip` をそのまま使う）。

### 調査済みの環境・制約

| 項目 | 状況 |
|---|---|
| `ffmpeg` | 導入済み（`/opt/homebrew/bin/ffmpeg`・Brewfileにも記載あり） |
| `yt-dlp` | **未導入**。Brewfileにも無い |
| 外部プロセス実行 | `Source/` 配下に `juce::ChildProcess` / `NSTask` / `system()` の使用箇所ゼロ。**このリポジトリ初** |
| App Sandbox | 未適用（entitlementsなし） |
| Hardened Runtime | ローカルビルドは `flags=0x0`。配布時に `--options runtime` を付けるが、**Hardened Runtimeが制限するのは dylib validation と JIT で、子プロセスの exec は禁止されない** → 問題なし |
| 長さ上限 | `AudioImporter::maxInputFrames = 100'000'000`（48kHzで約34分。`AudioImporter.h:25`） |
| メモリ | `Clip` は音声を全量メモリ常駐（`shared_ptr<AudioBuffer<float>>`）。34分ステレオで約800MB |
| `startImport` の型 | **現状 `void`**。シグネチャは `startImport(const juce::File&, int, juce::int64, bool othersSkipped = false)`（`MainComponent.h:136-137`）→ **第4引数は既に `bool`** |
| 表示名の決まり方 | `beginImportWorker()` が **`importDisplayName = source.getFileNameWithoutExtension();` で無条件に上書きする**（`MainComponent.cpp:1992`）。`importDisplayName` は `clip.name`（`:2076`）・新規トラック名（`:2099`）・ログ（`:2113`）・サンプル名（`:2164, 2174`）に使われる |
| 既存の取り込みガード | `startImportFlow()`（`MainComponent.cpp:1911`）と `startImport()`（`:1935`）はどちらも **`importActive \|\| bounceActive \|\| engine.isRecording()`** で弾く |
| CMake | **ソースを明示列挙**（`target_sources(daw PRIVATE …)` `CMakeLists.txt:86-107` / `target_sources(daw_tests PRIVATE …)` `:214-224`）→ 新規 `.cpp` は追加工程が必須 |
| dev版/Release版 | bundle id が別（`local.d0ne1s.daw.dev` / `local.d0ne1s.daw`）で並走できる。ただし**検証でRelease版（常用版）を起動しない**（後述） |

### `juce::ChildProcess` は使えない（実装を読んで確認済み）

`build/_deps/juce-src/modules/juce_core/native/juce_SharedCode_posix.h` を読んだ結果、3つの理由でこの用途に使えない:

1. **`read()` がブロッキング**（`:1193-1211`）。内部で `fread()` を呼ぶため、出力が止まったネットワーク処理では読み取りが返らず、ループ内で `threadShouldExit()` を観測できない。**キャンセルとアプリ終了が無期限に固まる**
2. **`killProcess()` は `::kill(childPID, SIGKILL)` のみ**（`:1222`）。子を `setpgid` していないので子はアプリと同じプロセスグループに入り、`killpg` による一括終了は自分を巻き込む。**yt-dlp が spawn した ffmpeg が確実に孤児化する**
3. **デストラクタが子を待たない／killしない**（`:1165-1172`。ヘッダにも "deleting this object won't terminate the child process" と明記）

よって**プロセス実行は自前で書く**（macOS限定プロジェクトなので POSIX API を直接使ってよい）:

- `posix_spawn` ＋ `POSIX_SPAWN_SETPGROUP` ＋ `posix_spawnattr_setpgroup(&attr, 0)` で**子を新しいプロセスグループのリーダーにする**（pgid == 子のpid）
- **`posix_spawn` は PATH 検索をしない**（するのは `posix_spawnp`）。実行ファイルは常に絶対パスで渡す
- pipe を `O_NONBLOCK` にし、`poll()` にタイムアウト（200ms程度）を与えて読む。タイムアウトのたびにキャンセル要求を確認する
- キャンセル時は `killpg(pgid, SIGTERM)` → 猶予後 `killpg(pgid, SIGKILL)`。**ffmpeg を含むプロセスツリー全体が確実に死ぬ**
- `waitpid` で回収してゾンビを残さない
- 副次効果として **stdout と stderr を別pipeで取れる**（JUCE版は両方を同一pipeにマージするため分離できない）

### コマンド組み立ての安全策

入力URLは「サイトを制限しない」方針だが、**yt-dlp のオプションとして解釈されることは防ぐ**。`--exec` のように外部コマンドを実行するオプションが実在するため。

- **argv配列で渡す**（`posix_spawn` に文字列配列を渡す。シェルを経由しない・文字列コマンドを組み立てない）
- **URLの直前に `--`（オプション終端）を置く**
- **入力は `http://` / `https://` で始まるものだけ受け付ける**。サイト（host）は制限しない ＝「URLの範囲を制限しない」という決定と矛盾しない。スキームを絞るのは入力ミスの早期検出も兼ねる
- **`--ignore-config`** を必ず付ける。ユーザーの `~/.config/yt-dlp/config` にある出力先・proxy・`--exec` 等がアプリの動作に混入するのを防ぐ（セキュリティというより**動作の再現性**のため）
- **`--no-exec`** も付ける（多層防御。Phase 1 でオプションの実在を確認する）

### メタ取得と長さ判定の前提

- **メタ取得は1行JSONで受ける**。yt-dlp の output template のフィールド選択＋JSON変換を使う:

  ```
  --print "LALA_META:%(.{title,duration,is_live})j"
  ```

  `j` はJSON変換、`#j` はpretty-print（複数行になる）なので**付けない**。固定prefixの平文フィールドを並べる方式だと、タイトルに改行や `LALA_DUR …` のような文字列が入ったときに行の境界が壊れる。hostを制限しない仕様なので「YouTubeで数件試して改行が無かった」は保証にならない。**JSONならタイトル内の改行・引用符はエスケープされ、必ず1行に収まる**。`-J`（全メタのJSON dump）は数百KBになるので使わない
- **duration が null / 非有限 / ライブ配信（`is_live` が true）の場合は DL前に拒否する**
- **長さ判定は 48kHz 換算で保守的に行う**: `duration秒 × 48000 > maxInputFrames`（≒ 2083秒 ≒ 34.7分）なら拒否。中間WAVの実SRは元ソース依存（YouTubeの opus は48k、AAC は44.1k）で事前に確定できないが、48k換算なら44.1kソースに対して安全側に働く。48kより高いSRのソースは既存 `AudioImporter` 側の判定に委ねる
- **プロジェクトのサンプルレート確定（`ensureProjectSampleRate()`）は「URL確定後・Downloader開始直前」で呼ぶ**。`startUrlImportFlow()` の冒頭で呼ぶと、**オーバーレイを開いてキャンセルしただけでSRが確定して dirty になる**。既存のファイル取り込みも「ファイルを選んでから」SRを確定する（`MainComponent.cpp:1939-1953`）ので、そちらに揃える

### 長時間処理の既存パターン（そのまま踏襲する）

`BounceRenderer` / `AudioImporter` が同型のAPIを持つ:
`start(Request&&)` / `cancel()` / `cancelAndWait()` / `status()` / `progress()`（atomic）/ `takeResult()`、`enum class Status { idle, running, success, cancelled, failed }`。
完了通知は push ではなく **pull型**（`MainComponent::timerCallback()` の30Hzポーリングで `pollXxx()`）。
進捗UIは `BounceOverlay`（自前描画・`setLabels()` で文言差し替え・キャンセルボタン）。
`ThreadWithProgressWindow` と `AlertWindow` はこのプロジェクトでは一切使っていない。

### /dig での決定事項

| 論点 | 決定 | 理由 |
|---|---|---|
| ダウンロード手段 | **yt-dlp をHomebrew前提で外部呼び出し** | 自前HTTP実装はYouTube側の署名難読化に追随不可。バンドルは30MB超＋codesign/notarize＋`-U`が署名を壊すので見送り。Brewfile管理方針とも整合 |
| URLの範囲 | **サイトは制限しない**（スキームは `http(s)` のみ）。メニュー名も「URLから読み込む…」 | host のバリデーションを書かない分コードが減り、SoundCloud等もそのまま落とせる。非対応サイトは yt-dlp がエラーを返す |
| メニュー位置 | File内「オーディオを読み込む…」の**直下** | やることが同じ「外から音を持ってくる」なので隣接が自然 |
| ショートカット | **⌥⌘I** | 既存の ⇧⌘I（オーディオを読み込む）と I を共有し修飾キーだけ変える。⌥⌘ は A/S で既に使っている帯 |
| 長さ上限 | **既存の34分上限を流用＋DL前に長さで弾く** | メモリ常駐設計のため上限を上げると重い。数十MBのDLを無駄にしない |
| 中間フォーマット | **`-x --audio-format wav`** | 劣化なし。opus/webmしか無い動画でも通る。一時ファイルは34分で約390MBだが即削除 |
| 表示名 | **動画タイトルをそのまま** | 既存の取り込みがファイル名を使うのと同じ振る舞い。整形ルールは永久にメンテが要るので持たない |
| DL失敗時 | **何もせず理由を見せる**（cookie対応は入れない） | 家庭回線からの通常の楽曲ならほぼ通る。起きてもいない問題のために cookie 周りの複雑さを背負わない |
| エラー案内 | 不在時＝Brewfile追加＋`brew bundle`、失敗時＝ERROR行＋`brew upgrade yt-dlp` の一文 | 運用上一番頻出するのは「yt-dlpが古くてYouTube側の変更に追いついていない」 |
| 入力UIの見た目 | **実装前にHTMLモックで確定** | このプロジェクト初の「テキスト入力を受けるオーバーレイ」なので手戻りを防ぐ |
| テスト | **yt-dlp出力のパース・URLマスク・残骸掃除の判定を純関数にして `daw_tests` でカバー** | 既存の `FileSortOrder` / `PreviewPolicy` と同じ位置づけ。ネットに繋がらないで壊れやすい部分を押さえられる |

### 変更するシグネチャ（確定）

既存の第4引数は `bool othersSkipped` なので、**表示名は第5引数として足す**。`beginImportWorker()` は表示名をファイル名で無条件に上書きしているため、そこまで伝播させないとトラック名が一時ファイル名（`audio`）になる。

```cpp
// MainComponent.h
bool startImport (const juce::File& source,
                  int targetTrack,
                  juce::int64 startSample,
                  bool othersSkipped = false,
                  const juce::String& displayName = {});   // 空 = 従来どおりファイル名から

bool beginImportWorker (const juce::File& source,
                        double targetRate,
                        bool othersSkipped,
                        const juce::String& displayName);   // 空 = 従来どおりファイル名から
```

`beginImportWorker()` 内（`MainComponent.cpp:1992`）:

```cpp
importDisplayName = displayName.isNotEmpty() ? displayName
                                             : source.getFileNameWithoutExtension();
```

URL側の呼び出し: `startImport (result.audioFile, -1, 0, false, result.title);`

既存の `beginImportWorker` 呼び出し2箇所（`:1958` の `startImport` 内、`:1983` の `startInstrumentImport` 内）は空文字を渡して従来どおりの挙動にする。

### URLフローの状態機械

`isUrlImporting()` が何を指すか曖昧にしないため、状態を明示的に持つ:

```cpp
enum class UrlStage { idle, enteringUrl, downloading };
UrlStage urlStage = UrlStage::idle;
bool isUrlImporting() const { return urlStage != UrlStage::idle; }
```

- `enteringUrl` — 入力オーバーレイ表示中。**副作用ゼロ**（SRも確定しない・一時ディレクトリも作らない）
- `downloading` — `UrlDownloader` 稼働中。`importOverlay` を「ダウンロード中」で表示
- `isUrlImporting()` は**両方**を含む（この間は bounce / import / 他のURL取り込みを弾き、Fileメニューを disable する）

**`urlDownloader.start()` が false（既に稼働中など）を返したときの rollback** を必ず実装する:
`urlStage = idle` → `importOverlay` を閉じる → `cleanupUrlTempDir()` → `refreshMacMenu()` → `showAlert()`。

### DL完了から取り込みへの引き渡し順序（重要）

`startImport()` のガードに `isUrlImporting()` を含めるため、**`urlStage` を先に `idle` に戻さないと `startImport()` が必ず false を返す**。`pollUrlImport()` の成功パスは以下の順で書く:

1. `urlDownloader.takeResult()` で Result を取得
2. **`urlStage = UrlStage::idle`**（これより前に `startImport` を呼ばない）
3. `urlTempDir = result.tempDirectory`（**一時ディレクトリの所有権を MainComponent へ移す**）
4. `importProgressBase = 0.7f; importProgressSpan = 0.3f;`
5. `startImport (result.audioFile, -1, 0, false, result.title)`
6. **false が返ったら** `importOverlay` を閉じて `cleanupUrlTempDir()` ＋ `refreshMacMenu()`（録音が始まっていた等でガードに掛かるケース）

### 一時ディレクトリの所有

**ディレクトリ名に自分のPIDを埋める**: `$TMPDIR/lala-url-<pid>-<UUID>/`

- dev版とRelease版は並走できるため、起動時に `lala-url-*` を無条件削除すると**別インスタンスがDL中の作業ディレクトリを消す**
- 版で名前空間を分ける（`-dev-` / `-release-`）だけでは同一版の複数起動をカバーできないので、**PIDで所有者を表す**
- **起動時の掃除は「名前から取り出したPIDが `kill(pid, 0)` で存在しないもの」だけ削除する**。生きているPIDのディレクトリは他インスタンスのものとして残す
- PIDが再利用されていた場合は「消し損ねる」だけで、他インスタンスの作業を壊さない（安全側）。残骸は次の起動で再判定される

**所有権の境界を `UrlDownloader::Result` で表す**:

```cpp
struct Result {
    juce::File   tempDirectory;   // 成功時のみ非空。takeされた時点で所有権は呼び出し側へ移る
    juce::File   audioFile;       // tempDirectory 内のwav
    juce::String title;
    juce::String errorMessage;    // 失敗時のみ（URLマスク済み）
    bool         ytDlpMissing = false;  // 不在は専用文言を出すため区別する
};
```

- **worker は成功Resultが take されるまで `tempDirectory` を所有する**
- **成功時だけ MainComponent へ移管**（`urlTempDir` に代入）
- **失敗・キャンセル時は worker が削除し、`tempDirectory` を空にして返す**（→ MainComponent 側は二重削除しない）

所有者は**「MainComponent の `urlTempDir`」と「未take の Result」の2箇所**になる。片方だけ掃除すると漏れるので、両方を畳む:

- `cleanupUrlTempDir()`（`urlTempDir` が非空なら再帰削除してクリア）を**以下すべてから呼ぶ**
  - `pollUrlImport()` の 失敗 / キャンセル（workerが消した後なら空なので no-op）
  - **`startImport()` が false を返したとき**（録音中・デバイス未準備・worker開始失敗）
  - `pollImport()` の 失敗 / キャンセル（デコード失敗を含む）
  - `finishImport()` の **成功・失敗の両方**（rename失敗・readback失敗も含めて末尾で必ず）
  - `cancelImportForClose()` / `cancelUrlImportForClose()`
- **未take の Result** は `cancelUrlImportForClose()` と `UrlDownloader` のデストラクタで畳む（次項）

### DL成功直後に閉じられたときの取りこぼし

`UrlDownloader` が success になってから `pollUrlImport()`（30Hz）が拾うまでの隙間にウィンドウを閉じると、**Result はまだ take されておらず `urlTempDir` は空**なので、`cleanupUrlTempDir()` だけでは一時ディレクトリが残る（次回起動の掃除まで残留する）。

`cancelUrlImportForClose()` は以下の順で書く:

1. `urlStage == downloading` なら `urlDownloader.cancelAndWait()`
2. **`takeResult()` して `result.tempDirectory` が非空なら直接削除**（success で終わっていたケースを回収する。cancelled/failed なら worker が消して空を返すので no-op）
3. `cleanupUrlTempDir()`（`urlTempDir` 側）
4. `urlStage = idle`、`importOverlay` を閉じる

保険として **`UrlDownloader` のデストラクタでも、未take の成功 Result が持つ `tempDirectory` を削除する**。

### ログに残すもの・残さないもの

- 残す: argv の**オプション部分**・終了コード・**PGID**（キャンセル検証で使う）・所要時間
- **URLは scheme + host + パスの先頭のみに切り詰める**。query / fragment に署名トークンが入るサイトがあるため
- **yt-dlp の stdout/stderr 本文も、ログ／アラートに渡す直前に同じマスクを通す**。エラーメッセージが元URLをそのまま含むことがある（例: `Unsupported URL: https://…`）ため、argv だけ伏せても漏れる。`showAlert()` は内部で `Log::error` を呼ぶ（`MainComponent.cpp:16-21`）ので、アラート文言も同じ扱いになる
- マスクは `redactUrls(text)` という純関数に切り出してテストする

## 実装計画

### 事前準備 [人間👨‍💻]
- [x] `~/Library/CloudStorage/Dropbox/Brewfile` に `brew 'yt-dlp'` を追加（brewセクションのアルファベット順）し、`brew bundle --file=~/Library/CloudStorage/Dropbox/Brewfile` を実行する
  - ※ グローバルルールにより `brew install` の直接実行は禁止。Brewfile経由で入れる
  - ※ 結果: AI側で Brewfile 追記と `brew bundle` の実行まで完了（`brew bundle` はグローバルルールの手順2そのものなので代行した）。yt-dlp 2026.07.04 が `/opt/homebrew/bin/yt-dlp` に入った

### Phase 1: yt-dlp の実挙動を確認して仕様を確定する [AI🤖]
- [x] `yt-dlp --version` で導入を確認
- [x] **`--no-exec` オプションの実在を確認**（`yt-dlp --help | grep -- --no-exec`）
- [x] **`--print "LALA_META:%(.{title,duration,is_live})j"` が意図どおり1行JSONを出すか確認**
- [x] `--progress-template` の書式を実出力で確定する
- [x] `-x --audio-format wav` の出力ファイル名の実際と、`--ffmpeg-location` を渡したときの動作を確認
- [x] **`--max-downloads 1` 到達時の exit code を確認**（101 を返す仕様なので、正常終了として扱う分岐が要る）
- [x] エラー時（存在しないID・非公開動画・非対応URL）の出力書式を確認し、ユーザーに見せる行の抽出条件を決める。**エラー本文にURLが含まれるケースも採取する**
- [x] 確認結果を本ファイルの「ログ > 試したこと・わかったこと」に記録する

### Phase 2: 純関数を切り出しテストを書く [AI🤖]
- [x] `Source/shared/YtDlpOutput.h`（GUI非依存・ヘッダオンリー）を作る
  - `parseProgress(line) -> std::optional<float>`（0.0〜1.0）
  - `parseMetadata(text) -> { title, durationSeconds, isLive, ok }` — `LALA_META:` の行を探し、以降を `juce::JSON::parse` で読む
  - `extractErrorLine(text) -> juce::String`（`ERROR:` 行を拾い、無ければ末尾の非空行）
  - `redactUrls(text) -> juce::String`（テキスト中のURLを scheme+host+パス先頭に切り詰める）
- [x] `Source/shared/TempDirSweep.h`（GUI非依存・ヘッダオンリー）を作る
  - `selectStaleTempDirs(names, isPidAlive) -> StringArray` — 名前一覧とPID生存判定を受け取り、**削除してよい名前だけ**返す純関数。実削除とプロセス生存確認（`kill(pid,0)`）は呼び出し側
- [x] `Tests/TestsMain.cpp` にテストを追加（**Phase 1 で採取した実出力をそのままケースに使う**）
  - 進捗: 正常行 / total不明の行 / 進捗以外の行 / 空行 / 分母0
  - メタ: 正常 / `duration` が null / `is_live` が true / **タイトルに改行・引用符・`LALA_META:` という文字列そのものが含まれるケース** / prefix行が存在しない / JSONが壊れている / 警告行が前後に混ざった出力
  - エラー: 複数行のERROR出力から1行を選ぶ / ERROR行が無いケース
  - マスク: query/fragment付きURLを含む本文 / URLを複数含む本文 / URLを含まない本文（不変であること）
  - 掃除判定: **現PIDのディレクトリは残る** / **生存中PIDは残る** / **存在しないPIDだけ削除対象になる** / **不正な名前（`lala-url-abc-…`・`lala-url-`・無関係な名前）は触らない**
- [x] `Tests/TestsMain.cpp` はヘッダオンリー対象のため CMake 変更不要。`cmake --build build --target daw_tests` してテストを実行し、全パスを確認

### Phase 3: プロセス実行の土台を作る [AI🤖]
- [x] `Source/shared/SpawnedProcess.h/.cpp` を作る（macOS前提のPOSIX直叩き。`juce::ChildProcess` は使わない）
  - `posix_spawn` ＋ `POSIX_SPAWN_SETPGROUP` ＋ `posix_spawnattr_setpgroup(&attr, 0)` で**子を新プロセスグループのリーダーにする**
  - **実行ファイルは絶対パス必須**（`posix_spawn` はPATH検索しない）
  - argv は `StringArray` 相当を受け取り配列で渡す（**シェルを経由しない・文字列コマンドを組み立てない**）
  - stdout / stderr を**別pipe**で受け、両方を `O_NONBLOCK` にして `poll()` で監視
  - `poll()` に 200ms のタイムアウトを与え、戻るたびに呼び出し側のキャンセル要求（`std::atomic<bool>`）を確認する
  - 読めた行を `onStdoutLine` / `onStderrLine` コールバックで返す（行バッファリングは内部で行う）
  - **`pgid()` を公開する**（キャンセルの検証で外から使うためログにも出す）
  - `terminate()`: `killpg(pgid, SIGTERM)` → 猶予（1秒程度）→ 生きていれば `killpg(pgid, SIGKILL)`
  - 終了時は必ず `waitpid` で回収し、exit code を返す。**デストラクタで確実に terminate ＋ 回収する**
- [x] **`CMakeLists.txt` の `target_sources(daw …)` と `target_sources(daw_tests …)` の両方に `Source/shared/SpawnedProcess.cpp` を追加する**（`shared/` セクションのアルファベット順）
- [x] `Tests/TestsMain.cpp` に単体検証を追加する（GUI不要・実プロセスを起動する。**すべて絶対パスで指定**）
  - `/bin/echo` で stdout が行単位に取れること
  - `/bin/sh -c 'echo out; echo err >&2'` で stdout/stderr が**分離して**取れること
  - `/bin/sh -c 'sleep 30 & sleep 30; wait'` のような子を持つプロセスを `terminate()` し、**PGIDに属するプロセスが0件になる**こと
  - 出力を一切出さないプロセス（`/bin/sleep 60`）に対して `terminate()` が**即座に効く**こと（＝ブロッキング読みで固まらないこと）
- [x] `cmake --build build --target daw_tests` で全パスを確認

### Phase 4: UrlDownloader（ワーカー）を実装する [AI🤖]
- [x] `Source/audio/UrlDownloader.h/.cpp` を作る。`AudioImporter` と同型のAPI（`private juce::Thread` + `Request`/`Result`/`Status` + atomic progress + `start`/`cancel`/`cancelAndWait`/`status`/`progress`/`takeResult`）
  - **`Result` は「一時ディレクトリの所有」節の定義どおり**（`tempDirectory` / `audioFile` / `title` / `errorMessage` / `ytDlpMissing`）
  - **所有権境界**: 成功Resultがtakeされるまで worker が `tempDirectory` を所有。失敗・キャンセル時は worker が削除して `tempDirectory` を空にして返す
  - **デストラクタ**: `cancelAndWait()` に加えて、**未take の成功Resultが持つ `tempDirectory` を削除する**（DL成功直後にアプリが終了したケースの保険）
  - ※ ネットワークI/Oでオーディオスレッドには一切触れないが、既存ワーカーとの対称性を優先して `Source/audio/` に置く
- [x] **`CMakeLists.txt` の `target_sources(daw …)` に `Source/audio/UrlDownloader.cpp` を追加**（`audio/` セクションのアルファベット順。`daw_tests` からは参照しないので追加しない）
- [x] yt-dlp の探索: `/opt/homebrew/bin/yt-dlp` → `/usr/local/bin/yt-dlp` の順に `existsAsFile()`。見つからなければ `ytDlpMissing = true` で `failed`
- [x] URLの受理チェック: `http://` / `https://` で始まらなければ即 `failed`（host は見ない）
- [x] 一時ディレクトリを `$TMPDIR/lala-url-<pid>-<UUID>/` で作る
- [x] ①メタ取得フェーズ: `SpawnedProcess` で起動 → `parseMetadata()`
  - duration が null / 非有限 / `is_live` → 「長さが取得できない動画は取り込めません」
  - `duration × 48000 > AudioImporter::maxInputFrames` → 「この動画は長すぎます（約34分まで）」
- [x] ②DLフェーズ: `parseProgress()` の結果を `progress` に書く（0.0〜1.0。写像は呼び出し側）
  - コマンドは**メタ取得と同じ選択条件**（`--no-playlist --playlist-items 1`）に `--max-downloads 1` を加える
  - exit code は **0 と 101（max-downloads到達）を成功**として扱う
  - 出力ファイルは一時ディレクトリ内を走査して `.wav` を1つ拾う（固定名を仮定しない）。0個ならエラー、複数なら最初の1つを使い残りは削除
- [x] キャンセル: `threadShouldExit()` を `SpawnedProcess` のキャンセルフラグに接続し、`terminate()` でプロセスグループごと終了
- [x] `Log::info` / `Log::error` を要所に入れる。**ログに渡すテキスト（argv・stdout・stderr・errorMessage）はすべて `redactUrls()` を通す**。PGID・終了コード・所要時間は残す

### Phase 5前の準備: 入力UIの見た目を決める [人間👨‍💻]
- [x] AIがscratchpadに作る単一HTMLモック（入力欄の幅・ボタン配置・プレースホルダー文言・進捗中への切り替わり方を数案）をブラウザで見て、方向を確定する
  - ※ AI側はモック作成と絶対パスの提示まで行う

### Phase 5: URL入力オーバーレイを実装する [AI🤖]
- [x] scratchpadに単一HTMLモックを作り `open` する。**返答に絶対パスを明記する**
- [x] 確定した案で `Source/ui/UrlImportOverlay.h` を実装する（ヘッダオンリーにできればCMake変更不要。`BounceOverlay` の半透明背景＋角丸パネルの質感を踏襲。色は `Theme.h`、フォントは `Fonts.h` から取る）
  - `juce::TextEditor` ＋ [取り込み][キャンセル]
  - `show()` 時にクリップボードを読み、`http://` / `https://` で始まっていれば自動プリフィルして全選択状態にする
  - Return で確定 / Esc でキャンセル / **`http(s)://` で始まらない入力では確定ボタンを無効**（空文字だけでなくスキームを見る）
  - `onSubmit(juce::String url)` / `onCancel()` コールバック
  - **開いてキャンセルするだけでは一切の副作用を起こさない**（SR確定もしない・一時ディレクトリも作らない）
- [x] `.cpp` に分けた場合は `CMakeLists.txt` の `target_sources(daw …)` に追加する

### Phase 6: MainComponent に組み込む [AI🤖]
- [x] **`startImport` / `beginImportWorker` を「変更するシグネチャ（確定）」のとおりに変更する**（bool化＋`displayName` 第5引数／第4引数）
  - `beginImportWorker()` 内の `importDisplayName` 代入（`MainComponent.cpp:1992`）を「空ならファイル名・非空なら渡された値」に変える
  - 既存の `beginImportWorker` 呼び出し2箇所（`:1958`, `:1983`）は空文字を渡す
  - false を返す条件: `importActive` / `bounceActive` / **`engine.isRecording()`**（既存3ガード・`:1935` を維持）/ **`isUrlImporting()`**（新規）/ `ensureProjectSampleRate()` が false / worker 開始失敗
  - 既存の3箇所の `startImport` 呼び出し（`MainComponent.cpp:134, 296, 1928`）は戻り値を無視してよい（挙動は不変）
- [x] SR確定処理を `ensureProjectSampleRate() -> bool` としてヘルパーに切り出し、`startImport()` の該当箇所（`MainComponent.cpp:1939-1953`）をそれに置き換える
- [x] `MainComponent` にメンバ追加: `UrlImportOverlay urlOverlay` / `UrlDownloader urlDownloader` / `juce::File urlTempDir` / `UrlStage urlStage`
- [x] **進捗写像**: `importProgressBase` / `importProgressSpan` をメンバに持ち、`pollImport()` の `setProgress` を `base + span × progress()` に変える。通常フローは `0.0 / 1.0`、URLフローは `0.7 / 0.3`。**`importOverlay` 1枚を DL開始から使い回す**（`urlProgressOverlay` は作らない・`AudioImporter` にも触らない）
- [x] `startUrlImportFlow()` を public に生やす
  - **`importActive` / `bounceActive` / `engine.isRecording()` / `isUrlImporting()` のいずれかなら入力オーバーレイ自体を開かない**（録音中にDLだけ進んで引き渡しで失敗するのを防ぐ）
  - **冒頭ではSRを確定しない**。`urlStage = enteringUrl` にしてオーバーレイを出すだけ
  - `onCancel` で `urlStage = idle` ＋ `refreshMacMenu()`（他に副作用なし）
  - `onSubmit` で `ensureProjectSampleRate()` → false ならエラー＋`urlStage = idle` で終了（DLしない）→ true なら `urlStage = downloading`・`importOverlay` を「ダウンロード中」で出し `urlDownloader.start()`
  - **`start()` が false なら rollback**（`urlStage = idle` → オーバーレイを閉じる → `cleanupUrlTempDir()` → `refreshMacMenu()` → `showAlert()`）
- [x] `timerCallback()` の末尾に `pollUrlImport()` を追加。成功パスは**「DL完了から取り込みへの引き渡し順序」の1〜6をその順で**実装する（`urlStage = idle` を `startImport()` より前に置く）
- [x] **`cleanupUrlTempDir()` を実装し、「一時ディレクトリの所有」に列挙した全経路から呼ぶ**
- [x] **`cancelUrlImportForClose()` を「DL成功直後に閉じられたときの取りこぼし」の1〜4のとおりに実装**し、`Main.cpp` のウィンドウクローズ経路（`Main.cpp:349` 付近）から呼ぶ
- [x] アプリ起動時に `$TMPDIR/lala-url-*` を走査し、`selectStaleTempDirs()`（Phase 2）に `kill(pid,0)` ベースの生存判定を渡して、**返ってきた名前だけ**削除する
- [x] 相互排他: `isBouncing()` / `isImporting()` の輪に `isUrlImporting()` を追加し、`startBounceFlow` / `startImport` / `startUrlImportFlow` の3者が互いをチェックする
- [x] `keyPressed` 冒頭のモーダルガードに `enteringUrl` / `downloading` を追加（Escでキャンセル・他キーは消費）
- [x] `refreshMacMenu()` を状態遷移のたびに呼んでFileメニューを disable/enable する
- [x] エラー表示を既存 `showAlert()` で出す（**渡す文字列は `redactUrls()` 済み**）
  - yt-dlp不在（`result.ytDlpMissing`）: 「yt-dlp が見つかりません」/「URLからの取り込みには yt-dlp が必要です。Brewfile に `brew 'yt-dlp'` を追加して `brew bundle` を実行してください。」
  - DL失敗: 「取り込めません」/ `extractErrorLine()` の結果 ＋ 改行 ＋「yt-dlp が古い場合は `brew upgrade yt-dlp` を試してください。」（全文は `~/Library/Logs/daw/` へ）

### Phase 7: メニューとショートカットを追加する [AI🤖]
- [x] `Source/ui/Shortcuts.h` の `enum class ID` に `importUrl` を追加し、テーブルに1行足す
  - `Category::project`・name `u8"URLから読み込む"`・keyLabel `u8"⌥⌘I"`・matcher・**第6要素 `menuKey`（メニュー掲載項目なので必須）**
  - 配置は `ID::importAudio` の直後（テーブル順＝⌘?一覧の表示順）
- [x] `Main.cpp:22` の `MenuCommands::enum` に `importUrl` を追加、`items[]` に `{ MenuCommands::importUrl, Shortcuts::ID::importUrl }` を追加
- [x] `getMenuForIndex` で `importAudio` の直後に `addCommandItem`
- [x] `getCommandInfo` に分岐を追加（`Shortcuts::name(ID) + "…"` から表示名を生成。文字列の手書き禁止）
- [x] `perform` に分岐を追加し `mainComp->startUrlImportFlow()` を呼ぶ。`ready` 判定に `isUrlImporting()` を含める
- [x] `cmake --build build` でビルドを通す（**新規`.cpp`のCMake追加漏れがないかここで最終確認**）

### Phase 8: 自己検証 [AI🤖]

**検証はすべて dev版（`LaLa-dev.app`）で行う。Release版（常用版）は起動しない** — 常用版が起動中なら既存プロセスが前面化されるだけで検証にならず、未起動なら本番bundle idのアプリをユーザーの環境に立ち上げてしまうため。並走時の掃除挙動は Phase 2 の `selectStaleTempDirs()` 単体テストでカバー済み。

- [x] `cmake --build build --target daw_tests` → 全テストパスを確認
- [x] **起動時の残骸掃除**（実配線）: `~/Library/Caches/LaLa-dev/` に「生きているPID」「死んだPID」「PIDが数字でない名前」「`lala-url-` だけの名前」の4つを置いて起動 → 死んだPIDのものだけ消え、他3つは残る（ログ `url.tempdir.swept count=1`）
- [x] **URL→WAVの通し確認**（GUIを介さず `UrlDownloader` 単体）: `LALA_VERIFY_URL=… ./daw_tests` で実行し、成功・タイトル取得・WAV生成（2ch/48000Hz/19.0055s）・所有権の受け渡し・進捗・デコード可否を確認
- [x] **異常系**（同上のテスト内）: `--exec…` を渡しても failed かつ `/tmp/pwned` が作られない ／ 存在しない動画で failed＋理由あり ／ DL中にキャンセルして cancelled・一時ディレクトリ削除・yt-dlp/ffmpeg のプロセスが残らない
- [x] **長さ上限の閾値**を単体テストで固定（約2083秒。5秒は通り1時間は弾く）
- [x] ログに渡す文字列が全経路 `redactUrls()` を通っていることをコードで確認（`UrlDownloader.cpp:120,135,150,151,196,219,220`）
- [x] dev版を `open -g` で起動して正常動作を確認（`screencapture -x -l` で選択画面を撮影）
- [ ] ~~AXPressでメニュー項目の存在と表示名を確認 → メニューをAXPressしてオーバーレイ表示を撮影~~
- [ ] ~~オーバーレイを開いてキャンセルするだけでは project.json が変わらないこと~~
- [ ] ~~アプリ経由での一気通貫（`import.done` ログ・project.json のトラック名・`clip-NNN.wav`）~~
- [ ] ~~録音中にメニューを叩いても開かないこと／DL中にウィンドウを閉じても固まらないこと／取り込みフェーズでのキャンセル~~
  - **上記4件は実施できず**: 検証中ユーザーが別Spaceで作業しており、dev版ウィンドウが `onscreen=false` で System Events から見えないためAX操作が一切効かない（`screencapture -x -l` での撮影のみ可能）。フォーカスを奪う `open`（-gなし）はユーザーの作業を中断するので撃たなかった。ロジック層は上記の通り実URLまで通して検証済みなので、**GUI配線の確認は「動作確認 [人間]」に委ねる**
- [ ] ~~34分超の動画URLで弾かれること／ライブ配信URLで弾かれること~~
  - **実施できず**: 手元で確実な長時間動画・ライブ配信のURLを確保できなかった（試した2件はいずれも yt-dlp 側で `not available` / `processing`）。閾値の計算は単体テストで固定し、`LALA_VERIFY_LONG_URL` を渡せば後からいつでも回せるようにしてある
- [x] 結果（実行したコマンド・実際の出力・判定根拠）を報告する

### 動作確認 [人間👨‍💻]
- [ ] ⌥⌘I でオーバーレイが開くこと（合成キーが届かないためここだけ実操作が必要）
- [ ] ブラウザでYouTube URLをコピーした状態でメニューを開き、**入力欄に自動でプリフィルされている**こと
- [ ] Return で確定・Esc でキャンセルできること
- [ ] **取り込まれたトラック名・リージョン名が動画タイトルになっている**こと（`audio` になっていたら displayName の伝播漏れ）
- [ ] 進捗オーバーレイの見た目に違和感がないこと（DL→取り込みの切り替わりで**バーが戻らず**ちらつかないか）
- [ ] 取り込まれた音声が実際に再生でき、音質・長さが元動画と一致すること
- [ ] （気が向いたら）34分を超える動画・ライブ配信のURLでそれぞれ専用の文言が出ること

## ログ

### 試したこと・わかったこと

#### 2026-07-26 Phase 1: yt-dlp の実挙動（yt-dlp 2026.07.04 / `/opt/homebrew/bin/yt-dlp`）

- **オプションは全部実在**: `--ignore-config` / `--no-exec` / `--max-downloads` / `--newline` / `--progress-template` / `--no-playlist` / `-I, --playlist-items`
- **メタ取得は計画どおり1行JSONで出る**（stdout）:
  `LALA_META:{"title": "Me at the zoo", "duration": 19, "is_live": false}`
- **`--max-downloads 1` の exit code は 101**（想定どおり。成功として扱う）
- **進捗テンプレートは `total_bytes` が入り `total_bytes_estimate` は `NA`** だった:
  `LALA_PROGRESS 1024 252182 NA` → **両方読んで有効な方を分母にする**実装が要る
- **正常時は stderr が空**。進捗もログ行も全部 stdout に出る
- **エラーは stderr に `ERROR: …` で出て exit 1**:
  - `ERROR: [youtube] AAAAAAAAAAA: Video unavailable`
  - `ERROR: [generic] foo?token=SECRET123#frag: Unable to download webpage: HTTP Error 404 …`
- **URL漏れが2種類あることを実測で確認**:
  1. stdout の `[youtube] Extracting URL: https://www.youtube.com/watch?v=…` — scheme付きなので `redactUrls()` でマッチできる
  2. stderr の `[generic] foo?token=SECRET123#frag:` — **scheme も host も落ちた形でクエリだけ残る**。パターンマッチでは拾えない
  → `redactUrls()` は「①テキスト中の `https?://…` を scheme+host+パス先頭に切り詰める」に加えて、**「②渡した元URLの query/fragment 文字列が本文に含まれていたら伏せる」の2段構え**にする（自分が渡したURLは既知なので確実に消せる）
- **出力は `-o '<dir>/audio.%(ext)s'` で `audio.wav` になる**（中間の `audio.webm` は yt-dlp が自動削除）
- **生成WAVは 2ch / 48000Hz / Int16**（19秒で3.6MB）。YouTubeのopus由来は48kなので、**長さ判定の48kHz換算が実態と一致**している

#### 2026-07-26 planレビュー1回目の反映（実装前）
- `juce::ChildProcess` は本用途に使えないことをJUCEソースで確認（`juce_SharedCode_posix.h`）。①`read()`が`fread()`でブロッキング（`:1193`）②`kill()`が`::kill(childPID)`のみでプロセスグループ非対応・子は`setpgid`されない（`:1222`）③デストラクタが子を待たない（`:1165`）。→ `posix_spawn` + `POSIX_SPAWN_SETPGROUP` + `poll()` の自前実装（Phase 3）に変更
- 副次効果として stdout/stderr を別pipeで取れる（JUCE版は同一pipeにマージされ分離不可）
- レビュー指摘のうち **`brew install` → Brewfile案内 は反映、`brew upgrade` はそのまま**。グローバルルールの禁止対象は `brew install` / `brew uninstall` のみで `upgrade` は含まれないため

#### 2026-07-26 planレビュー2回目の反映（実装前）
- `startImport` / `beginImportWorker` は現状 `void`（`MainComponent.h:136,141`）→ Phase 6 で `bool` 化を明記
- `CMakeLists.txt` はソース明示列挙（`:86-107` / `:214-224`）→ 新規`.cpp`の追加を各Phase内に明記（Phase 7 まで気づかない構成だった）
- メタ取得を固定prefixの平文フィールドから **1行JSON（`--print "LALA_META:%(.{title,duration,is_live})j"`）** に変更。hostを制限しない以上、タイトル内の改行やprefix文字列の混入は「YouTubeで数件試して大丈夫だった」では保証できないため
- `ensureProjectSampleRate()` の呼び出しを `startUrlImportFlow()` 冒頭から **`onSubmit` 内（URL確定後・DL開始直前）** へ移動。冒頭で呼ぶとオーバーレイを開いてキャンセルしただけで dirty になる
- 一時ディレクトリ名に **PID を埋める**（`lala-url-<pid>-<UUID>`）。dev/Release が並走できるため、起動時の `lala-url-*` 一括削除は他インスタンスの作業を壊す
- キャンセル検証を `pgrep -f yt-dlp` から **PGIDベース**へ変更（ユーザーが別用途でffmpegを動かしていると誤判定するため）
- ログのURLは **scheme+host+パス先頭まで**に切り詰める（query/fragment に署名トークンが入るサイトがあるため）

#### 2026-07-26 planレビュー3回目の反映（実装前）
- **`startImport()` のガードに `isUrlImporting()` を入れると、`pollUrlImport()` からの引き渡しが常に false になる**（自分の状態で自分を弾く）。→「DL完了から取り込みへの引き渡し順序」を1〜6の手順として明記し、`urlStage = idle` を `startImport()` より前に置くことを固定
- `isUrlImporting()` の意味が曖昧だったので **`UrlStage { idle, enteringUrl, downloading }`** を定義。`urlDownloader.start()` 失敗時の rollback（状態・オーバーレイ・一時ディレクトリ・メニュー・アラート）も明記
- 既存ガードの **`engine.isRecording()`**（`MainComponent.cpp:1911, 1935`）が bool化の一覧から抜けていた → 維持を明記し、`startUrlImportFlow()` も録音中は**入力オーバーレイ自体を開かない**ことにした
- `UrlDownloader::Result` の中身と**所有権の境界**（成功takeまでworkerが所有／成功時のみ移管／失敗・キャンセルはworkerが削除して空を返す）を明記
- **Phase 8 の dev/Release 並走GUIテストを削除**。常用版を起動するとユーザー環境に干渉し、かつ既存プロセス前面化で検証にならない。代わりに `selectStaleTempDirs()` を純関数化して Phase 2 で単体テストする
- URLマスクは argv だけでなく **yt-dlp の stdout/stderr 本文にも適用**（`Unsupported URL: https://…` のようにエラー本文が元URLを含む）
- `posix_spawn` は PATH 検索しないため、Phase 3 のテストは `/bin/sh` `/bin/echo` `/bin/sleep` と**絶対パスで指定**する

#### 2026-07-26 planレビュー4回目の反映（実装前）
- **`startImport()` の第4引数は既に `bool othersSkipped`**（`MainComponent.h:136-137`）。表示名を第4引数に渡す擬似コードになっていたので、**`displayName` は第5引数**に確定（「変更するシグネチャ（確定）」節を新設）
- **`beginImportWorker()` が `importDisplayName = source.getFileNameWithoutExtension();` で無条件に上書きする**（`MainComponent.cpp:1992`）。ここまで `displayName` を伝播しないとトラック名が一時ファイル名（`audio`）になる → `beginImportWorker` にも第4引数を足し、「空ならファイル名・非空なら渡された値」に変更。既存呼び出し2箇所（`:1958`, `:1983`）は空文字を渡す
- **DL成功〜`pollUrlImport()` の隙間に閉じると、未take Result が持つ一時ディレクトリが誰にも消されない**（`urlTempDir` はまだ空なので `cleanupUrlTempDir()` は no-op）。→「DL成功直後に閉じられたときの取りこぼし」節を新設し、`cancelUrlImportForClose()` で `takeResult()` して直接削除する手順を固定。保険として `UrlDownloader` のデストラクタでも未take Result のディレクトリを削除する

### 方針変更

#### 2026-07-26 一時ディレクトリの場所（`$TMPDIR` ではなかった）
`juce::File::getSpecialLocation(juce::File::tempDirectory)` は macOS では **`~/Library/Caches/<実行ファイル名>/`** を返す（`juce_Files_mac.mm` で確認）。`$TMPDIR` を前提に検証したら掃除が動かず発覚。
実害はなく、むしろ**dev版（`LaLa-dev`）とRelease版（`LaLa`）で自動的に別ディレクトリになる**ので並走時の衝突が構造的に起きない。PID判定はクラッシュ後の残骸判定として引き続き有効。仕様セクションの `$TMPDIR/lala-url-<pid>-<UUID>/` はこの実体を指す。

#### 2026-07-26 起動時の掃除を MainComponent → Main.cpp へ移動
`sweepStaleUrlTempDirs()` を `MainComponent` のコンストラクタに置いたが、**起動直後はプロジェクト選択画面で MainComponent が存在せず呼ばれない**（実測）。`DawApplication::initialise` に移し、`MainComponent` の static public メソッドとして公開した。

#### 2026-07-26 `UrlDownloader.cpp` を daw_tests にも追加
当初「daw_tests からは参照しないので追加しない」としていたが、GUI を介さずワーカー単体で実URLの通し確認をしたくなったため追加。テストは環境変数 `LALA_VERIFY_URL` が指定されたときだけ走るので、通常の `ctest` はネットワークに依存しない。

#### 2026-07-26 実装レビューの反映（4件）
1. **`terminate()` が孫を取り逃していた**（P1）: 「直接の子を回収できたら return」にしていたため、子（yt-dlp）が先に終了して孫（SIGTERMを無視する ffmpeg 等）が残るケースで SIGKILL まで進まなかった。**PGIDを `childPid` とは別に保持**し、子を回収した後も `killpg(pgid, 0)` でグループの消滅を確認しながら SIGTERM → SIGKILL と進むように変更。ゾンビが「居る」と数えられないよう、生存確認の前に必ず `reapChildIfExited()` を挟む。回帰テスト（`/bin/sh -c "(trap '' TERM; sleep 30) & exit 0"`）を追加し、**旧挙動に戻すとそのテストだけが FAIL することも確認済み**
2. **`run()` が status を先に公開していた**（P1）: `currentStatus.store()` の後にログを書いていたため、その隙間に `pollUrlImport()` が `takeResult()` を呼ぶと `jassert(! isThreadRunning())` に掛かる。既存の `AudioImporter` / `BounceRenderer` は「status の更新は run() の最後の1文」とコメント付きで規約化しており、それに揃えた
3. **fragment だけの機密値がマスクされなかった**（P2）: `?` 以降しか見ていなかったので `https://…/foo#SECRET` が素通りしていた。query を先に、残った fragment を後で伏せる2段に変更
4. **liveキャンセルテストが名前でプロセスを数えていた**（P2）: ユーザーが別用途で動かしている ffmpeg を誤検知する。`UrlDownloader::pgid()` を公開して**PGIDベースの判定**に統一（同ファイル内の `SpawnedProcess` テストと同じやり方に揃えた）

#### 2026-07-26 実装レビュー2回目の反映（4件）
1. **完了statusの競合が残っていた**（P1）: `run()` の最終行で `store` しても、**JUCEがスレッドハンドルを閉じるのは `run()` から戻った後**なので、その隙間では `isThreadRunning()` がまだ true。`takeResult()` の冒頭で `waitForThreadToExit(-1)` して確実に見送るようにした（status が terminal なら即座に返る）。`jassert` は不要になったので削除
2. **`startThread()` の戻り値を無視していた**（P2）: JUCE 8 の `startThread()` は `bool` を返す。作成失敗時に status が running のままになり、進捗オーバーレイが永久に閉じなかった。失敗時は `idle` に戻して false を返す
3. **URLの userinfo がログに残った**（P2）: `shortenUrl()` が authority 全体をホストとして残していたため `https://user:password@example.com/…` のパスワードが漏れた。**最後の `@` より前を伏せる**ように変更（ホスト名は「どのサイトか」の判断に要るので残す）
4. **標準テストで Debug assertion が発火していた**（P2）: `juce::String(const char*)` に日本語UTF-8を渡していた（JUCEの `String(const char*)` は Latin-1 想定で、非ASCIIバイトに `jassert` が入っている）。`expect` は `const char*` をそのまま `std::cout` に流すだけなので、`juce::String` を経由せず文字列リテラルを直接渡す形にした
   - **裏取り**: 旧コードは `lldb -b -o run` で `stop reason = EXC_BREAKPOINT` で停止しテストが完走しなかった。修正後は LLDB 下でも `all tests passed` まで到達する

※ 1 と 2 は既存の `AudioImporter` / `BounceRenderer` も同じ構造を持つ（`startThread()` の戻り値を見ておらず、`takeResult()` は `jassert` のみ）。今回のスコープ外なので触っていないが、同種の不具合が起きうる。

#### 2026-07-26 GUI操作を伴う自己検証は実施できず
検証中ユーザーが別Spaceで作業しており、dev版のウィンドウが `onscreen=false`。System Events からウィンドウが見えずAX操作が一切効かなかった（`screencapture -x -l` での撮影のみ可能）。フォーカスを奪う起動はユーザーの作業を中断するので撃たず、GUI配線の確認は「動作確認 [人間]」へ委ねた。詳細は Phase 8 の該当項目に記載。
