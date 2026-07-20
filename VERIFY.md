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
- **リビルドするとマイク権限が再要求される**（ad-hoc署名でcdhashが変わるため）。ダイアログで許可し直す

## CLI＋AppleScriptでの半自動確認

JUCEアプリはAppleScriptの合成キーストローク・座標クリック（`click at`）が効かないが、
**ボタンのAXPress（`click button "名前"`）とスクリーンショットの組み合わせ**で大半のフローを確認できる。

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
# マイク権限ダイアログが出たら:
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
3. **トラック追加**: `click button "＋トラック"` → ヘッダに新トラックが増え選択される
4. **保存**: closeボタン（`first button of window 1 whose subrole is "AXCloseButton"`）→
   「保存して終了」ダイアログ → `click button "保存して終了"` → プロセス終了と
   `python3 -m json.tool ~/Music/daw/cli-test/project.json` で bpm/sampleRate/tracks/clips を確認
5. **復元**: 再起動 → 「開く」→ スクショでBPM・トラック名・音量スライダー位置・クリップ位置が一致すること

## キー操作の確認（要ユーザー操作）

JUCEアプリには合成キーストロークが届かないため、ショートカットは実操作で確認する:

1. Space = 再生/停止、`r` = 録音、`m` = 選択トラックのミュート
2. `,`/`.` = 1拍シーク（拍の途中なら拍頭へ）、`Shift+,`/`.` = 1小節シーク
3. `⌘←`/`⌘→` = 横ズームアウト/イン（ピンチも可）。ズームインでグリッドが 拍 → 1/8 → 1/16 と細かくなり、クリックシークがその単位になる
4. `⌘S` = 保存、`⌘⌥A` = トラック追加、Delete = 選択クリップ削除

## 音が絡む確認（要ユーザー操作）

1. クリックトグルON → 再生 → BPMに合ったクリック音（小節頭は高いピッチ）が鳴る
2. 録音ボタン → 1小節のカウントインが鳴ってから録音が始まり、歌った内容がクリップ波形に出る
3. 重ね録り: 同じ小節に2クリップ置いて再生 → 両方鳴る（加算再生）
4. ミュート/ソロ/音量スライダーが再生中に即反映される
5. デバイス設定でオーディオインターフェースに切り替えて録音できる
6. サンプルレート不一致（プロジェクト48kHz・デバイス44.1kHz等）でトランスポートバーに警告が出て録音がブロックされる
