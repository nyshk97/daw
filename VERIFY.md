# VERIFY — 動作確認手順

## ビルド

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug   # 初回のみ時間がかかる（JUCEのfetch）
cmake --build build
```

- `Built target daw` 〜 `[100%] Built target daw_tests` まで出ればOK（JUCE内部の警告は無視してよい。自作コード由来の警告はゼロが基準）
- **Debugビルドは dev 版**（アプリ名 `LaLa-dev`・bundle id `local.d0ne1s.daw.dev`・DEVリボン付きアイコン）。常用版は `cmake -B build-release -DCMAKE_BUILD_TYPE=Release` → `LaLa.app`。動作確認は以下すべて dev 版で行う
- 初回 configure 時に `scripts/fetch-sparkle.sh` が自動実行され Sparkle.framework を `.sparkle/` に取得する（ネットワーク必要）

## 起動確認（CLIから可能な範囲）

```sh
open build/daw_artefacts/Debug/LaLa-dev.app
sleep 3 && pgrep -fl "LaLa-dev.app/Contents/MacOS/LaLa-dev"   # プロセス生存確認
```

### dev版の検証フック（起動引数。Main.cpp・実機能と同一経路）

合成クリック・フォーカス奪取なしでUI検証まで通せる（Salvaの `--snapshot` と同じ流儀）:

```sh
open -g build/daw_artefacts/Debug/LaLa-dev.app --args \
  --open ~/Music/daw/<プロジェクト名> --eq-editor --play --snapshot /tmp/check.png
sleep 6 && tail -5 ~/Library/Logs/daw/"$(ls -t ~/Library/Logs/daw | head -1)"  # debug.snapshot ok=1 を確認
```

- `--open <projectDir>`: 選択画面を迂回してプロジェクトを開く（ログ `project.open`）
- `--eq-editor`: FXパネル＋EQ詳細エディタを開く（ログ `fxdetail.open fx=EQ`）
- `--comp-editor`: FXパネル＋Comp詳細エディタを開く（ログ `fxdetail.open fx=Comp`）
- `--comp-demo`: 選択トラックにデモ設定のComp（-30dB/8:1/5ms/80ms・ON）を適用（GR表示・書き出し検証用）
- `--sat-editor`: FXパネル＋Satエディタを開く（ログ `fxdetail.open fx=Sat`。伝達カーブ・倍音バー・Drive/Mixノブ）
- `--lofi-editor`: FXパネル＋Lo-fiエディタを開く（ログ `fxdetail.open fx=Lo-fi`。Toneカーブ・4ノブ）
- `--reverb-editor <0|1>`: FXパネル＋バスReverbエディタを開く（0=A/1=B。ログ `fxdetail.open fx=Reverb`。
  Size/Damp/Width/Pre-delay/Low Cutの5ノブ）
- `--delay-editor`: FXパネル＋バスDelayエディタを開く（ログ `fxdetail.open fx=Delay`。
  Time4値ボタン・Feedback/Toneノブ・Ping-pongトグル）
- FX（Sat/Lo-fi/バスReverb/Delay）の確認用プロジェクトは `scripts/seed-fx-test.sh` で生成（冪等）:
  `~/Music/daw/0-0-fx-test`（90BPM/48k/8小節。Drums=Lo-fi軽め・Keys=Sat 35%＋RevA send・
  Pad=RevB send・Lead=Delay＋RevA send。トラック構成と確認項目の対応はスクリプト冒頭のコメント参照）
- seeder 再実行時の注意: **アプリで対象プロジェクトを開いたままだと反映されない**（開き直しが必要）。
  また旧インスタンスが同プロジェクトを未保存（タイトル●）で開いていると、その保存で seeder の内容が
  旧状態へ巻き戻る。「FXが効いていない」ように見えたら、まず開いているプロジェクトのトラック構成・
  send値が seeder の想定（スクリプト冒頭コメント）と一致するか確認する
- `--play`: 開いた後に再生開始（ログ `transport.play`）。**音を出したくない検証**はトラックの
  `volume: 0.001`（-60dB＝実質無音）にする — EQのアナライザ・CompのGR検波はどちらも
  フェーダー前タップなので表示はフルに出る。Compの表示検証は `~/Music/daw/0-0-comp-test`
  （2Hzバースト音・GRがポンピングして見える）が使える
- `--snapshot <path>`: 表示完了後（2秒後）のUIをPNG保存（createComponentSnapshot。別Spaceでも
  ユーザーのフォーカスも奪わない。ログ `debug.snapshot ok=1` で裏取り）

- 起動するとプロジェクト選択画面が出る（`~/Music/daw/` のフォルダ一覧＋新規作成）
- **2つ目のインスタンスは起動できない**（`moreThanOneInstanceAllowed() == false`）。`open -n` でも既存インスタンスが前面化されるだけなので、新旧バイナリの並行確認はできない（dev版とRelease版はbundle idが別なので同時起動は可能）。「起動したのにログへ `session.start` が増えない」ときはこれ
- **実機の状態はウィンドウタイトルで読める**（`CGWindowListCopyWindowInfo` の name）: `LaLa-dev` だけなら選択画面、`LaLa-dev — <プロジェクト名>` ならメイン画面、**末尾の `●` は未保存**。`●` があるときにquitすると保存ダイアログが出てユーザーの作業を止めるので、起動中インスタンスをquit・再起動する前に ①タイトルの `●` ②アプリログ末尾の操作イベント ③`ioreg -c IOHIDSystem` のHIDIdleTime ④frontmostプロセス、を確認する
- マイク権限のplist文言確認: `plutil -extract NSMicrophoneUsageDescription raw build/daw_artefacts/Debug/LaLa-dev.app/Contents/Info.plist`
- **リビルドしてもマイク権限は再要求されない**（POST_BUILD でバンドルリソースを先行コピーした上で Apple Development 証明書により再署名しているため。`--target daw` 単体ビルドや `--clean-first` でも維持される。bundle id を分けた直後の dev 版初回起動のみ再付与が必要）。ダイアログが出たら署名が壊れている兆候なので以下を確認:
  - `codesign -dvv build/daw_artefacts/Debug/LaLa-dev.app 2>&1 | grep Signature` → `Signature=adhoc` になっていたら、configure 時の証明書自動解決が失敗して ad-hoc フォールバックしている（`cmake -B build` を再実行して `Codesign identity:` の STATUS 行と WARNING の有無を見る）
  - 署名の安定性確認: リビルド前後で `codesign -dr - <app>` の出力が一致すること（証明書更新後もこれで確認する）

### Release版並走での見た目検証（dev版をユーザーが使用中のとき）

検証フック（`--open` / `--snapshot`）は dev 限定なので、選択画面の「Create」で一時プロジェクトを作ってメイン画面へ入る:

```sh
cmake --build build-release --target daw && open -g build-release/daw_artefacts/Release/LaLa.app; sleep 4
osascript -e 'tell application "System Events" to tell process "LaLa" to click button "Create" of window 1'; sleep 3
# Release は再署名のたびにマイク許可が出て、出ている間は AX が window 1 を取れない（-1719）
osascript -e 'tell application "System Events" to tell process "UserNotificationCenter" to click button "許可" of window 1'
# windowID は CGWindowListCopyWindowInfo(.optionAll) で owner=LaLa の行から取る。
# onscreen=false ならユーザーが別 Space にいる → true になるまでポーリングしてから AX を撃つ
screencapture -x -l <windowID> shot.png
osascript -e 'tell application "LaLa" to quit'; rm -rf ~/Music/daw/$(date +%Y-%m-%d)-*   # 一時プロジェクトの片付け（名前は「日付-単語」）
```

メイン画面の AX 名: 停止 / 再生 / 録音 / サイクル / クリック / FXパネル（パネルが開いている間は非表示）/ ×（FXパネルを閉じる）/ プロジェクトメモ / オーディオファイル / ドラムガチャ / オーディオ設定 / M / S / トラックを追加

## アプリログでの裏取り

操作がスクショで判別しにくいときは `~/Library/Logs/daw/` のセッションログを ground truth にする:

```sh
tail -20 ~/Library/Logs/daw/"$(ls -t ~/Library/Logs/daw | head -1)"   # 最新セッションのログ
```

- 1行 = `<ISO8601ミリ秒> LEVEL イベント名 key=value ...`。主なイベント:
  `session.start/end`・`project.open/save/close`・`audio.device`（デバイス名/SR/ブロックサイズ）・
  `transport.play/stop`・`record.start/stop/discard/start_failed`・`track.add/delete`・`edit.undo/redo` 等
- **正常終了の確認**: 終了後にログ末尾が `session.end` であること
- **異常終了の検知**: 次回起動時に `session.previous_abnormal` の WARN が出る（`pkill -9 -x LaLa-dev` → 再起動で再現可能）
- **エラー系の確認**: ダイアログ表示（`ui.alert`）・オーディオスレッドの異常（`audio.midi_overflow` / `audio.record_fifo_drop`、2秒集約）は ERROR/WARN で残る
- ログはセッションごと1ファイル・新しい20世代のみ保持・1セッション1MiB上限

## 起動直後にUIが固まって見えるとき（オーディオデバイス起動ハング）

ログに `project.open` が出ているのに画面が選択画面のまま更新されず、System Events が「window 1 を取り出せません」（AXが窓0）を返すことがある。CoreAudio のデバイス起動（`HALB_IOThread::StartAndWaitForState`）でメッセージスレッドが数分ブロックする環境事象で、アプリは死んでいない。`sample <pid>` のメインスレッドスタックで確認できる。`audio.device` ログ行が出れば解除される（失敗時は `name=(none) sr=0` でデバイスなしフォールバック。音は出ないがUI検証は続行可）。自分の変更によるフリーズと誤診しないこと

## CLI＋AppleScriptでの半自動確認

JUCEアプリはAppleScriptの合成キーストローク・座標クリック（`click at`）が効かないが、
**ボタンのAXPress（`click button "名前"`）とスクリーンショットの組み合わせ**で大半のフローを確認できる。

- **同名ボタン（トラックごとのM/S等）のAX参照は2個目以降が壊れる**: `every button of window 1 whose name is "M"` の item 2 をclickしても1個目に着弾する（`position of` も全itemが同一座標を返す）。`button <index> of window 1`（`name of every button` の並びのindex）で2個目に届くこともあるが不発（no-op）のこともある。**どの方式でも1クリックごとにピクセル差分のbboxで「意図した行に着弾したか」を裏取りする**（ミュート点灯のような大差分は maxDiff 400超、AXPress後にボタン上へ残るホバー状態は同領域40×36px@2x・maxDiff 50前後の微差分として出る。後者は原状復帰の差分ゼロ判定で誤検知しやすい）

- **ダイアログ／独立ウィンドウが開くと `window 1` はそちらに移る**: メイン画面のボタンを押すつもりの `button "名前" of window 1` が `-1728`（取り出すことはできません）で落ちる。ウィンドウ名で指定する（`button "オーディオ設定" of window "LaLa-dev — <プロジェクト名>"`）。**非モーダルなウィンドウなら背面のメイン画面のボタンもAXPress1回で効く**ので、モーダル/非モーダルの切り分け自体にも使える（モーダルだとクリックが届かず無反応になる）

ユーザーが他アプリで作業中にフォーカスを奪いたくないときは、`open -g` でバックグラウンド起動し、
`screencapture -x -l <windowID>` で背面のまま撮る（AXPressは背面ウィンドウにも効くが**同一Spaceに限る**。
別SpaceだとSystem Eventsからウィンドウ自体が見えなくなる。切り分けと対処、windowIDの取り方は
グローバルCLAUDE.mdの「Macアプリの変更」参照）。

確認の前に旧インスタンスを終了させる（多重起動不可のため、起動中だと新ビルドが立ち上がらない）:

```sh
osascript -e 'tell application "LaLa-dev" to quit' &   # 正規quit。未保存変更があれば保存ダイアログが出る（処理はユーザーに委ねる）
for i in $(seq 1 30); do pgrep -x LaLa-dev >/dev/null || break; sleep 3; done
```

- quitイベントは何も破棄しない。ダイアログ待ちでosascriptに `-128`（ユーザによってキャンセル）が返ることがあるが、プロセスが終了していれば問題ない
- **quitを送る前に、ユーザーがそのインスタンスを操作中でないかアプリログで確認する**（直近数分に `marker.add` / `edit.*` 等の操作イベントがあれば操作中とみなす）。未保存変更があるとquitは保存ダイアログを出してユーザーの作業を中断させる。誤って出してしまったら「キャンセル」だけ押して復元し、以降そのインスタンスには触らない
- **ユーザーがdev版を使用中でも検証は止めない**: dev版とRelease版は bundle id が別で同時起動できるため、`cmake --build build-release` → `open -g build-release/daw_artefacts/Release/LaLa.app` で並走検証する（描画コードは同一なのでスクショ検証として等価。検証後は自分でquitして片付ける）。ただし**検証フック（`--open` `--snapshot` 等）は `#if JUCE_DEBUG` なのでRelease版では動かない**。フックを使う検証は並走で代替できず、既存dev版インスタンスを安全確認（タイトル●・ログ末尾・HIDIdleTime・frontmost）してからquit→新devバイナリで行う
- **dev版とRelease版を並走しているときは、表示名でquit対象を指定しない**: `tell application "LaLa" to quit` の実行後にRelease版とdev版の両方が終了した実例がある。Release版は `tell application id "local.d0ne1s.daw" to quit`、dev版は `tell application id "local.d0ne1s.daw.dev" to quit` とbundle idで限定し、実行前後に `pgrep -x LaLa` / `pgrep -x LaLa-dev` で対象PIDだけが変化したことを確認する

