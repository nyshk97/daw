# VERIFY — 動作確認手順

## ビルド

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug   # 初回のみ時間がかかる（JUCEのfetch）
cmake --build build
```

- `[100%] Built target daw` が出ればOK（JUCE内部の警告は無視してよい）

## 起動確認（CLIから可能な範囲）

```sh
open build/daw_artefacts/Debug/daw.app
sleep 3 && pgrep -fl "daw.app/Contents/MacOS/daw"   # プロセス生存確認
```

- 初回起動時はマイク権限ダイアログが出る（許可はユーザー操作が必要）
- マイク権限のplist文言確認: `plutil -extract NSMicrophoneUsageDescription raw build/daw_artefacts/Debug/daw.app/Contents/Info.plist`

## 録音・再生の確認（要ユーザー操作）

1. 「録音」→ 数秒話す → 「録音停止」
2. 波形表示に入力波形がスクロール表示され、下部のレベルメーターが振れること
3. 「再生」で録音した音が聞こえ、再生位置の秒数表示が進むこと
4. 録音ファイルの実体確認: `afinfo ~/Documents/daw_tier0_recording.wav`（モノラル・16bit・実サンプルレートであること）
