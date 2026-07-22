# VERIFY — 動作確認手順

## ビルド

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug   # 初回のみ時間がかかる（JUCEのfetch）
cmake --build build
```

- `Built target daw` 〜 `[100%] Built target daw_tests` まで出ればOK（JUCE内部の警告は無視してよい。自作コード由来の警告はゼロが基準）
- **Debugビルドは dev 版**（アプリ名 `daw-dev`・bundle id `com.yourcompany.daw.dev`・DEVリボン付きアイコン）。常用版は `cmake -B build-release -DCMAKE_BUILD_TYPE=Release` → `daw.app`。動作確認は以下すべて dev 版で行う

## 起動確認（CLIから可能な範囲）

```sh
open build/daw_artefacts/Debug/daw-dev.app
sleep 3 && pgrep -fl "daw-dev.app/Contents/MacOS/daw-dev"   # プロセス生存確認
```

- 起動するとプロジェクト選択画面が出る（`~/Music/daw/` のフォルダ一覧＋新規作成）
- マイク権限のplist文言確認: `plutil -extract NSMicrophoneUsageDescription raw build/daw_artefacts/Debug/daw-dev.app/Contents/Info.plist`
- **リビルドしてもマイク権限は再要求されない**（POST_BUILD でバンドルリソースを先行コピーした上で Apple Development 証明書により再署名しているため。`--target daw` 単体ビルドや `--clean-first` でも維持される。bundle id を分けた直後の dev 版初回起動のみ再付与が必要）。ダイアログが出たら署名が壊れている兆候なので以下を確認:
  - `codesign -dvv build/daw_artefacts/Debug/daw-dev.app 2>&1 | grep Signature` → `Signature=adhoc` になっていたら、configure 時の証明書自動解決が失敗して ad-hoc フォールバックしている（`cmake -B build` を再実行して `Codesign identity:` の STATUS 行と WARNING の有無を見る）
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
- **異常終了の検知**: 次回起動時に `session.previous_abnormal` の WARN が出る（`pkill -9 -x daw-dev` → 再起動で再現可能）
- **エラー系の確認**: ダイアログ表示（`ui.alert`）・オーディオスレッドの異常（`audio.midi_overflow` / `audio.record_fifo_drop`、2秒集約）は ERROR/WARN で残る
- ログはセッションごと1ファイル・新しい20世代のみ保持・1セッション1MiB上限

## CLI＋AppleScriptでの半自動確認

JUCEアプリはAppleScriptの合成キーストローク・座標クリック（`click at`）が効かないが、
**ボタンのAXPress（`click button "名前"`）とスクリーンショットの組み合わせ**で大半のフローを確認できる。

ユーザーが他アプリで作業中にフォーカスを奪いたくないときは、`open -g` でバックグラウンド起動し、
`screencapture -x -l <windowID>` で背面のまま撮る（AXPressは背面ウィンドウにも効くが**同一Spaceに限る**。
別SpaceだとSystem Eventsからウィンドウ自体が見えなくなる。切り分けと対処、windowIDの取り方は
グローバルCLAUDE.mdの「Macアプリの変更」参照）。

確認の前に旧インスタンスを終了させる（多重起動不可のため、起動中だと新ビルドが立ち上がらない）:

```sh
osascript -e 'tell application "daw-dev" to quit' &   # 正規quit。未保存変更があれば保存ダイアログが出る（処理はユーザーに委ねる）
for i in $(seq 1 30); do pgrep -x daw-dev >/dev/null || break; sleep 3; done
```

- quitイベントは何も破棄しない。ダイアログ待ちでosascriptに `-128`（ユーザによってキャンセル）が返ることがあるが、プロセスが終了していれば問題ない

```sh
# テストプロジェクトをCLIで用意（新規作成フローを迂回）
mkdir -p ~/Music/daw/cli-test
cat > ~/Music/daw/cli-test/project.json <<'EOF'
{"version": 1, "bpm": 100.0, "sampleRate": 0.0, "tracks": [{"type": "audio", "name": "ボーカル", "mute": false, "solo": false, "volume": 0.8, "clips": []}]}
EOF

open build/daw_artefacts/Debug/daw-dev.app && sleep 3
osascript -e 'tell application "daw-dev" to activate' \
          -e 'delay 0.5' \
          -e 'tell application "System Events" to tell process "daw-dev" to click button "開く" of window 1'
# マイク権限ダイアログが出たら（通常は初回許可後は出ない。tccutil reset 後の初回のみ）:
osascript -e 'tell application "System Events" to tell process "UserNotificationCenter" to click button "許可" of window 1'