```sh
# テストプロジェクトをCLIで用意（新規作成フローを迂回）
mkdir -p ~/Music/daw/cli-test
cat > ~/Music/daw/cli-test/project.json <<'EOF'
{"version": 1, "bpm": 100.0, "sampleRate": 0.0, "tracks": [{"type": "audio", "name": "ボーカル", "mute": false, "solo": false, "volume": 0.8, "clips": []}]}
EOF

open build/daw_artefacts/Debug/LaLa-dev.app && sleep 3
# マイク権限ダイアログが出たら（通常は初回許可後は出ない。tccutil reset 後の初回のみ）:
osascript -e 'tell application "System Events" to tell process "UserNotificationCenter" to click button "許可" of window 1'

# ウィンドウ位置を取ってスクリーンショットで目視確認
osascript -e 'tell application "System Events" to tell process "LaLa-dev" to get position of window 1'
screencapture -x -R<x,y,w,h> /tmp/daw-check.png
```

- プロジェクト選択画面に「開く」ボタンはない。開くのは**ダブルクリック or Return**の2経路。リストは**更新日時の降順**（`project.json` のmtime）で、先頭行が自動選択される（AXでは行選択不可）
- **CLIから開くにはCGEvent合成の「単クリック→スクショで選択確認→同座標にダブルクリック」**（検証済み）。ダブルクリックは down/up (clickState=1) → 150ms → down/up (clickState=2) の順で `mouseEventClickState` を明示する（単クリック2連打のOS集約任せは不安定）。行の座標目安: コンテンツ上端＝ウィンドウ上端+32pt（タイトルバー）、行高48pt、行1中心 ≈ +86pt・行2中心 ≈ +134pt（左右はウィンドウ中央でよい）
- **ウィンドウをタイトルバーから動かせるか（TitleBarStyle・styleMask を触った後）**: `activate` → 被覆チェック → タイトル文字の位置（ウィンドウ中央・上端+14pt）から CGEvent 合成ドラッグ（down→補間drag→up）→ `osascript -e 'tell application "System Events" to tell process "LaLa-dev" to get position of window 1'` の前後差がドラッグ量と一致すれば pass。確認後は `set position of window 1 to {x, y}` で元に戻す。上部バー（+14〜+82pt）はボタン群があるので掴み点にしない
- **テストプロジェクトを先頭に出すには `touch ~/Music/daw/<name>/project.json`**（更新日時降順のため。旧「`0-` 始まりの名前で辞書順先頭」は効かない）。開いた直後にタイトルバーの名前で対象プロジェクトか確認する
- **新規作成の自動命名**: 名前欄には候補名 `YYYY-MM-DD-<ランダム英単語>`（例: `2026-07-22-dawn`）がプリフィルされており、そのまま「Create」AXPress（またはEnter）で候補名のまま作成される（フォーカスすると全選択になり、打ち始めれば置き換わる）。作成された名前は `ls ~/Music/daw/` とログ `project.create name=` で裏取りする。検証で作ったプロジェクトは終了後に削除して片付ける
  - 選択画面が他アプリの大窓に覆われて合成クリックが撃てない（被覆チェックでABORTする）ときも、Create のAXPressは**背面のまま効く**ので、新規プロジェクト作成でプロジェクト画面の検証に入る代替になる（検証済み）
- **スライダー・ノブの値はAXから変更できない**（`set value of slider` / `increment` とも無反応。実測済み）。パン・send量など特定の値での見た目確認は、project.json に値を書いたテストプロジェクトを用意して開く（トラックの `pan` / `sends` / `volume` フィールド。形式は既存プロジェクトの project.json を参照）
- **フォーカス復帰の一覧再読込**: 選択画面表示中に `mkdir` 等で `~/Music/daw/` を変更 → `osascript activate` → スクショで一覧に反映されること（選択行は名前で追従・入力欄の候補名は変わらないこと）
- **行のカラーバー・ミニ波形・メタ情報**: 起動後のスクショで、各行左端に名前ハッシュ由来のカラーバー、サブテキストに「更新日時 · 曲長 · BPM · トラック数」、音声クリップを持つプロジェクトの右側にミニ波形（非選択=プロジェクト色/選択=白）が出ること。オーバービューのキャッシュは `~/Library/Caches/daw/overviews/*.bin`（project.jsonのmtimeで無効化。強制再計算はこのディレクトリを消す）。クリップなし・MIDIのみのプロジェクトは波形なし、クリップ空なら曲長なしが正しい
- **大きいウィンドウでの中央寄せ（フルスクリーン遷移の代替検証）**: フルスクリーン突入はSpace切り替えでユーザーの画面を乗っ取るため自動検証しない。同じコードパス（選択画面が設計サイズ520×584より大きい領域に置かれる）は `set size of window 1 to {1400, 900}` でウィンドウを拡大 → スクショで「コンテンツが中央に設計サイズで置かれ、引き伸ばされない」ことで確認できる。フルスクリーン中に⌘Oで閉じる実地確認だけユーザーに依頼する
- **ヒーローカード（直近プロジェクト）**: リスト上に大型カードが出て、単クリックで即開く（ログ `project.open`）。右クリックで「Finderで表示」メニュー。リストは2番目以降のみ（重複しない）。起動時はヒーローが選択状態（青塗り＋白波形）で、Return=開く・↓=リストへ・↑（先頭行から）=ヒーローへ戻る。↑↓ReturnはListBox任せでなくchooserの `keyPressed` 自前処理（ListBoxはフォーカスを持たない設計）。プロジェクトが0件のときはカード非表示
- **検証用インスタンスのウィンドウはユーザーの目に入る**: Release並走の検証ウィンドウをユーザーが「見慣れない窓」として閉じることがある（バツ=アプリ終了）。検証が中断されたら pgrep とログの session.end で「自分のインスタンスが閉じられた」ことを先に確認する。`osascript activate` は死んだインスタンスを**ユーザーの現在のSpaceに再起動してしまう**ので、activate前に pgrep で生存確認する。閉じられるだけでなく**そのまま使い始められることもある**（新機能入りアプリとして操作され、選択画面のつもりの窓がプロジェクト画面に変わっていた実例あり）。ログの `project.open` / `track.add` 等が自分の操作と対応しないときは接収とみなし、合成操作を中止してユーザーに委ねる
- **行の右クリック「Finderで表示」**: Ctrl+左クリック合成（`flags = .maskControl`）でメニューが開く。メニュー検出（CGWindowListでowner=LaLa-dev・幅500未満の小窓）→項目クリックまで**1プロセスの実行ファイル内で完結させ**、`osascript -e 'tell application "Finder" to get selection'` で対象フォルダが選択されていることを裏取りする
- **プロジェクトフォルダのドラッグ&ドロップで開く**: AppleScriptでFinderウィンドウを既知の位置にリスト表示で開き（`set bounds`）、スクショで行座標を較正 → CGEvent合成ドラッグ（down→400ms保持→30ms間隔の補間drag→700ms保持→up）で行をdawの選択画面へ落とす → ログ `project.open name=` で裏取り。**dev版とRelease版は `centreWithSize` で同一座標に重なる**ため、並走検証でドロップ先が背面に隠れていないか `winlist`（CGWindowListは前面→背面順）で確認し、必要なら `set position of window 1` で自ウィンドウを退避してから撃つ

確認できること:

1. **再生**: `click button "再生"` → 数秒後のスクショで位置表示（小節.拍｜秒）が進み、再生ヘッドが移動・追従スクロールする。▶は再生中に緑点灯し、**再生中にもう一度押しても何も起きない**（トグルではない）。停止は `click button "停止"`（■。録音中は録音終了）。Space だけがトグル。送信の前後で `transport.play/stop` ログと突き合わせて意図どおりかを裏取りし、身に覚えのないイベントが混ざったら合成操作を中止する
2. **録音**: `click button "録音"` → 7秒待つ → もう一度 `click button "録音"`（AX名は録音中も「録音」のまま。「録音停止」では引けない）→
   - `afinfo ~/Music/daw/cli-test/clip-001.wav` が モノラル・24bit・デバイスレート であること
   - 長さ ≒ 録音時間 − 1小節（カウントイン分）であること
   - スクショで選択トラックの小節頭にクリップが置かれ、タイトルに未保存マーク「●」が付くこと
3. **トラック追加**: `click button "トラックを追加"`（左下の＋ボタン）→ 自前オーバーレイのメニューがボタン直上に開く（スクショで位置確認）。項目は自前描画でAXPress不可のため、項目クリック〜トラック追加まではCGEvent合成クリックか実手動で確認する
4. **保存**: closeボタン（`first button of window 1 whose subrole is "AXCloseButton"`）→
   「保存して終了」ダイアログ → `click button "保存して終了"` → プロセス終了と
   `python3 -m json.tool ~/Music/daw/cli-test/project.json` で bpm/sampleRate/tracks/clips を確認
5. **復元**: 再起動 → 行をダブルクリック（上記CGEvent手順。直前に触ったプロジェクトが先頭に来る）→ スクショでBPM・トラック名・音量スライダー位置・クリップ位置が一致すること
6. **右クリックメニューの項目クリック（リージョンのミュート・分析等）は1プロセスのCGEventツールで**: ①Ctrl+左クリック（flags に maskControl）でメニューを開く ②CGWindowList で「クリック後に新出した owner=LaLa-dev の小窓」= メニューを特定 ③高さ÷項目数で等分した行の中心をクリック。項目数はメニュー構成から数える（オーディオリージョンは8項目: ミュート/ゲイン/複製/ループ解除/分割/書き出し/リファレンス分析/削除）。着弾はアプリログで裏取りする（実装例: セッションscratchpadの refmenu.swift）
7. **合成クリックの座標がウィンドウ矩形の外に出るとデスクトップ/Finderに落ち、以後LaLa-devのボタンが不発になる**（フォーカスがFinderへ移るため）。クリック前にウィンドウ矩形と突き合わせ、外していたら `activate` で戻してから撃ち直す

## プロジェクトを閉じる/選択画面まわりの確認

バツ＝プロジェクトを閉じて選択画面へ（アプリは生存）、選択画面のバツ＝アプリ終了、⌘O＝バツと同じ（要実キー）。CLIで確認できる流れ:

1. **変更なしで閉じる**: 開く → closeボタンAXPress → ダイアログなしで選択画面に戻る。裏取り: `pgrep` でプロセス生存・ログに `project.close ... dirty=0`・ウィンドウタイトルがアプリ名のみ（`LaLa-dev`。プロジェクト名なし）に戻り選択画面サイズになる（winlist系ツールで確認）
2. **別プロジェクトへの乗り換え**: 選択画面から行を単クリック→ダブルクリック（上記CGEvent手順）→ ログの `project.open name=` が対象名であること。閉じる→開くを繰り返してもクラッシュ・`ERROR` ログがないこと
3. **未保存で閉じる**: 変更を作って closeボタン → 「保存して閉じる/保存せず閉じる/キャンセル」の3択が出る。**ダイアログはプロジェクトウィンドウとは別ウィンドウ**なので、スクショはwinlistで小さいウィンドウ（約260×270）のIDを引いて撮る。ボタンはAXPress可。キャンセル→プロジェクト画面に留まる（●維持）→再度バツでまたダイアログが出る（連打ガードが解除されている証拠）
4. **録音中に閉じる**: `click button "録音"` → 数秒 → closeボタン → 録音が先にクリップ化されて（ログ `record.stop`）からダイアログが出る。「保存して閉じる」後に project.json の clips と `clip-NNN.wav` の実在で裏取り
5. **選択画面で閉じる**: closeボタン → プロセス終了（`pgrep` 空）・ログ末尾が `session.end`

- **ヒーローカード（最上部の大きいカード）は単クリックだけで開ける**（リスト行の「選択→ダブルクリック」不要）。直前に開いたプロジェクトがヒーローに昇格するため、同じプロジェクトの再検証は「ヒーロー中央を1クリック → ログ `project.open` で裏取り」が最短

## 自動テスト（CTest）

```sh
cmake --build build --target daw_tests && ctest --test-dir build --output-on-failure
```

- モデル（保存/読込・ID・PPQ境界・undo・WAV GC保護）、GM音源（DLSMusicDevice）、
  再生エンジン（MIDI再生・シーク再発音・停止消音・ミュート時のイベント継続・プレビュー経路）をGUIなしで検証する
- テストは一時ディレクトリのみ使用（`~/Music/daw` には触れない）
- **Debug assertion はデバッガ無しでは見逃す**: `timeout 700 lldb -b -o run -o quit build/daw_tests_artefacts/Debug/daw_tests` で走らせ、`stop reason = EXC_BREAKPOINT` が出ないことを確認する（出たら `jassert` に引っかかっている。通常実行では静かに通過して `all tests passed` になるため、テストが緑でも潜んでいることがある）

## URLからの取り込み（yt-dlp）の確認

