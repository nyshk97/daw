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

- 起動するとプロジェクト選択画面が出る（`~/Music/daw/` のフォルダ一覧＋新規作成）
- マイク権限のplist文言確認: `plutil -extract NSMicrophoneUsageDescription raw build/daw_artefacts/Debug/LaLa-dev.app/Contents/Info.plist`
- **リビルドしてもマイク権限は再要求されない**（POST_BUILD でバンドルリソースを先行コピーした上で Apple Development 証明書により再署名しているため。`--target daw` 単体ビルドや `--clean-first` でも維持される。bundle id を分けた直後の dev 版初回起動のみ再付与が必要）。ダイアログが出たら署名が壊れている兆候なので以下を確認:
  - `codesign -dvv build/daw_artefacts/Debug/LaLa-dev.app 2>&1 | grep Signature` → `Signature=adhoc` になっていたら、configure 時の証明書自動解決が失敗して ad-hoc フォールバックしている（`cmake -B build` を再実行して `Codesign identity:` の STATUS 行と WARNING の有無を見る）
  - 署名の安定性確認: リビルド前後で `codesign -dr - <app>` の出力が一致すること（証明書更新後もこれで確認する）

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
- **ユーザーがdev版を使用中でも検証は止めない**: dev版とRelease版は bundle id が別で同時起動できるため、`cmake --build build-release` → `open -g build-release/daw_artefacts/Release/LaLa.app` で並走検証する（描画コードは同一なのでスクショ検証として等価。検証後は自分でquitして片付ける）

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
- **テストプロジェクトを先頭に出すには `touch ~/Music/daw/<name>/project.json`**（更新日時降順のため。旧「`0-` 始まりの名前で辞書順先頭」は効かない）。開いた直後にタイトルバーの名前で対象プロジェクトか確認する
- **新規作成の自動命名**: 名前欄には候補名 `YYYY-MM-DD-<ランダム英単語>`（例: `2026-07-22-dawn`）がプリフィルされており、そのまま「Create」AXPress（またはEnter）で候補名のまま作成される（フォーカスすると全選択になり、打ち始めれば置き換わる）。作成された名前は `ls ~/Music/daw/` とログ `project.create name=` で裏取りする。検証で作ったプロジェクトは終了後に削除して片付ける
- **フォーカス復帰の一覧再読込**: 選択画面表示中に `mkdir` 等で `~/Music/daw/` を変更 → `osascript activate` → スクショで一覧に反映されること（選択行は名前で追従・入力欄の候補名は変わらないこと）
- **行のカラーバー・ミニ波形・メタ情報**: 起動後のスクショで、各行左端に名前ハッシュ由来のカラーバー、サブテキストに「更新日時 · 曲長 · BPM · トラック数」、音声クリップを持つプロジェクトの右側にミニ波形（非選択=プロジェクト色/選択=白）が出ること。オーバービューのキャッシュは `~/Library/Caches/daw/overviews/*.bin`（project.jsonのmtimeで無効化。強制再計算はこのディレクトリを消す）。クリップなし・MIDIのみのプロジェクトは波形なし、クリップ空なら曲長なしが正しい
- **大きいウィンドウでの中央寄せ（フルスクリーン遷移の代替検証）**: フルスクリーン突入はSpace切り替えでユーザーの画面を乗っ取るため自動検証しない。同じコードパス（選択画面が設計サイズ520×584より大きい領域に置かれる）は `set size of window 1 to {1400, 900}` でウィンドウを拡大 → スクショで「コンテンツが中央に設計サイズで置かれ、引き伸ばされない」ことで確認できる。フルスクリーン中に⌘Oで閉じる実地確認だけユーザーに依頼する
- **ヒーローカード（直近プロジェクト）**: リスト上に大型カードが出て、単クリックで即開く（ログ `project.open`）。右クリックで「Finderで表示」メニュー。リストは2番目以降のみ（重複しない）。起動時はヒーローが選択状態（青塗り＋白波形）で、Return=開く・↓=リストへ・↑（先頭行から）=ヒーローへ戻る。↑↓ReturnはListBox任せでなくchooserの `keyPressed` 自前処理（ListBoxはフォーカスを持たない設計）。プロジェクトが0件のときはカード非表示
- **検証用インスタンスのウィンドウはユーザーの目に入る**: Release並走の検証ウィンドウをユーザーが「見慣れない窓」として閉じることがある（バツ=アプリ終了）。検証が中断されたら pgrep とログの session.end で「自分のインスタンスが閉じられた」ことを先に確認する。`osascript activate` は死んだインスタンスを**ユーザーの現在のSpaceに再起動してしまう**ので、activate前に pgrep で生存確認する。閉じられるだけでなく**そのまま使い始められることもある**（新機能入りアプリとして操作され、選択画面のつもりの窓がプロジェクト画面に変わっていた実例あり）。ログの `project.open` / `track.add` 等が自分の操作と対応しないときは接収とみなし、合成操作を中止してユーザーに委ねる
- **行の右クリック「Finderで表示」**: Ctrl+左クリック合成（`flags = .maskControl`）でメニューが開く。メニュー検出（CGWindowListでowner=LaLa-dev・幅500未満の小窓）→項目クリックまで**1プロセスの実行ファイル内で完結させ**、`osascript -e 'tell application "Finder" to get selection'` で対象フォルダが選択されていることを裏取りする
- **プロジェクトフォルダのドラッグ&ドロップで開く**: AppleScriptでFinderウィンドウを既知の位置にリスト表示で開き（`set bounds`）、スクショで行座標を較正 → CGEvent合成ドラッグ（down→400ms保持→30ms間隔の補間drag→700ms保持→up）で行をdawの選択画面へ落とす → ログ `project.open name=` で裏取り。**dev版とRelease版は `centreWithSize` で同一座標に重なる**ため、並走検証でドロップ先が背面に隠れていないか `winlist`（CGWindowListは前面→背面順）で確認し、必要なら `set position of window 1` で自ウィンドウを退避してから撃つ