# ウィンドウ位置を取ってスクリーンショットで目視確認
osascript -e 'tell application "System Events" to tell process "daw-dev" to get position of window 1'
screencapture -x -R<x,y,w,h> /tmp/daw-check.png
```

- プロジェクト選択画面は**アルファベット順の先頭行が自動選択**され、「開く」は選択行を開く（AXでは行選択不可）。複数のテストプロジェクトがあると `cli-test` が先頭にならないため、AX確認用は `0-` 始まりの名前で常に先頭へソートさせる（`0-ms-test`＝オーディオ2＋MIDI 1の構成で作成済み）
- **「0-」系テストプロジェクトが複数あると辞書順で先のものが開く**（例: `0-0-split-test` を作っても既存の `0-0-region-mute` が先頭になる）。新規テストプロジェクトは `ls ~/Music/daw/` で既存名を確認して辞書順で前になる名前にし、開いた直後にタイトルバーの名前で対象プロジェクトか確認する

確認できること:

1. **再生**: `click button "再生"` → 数秒後のスクショで位置表示（小節.拍｜秒）が進み、再生ヘッドが移動・追従スクロールする。表示は■アイコンに変わるが**AX名は「再生」のまま**なので、停止はもう一度 `click button "再生"`
2. **録音**: `click button "録音"` → 7秒待つ → もう一度 `click button "録音"`（AX名は録音中も「録音」のまま。「録音停止」では引けない）→
   - `afinfo ~/Music/daw/cli-test/clip-001.wav` が モノラル・24bit・デバイスレート であること
   - 長さ ≒ 録音時間 − 1小節（カウントイン分）であること
   - スクショで選択トラックの小節頭にクリップが置かれ、タイトルに未保存マーク「●」が付くこと
3. **トラック追加**: `click button "トラックを追加"`（左下の＋ボタン）→ 自前オーバーレイのメニューがボタン直上に開く（スクショで位置確認）。項目は自前描画でAXPress不可のため、項目クリック〜トラック追加まではCGEvent合成クリックか実手動で確認する
4. **保存**: closeボタン（`first button of window 1 whose subrole is "AXCloseButton"`）→
   「保存して終了」ダイアログ → `click button "保存して終了"` → プロセス終了と
   `python3 -m json.tool ~/Music/daw/cli-test/project.json` で bpm/sampleRate/tracks/clips を確認
5. **復元**: 再起動 → 「開く」→ スクショでBPM・トラック名・音量スライダー位置・クリップ位置が一致すること

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
  - **メニュー表示中に別プロセスのCLIツール（AppKitをリンクしたswift製など）を起動するとメニューが閉じる**。検証は「Ctrl+左クリックで開く → CGWindowListで owner=daw-dev・name=menu のウィンドウboundsを取る → bounds高さを項目数で等分して対象項目の中心を左クリック」までを**1プロセス内**で完結させる（各操作の間に1秒程度sleep）。着弾はアプリログ（`region.split` / `region.mute` 等）で裏取りする
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

## トラックレベルメーターの確認

再生中にトラックヘッダの音量スライダー上へ緑バー（dBスケール・クリップレベルで赤）が重なる。**再生ヘッドがリージョン/クリップを通過中にしか点灯しない**ので、`click button "再生"` の直後 0.5〜1.5 秒でスクショを撮る。リージョン通過後・停止後に消灯すること、クリップの無いトラックに出ないことも同じ流れで確認できる。

- スクショはウィンドウID指定（`screencapture -x -l <id>`）ならアプリが背面でも撮れる。IDは CGWindowList を `番号\t名前` で出すswift小ツール（グローバルCLAUDE.md「Macアプリの変更」参照）を作って `awk -F'\t' '$2 ~ /<プロジェクト名>/ {print $1}'` で引く
- メーターの点灯が小さすぎる/見えないときは、撮影タイミングが通過後でないかを先に疑う（ディケイは約1秒でフルスケールが消える）

## 明滅・アニメーションUIの確認（録音グロー等）

スクショ1枚では明滅は判定できない。**録音中に0.3〜0.4秒間隔で複数枚撮り、連続ペアのピクセル差分**で機械判定する:

- 撮影間隔を明滅周期のちょうど半分にしない（録音グローは周期1.6秒。0.8秒間隔だと位相の対称点で差が出ないことがある）
- 差分は swift + `NSBitmapImageRep.colorAt` の小ツールでRGB合計差を出す（グローが動いていればボタン領域で maxDiff 0.1〜0.3・数千px変化。0.03以下なら効いていないか、録音がSR不一致アラート等で始まっていない）
- 差分ゼロが出たら先に crop 座標と、ログの `record.start` があるか（＝本当に録音中だったか）を疑う

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

## キー操作の確認（要ユーザー操作）

JUCEアプリには合成キーストロークが届かないため、ショートカットは実操作で確認する:

1. Space = 再生/停止、`r` = 録音（MIDIトラック選択中は無効）、`m` = 選択トラックのミュート
2. `,`/`.` = 1拍シーク（拍の途中なら拍頭へ）、`Shift+,`/`.` = 1小節シーク、`⌥,`/`⌥.` = 前/次のセクション頭へシーク（マーカーなしはno-op）
3. `⌘←`/`⌘→` = 横ズームアウト/イン（ピンチも可）。ズームインでグリッドが 拍 → 1/8 → 1/16 と細かくなり、クリックシークがその単位になる
4. `⌘S` = 保存、`⌘⌥A` = オーディオトラック追加、`⌘⌥S` = ソフトウェア音源トラック追加、Delete = 選択クリップ/リージョン/ノート削除
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