`yt-dlp` は Brewfile 経由で入れる（`brew 'yt-dlp'` → `brew bundle`）。アプリは
`/opt/homebrew/bin/yt-dlp` → `/usr/local/bin/yt-dlp` の順に**絶対パスで探す**（`.app` は launchd 起動で PATH が最小限のため）。

```sh
# GUIを介さずワーカー単体で通し確認（ネットに出るので環境変数を渡したときだけ走る）
LALA_VERIFY_URL='https://www.youtube.com/watch?v=jNQXAC9IVRw' build/daw_tests_artefacts/Debug/daw_tests
# 長すぎる動画がDL前に弾かれるかも見るなら:
LALA_VERIFY_URL='…' LALA_VERIFY_LONG_URL='<34分超の動画>' build/daw_tests_artefacts/Debug/daw_tests
```

このテストで見ているもの: 成功時のタイトル取得・WAV生成（`2ch 48000Hz`）・一時ディレクトリの所有権受け渡し・進捗の更新・デコード可否／`--exec…` のようなオプション文字列が実行されないこと／存在しない動画で理由付き失敗／DL中キャンセルで一時ディレクトリが消え yt-dlp・ffmpeg のプロセスが残らないこと。

- **一時ディレクトリは `$TMPDIR` ではなく `~/Library/Caches/<実行ファイル名>/`**（JUCEの `tempDirectory` の実体。dev版は `LaLa-dev`、Release版は `LaLa` で自動的に分かれる）。`lala-url-<pid>-<uuid>/` という名前で作られ、取り込み完了・失敗・キャンセルのいずれでも消える
- **起動時の残骸掃除**は名前に埋めたPIDの生存で判定する。検証は偽の残骸を置いてから起動する:
  ```sh
  C=~/Library/Caches/LaLa-dev; nohup sleep 400 >/dev/null 2>&1 & P=$!; disown
  mkdir -p "$C/lala-url-$P-alive" "$C/lala-url-99997-dead" "$C/lala-url-notanumber-x" "$C/lala-url-"
  open -g build/daw_artefacts/Debug/LaLa-dev.app; sleep 6; ls "$C"; kill $P
  # → dead だけ消え、他3つは残る。ログに url.tempdir.swept count=1
  ```
  **Bash tool のシェルはコマンドごとにPIDが変わる**ので、`$$` を「生きているPID」の検証に使うと次のコマンド実行時には死んでいて誤判定する。`nohup sleep` 等の長命プロセスを使う
- **403フォールバック（player_client リトライ）の検証**: まず CLI で `yt-dlp --ignore-config -x --audio-format wav -o 'x.%(ext)s' '<URL>'` がデフォルト経路で `HTTP Error 403` になる動画を見つけ（403 が出るかは動画依存。GOTCHAS.md「yt-dlp の YouTube 403」参照）、その URL を `LALA_VERIFY_URL` に渡して成功すればリトライ経路が機能している。CLI 側が同時点で 403 のままであることも併せて確認する（「たまたま通った」の除外）。**必ず `PATH=/usr/bin:/bin:/usr/sbin:/sbin` を付けて launchd 相当の最小 PATH で回す**（リトライ先の web 系 client は deno が要るため、ターミナルの PATH だと .app で再現する失敗を取りこぼす）
- **ログにフルURLを残さない**（query/fragment に署名トークンを載せるサイトがあるため）。`Log::` と `showAlert` に渡す文字列は全て `YtDlpOutput::redactUrls()` を通す。yt-dlp のエラー本文は `[generic] foo?token=…:` のように scheme も host も落ちた形でクエリだけ出すことがあるので、パターンマッチだけでなく「渡した元URLの `?` 以降を直接伏せる」2段構えになっている

## エンジン/バウンスの数値回帰確認（ビット一致）

```sh
# 変更前に基準を採取（Debug/Release両構成でビルド→全テスト実行→hash-行を保存）
scripts/check-render-hashes.sh capture
# エンジン変更後に比較（不一致なら diff を表示して非0終了。テスト失敗も検出する）
scripts/check-render-hashes.sh compare
```

- ベースラインは `.render-hash-baseline/`（gitignore済み。build/ 外なのでクリーンビルドでも消えない）
- ハッシュは5本: `hash-engine` / `hash-bounce`（testMonoRenderRegressionHash＝FX中立の高速パス）と
  `hash-fx-engine` / `hash-fx-bounce` / `hash-fx-project-bounce`（testTrackFxRegressionHash＝
  EQ/Comp有効のactive経路を6経路すべて通す。project-bounce は Project::save→load 経由）
- スクリプトはハッシュ行の本数・名前の完全一致も検査する（経路の出力漏れ検知）。テスト側で
  hash- 行を増減させたらスクリプト冒頭の `expected_names` も更新する
- `daw_tests | grep hash-` の直結は使わない（テストがハッシュ出力後に落ちても grep の 0 が失敗を隠す）
- testMonoRenderRegressionHash が「重なりクリップ×pan×send×Master」の決定的レンダリングをFNV-1aで出力する
- ハッシュの期待値はテストにハードコードしない（浮動小数点の積和順序がコンパイラ・環境依存。同一環境の変更前後比較にのみ使う）
- **仕様変更でハッシュが変わるとき**: fixture が対象機能を含むなら変化は「意図した仕様変化」（実例:
  バッチ4で素通しバス→wet返しになり、send>0 を含む全 fixture のハッシュが変化）。変更前に capture →
  compare の diff で**変わった本数・経路が意図と一致するか**を確認（変わるはずのない経路の変化はバグ）→
  説明がついたら capture し直して新基準にする。「意図した変化かどうか」は別テスト（不変であるべき性質の
  直接検証）で裏取りする
- 不一致＝モノのみトラック経路の演算順序が変わった兆候（PlaybackEngine/BounceRendererの2経路構造のコメント参照）
- **変更前のハッシュを採り忘れたときは `git stash` で取れる**（作業ツリーを退避して同じ手順を実行 → `git stash pop` で戻す）。Masterゲイン経路に手を入れる変更（曲末フェード等）で「既存プロジェクトの出音を変えていない」ことを実証するのに使える:
  ```sh
  git stash push -u -m wip && cmake --build build --target daw_tests \
    && build/daw_tests_artefacts/Debug/daw_tests | grep hash- ; git stash pop
  ```

## オーディオ取り込みの確認

```sh
# L=440Hz/R=880Hzのステレオ素材（左右で違う音が鳴る=ステレオ再生の耳確認用）
python3 -c "
import wave, struct, math
w = wave.open('/tmp/beat.wav', 'w'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b''.join(struct.pack('<hh', int(16000*math.sin(2*math.pi*440*i/44100)), int(16000*math.sin(2*math.pi*880*i/44100))) for i in range(44100*4)))"
afconvert -f m4af -d aac /tmp/beat.wav /tmp/beat.m4a   # CoreAudio経路（m4a）の確認用
```

- 導線: Fileメニュー「オーディオを読み込む…」（⇧⌘I）＝新規トラック小節1 / FinderからタイムラインへD&D＝ドロップ位置（空白は新規トラック。**MIDIトラックの行・ヘッダーへのドロップはサンプル音源の割り当て**＝下記「サンプル音源の確認」）
- 取り込み後の裏取り: アプリログの `import.start` / `import.done`（frames・ch・sourceSr）/ `import.fail`
  - 成果物: project.json が version 8・クリップに `name`・SR未確定プロジェクトなら `sampleRate` が確定済み。`afinfo <project>/clip-NNN.wav` で 2ch・プロジェクトSR・24bit
- 変換仕様（SR変換長・末尾パルス・ch規則・GC安全性）はCTestの testAudioImporter / testStereoClipLoadAndV6 が自動で固定している

## サンプル音源（サンプラートラック）の確認

レンダリング仕様（One Shot/追従・ボイス管理・SR比・バウンス範囲）はCTestの `SamplerEngine *` /
`Sampler through PlaybackEngine` / `SynthBank sampler instances` / `BounceRenderer sampler` /
`sampler project roundtrip and gc` が担う。アプリ統合の確認:

```sh
# サンプル音源入りテストプロジェクトをCLIで用意（D&Dの割り当てを迂回して読込・再生・書き出しを見る）
DIR=~/Music/daw/sampler-test; mkdir -p $DIR
python3 -c "
import wave, struct, math
sr, dur = 44100, 6.0            # 44.1k素材。デバイス48kとの比率変換も同時に確認できる
w = wave.open('$DIR/instr-001.wav','w'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(28000 * (0 if i/sr < 0.003 else math.exp(-i/sr*0.55))
                                   * math.sin(2*math.pi*55*i/sr))) for i in range(int(sr*dur))))"
# project.json は MIDIトラックに "instrument": "sample" と sample* 一式を書く（形式は既存プロジェクト参照）
```

- 割り当て導線: 右パネルのファイルブラウザ or Finder から **MIDIトラックのヘッダー／タイムラインの行**へD&D
  （行全体のハイライト＋"Instrument" ラベルが出る。クリップ配置の縦線とは見た目で区別される）。
  オーディオトラックのヘッダーは減光＝不受理
- 裏取りはログ: `import.start kind=instrument` → `instrument.assign`（file・name・sourceSr・startOffset）/
  失敗は `instrument.load_fail`。プルダウンでGMへ戻すと `instrument.revert_gm`
- 成果物: `<project>/instr-NNN.wav`（**元のSRのまま**・24bit。プロジェクトSRへ変換されない）と
  project.json の `sampleFile` / `sampleSourceRate`。`afinfo` で元SRのままであることを確認する
- ピアノロール（固定モード）: 置ける行は1行だけ（ルート音）で、**それ以外の行は減光**される。
  ノートは押した行でなく固定行にでき、その行が画面外なら自動でスクロールする。
  **リージョン終端より右は減光＋境界線**（編集範囲外）。そこをダブルクリックすると
  **リージョンが伸びて押した場所にノートができる**（元の長さが小節ちょうどなら小節単位で伸びる）。
  タイムライン側のリージョンも同時に伸び、⌘Zで長さごと戻る
- FXパネル: MIDIトラック選択時、EQサムネイルの下に Instrument スロットが出る
  （サンプル=点灯＋クリック可 / 内蔵GM=グレーでクリック不可）。クリックで下部エディタが開き
  ログ `fxdetail.open fx=<サンプル名>`
- 下部エディタの `VOICE: Mono`: ONで「新しい打点が前の音を切る」（Logicの Polyphony:1 相当）。
  1.7秒の808を16分連打したとき、OFFだと重なってクリップ＋うなり（合計ピーク1.18）・ONだと単発と同程度
  （0.65）に収まる。短いハイハット（0.05秒）はそもそも重ならないのでOFFのままで正常
- 下部エディタ（合成クリックで確認できる範囲）: 「追従」を押すとROOTが有効化 → Fileメニューの`保存`で
  project.json の `samplePitchFollow` が true になる → 「固定」で false に戻る。
  波形の緑線ドラッグ（頭カット）と波形クリック試聴は実マウス（要ユーザー操作）で確認する
- 全体バウンスは**固定モードのワンショットが鳴り切るまで範囲が延びる**: 曲末に6秒サンプルを置くと
  ログの `bounce.start endSample=` がリージョン終端ではなく「最後のノート位置＋サンプル全長」になる
  （例: 90BPM・4拍目のノート＋6秒サンプル・48kHz → endSample=384000 = 8.0秒）。
  出力WAVの末尾付近に音が残っていること（テールの-60dB打ち切りに掛かっていないこと）も確認する
- **合成クリックの被覆チェックは「クリック点を含むlayer=0のウィンドウが対象アプリか」で見るが、
  デスクトップ上（該当ウィンドウなし）は素通りする**。ウィンドウが別ディスプレイへ移動していた事例があるので、
  クリックの直前に winlist でウィンドウ矩形を取り直し、クリック点がその矩形内にあることも確かめる

## 右パネル（メモ／オーディオファイル）の確認