確認できること:

1. **再生**: `click button "再生"` → 数秒後のスクショで位置表示（小節.拍｜秒）が進み、再生ヘッドが移動・追従スクロールする。表示は■アイコンに変わるが**AX名は「再生」のまま**なので、停止はもう一度 `click button "再生"`。トグルなので、ユーザーが並行操作していると「停止のつもりが再生」になり得る — 送信の前後で `transport.play/stop` ログと突き合わせて意図どおりかを裏取りし、身に覚えのないイベントが混ざったら合成操作を中止する
2. **録音**: `click button "録音"` → 7秒待つ → もう一度 `click button "録音"`（AX名は録音中も「録音」のまま。「録音停止」では引けない）→
   - `afinfo ~/Music/daw/cli-test/clip-001.wav` が モノラル・24bit・デバイスレート であること
   - 長さ ≒ 録音時間 − 1小節（カウントイン分）であること
   - スクショで選択トラックの小節頭にクリップが置かれ、タイトルに未保存マーク「●」が付くこと
3. **トラック追加**: `click button "トラックを追加"`（左下の＋ボタン）→ 自前オーバーレイのメニューがボタン直上に開く（スクショで位置確認）。項目は自前描画でAXPress不可のため、項目クリック〜トラック追加まではCGEvent合成クリックか実手動で確認する
4. **保存**: closeボタン（`first button of window 1 whose subrole is "AXCloseButton"`）→
   「保存して終了」ダイアログ → `click button "保存して終了"` → プロセス終了と
   `python3 -m json.tool ~/Music/daw/cli-test/project.json` で bpm/sampleRate/tracks/clips を確認
5. **復元**: 再起動 → 行をダブルクリック（上記CGEvent手順。直前に触ったプロジェクトが先頭に来る）→ スクショでBPM・トラック名・音量スライダー位置・クリップ位置が一致すること

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

## キー操作の確認（要ユーザー操作）

