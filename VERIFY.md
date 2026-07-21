# VERIFY — 動作確認手順

## ビルド

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug   # 初回のみ時間がかかる（JUCEのfetch）
cmake --build build
```

- `[100%] Built target daw` が出ればOK（JUCE内部の警告は無視してよい。自作コード由来の警告はゼロが基準）

## 起動確認（CLIから可能な範囲）

```sh
open build/daw_artefacts/Debug/daw.app
sleep 3 && pgrep -fl "daw.app/Contents/MacOS/daw"   # プロセス生存確認
```

- 起動するとプロジェクト選択画面が出る（`~/Music/daw/` のフォルダ一覧＋新規作成）
- マイク権限のplist文言確認: `plutil -extract NSMicrophoneUsageDescription raw build/daw_artefacts/Debug/daw.app/Contents/Info.plist`
- **リビルドしてもマイク権限は再要求されない**（POST_BUILD で Apple Development 証明書により再署名しているため）。ダイアログが出たら署名が壊れている兆候なので以下を確認:
  - `codesign -dvv build/daw_artefacts/Debug/daw.app 2>&1 | grep Signature` → `Signature=adhoc` になっていたら POST_BUILD 署名が走っていない
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
- **異常終了の検知**: 次回起動時に `session.previous_abnormal` の WARN が出る（`pkill -9 -x daw` → 再起動で再現可能）
- **エラー系の確認**: ダイアログ表示（`ui.alert`）・オーディオスレッドの異常（`audio.midi_overflow` / `audio.record_fifo_drop`、2秒集約）は ERROR/WARN で残る
- ログはセッションごと1ファイル・新しい20世代のみ保持・1セッション1MiB上限

## CLI＋AppleScriptでの半自動確認

JUCEアプリはAppleScriptの合成キーストローク・座標クリック（`click at`）が効かないが、
**ボタンのAXPress（`click button "名前"`）とスクリーンショットの組み合わせ**で大半のフローを確認できる。

ユーザーが他アプリで作業中にフォーカスを奪いたくないときは、`open -g` でバックグラウンド起動し、
`screencapture -x -l <windowID>` で背面のまま撮る（AXPressは背面ウィンドウにも効く。
windowIDの取り方と別Spaceの罠はグローバルCLAUDE.mdの「Macアプリの変更」参照）。

```sh
# テストプロジェクトをCLIで用意（新規作成フローを迂回）
mkdir -p ~/Music/daw/cli-test
cat > ~/Music/daw/cli-test/project.json <<'EOF'
{"version": 1, "bpm": 100.0, "sampleRate": 0.0, "tracks": [{"type": "audio", "name": "ボーカル", "mute": false, "solo": false, "volume": 0.8, "clips": []}]}
EOF

open build/daw_artefacts/Debug/daw.app && sleep 3
osascript -e 'tell application "daw" to activate' \
          -e 'delay 0.5' \
          -e 'tell application "System Events" to tell process "daw" to click button "開く" of window 1'
# マイク権限ダイアログが出たら（通常は初回許可後は出ない。tccutil reset 後の初回のみ）:
osascript -e 'tell application "System Events" to tell process "UserNotificationCenter" to click button "許可" of window 1'

# ウィンドウ位置を取ってスクリーンショットで目視確認
osascript -e 'tell application "System Events" to tell process "daw" to get position of window 1'
screencapture -x -R<x,y,w,h> /tmp/daw-check.png
```

確認できること:

1. **再生**: `click button "再生"` → 数秒後のスクショで位置表示（小節.拍｜秒）が進み、再生ヘッドが移動・追従スクロールする。ボタンは「停止」表示になる
2. **録音**: `click button "録音"` → 7秒待つ → `click button "録音停止"` →
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
  - スクロールが必要な確認（ピアノロールの鍵盤帯域移動等）はCGEventの `scrollWheelEvent2Source`（units: .pixel）を対象座標に連打すればViewportに効く。ピアノロールを開いた直後は最上部（pitch 127側）表示なので、GMドラム名の確認は下方向へスクロールしてから撮る
- 座標の目安（ウィンドウ位置 X,Y・デフォルトズーム pxPerBar=80）: タイムライン左端 = X+200、
  レーン先頭 = Y+28(タイトルバー)+44(トランスポート)+26(ルーラー)、トラック行高 = 84

## トラックレベルメーターの確認

再生中にトラックヘッダの音量スライダー上へ緑バー（dBスケール・クリップレベルで赤）が重なる。**再生ヘッドがリージョン/クリップを通過中にしか点灯しない**ので、`click button "再生"` の直後 0.5〜1.5 秒でスクショを撮る。リージョン通過後・停止後に消灯すること、クリップの無いトラックに出ないことも同じ流れで確認できる。

- スクショはウィンドウID指定（`screencapture -x -l <id>`）ならアプリが背面でも撮れる。IDは CGWindowList を `番号\t名前` で出すswift小ツール（グローバルCLAUDE.md「Macアプリの変更」参照）を作って `awk -F'\t' '$2 ~ /<プロジェクト名>/ {print $1}'` で引く
- メーターの点灯が小さすぎる/見えないときは、撮影タイミングが通過後でないかを先に疑う（ディケイは約1秒でフルスケールが消える）

## キー操作の確認（要ユーザー操作）

JUCEアプリには合成キーストロークが届かないため、ショートカットは実操作で確認する:

1. Space = 再生/停止、`r` = 録音（MIDIトラック選択中は無効）、`m` = 選択トラックのミュート
2. `,`/`.` = 1拍シーク（拍の途中なら拍頭へ）、`Shift+,`/`.` = 1小節シーク
3. `⌘←`/`⌘→` = 横ズームアウト/イン（ピンチも可）。ズームインでグリッドが 拍 → 1/8 → 1/16 と細かくなり、クリックシークがその単位になる
4. `⌘S` = 保存、`⌘⌥A` = オーディオトラック追加、`⌘⌥S` = ソフトウェア音源トラック追加、Delete = 選択クリップ/リージョン/ノート削除
5. `⌘Z`/`⇧⌘Z` = undo/redo（構造編集のみ。音量・ミュート・ソロは対象外）
6. ピアノロール（リージョンをダブルクリックで開閉）: `↑`/`↓` = 選択ノートを半音移動、`⌥↑`/`⌥↓` = オクターブ移動、`⌘C`/`⌘V` = ノートコピー/再生ヘッド位置に貼り付け

## 音が絡む確認（要ユーザー操作）

1. クリックトグルON → 再生 → BPMに合ったクリック音（小節頭は高いピッチ）が鳴る
2. 録音ボタン → 1小節のカウントインが鳴ってから録音が始まり、歌った内容がクリップ波形に出る
3. 重ね録り: 同じ小節に2クリップ置いて再生 → 両方鳴る（加算再生）
4. ミュート/ソロ/音量スライダーが再生中に即反映される
5. オーディオ設定（右上の歯車ボタン。AX名「オーディオ設定」、`⌘,` でも開く）でオーディオインターフェースに切り替えて録音できる
6. サンプルレート不一致（プロジェクト48kHz・デバイス44.1kHz等）でトランスポートバーに警告が出て録音がブロックされる