- プロジェクト画面では `button "プロジェクトメモ"` / `button "オーディオファイル"` をAXPressできる。選択中のボタンを再度AXPressすると閉じ、他方を押すと開いたまま内容が切り替わる
- 背面のまま見た目を確認する場合は、CGWindowListでLaLaのタイトル付きwindow IDを取得し、`screencapture -x -l <windowID> /tmp/lala-right-panel.png` で撮る。初期幅300px、上部バー直下から下端まで、下部エディタが開いてもパネル下へ潜らないことを見る
- ファイルパネルの初期パスは `~/Downloads`。一覧はフォルダ＋WAV/AIF/AIFF/FLAC/MP3/M4Aのみ・単一選択。**既定は追加日（作成日時）の新しい順**で、パンくず行の並べ替えアイコンから名前順へ切り替えられる（セッション内のみ保持・起動時は追加日順に戻る）。フォルダはファイルと混ぜて並ぶ。下部に選択名・長さ・状態（読み込み中・エラー）が出る
- **試聴は「選択＝即試聴」**（オートプレビュー）。行を選ぶと200ms後に鳴る。行の左端アイコンは 通常`♫` / ホバー`▶` / 試聴中`■`（loading含む）で、クリックでその行の再生・停止をトグルする。フォルダ行は常に `▸` でクリック領域も持たない。パンくず行右端のスピーカーがオート試聴のON/OFF（セッション内のみ・起動時ON）。**OFFはアイコン全体に斜線**が入り、色だけに頼らず形で見分けられる
- パネルを閉じる・メモへ切り替える・取り込みを始めると、予約中のオート試聴ごと畳まれる（閉じた直後に鳴り出さないこと）。試聴の失敗表示は、別のファイルやフォルダを選んだ時点で消える
- オート試聴まわりの状態遷移はログ（`file_preview.start` / `file_preview.stop`）で裏取りする。`↑↓`連打で最後の1件だけstartが出る（デバウンス200ms）／オート試聴中にSpaceを押すとstopが出る／走行中は選択してもstartが出ないが行`▶`なら出る／予約中・loading中・playing中のどれでトグルOFFにしても止まる／手動`▶`由来の試聴はSpaceでもトグルOFFでも止まらない
- 試聴の判定ロジック（auto/manual・予約・トランスポート・トグル・自然終了での解除）は自動テスト `preview policy` が、並び順の規則は `file sort order` が固定している。UIを触らずに回帰を見られる
- **`DirectoryContentsList` の並びは名前の自然順で固定**（`JUCE_WINDOWS` のときだけフォルダが先。macOSでは混在）。表示順は `FileSortOrder::sortedIndices` が作るインデックス列で差し替えており、行番号は contents 側の index と一致しない（`fileAt()` / `rowForFile()` を必ず経由する）
- **JUCEの `creationTime` は秒精度**（`st_birthtime × 1000`）で、macOSでは `File::setCreationTime` が無効（POSIX実装が引数を捨てている）。実ファイルで追加日順を検証するときは1秒以上あけて作る。同じ秒に作られたファイルはファイル名で並ぶ
- パス欄はパンくず（`/ › Users › d0ne1s › Downloads`）。祖先クリックでその階層へ移動、幅が足りないときは左から `…` に畳まれる（`…` クリックで畳まれた祖先のポップアップ。親と現在フォルダは必ず残る）。履歴の戻る/進むは ⌘[ / ⌘]（ファイルパネルを開いているときだけ効く）
- **ブラウザの操作挙動（オート試聴・ホバー・行アイコン）はアプリを起動せずに実挙動として検証できる**。実機が使えないとき（別Space・画面収録権限なし・ユーザーが作業中）の主力手段:
  1. `CMakeLists.txt` の `daw_tests` に `Source/ui/AudioFileBrowserView.cpp` と `JUCE_MODAL_LOOPS_PERMITTED=1` を一時追加（`runDispatchLoopUntil` はこの定義がないとコンパイルが通らない）
  2. `AudioFileBrowserView` の `final` を外し、`navigate` を public へ、`probeListBox()` / `probeToggle()` / `probeHoveredRow()` / `probeIsActive()` の一時アクセサを足す
  3. テストで一時ディレクトリにWAVを並べて `navigate` → `runDispatchLoopUntil` でスキャン完了を待ち、`ListBox::selectRow` と `juce::MouseEvent` を組んだ `mouseDown` / `mouseMove` で操作する。デバウンスやTimerも `runDispatchLoopUntil` で進む
  4. 「アイコン以外は下の行に通る（＝選択・D&Dが生きている）」は `rowComponent->getComponentAt (x, y) == nullptr` で機械判定できる
  5. 確認後は1〜2を戻し、`git diff CMakeLists.txt Source/ui/AudioFileBrowserView.h` に一時変更が残っていないことを見る
  - **行の並びはフォルダが先ではなく名前順**（`DirectoryContentsList` の順）。行番号とファイルの対応を思い込みで書くと、実装は正しいのにテストだけ落ちる
- パンくずの畳み・省略はアプリを起動せずに検証できる: JUCEのコンソールアプリを1本足して `BreadcrumbBar` を任意の幅・パスで `setBounds`→`paintEntireComponent`し、`PNGImageFormat` で1枚のシートに並べて目視する（ホバーは `MouseEvent` を組んで `mouseMove` に渡す）。ユーザーが実機を使用中でUIを触れないときでもレイアウト回帰を機械的に見られる
- 自動テスト `Audio file browser filter and preview` が、対応拡張子、22.05kHz→44.1kHz線形補間、ステレオ維持、ブロック境界をまたぐ位置、停止後無音を固定している

## MIDI機能の半自動確認

```sh
# MIDIトラック入りテストプロジェクトを用意
mkdir -p ~/Music/daw/cli-midi-test
cat > ~/Music/daw/cli-midi-test/project.json <<'EOF'
{"version": 2, "nextId": 10, "bpm": 120.0, "sampleRate": 0.0, "tracks": [
 {"id": 1, "type": "audio", "name": "ボーカル", "mute": false, "solo": false, "volume": 0.8, "clips": []},
 {"id": 2, "type": "midi", "name": "Piano", "mute": false, "solo": false, "volume": 0.8, "gmProgram": 0, "drums": false,
  "regions": [{"id": 3, "startPpq": 0, "lengthPpq": 3840,
    "notes": [{"id": 4, "pitch": 60, "startPpq": 0, "lengthPpq": 960, "velocity": 100},
              {"id": 5, "pitch": 64, "startPpq": 960, "lengthPpq": 960, "velocity": 100},
              {"id": 6, "pitch": 67, "startPpq": 1920, "lengthPpq": 1920, "velocity": 100}]}]}
]}
EOF
```

- 開いてスクショ: MIDIトラックのヘッダに楽器ドロップダウン（3行レイアウト）、タイムラインに緑のリージョン＋ノートミニチュアが出ること
- `click button "再生"` → 位置表示が進むこと（音の確認は要ユーザー）。MIDIトラック選択中は録音ボタンがグレーアウトすること
- **リージョン操作の検証はCGEvent合成マウス＋保存後のproject.json裏取り**が確実:
  - AppleScriptのAXクリックはJUCEのPopupMenu項目・ComboBox項目には効かない。CGEventの座標クリック（sandbox無効実行が必要）なら効く
  - スクショの目視だけで判定しない。閉じる→「保存して終了」→ `python3 -m json.tool project.json` で startPpq/lengthPpq/drums 等の値を確認する
  - 合成クリックを短時間に連続して撃つとOSのクリック集約で意図しないダブルクリックになる。操作間に時間を空ける
  - **リージョンの右クリックメニューはCGEventの `.rightMouseDown` では開かない**（クリップ選択まではされるがJUCE側でポップアップトリガー扱いにならない。clickState/buttonNumber明示・hid/sessionタップどちらも不可）。**Ctrl+左クリック**（イベントの `flags` に `.maskControl` を直接セットし `.cgSessionEventTap` へpost）なら開く
  - **メニュー表示中に別プロセスのCLIツール（AppKitをリンクしたswift製など）を起動するとメニューが閉じる**。検証は「Ctrl+左クリックで開く → CGWindowListで owner=LaLa-dev・name=menu のウィンドウboundsを取る → bounds高さを項目数で等分して対象項目の中心を左クリック」までを**1プロセス内**で完結させる（各操作の間に1秒程度sleep）。着弾はアプリログ（`region.split` / `region.mute` 等）で裏取りする
  - 分割の確認例: ルーラークリックでシーク → リージョンをCtrl+左クリック → 「再生ヘッド位置で分割」 → 保存後のproject.jsonで オーディオは同一`file`参照2クリップの `offsetSamples`/`lengthSamples` が連続すること、MIDIは右リージョンのノート `startPpq` が相対シフトされていることを確認
  - ループの確認例: リージョンを1回クリックして選択（**ハンドルは選択中しか出ない**）→ 下辺の右寄り40px×6px（`loopHandleRect`）をCGEvent合成でドラッグ → ログ `region.loop ... count=` と保存後のproject.jsonの `loopCount` で裏取り。ループ中は右端8pxのリサイズが無効になるので、長さを変えたいときは先にCtrl+左クリック →「ループ解除」。再生に乗っているかは⌘Bの出力WAV長を `afinfo` で見るのが確実（ループ終端まで伸びる）
  - フェードの確認例（オーディオリージョンのみ）: リージョンを1回クリックして選択（**ハンドルは選択中しか出ない**）→ **上辺**の左右のハンドル（10px×14px・`fadeHandleRects`。位置はフェード終端に追従し、連なり全体の幅が40px未満なら出ない）をドラッグ → ログ `region.fade which=in|out ... samples=` と保存後のproject.jsonの `fadeInSamples` / `fadeOutSamples`（0のときは書かれない）で裏取り。音に乗っているかは⌘Bの出力WAVの先頭/末尾サンプルを見る（`python3` で読んで最終サンプルが≒0）。ドラッグ中の追従・ポップアップの見た目はオフスクリーン描画（下記「アプリを起動せずに描画を確認する」の合成MouseEvent）で先に潰せる
  - スクロールが必要な確認（ピアノロールの鍵盤帯域移動等）はCGEventの `scrollWheelEvent2Source`（units: .pixel）を対象座標に連打すればViewportに効く。横スクロールは `wheelCount: 2` で `wheel2` に値を渡す（タイムラインの後方マーカー確認等で使用）。ピアノロールを開いた直後は最上部（pitch 127側）表示なので、GMドラム名の確認は下方向へスクロールしてから撮る
- 座標の目安（ウィンドウ位置 X,Y・デフォルトズーム pxPerBar=80）: タイムライン左端 = X+200、
  マーカーレーン中心 = Y+28(タイトルバー)+54(トランスポート)+26(ルーラー)+9、
  レーン先頭 = Y+28+54+26+18(マーカーレーン)、トラック行高 = 92

## セクションマーカーの半自動確認

```sh
# マーカー入りテストプロジェクト（採番確認用に同種別2個＋後方マーカー）
mkdir -p ~/Music/daw/0-0-marker-test
cat > ~/Music/daw/0-0-marker-test/project.json <<'EOF'
{"version": 3, "nextId": 10, "bpm": 120.0, "sampleRate": 0.0,
 "tracks": [{"id": 1, "type": "audio", "name": "ボーカル", "mute": false, "solo": false, "volume": 0.8, "clips": []}],
 "markers": [{"bar": 3, "type": "verse"}, {"bar": 5, "type": "verse"}, {"bar": 100, "type": "hook"}]}
EOF
```

- 開いてスクショ: ルーラー直下のマーカーレーン（高さ18px）に verse1/verse2 の緑帯が出て、最初のマーカーより前（bar 1-2）は空白であること。後方マーカー（bar 100）は右スクロールで到達できる（コンテンツ幅がマーカーまで伸びる）
- 操作はCGEvent合成マウス＋アプリログ（`marker.add/remove/type/move`）＋保存後のproject.json裏取り:
  - 空白レーンの左クリック → 6種メニュー（項目高≈18px）。既存セクションの左クリックはセクション頭へのシーク（LCDのTIME表示で確認: bar N = (N-1)×拍長×4秒）
  - 右クリックメニューはCtrl+左クリック合成で開く。「ここにセクションを追加/種別を変更」のサブメニューは**親項目にmouseMovedでホバーすると開く**（1.5秒待ってからサブメニューウィンドウを CGWindowList で探す）
  - 移動はマーカー本体のドラッグ（相対移動）または開始境界±4pxのドラッグ（吸着）。down→drag→up で `marker.move` がログに出る（隣マーカー手前でクランプ）。本体を動かさず離すとシークになる
  - 配置・移動のスナップは**表示中グリッド準拠で上限は拍**（デフォルトズーム pxPerBar=80 では拍=20px刻み。ズームアウトすると小節頭のみ）。拍位置への追加はログの `beat=1..3` とJSONの `"beat"` で確認
- 保存後の `python3 -m json.tool project.json` で markers が bar/beat 昇順・番号なし（採番は表示専用）であること。`"beat"` 省略の旧形式は beat 0 として読める
- モデル層（ヘルパー・不正値除外・undo）は ctest の `section markers` / `UndoStack` テストが網羅する

## サイクル（ループ範囲）の確認

ループ・書き出し範囲・永続化のロジックはCTest（`cycle range roundtrip and v4 defaults` / `PlaybackEngine cycle loop` / `BounceRenderer cycle range`）が担う。アプリ統合の確認:

- **範囲の作成**: ルーラー（上26px帯）を左右にドラッグ → 黄色の帯が出て自動でON。動かさず離すと従来どおりシーク（シーク発火はmouseUp時）。CGEvent合成ドラッグでも可（down→drag→up。座標目安はMIDIセクションの表記参照。ルーラー中心 = ウィンドウY+28+54+13）
- **裏取りはログ**: ドラッグ確定で `cycle.range start=<16分音符単位> end=<同> enabled=1`、幅ゼロに潰すと `cycle.clear`、Cキーで `cycle.toggle`。保存後は `python3 -m json.tool project.json` の `cycle` キー（16分音符単位・`[start, end)`）
- **ループ再生**: 再生ヘッドが範囲末尾で頭に戻る（スクショ2枚を数秒空けて撮り、ヘッドが帯の内側に留まることで判定）。範囲外から再生すると `transport.play pos=` が範囲頭のサンプル位置になる
- **端のリサイズ**: 帯の端±4pxでリサイズカーソルに変わる。ドラッグで伸縮・帯の内側ドラッグで移動
- **⌘B連動**: サイクルON時の `bounce.start` に `startSample=`（範囲頭）が出て、`bounce.done samples=` が範囲サンプル長と一致（テールなし）
- 旧プロジェクト（v4以前）は cycle キーなし → 範囲なし・OFFで開ける（保存するとv5になる）

## 再生ヘッドと再生開始位置の分離の確認