JUCEアプリには合成キーストロークが届かないため、ショートカットは実操作で確認する:

1. Space = 再生/停止、`r` = 録音（MIDIトラック選択中は無効）、`m` = 選択トラックのミュート
2. `,`/`.` = 1拍シーク（拍の途中なら拍頭へ）、`Shift+,`/`.` = 1小節シーク、`⌥,`/`⌥.` = 前/次のセクション頭へシーク（マーカーなしはno-op）
3. `⌘←`/`⌘→` = 横ズームアウト/イン（ピンチも可）。ズームインでグリッドが 拍 → 1/8 → 1/16 と細かくなり、クリックシークがその単位になる
4. `⌘S` = 保存、`⌘O` = プロジェクトを閉じて選択画面へ（未保存なら3択ダイアログ）、`⌘⌥A` = オーディオトラック追加、`⌘⌥S` = ソフトウェア音源トラック追加、Delete = 選択クリップ/リージョン/ノート削除
5. `⌘Z`/`⇧⌘Z` = undo/redo（構造編集のみ。音量・ミュート・ソロは対象外）
6. ピアノロール（リージョンをダブルクリックで開閉）: `↑`/`↓` = 選択ノートを半音移動、`⌥↑`/`⌥↓` = オクターブ移動、`⌘C`/`⌘V` = ノートコピー/再生ヘッド位置に貼り付け
7. `⌘?`（`⌘/` でも可）= ショートカット一覧オーバーレイの開閉。表示中は Esc/⌘? 以外のキーが効かない（Spaceで再生が始まらない）こと、パネル外クリックでも閉じること

## ショートカット表示の確認

- ボタンのホバーツールチップ: 再生「再生/停止 (Space)」・録音「録音 (R)」・歯車「オーディオ設定 (⌘,)」のようにショートカットが併記される（ツールチップ表示にはマウスホバーが必要。合成マウスで確認する場合はカーソル移動後1秒程度待ってから領域スクショ）
- 右クリックメニュー: リージョン/クリップのメニューに ⌃M（ミュート）・⌘T（分割）・Delete（削除）、トラックヘッダのメニューに ⌘Delete が項目右側に表示される（メニューはCtrl+左クリック合成で開ける。上記「リージョン操作の検証」参照）
- 表記の変更・追加はすべて `Source/ui/Shortcuts.h` のテーブル経由（CLAUDE.md「ショートカットキーの追加ルール」）

## 音が絡む確認（要ユーザー操作）

1. クリックトグルON → 再生 → BPMに合ったクリック音（小節頭は高いピッチ）が鳴る
2. 録音ボタン → 1小節のカウントインが鳴ってから録音が始まり、歌った内容がクリップ波形に出る
3. 重ね録り: 同じ小節に2クリップ置いて再生 → 両方鳴る（加算再生）
4. ミュート/ソロ/音量スライダーが再生中に即反映される
5. オーディオ設定（右上の歯車ボタン。AX名「オーディオ設定」、`⌘,` でも開く）でオーディオインターフェースに切り替えて録音できる
6. サンプルレート不一致の警告と録音ブロックは、自動追従（上記セクション）が効かないケースでのみ出る（デバイスがプロジェクトSR非対応・設定画面で手動でSRをずらした場合）

## リリース・アップデート（Sparkle）の確認

- **メニュー疎通（AIで自動確認可）**: アプリメニューに `Check for Updates…` が出る。ネイティブメニューなのでAXでクリックできる（JUCEのPopupMenuと違いCGEvent合成は不要）:
  ```sh
  osascript -e 'tell application "System Events" to tell process "LaLa-dev" to click menu item "Check for Updates…" of menu 1 of menu bar item 2 of menu bar 1'
  grep "update.check_started" ~/Library/Logs/daw/"$(ls -t ~/Library/Logs/daw | head -1)"   # 着弾の裏取り
  ```
  dev 版は SUFeedURL 未設定なので、クリック後に Sparkle のエラーダイアログが出るのが正常（出たら手で閉じる）
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