仕様は [docs/design/transport-playhead.md](docs/design/transport-playhead.md)。位置の算出は CTest（`TransportState::uiPositionSample`）が担う。アプリ統合の確認:

- **停止でヘッドが残る**: 再生 → Space で停止すると `transport.stop pos=<止めた位置> startPos=<開始位置>` が出る。**pos と startPos が違う値になる**のが分離できている証拠（以前は `returnTo=` で巻き戻していた）。続けて再生すると `transport.play pos=` が startPos と一致する
- **止めた位置で切れる**: 停止後に ⌘T（または右クリック → 分割）で `region.split pos=` が停止位置と一致する
- **録音は開始位置から**: `r` の `record.start` に `punchIn=`（開始位置を小節頭へ丸めた値）と `countInStart=`（punchIn の1小節前）が出る。**録音 → 停止 → もう一度録音で同じ `punchIn` が出る**ことが「同じ場所へ録り直せる」の裏取り。`countInStart` が `punchIn - 1小節` であることでカウントインが生きていると分かる（`playheadSamplePos` の直接観測は一瞬しか成立しないので使わない）
- **マーカーの描画**: ルーラー上端に中抜きの三角が出る。重なっているときはヘッドの塗りに隠れて1本に見えるのが正しい。オフスクリーンで撮るなら `TimelineView` を daw_tests に一時追加し（「アプリを起動せずに描画を確認する」参照）、`view.setPlayStartSample()` とヘッド位置を別々に与えて2状態を並べる

## トラックレベルメーターの確認

再生中にトラックヘッダの音量バー（カプセル）内へL/R 2本の緑レーン（固定dBスケール・緑→黄→赤・ピークホールド付き）が点灯する。**再生ヘッドがリージョン/クリップを通過中にしか点灯しない**ので、`click button "再生"` の直後 0.5〜1.5 秒でスクショを撮る。リージョン通過後・停止後に消灯すること、クリップの無いトラックに出ないことも同じ流れで確認できる。FXパネル（VOLUME区画）とミキサーのdB数値ボックス右側（ピーク保持）は**停止後も値が残る**のが正しい（次の再生開始でリセット）。

- スクショはウィンドウID指定（`screencapture -x -l <id>`）ならアプリが背面でも撮れる。IDは CGWindowList を `番号\t名前` で出すswift小ツール（グローバルCLAUDE.md「Macアプリの変更」参照）を作って `awk -F'\t' '$2 ~ /<プロジェクト名>/ {print $1}'` で引く
- メーターの点灯が小さすぎる/見えないときは、撮影タイミングが通過後でないかを先に疑う（ディケイは約1秒でフルスケールが消える）

## 明滅・アニメーションUIの確認（録音グロー等）

スクショ1枚では明滅は判定できない。**録音中に0.3〜0.4秒間隔で複数枚撮り、連続ペアのピクセル差分**で機械判定する:

- 撮影間隔を明滅周期のちょうど半分にしない（録音グローは周期1.6秒。0.8秒間隔だと位相の対称点で差が出ないことがある）
- 差分は swift + `NSBitmapImageRep.colorAt` の小ツールでRGB合計差を出す（グローが動いていればボタン領域で maxDiff 0.1〜0.3・数千px変化。0.03以下なら効いていないか、録音がSR不一致アラート等で始まっていない）
- 差分ゼロが出たら先に crop 座標と、ログの `record.start` があるか（＝本当に録音中だったか）を疑う

## UI変更のピクセル差分検証

見た目の変更・リファクタは、変更前後のウィンドウキャプチャのピクセル差分で機械判定する:

1. 変更前のビルドでプロジェクトを開き `screencapture -x -o -l <windowID>` でキャプチャ（`-o` で影を除外）
2. 変更後に同条件で再キャプチャし、swift小ツール（`NSBitmapImageRep.colorAt` で全画素比較し diffPixels / maxDiff / 差分bbox を出す）で比較
3. 判定基準: **色リファクタ＝差分ゼロ**、**局所変更＝bboxが意図した領域に閉じている**、**hover/押下のみの変更＝静止状態で差分ゼロ**（押下の見た目だけユーザーに目視依頼）

- **タイトルバー帯（@2xで上約62px）は比較から除外する**。macOS側の合成（背後との透け・フォーカス状態）で毎回変わり、アプリ描画と無関係な差分が出る。クロップしてから比較する
- 差分ゼロを期待して大きな差分が出たら、先に「同じバイナリか」（プロセス起動時刻 vs バイナリmtime）と「プロジェクトファイルが変わっていないか」（ユーザーが保存した等）を疑う

## FXパネル（左ラック・下部詳細・ノブ）の見た目確認

FX周りはトラック画面と別の文法（Illustrated hardware × 1FX 1色。`docs/design/ui-principles.md`）で描く。
ノブ・地・メーター窓・ラベル・ラックLEDを触ったら全FXを一巡して崩れを見る:

```sh
tools/fx-snapshots.sh <outdir>            # 8枚（eq/comp/sat/lofi/reverbA/reverbB/delay/limiter.png・2200×1400）
# 全行 snapshot_ok=1 を確認。見比べは <outdir>/*.png を2列グリッドのHTMLに並べて agent-browser で1枚に撮ると速い
```

- 見るポイント: ①各FXの線（カーブ/GR/倍音）・ノブの点灯目盛り・ラックLEDが `Theme::fxHue` の固有色になっている ②地・ノブ本体・ラベル・タイトルは全FX共通（固有色が漏れていない） ③Limiter の LUFS/相関/True Peak は意味色（緑黄赤）のまま ④Instrument スロットにLEDが無い
- **再生中CPUの前後比較は `top` で採る**（`ps -o %cpu` は起動からの累積平均で単調増加するため瞬間値の比較に使えない）:
  `open -g <app> --args --open <proj> --comp-editor --comp-demo --play; sleep 12; top -l 7 -s 5 -pid $(pgrep -x LaLa-dev) -stats cpu | grep -E '^[0-9]+\.[0-9]+$' | tail -6 | sort -n` → 中央値で比較（2026-08-20 基準: 約58%）
- ミキサー（`MixerOverlay`）のストリップにも同じ `SlotPill` が載る。起動フックが無いので M キーで開いてLEDと固有色を目視する

## アプリを起動せずに描画を確認する（オフスクリーン・スナップショット）

実機が使えない（ユーザーが操作中・未保存変更あり・別Space）ときは、`daw_tests` に一時的な描画コードを足してコンポーネントの `paint*` を直接 `juce::Image` へ描き、PNGで目視する。アプリ起動もフォーカス奪取も不要:

```cpp
// Tests/TestsMain.cpp に一時追加（確認後に必ず削除）
juce::Image img (juce::Image::ARGB, (int) (W * scale), (int) (H * scale), true);
juce::Graphics g (img);
g.addTransform (juce::AffineTransform::scale (scale)); // scale=2でRetinaの見え方を再現
g.fillAll (Theme::windowBg);
IconButton btn (icon, "probe"); btn.setBorderless (true); btn.setBounds (b);
btn.setToggleState (on, juce::dontSendNotification);
{ juce::Graphics::ScopedSaveState s (g); g.setOrigin (b.getX(), b.getY());
  btn.paintButton (g, hover, down); }   // hover/押下/ONを状態ごとに描き分けられる
juce::FileOutputStream out (file); juce::PNGImageFormat().writeImageToStream (img, out);
```

- **必ず2xで描く**。1x画像では細線（線画アイコンの1.1px等）が潰れて判断できない
- **罠: `img.getWidth()` はスケール後のピクセル数**。レイアウト計算にそのまま渡すと論理座標を外れて何も描かれない（`img.getWidth() / scale` を使う）
- 拡大版は `img.rescaled (w * 3, h * 3, juce::Graphics::lowResamplingQuality)`（nearest。補間するとアンチエイリアスの評価ができない）
- レイアウトを再現するときは `resized()` と同じ計算式をそのまま写す（間隔・区切り線の検証になる）
- キーボードフォーカス依存の見た目（`hasKeyboardFocus`）はこの方法では再現できない。実機で確認する
- 確認後は一時コードを削除し、`git diff Tests/TestsMain.cpp` が空になることを確認する

**ビュー丸ごと（タイムライン等）を撮るときは `paintEntireComponent`**: `TimelineView` のようにViewport＋子コンポーネントで組まれた画面は、`paint()` を呼んでも中身（レーン・クリップ）が描かれない。`view.setBounds(...)` → `view.refresh()` → `view.paintEntireComponent (g, false)` で子まで含めて描ける。プロジェクトを開く操作（合成ダブルクリック）が要らないので、ユーザーが別アプリで作業中でもクリップ描画の検証ができる（リージョンゲインの波形スケール・dBバッジの確認に使用）。

```cpp
// daw_tests に TimelineView を含める必要がある（CMakeLists.txt の daw_tests ソース一覧へ
// Source/ui/TimelineView.cpp を一時追加 → 確認後に戻す）
TransportState transport; transport.sampleRate.store (48000.0);
Project project;  /* Clip を組んで clip.buildPeakCache() を忘れない（波形が出ない） */
TimelineView view (transport);
view.setProject (&project);
view.setBounds (0, 0, 900, 3 * TimelineView::trackHeight + TimelineView::topHeight);
view.selectItem (1, 0, false);   // 選択時のみ出る装飾（ループハンドル等）も撮れる
view.refresh();
view.paintEntireComponent (g, false);
```

- 出力先は環境変数で受けて `if (getenv("...") != nullptr) { render(); return 0; }` と main の先頭で分岐させると、通常のテスト実行を汚さずに撮れる
- **選択中のみ出る装飾を複数ケース並べたいときは「ケースごとに描いて該当行だけ切り出し、1枚へ縦に積む」**。選択は1つしか持てないので、7ケース＝7回描いて `cg.drawImage` で行を貼る（画像1枚で全ケースを見られる）
- **罠: 同じ `Graphics` に `paintEntireComponent` を2回呼ぶと1回目が描かれない**（`ScopedSaveState` ＋ `setOrigin` で位置をずらしても、その領域が空のまま残る）。状態違いを1枚に積むときは上と同じく、ケースごとに**別 `Image`** へ描いてから `drawImageAt` で貼る

**ドラッグ中の見た目とドラッグのロジックは、レーンへ合成MouseEventを送って確認できる**（実機・合成マウス不要。フェードハンドルのドラッグ検証に使用）:

```cpp
// lanes は private なので Viewport 経由で取り出す
juce::Component* lanes = nullptr;
for (int i = 0; i < view.getNumChildComponents(); ++i)
    if (auto* vp = dynamic_cast<juce::Viewport*> (view.getChildComponent (i)))
        lanes = vp->getViewedComponent();

const auto source = juce::Desktop::getInstance().getMainMouseSource(); // 参照で受けると束縛エラー
auto ev = [&] (juce::Point<int> pos, juce::Point<int> down)
{
    return juce::MouseEvent (source, pos.toFloat(), juce::ModifierKeys(),
                             juce::MouseInputSource::defaultPressure,
                             juce::MouseInputSource::defaultOrientation,
                             juce::MouseInputSource::defaultRotation,
                             juce::MouseInputSource::defaultTiltX,
                             juce::MouseInputSource::defaultTiltY,
                             lanes, lanes, juce::Time::getCurrentTime(),
                             down.toFloat(), juce::Time::getCurrentTime(), 1, false);
};
lanes->mouseDown (ev (start, start));
lanes->mouseDrag (ev (start.translated (120, 0), start)); // down位置を渡すと閾値判定が効く
view.paintEntireComponent (g, false);                     // ドラッグ中の状態で撮れる（ポップアップも出る）
lanes->mouseUp (ev (start.translated (120, 0), start));
```

- **pass/fail はスクショの目視でなくモデルの値で裏取りする**（例: フェードを上限まで引いて `fadeIn == 全長 - fadeOut` かつ相手が変わっていないこと、ループを縮めてから同じジェスチャー内で戻すとフェードが元の長さへ復元されること）
- 1ジェスチャー内で複数の `mouseDrag` を送れるので、「縮めてから戻す」ような**経路依存のバグ**（現在値クランプ vs 元値クランプ）もここで捕まえられる

**操作系ウィジェット（ボタン・コンボ）もオフスクリーンで駆動できる**（GachaPanelView のタブ切り替え・カード選択バグの実証に使用。対象の .cpp を daw_tests へ一時追加 → 確認後に削除、の流儀は上と同じ）:

- `MessageManager::runDispatchLoopUntil` はこのビルドでは使えない（modal loop 無効）。Button は `triggerClick()`（非同期）でなく public な `onClick()` を直接呼ぶ
- private な子ウィジェットは再帰 `getChildComponent` ＋ `dynamic_cast<juce::TextButton*>` 等で外から特定できる（`getButtonText()` で照合）
- ComboBox は `setSelectedId (id, juce::sendNotificationSync)` で onChange まで同期実行される。「onChange が変更前の状態を読んでいる」系の配線バグはこれで再現・修正実証できる（UI の .cpp は常設テストに含めない設計なので、この一時ハーネスが配線の唯一の検証手段）

## サンプルレート自動追従の確認（CLI）

プロジェクトを開くとデバイスSRがプロジェクトSRへ自動で合わせられる（`audio.device.rate_change` がログに出る）。
デバイスSRはCoreAudio APIを叩くswift小ツールでCLIから読み書きできる:

```sh
# setsr.swift: デフォルト出力デバイスの nominal sample rate を読み書き（引数なし=表示、引数あり=設定）
# kAudioHardwarePropertyDefaultOutputDevice でデバイスIDを取り、
# kAudioDevicePropertyNominalSampleRate を AudioObjectGet/SetPropertyData で読み書きするだけ（各Double・scopeはGlobal）
swiftc -o /tmp/setsr setsr.swift
/tmp/setsr 44100   # 44.1kに設定（システム全体に効く。sleep 0.5後に読み返して確認）
```

確認手順: アプリ終了 → `setsr` でプロジェクトと異なるSRに設定 → 起動してプロジェクトを開く →
ログに `audio.device.rate_change sr=<プロジェクトSR>` が出て `setsr` の読み値も変わること。

- **JUCEは入出力が別デバイスだとSR未指定オープン時に44100以上の最初のレートを勝手に選ぶ**（combiner経由）。
  そのため44100のプロジェクトでは自動追従が動いたのかJUCEのデフォルトなのか区別できない。
  検証は**48000のプロジェクト**（`"sampleRate": 48000.0` のテストプロジェクトをCLIで作る）×44.1kデバイスで行う
- 自動追従は1デバイスにつき1回だけ（設定画面でのユーザー手動変更と戦わないため）。デバイスが替わるとやり直す

## バウンス（書き出し）の確認

レンダリング品質（ミックス・クリッピング保護・DLSテール）はCTestの `BounceRenderer *` テストが担う。アプリ統合の確認:

```sh
# メニューから駆動（メニューのAXPressはアプリが別Spaceにあっても効く）
osascript -e 'tell application "System Events" to tell process "LaLa-dev" to click menu item "書き出し…" of menu "File" of menu bar 1'
```

- 保存パネルはリモートビュー（openAndSavePanelService）のため**AXでボタンが見えない**が、ネイティブパネルにはキー合成が効く → Return（=Save）で確定できる（JUCE本体ウィンドウにキー合成が効かないのと対照的）
- 上書き確認アラートの「Replace」は破壊的ボタンで**Returnでは押せない** → 新しいファイル名で保存するのが確実
- 裏取りはログ: `bounce.start`（target/sr/endSample/tracks/tail）→ `bounce.done`（samples/peak/scaled）。キャンセル=`bounce.cancelled`・失敗=`bounce.failed`。ミュート/ソロの反映は `bounce.start` の tracks数とendSampleで判定できる
- 成果物検証: `afinfo <出力.wav>` で 2ch/24bit/プロジェクトSR・長さ（=endSample＋テール）を確認。**~/Desktop はTCCでsandboxから読めない**（afinfoが `AudioFileOpenURL failed` になる）→ Finder経由でコピーしてから検証する:
  `osascript -e 'tell application "Finder" to duplicate file (POSIX file "/Users/.../out.wav") to folder (POSIX file "/tmp/dir") with replacing'`
- 一時ファイル（出力先ディレクトリの `.<name>.wav.f32.tmp` / `.<name>.wav.tmp`）が完了・キャンセル後に残っていないこと
- v17以降: `bounce.done` に `lufs=` / `tpDb=`（出来上がったファイルのIntegrated LUFS / トゥルーピーク）が載り、完了オーバーレイの2行目にも同じ値が出る

## Master Limiter・マスターメーター（LUFS/相関/TP）の確認

DSP・計測の正しさはCTest（`MasterLimiter *` / `loudness meter standard` / `master meter pipeline` 等）が担う。アプリ統合の確認:

```sh
# dev版フックでLimiterエディタ（マスターメーター群）を開いてスナップショット
open -g build/daw_artefacts/Debug/LaLa-dev.app --args \
  --open "$HOME/Music/daw/<プロジェクト>" --limiter-editor --play --snapshot /tmp/limiter-ui.png
```

- スナップショットで確認: ノブ3（GAIN/CEILING/RELEASE）・GR縦バー・LUFS横バー（-14/-9ライン・integrated▲マーカー）・相関バー・数値3つ（SHORT TERM / INTEGRATED / TRUE PEAK）が再生に追従して埋まる
- LUFS/TPの数値照合は `scripts/check-loudness.sh [wav...]`（ffmpeg ebur128と突き合わせ。引数なしは合成信号2本）。**クリック列のような単発インパルス素材はTPが0.5dB超割れることがある**（帯域制限補間のフィルタ長依存。スクリプト冒頭の注記参照）
- レンダー回帰は `scripts/check-render-hashes.sh compare`（Limiter導入後の基準: bounce系は素材が天井以下なら不変・engine系は2ms遅延込み）

## キー操作の確認（要ユーザー操作）

JUCEアプリには合成キーストロークが届かないため、ショートカットは実操作で確認する:

1. Space = 再生/停止、`r` = 録音（MIDIトラック選択中は無効）、`m` = 選択トラックのミュート
2. `,`/`.` = 1拍シーク（拍の途中なら拍頭へ）、`Shift+,`/`.` = 1小節シーク、`⌥,`/`⌥.` = 前/次のセクション頭へシーク（マーカーなしはno-op）
3. `⌘←`/`⌘→` = 横ズームアウト/イン（ピンチも可）。ズームインでグリッドが 拍 → 1/8 → 1/16 と細かくなり、クリックシークがその単位になる
4. `⌘S` = 保存、`⌘O` = プロジェクトを閉じて選択画面へ（未保存なら3択ダイアログ）、`⌘⌥A` = オーディオトラック追加、`⌘⌥S` = ソフトウェア音源トラック追加、Delete = 選択クリップ/リージョン/ノート削除
5. `⌘Z`/`⇧⌘Z` = undo/redo（構造編集のみ。音量・ミュート・ソロは対象外）
6. ピアノロール（リージョンをダブルクリックで開閉）: `↑`/`↓` = 選択ノートを半音移動、`⌥↑`/`⌥↓` = オクターブ移動、`⌘C`/`⌘V` = ノートコピー/再生ヘッド位置に貼り付け
7. 下部エリア（ピアノロール／FX詳細）の `E` = 表示/非表示・`[`/`]` = 履歴の戻る/進む:
   - `E` で閉じ、もう一度 `E` で**同じ中身**が戻る（`×` で閉じた後も同じ）
   - ピアノロール → FXパネル（`I`）のサンプルスロット → 別リージョン、と3点開いてから `[` `[` で最初まで戻り、`]` `]` で戻った先まで進める
   - FX詳細に戻ったとき、左FXパネルの表示チャンネルとハイライトされたスロットが一致し、本文（サンプル波形・GAIN等）も出ている
   - 履歴移動でトラック選択（ヘッダーのハイライト・`R` の録音対象）が**変わらない**
   - 履歴にあるリージョンを削除してから `[` で辿ると、そのエントリを飛ばして次の有効なものへ行く（`bottom.history` のログに `pos=` が出る）
   - FX詳細を開いたまま別トラックを選択して追従させ、`E` を2回押すと**追従後**（いま見えている方）が戻る（履歴カーソルは1件も消費されない）
   - トラック追加オーバーレイ表示中は `E`/`[`/`]` が背後に効かない
   - ログ（`~/Library/Logs/daw/`）に `bottom.toggle` / `bottom.history` が出るので、キーが届いたかの裏取りに使える
8. `⌘N` = 右ドックのメモを開閉。開くとメモ欄にフォーカスが入るが、そのまま `⌘N` をもう一度で閉じられる（⌘付きなのでTextEditorに消費されない）こと。メモ表示中に `F` → `⌘N` でメモへ戻れること
9. `F` = 右ドックのファイルパネルを開閉。リストの行をクリックしてフォーカスを与えた状態でも `F` で閉じられる（ListBoxは文字キーを消費しない）こと。ファイル表示中に `⌘N` を押すとメモに切り替わり、プレビュー再生が止まること
10. `⌘?`（`⌘/` でも可）= ショートカット一覧オーバーレイの開閉。表示中は Esc/⌘? 以外のキーが効かない（Spaceで再生が始まらない）こと、パネル外クリックでも閉じること

## ショートカット表示の確認

- ボタンのホバーツールチップ: 再生「再生/停止 (Space)」・録音「録音 (R)」・歯車「オーディオ設定 (⌘,)」のようにショートカットが併記される（ツールチップ表示にはマウスホバーが必要。合成マウスで確認する場合はカーソル移動後1秒程度待ってから領域スクショ）
- 右クリックメニュー: リージョン/クリップのメニューに ⌃M（ミュート）・⌘T（分割）・Delete（削除）、トラックヘッダのメニューに ⌘Delete が項目右側に表示される（メニューはCtrl+左クリック合成で開ける。上記「リージョン操作の検証」参照）
- 表記の変更・追加はすべて `Source/ui/Shortcuts.h` のテーブル経由（CLAUDE.md「ショートカットキーの追加ルール」）
- **追加したキーの衝突チェックはCLIで機械判定できる**（実操作の前にここまで自走する）。`Tests/TestsMain.cpp` に一時関数を足し、テーブル全走査で「そのKeyPressにマッチする項目がちょうど1件」を確認する。同じ一時関数の中で `ShortcutListOverlay` をオフスクリーン描画（上記「アプリを起動せずに描画を確認する」）すれば、⌘?一覧に載る位置とレイアウト（カテゴリの行数増でパネル高が自動追従する）も同時に見られる。確認後は一時コードを削除して `git diff Tests/TestsMain.cpp` が空になることを確認する
  ```cpp
  int hits = 0;
  for (const auto& e : Shortcuts::table) if (e.matcher (cmdN)) ++hits;   // hits == 1 なら衝突なし
  ```
- キーの選び方（修飾なし1文字にできるか・入力欄にフォーカスがあっても効くか）は GOTCHAS.md「修飾なし1文字のショートカットは『そのとき誰がフォーカスを持つか』で可否が決まる」を参照

## 音が絡む確認（要ユーザー操作）

**「音が変」の切り分けは、耳の前に数値で行う**（推測で実装を変える前にここまで自走する）:

1. 実素材（`~/Downloads` の購入サンプル等）をPythonで読み、**エンジンと同じ規則**で合成してピークを測る
   — 再生レート = `sourceRate / deviceRate`・ボイスゲイン = `sampleGain × velocity/127`・重なりは加算・
   最後にトラックゲイン（既定0.8）とMaster。1.0を超えていれば出力段のクリップ（歪み）が原因
2. 超えていなくても、**音程のあるサンプルが重なるとうなり（beating）**が出る（同じ波形が時間差で足されると
   周期的に強め合う/打ち消し合う）。音量を下げても消えないのでクリップと区別できる
3. 切り分けたら「**単発 / 現状 / 音量半分 / 対策後**」の4本をWAVに書き出してユーザーに聴かせる。
   単発→現状で症状の再現、現状→音量半分で歪み分の切り分け、音量半分→対策後で残りの原因が確定する

実例（2026-07サンプル音源）: 1.71秒の808を16分（167ms間隔）で連打 → 最大10発重なり合計ピーク**1.176**（+1.4dB
オーバー）。0.05秒のハイハットは重ならず全打点0.56で正常 → 「長い＋音程あり＋速い連打」固有と判明した。

1. クリックトグルON → 再生 → BPMに合ったクリック音（小節頭は高いピッチ）が鳴る
2. 録音ボタン → 1小節のカウントインが鳴ってから録音が始まり、歌った内容がクリップ波形に出る
3. 重ね録り: 同じ小節に2クリップ置いて再生 → 両方鳴る（加算再生）
4. ミュート/ソロ/音量スライダーが再生中に即反映される
5. オーディオ設定（右上の歯車ボタン。AX名「オーディオ設定」、`⌘,` でも開く）でオーディオインターフェースに切り替えて録音できる
6. サンプルレート不一致の警告と録音ブロックは、自動追従（上記セクション）が効かないケースでのみ出る（デバイスがプロジェクトSR非対応・設定画面で手動でSRをずらした場合）

## 移調・タイムストレッチ（クリップの非破壊移調/伸縮）の確認

```sh
# 実機確認用プロジェクトは seeder で生成（冪等）: 440Hz 2秒を +2半音/1.25倍で敷いたもの。
# 開いて数秒でクリップが 0.75→0.9375小節へ伸び、+2 バッジが出れば読込→一括再生成→装着まで正常
bash scripts/seed-stretch-test.sh
open -g build/daw_artefacts/Debug/LaLa-dev.app --args \
  --open ~/Music/daw/0-0-stretch-test --snapshot /tmp/stretch-check.png
sleep 8 && grep -E "project.open|render_failed" ~/Library/Logs/daw/"$(ls -t ~/Library/Logs/daw | head -1)"

# ① モデル・レンダラーの回帰は CTest に集約済み（GUI 不要）
#    - ClipStretcher render: ピッチ(+12→2倍)・長さの厳密一致・固定シード・短チョップ・異常値の拒否
#    - stretch domain math: chain 変換のフェード・分割境界・退化view・ピーク再集計
#    - render cache pipeline: 要求/装着/失敗巻き戻し/undo/キャッシュヒット/寿命
#    - stretch split -> save -> reload: 分割→保存→再読込のビット一致（固定シードの実地）
./build/daw_tests_artefacts/Debug/daw_tests 2>&1 | grep -E "stretch|ClipStretcher|render cache"
```

- GUI 側の操作: クリップ右クリック →「移調・伸縮…」。移調スライダー（±12半音）＋小節数入力
  （現在の見かけ長を丸めず表示・編集するまで伸縮しない）＋倍率併記（0.9〜1.1倍の外は色が変わる）
- 移調が0以外のクリップは右上に `+2` バッジ（ゲインバッジの左）。伸縮は長さ自体が変わる
- 値の変更直後は**古い音のまま**鳴り、レンダー完了時に音・長さ・波形が一斉に切り替わる（正常動作）。
  処理中は「分割」「複製」がグレーアウトする
- ログでの裏取り: `clip_stretch.request`（要求）/ `clip_stretch.render_failed`（失敗。トーストと
  値の巻き戻し＋dirty化を伴う）
- 保存値は project.json の clips[].transposeSemitones / stretchRatio / renderDomain*（v20）。
  加工済みWAVは書かれない（読込時に固定シードで再生成 = 再起動後も同じ音）

## ボーカルのピッチ補正（独立ウィンドウ）の確認

```sh
# ① モデル・検出・レンダーの回帰は CTest に集約済み（GUI 不要・合成音のみ。実声はコミットしない）
#    - PitchAnalyzer YIN vs known f0: GPE 0%・voicing P≥0.95/R≥0.90・微小誤差 <5 cent
#    - PitchSidecar / PitchNotes / PitchAnalysisWorker: 世代不変・検証・分割規則・generation 着地
#    - TimeMap / PitchCorrection timeMap / split / merge / targetCurve / autoSnap+detach
#    - VocalResynth (WORLD): 目標との差 中央値 <20 cent（A/C/B/D）・長さ不変・ビット一致
#    - pitch correction project v21: 保存→再読込ビット一致・分割の親子 digest 一致・サイドカー欠損の無効化
./build/daw_tests_artefacts/Debug/daw_tests 2>&1 | grep -E "Pitch|TimeMap|VocalResynth|pitch correction|FAILED|passed"

# ② 実アプリ（dev 版限定の検証フック。実機能と同一経路）。対象プロジェクトは ~/Music/daw をコピーして使う
#    --pitch-editor <track> <clip> でエディタを開き、--pitch-action を解析完了後に順に実行、
#    --pitch-snapshot でウィンドウ中身を PNG（ログ debug.pitch_snapshot に mode/notes/preview/corrected/dirty/visible）
#    action: enable / bypassN / moveN（+40ms）/ targetN:+M / kero / resnap / apply / cancel / reset / reanalyze / close / save / undo / bounce
SP=/tmp/pitch-check; rm -rf "$SP"; mkdir -p "$SP"; cp -R ~/Music/daw/2026-08-18-tundra "$SP/tundra"
run() { pkill -x LaLa-dev; sleep 1; before=$(ls -t ~/Library/Logs/daw | head -1)
  open -g build/daw_artefacts/Debug/LaLa-dev.app --args --open "$SP/tundra" "$@"
  for i in $(seq 1 90); do sleep 1; f=~/Library/Logs/daw/$(ls -t ~/Library/Logs/daw | head -1)
    [ "$(basename $f)" = "$before" ] && continue; grep -q "debug.pitch_snapshot" "$f" && break; done
  grep -E "pitch\.|debug\." "$f"; }
# 補正なしで開く → 解析 → 未確定プレビュー（mode=2 corrected=0 dirty=0。開いた直後は Strength 0% なので preview=0＝原音のまま）
run --pitch-editor 2 0 --pitch-snapshot "$SP/1-preview.png"
# Enable → バイパス → 横移動 → 目標 +2 → 保存（mode=3 corrected=1 dirty=0。project.json v21・clip-008.<digest>.pitch）
run --pitch-editor 2 0 --pitch-action enable --pitch-action bypass1 --pitch-action move3 --pitch-action "target2:+2" --pitch-action save --pitch-snapshot "$SP/2-edited.png"
ls "$SP/tundra" | grep pitch; python3 -c "import json; p=json.load(open('$SP/tundra/project.json')); print(p['version'], [c.get('pitchCorrection') is not None for t in p['tracks'] for c in t.get('clips',[])])"
# 保存済みを開き直す: 再解析せず確定状態のまま（pitch.analyze が出ない・mode=3）
run --pitch-editor 2 0 --pitch-snapshot "$SP/3-reopen.png"
# 未確定中に ⌘B: プレビューを破棄して閉じる（pitch.preview_discarded → pitch.closed・visible=0・dirty=0）
run --pitch-editor 3 0 --pitch-action bounce --pitch-snapshot "$SP/4-bounce.png"
# Enable → undo: 補正が消えエディタが閉じる（edit.undo → pitch.closed・corrected=0）
run --pitch-editor 3 0 --pitch-action enable --pitch-action undo --pitch-snapshot "$SP/5-undo.png"
pkill -x LaLa-dev
```

- GUI 側の操作: オーディオリージョン右クリック →「ピッチ補正…」（有効なら「有効」併記）。上部バーは左＝音の決め方
  （Scale・鍵盤アイコン（スケール表示）｜Strength・Strength〔off〜full〕・Speed〔hard〜natural。ケロケロ = Strength full × Speed hard〕）。Scale を選ぶ＝そのスケールで付け直すプレビュー（元の目標は点線ゴースト・右端 Apply/Cancel）／右端＝確定系（未確定中だけ Enable、変更プレビュー中だけ Apply/Cancel。確定後は何も出ない）。
  Reset（自動スナップからやり直す）・再解析・推定キーの設定はグリッドの右クリックメニュー。キー未設定時はバナーに推定キーと「Use Dm」ボタン
- ブロブは帯型（太さ＝音量・中心＝補正後ピッチ・塗り＝移動量で青→暖色・目標の段は枠だけ・元ピッチは細線）。無声（息・子音）は下端の細いレーンに灰色で音量だけ
- 上下ドラッグ中はその音が新しい目標音で鳴る（`VocalNoteAudition`: ドラッグ開始時にノート範囲だけ WORLD で分解・半音ごとに合成。メイン再生中は鳴らない。ログ `pitch.drag_audition_prepare`）
- ブロブ: 上下ドラッグ＝目標音（半音）／横ドラッグ＝タイミング（隣接が吸収・1/16 スナップ・⌥で解除・吸収区間に斜線＋ゴースト）／
  クリック＝単独試聴（メイン再生中は何もしない）／ダブルクリック or B＝バイパス／⌘クリック＝分割（⌘を押してブロブに乗ると分割位置の点線が出る。右クリック「ここで分割」でも可）／隣接2つを Shift クリックで選んで M＝結合
- Scale の右の鍵盤アイコンでスケール音の段のハイライトを ON/OFF（キー未設定なら推定キー）。Re-snap/Reset/Re-analyze の結果は上部バー右に数秒表示（「結果は同じ」等）
- ズーム: ⌘←/→（ウィンドウ内ではタイムラインでなくエディタが拡大）・トラックパッドのピンチ・⌘＋ホイール（カーソル中心）。スクロール: 横スワイプ／ホイール
- 補正が鳴っているクリップは `♪` バッジ（移調バッジの左）。Space・⌘Z・, . はエディタにフォーカスがあってもメインに効く
- 人間確認が要るもの: 音質（補正の自然さ・ケロケロの「らしさ」）とデュアルモニターでの使用感、ドラッグの掴みやすさ

## リリース・アップデート（Sparkle）の確認

- **メニュー疎通（AIで自動確認可）**: アプリメニューに `Check for Updates…` が出る。ネイティブメニューなのでAXでクリックできる（JUCEのPopupMenuと違いCGEvent合成は不要）:
  ```sh
  osascript -e 'tell application "System Events" to tell process "LaLa-dev" to click menu item "Check for Updates…" of menu 1 of menu bar item 2 of menu bar 1'
  grep "update.check_started" ~/Library/Logs/daw/"$(ls -t ~/Library/Logs/daw | head -1)"   # 着弾の裏取り
  ```
  dev 版は SUFeedURL 未設定なので、クリック後に Sparkle のエラーダイアログが出るのが正常（出たら手で閉じる）
- **バージョン表示（AIで自動確認可）**: 出る場所は2つ。①プロジェクト選択画面の `PROJECTS` 行の右端に `v<PROJECT_VERSION>`（背面キャプチャで目視） ②アプリメニュー `About LaLa-dev` の標準パネル。パネルはタイトルなしウィンドウとして出るので、CGWindowListで**名前が空のonscreenウィンドウ**を拾って撮る:
  ```sh
  osascript -e 'tell application "System Events" to tell process "LaLa-dev" to click menu item "About LaLa-dev" of menu 1 of menu bar item 2 of menu bar 1' -e 'delay 1.5'
  screencapture -x -o -l <windowID> /tmp/about.png   # 「Version 0.7.0」が1つだけ（括弧の二重表示がない）こと
  osascript -e 'tell application "System Events" to tell process "LaLa-dev" to click button 1 of window 1'   # 閉じる
  ```
  メニューの並び自体は押さずに読める: `get name of menu items of menu 1 of menu bar item 2 of menu bar 1`（`menu bar item 1` はAppleメニュー・`2` がアプリメニュー）
- **署名の確認（clean build 直後に行う）**: インクリメンタルビルドでは順序バグがあっても偶然通るため、`--clean-first` 直後で見る:
  ```sh
  cmake --build build --target daw --clean-first
  codesign --verify --strict build/daw_artefacts/Debug/LaLa-dev.app
  ```
- **配布用再署名の確認（notarize 手前まではAIで自動確認可）**: scripts/build.sh の codesign 部分を実行後、
  `codesign -dvvv --entitlements - /tmp/daw-export/LaLa.app` で ①`flags=...(runtime)` ②`Timestamp=` あり ③entitlements が audio-input のみ（get-task-allow 不在）の3点を確認
- **notarize 以降とリリース本番**: `scripts/release.sh <version>` を**ユーザーの Terminal で**実行（notarytool は Claude Code の Bash から keychain に届かない）
- **アップデート一連の確認**: 低い VERSION でビルドした dmg の .app を `/Applications` にコピーして起動（Translocation 回避）→ Check for Updates → 新版が offer → 更新完走後に `plutil -extract CFBundleVersion raw /Applications/LaLa.app/Contents/Info.plist` と `codesign --verify --strict /Applications/LaLa.app` を確認
- **旧名 daw からの初回アップデート（0.2.x 以前 → LaLa 初版）**: Sparkle は既存 `.app` のパスへ置き換えるため、更新後も `/Applications` のファイル名が `daw.app` のまま残る可能性がある（メニューバー・タイトルバーの表示名は Info.plist 由来で LaLa になる）。残っていたら Finder で `LaLa.app` に手動リネームする（bundle id 不変なので TCC・Sparkle の連続性に影響なし）。この確認は初回リリース時に一度だけ必要

## 外部ツール（分析パイプライン・ガチャ）の確認

アプリ外の Python ツールの回帰。どちらも fixture のみで数秒・ネット不要:

```sh
mise run ref:test     # tools/reference/ の card.py / gates.py / analyze.py / render_report.py / report.py / report.sh（テストファイルごとに OK 行が1行ずつ出る）
mise run gacha:test   # tools/gacha/ の drums.py
```

- 判定: 末尾に `OK: <N> 件のチェックが通った` が出て exit 0。assert で落ちたらメッセージに期待値と実測値が出る（gacha:test は drums / bass の2ファイルで OK 行が2行）
- `gacha:test`（drums 側）は固定 fixture・固定 seed の出力を**既知 SHA-256 で焼き込んでいる**ため、seed 導出式・生成ロジック・wav 合成のどれを変えても落ちるのが正しい（落ちたら「同じファイル名で以前と違う音が出る」変更をした、ということ。意図的なら GENERATOR_VERSION を上げてテスト内の既知ハッシュを採り直す）
- 分析パイプラインの通し確認（進捗行の実出力を見たいとき）: 既存リファレンスの `track.wav` と `stems/` だけを scratchpad の新フォルダにコピーして `tools/reference/.venv/bin/python tools/reference/analyze.py <コピー先>` を実行（demucs はキャッシュ skip され1周 約1.5分・元データ無変更）。判定: exit 0・stdout の `==> P% | N/M 完了 ／ 実行中: …` 行の P が単調非減少で 99 以下（書式は shared/AnalyzeProgress.h の parse と対）
- 実カードでの通し確認: `mise run gacha:drums <リファレンスフォルダ> -- --seed <固定値>` → 3ファイル×候補数が `gacha/` にでき、stdout の密度・swing がカードの性格と大きく乖離しないこと（少サンプルの密度は二項ノイズで ±1発/小節 程度ぶれる。乖離を疑うときは 1000 seed のシミュレーションで発火率 vs profile を見る — plan 2026-08-04 のログ参照）

### 好きなビートcorpus（taste）

```sh
mise run taste:test
mise run taste:sync --dry-run
mise run taste:analyze --dry-run
mise run taste:grid-followup
mise run taste:review-followup
```

- 判定: `taste:test` がexit 0、syncが`analyzed=23 failed=0`、analyzeが全23曲`unchanged`。cleanup済み曲でdownloadが再発したらmaterialization/provenanceの回帰
- 横断成果物: `docs/labs/reference-beat-taste-data.json` に絶対path/NaNが無く、`~/Music/daw/reference-beat-corpus/analysis/machine-draft.json` の各viewで距離表が参加曲数の正方行列・対称・対角0・finite
- grid契約: manifestの`auto_verified_4_4`曲は`source.wav`不在＋`disposed_after_verified`、`needs_review`曲はsource保持。後者だけ`review/grid/<video-id>/`に先頭／中盤／終盤の3 WAVがある
- grid再確認: 記入済み`review/grid/README.md`の`NG`にはA〜D各3本、`?`にはlouder 3本が`review/grid-followup/`へ生成される。全WAVが2秒以上・RMS非無音・peak 0dBFS以下で、NGのA〜Dが同一波形でないこと
- Phase 5素材: `review/phase5/manifest.json` の全WAVが1秒以上・RMS非無音・peak 0dBFS以下。群なしviewも距離地図の中心／両端素材がある
- Phase 5追加素材: 初回耳確認後だけ`taste:review-followup`を実行し、`review/phase5-followup/`に14 WAV
  （FALL IN LOVE固定timeline 1、上モノ1、drum audio 4、bass/top-bass 8）が生成される。全件1秒以上・
  RMS非無音・peak 0dBFS以下で、元の`review_materials` provenance hashが不変

### ベースガチャ（CLI）

```sh
# ドラム候補を1件作り、そのサイドカーを --drums に渡してベース＋mixを生成（耳チェックの最短経路）
mise run gacha:drums <リファレンスフォルダ> -- --seed 100 --count 1 --bars 4
mise run gacha:bass <リファレンスフォルダ> -- --key F#:major --seed 200 --count 4 \
  --drums <リファレンスフォルダ>/gacha/drums-*.json
```

- 判定: `gacha/` に `bass-p*-r*-<hash>` の3点セット×候補数と、候補ごとの `*.mix-<drumsID>.wav`（使い捨て）ができる。mix を再生してドラムとベースが同じ小節境界で鳴っていること
- chords ありのカード（my-way 等）はルートが動き、chords 無し/変化0（kzm・watson）はルート連打へ退化する（stderr の `[退化]` 申告）
- キー・実効BPM・キック位置のどれかを変えると候補ファイル名（設定ハッシュ部）が変わる＝別候補として共存する

## ベースガチャ（LaLa 統合）の確認

- 前提: card.json のあるプロジェクト・プロジェクトキー設定済み（ヘッダーLCDの KEY をクリック）
- ガチャパネル（🎲）上部の Drums / Bass タブでパーツを切り替える。カード・候補・ロックはパーツ別
- 手順: Drums で候補を仮配置 → Bass タブ → 振り直す → 候補クリックで**ドラムと同じ開始位置**に仮配置（Bass トラック自動作成・Finger Bass）→ どちらかの行の「仮配置中」チップ →「ビートを残す」で全パーツ一括確定（⌘Z 1回で両方戻る）
- キー未設定で Bass を振ると下部ガイドに「キーが未設定です—」が出て何も起きない（ログ `gacha.bass_roll_blocked reason=no_key`）
- ログの裏取り: `gacha.roll part=bass key=<F#:minor等> bars=<N> kicks=<M>` → `gacha.pick part=bass` → `gacha.keep`。ドラム延長が走ったときは `gacha.extend bars=<N>`
- Bass のカード変更は Bass の仮配置だけ撤去する（Drums の仮配置は残る。ログ `gacha.cancel_part part=bass`）
- 1小節ベース（watson 等）＋4小節ドラムの組で、仮配置されたベースリージョンが4小節（パターン繰り返し）になり後半が無音でないこと

## ループ検索ガチャ（Loops タブ）の確認

- 前提: ライブラリ整備済み（`mise run lib:setup` → パック配置 → `mise run lib:index`）＋分析済みリファレンスのあるプロジェクト。検証用は `~/Music/daw/loop-lab`（リファレンス5曲分析済み）
- ガチャパネル（🎲）→ Loops タブ → カード選択 →「✨ おすすめを出す」→ 候補10行＋「候補 N本中 x〜y位」。初回は上モノ特徴量の計算で数秒かかる（2回目からキャッシュ）
- **パネルを開いたまま**「リファレンスとして分析…」を完了させても、カード一覧に新しいリファレンスが現れる（開き直し不要）
- 行クリック＝試聴のトグル（▮▮表示）。「次の10本 →」でページ送り。2ページ目は行の順位表示が「11.」から始まる（ページ情報と一致）。ログ `gacha.loop_recommend ref=<名前> page=<N> total=<M>` / `gacha.loop_audition file=<wav>`
- 右端の ✓ → **ダイアログは即出る**（進行検出はダイアログ表示中にワーカーで並走。読み終わる前に「設定して採用」を早押しした場合だけパネルに「進行を検出中…」が出て、完了後に自動で配置される）。確定後は「配置しています…」→ 反映
- ✓ → ダイアログ「設定して採用」→ **その場で確定**: 1小節目に**ループ名の専用トラック**（採用のたび新規。同名は連番）＋ループ（2周）・クリップは即 `clip-NNN.wav` に実体化・LCD がループのキー/BPM に変わる・タブ直下に ⚓ アンカーカード（**全タブ共通**。名前＋キー/BPM/小節数のチップ。Bass タブでは追従チップが濃色になる。**ホバーのツールチップで進行のルート列・検出信頼度まで見える**）。「ビートを残す」は不要で、⌘Z 1回で採用ごと戻る。ログ `gacha.loop_pick loop=<相対パス> applyKeyBpm=1` → `gacha.keep`
- **ベースの自動追従**: アンカーがある状態で Bass を振ると、ログが `gacha.roll part=bass key=<ループのキー> roots=anchor` になる（カード駆動は `roots=card`）。生成キーはプロジェクトでなく**アンカーのキー**
- ⌘S → プロジェクトを開き直して Bass を振り直しても `roots=anchor` のまま（アンカーの再読込追従）
- ⌘Z 1回で採用前（トラック・BPM・キー・⚓）まで全復元。⚓行の「解除」→ ⚓ だけ消える（クリップ・BPM は残る）→ ⌘Z で復活。ログ `gacha.anchor_release`
- 副作用チェック: 新規プロジェクトで ✓ → キャンセル → ウィンドウを閉じても保存確認が出ない（キャンセルだけで SR/dirty が残らない）
- ツール・index の無いマシンでも Loops タブは開ける（「おすすめを出す」だけ無効・⚓ の解除は可能）

## 分析レポートの生成・閲覧の確認

### 閲覧（レポートウィンドウ）

- 前提: `references/<名前>/` に card.json と report.md があるプロジェクト。実レポートが無ければ `cp tools/reference/report-example.md ~/Music/daw/<プロジェクト>/references/<名前>/report.md` で疑似的に用意できる
- dev版 → ガチャパネル（🎲）→ カード選択 → 「📄 レポートを開く」→ 別ウィンドウにダーク配色の表・日本語が表示される。ログに `report.open folder=<名前>`
- もう一度押すと閉じる（`report.close source=button`）。再度開いたときに `report.html` の mtime が変わっていなければ HTML キャッシュの再利用が働いている（変換をやり直すのは report.md か render_report.py が新しいときだけ）
- 変換だけの確認: `tools/reference/.venv/bin/python tools/reference/render_report.py <リファレンスフォルダ>` → `report.html` ができてブラウザで開ける（外部参照ゼロの自己完結HTML）

### 生成フロー（トークン消費なしの疑似実行・約10秒）

```sh
CLAUDE_BIN="$PWD/tools/reference/tests/fake-claude-demo.sh" \
  build/daw_artefacts/Debug/LaLa-dev.app/Contents/MacOS/LaLa-dev
```

- report.md が**無い**リファレンスを選ぶ → ボタンが「＋ レポートを書く（約8分）」（破線枠）→ 押すと経過行（`0:04 ｜ （疑似実行）…`）とキャンセルに変わる → 約10秒で完了トースト（右下）→ クリックでレポートウィンドウが開く
- ログの並び: `report.generate.start` → `report.generate.spawn` → `report.generate.end status=2`（2=success）→ `report.open`
- 生成中に ⌘Q →「レポート生成中です」の確認が**未保存確認より先に**出る。キャンセルを選ぶと生成は継続（経過行が進み続ける）、「中断して終了」で `report.generate.cancel reason=close`
- 実 claude での生成（約8分・Claude Code の利用枠を消費）: `CLAUDE_BIN` を付けずに起動したアプリのボタンから、またはターミナルで `./tools/reference/report.sh <リファレンスフォルダ>`（アプリと同じトランザクション・排他が効く）

## Salva（apps/salva/）の確認

### ビルド・テスト・起動

```sh
mise run build:salva   # dev版（Salva-dev.app）
mise run test:salva    # salva_tests（read-aheadの時間整合・BPM逆算・設定JSON）
mise run start:salva
```

### 再生コアのスモーク（音を出さず半自動・dev版のみ）

```sh
# 無音WAVを作る（20秒・48kHz）
python3 -c '
import wave
w = wave.open("/tmp/salva-silent.wav", "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"\x00" * 48000 * 2 * 2 * 20); w.close()'

# --select <start> <end>（サンプル） --autoplay はdev版限定の検証フック。
# 1.0〜3.4秒の2.4秒ループ = 「1小節・100.0 BPM」表示と salva-silent_1bars_100bpm.wav のファイル名が出るはず
open -g build/apps/salva/salva_artefacts/Debug/Salva-dev.app \
  --args /tmp/salva-silent.wav --select 48000 163200 --autoplay
```

- 確認点: ①時刻表示が実時間で進み、選択範囲内で折り返す ②ログ（`~/Library/Logs/salva/salva-*.log`）に `file.open` → `transport.play loop=1` があり `stream.starved` が**出ない** ③選択なし＋`--autoplay` なら終端で `transport.end_of_file` が出て停止する
- UIのスクショは `--snapshot <path>` が確実: `open -g ... --args <file> --snapshot /tmp/x.png` で表示完了後（1.5秒後）のUIをPNG保存する（JUCEのcreateComponentSnapshot。**別Spaceでも失敗せず、ユーザーのフォーカスも奪わない**。`screencapture -l` は別Spaceで "could not create image" になることがある）
- dev版の検証フック一覧（Main.cpp・実機能と同一経路）: `--select <a> <b>` / `--autoplay` / `--stemgroup <n>` / `--separate` / `--export` / `--popup-cache` / `--popup-output`（ポップアップは要前面。`open -g` では即dismiss） / `--switch-output "<name>"`（5秒後に出力切替） / `--snapshot <path>` / `--snapshot-delay <ms>`（撮影を遅らせる。既定1500） / `--record-view`（録音画面を開く。ファイル引数不要） / `--record-start`（録音画面＋実録音開始。**実入力から保存先へ本当に録るので、確認後は生成されたWAVをログの `record.start path=` から特定して削除する**）。再生系は毎秒の実出力ピークログ `debug.outpeak` で音の疎通を裏取りできる。録音画面の見た目確認は `--record-start --snapshot /tmp/x.png --snapshot-delay 7000` で録音6秒経過時点の波形ロール・タイマー・停止ボタンが撮れる
- 設定の永続化: `~/Library/Application Support/salva/settings.json` に recentFiles・outputDevice が入る

### ステム分離・M/S・書き出しのスモーク（dev版のみ・音を出さず半自動）

```sh
# 前提: ~/daw/tools/reference/.venv に demucs 入りvenv（無ければ mise run ref:setup）
# ① アプリ経由の分離（無音20秒なら約10秒で完了）
open -g build/apps/salva/salva_artefacts/Debug/Salva-dev.app \
  --args /tmp/salva-silent.wav --separate
# ログに separate.start → separate.done identity=<hash> が並び、
# ~/Library/Application Support/salva/stems/<hash>/ に manifest.json と runs/<uuid>/ ができる

# ② 6ステム構成でループ再生（--stemgroup 0=4ステム, 1=6ステム）
open -g build/apps/salva/salva_artefacts/Debug/Salva-dev.app \
  --args /tmp/salva-silent.wav --stemgroup 1 --select 48000 163200 --autoplay
# ログに stems.group id=htdemucs_6s / transport.play loop=1。20秒待って stream.starved が出ないこと

# ③ 聴こえている構成の書き出し（settings.json の exportDirectory を先に設定しておく）
open -g build/apps/salva/salva_artefacts/Debug/Salva-dev.app \
  --args /tmp/salva-silent.wav --stemgroup 0 --select 48000 163200 --export
# ログに export.start → export.done gain=1.0000、出力先に <base>_1bars_100bpm.wav（2.4秒・24bit）
```

- separate.sh 単体の契約検証: identityディレクトリに `lock/` をmkdirしてから
  `bash <app>/Contents/Resources/separate.sh <入力> <identityディレクトリ> <venv>/bin/python <SR> <サンプル長>`（SR・長さはアプリが渡す契約。m4a対応のため）。
  検証点は manifest.json の source identity（mtimeMs = st_mtime_ns//1e6 でJUCEと一致）・
  status=complete・全ステムが元音源と同SR・同長・ステレオ・PCM_24・runs/<uuid>/work が消えていること
- キャッシュの排他はテストで固定済み（`mise run test:salva` の StemCache 群）。手動確認するなら
  dev/release 2プロセスで同じファイルの分離を同時に押し、後発が「別プロセスが分離中」トーストになること
