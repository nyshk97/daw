# daw — 個人用DAW

## プロジェクト概要

### 目的
- Logic Proの機能過多による認知コストを削減するため、自分が本当に使う機能だけに絞った個人用DAWをJUCEでゼロから自作する
- 「全部入り」ではなく「引き算」が価値。機能追加の提案は常に「本当に自分が使うか」で判断する

### 非目的
- Apple純正音源（Logic付属音源）の代替は狙わない
- 大量トラック運用（数十〜数百トラック）は想定しない
- 商用配布・他人向けの汎用性は考えない

## 現在のTierとゴール

**Tier 0**: 1トラックの録音・再生・波形表示のみ

- まだ作らないもの: プラグインホスティング / MIDI編集 / ミキサー / 複数トラック
- Tier 0のスコープ外の実装を先回りして入れない（抽象化のしすぎに注意）
- 波形表示はマイク入力のライブスクロール表示のみ（**再生音は波形に反映されない**）。バグではなく Tier 0 の仕様。クリップ波形＋再生ヘッド表示は編集機能の Tier で作り直す

## 技術スタック

- JUCE（GUI・オーディオI/O・波形描画）
- CMake（JUCEはFetchContentまたはsubmoduleで取得）
- C++17以上（JUCEの要求に合わせる）
- macOS限定（CoreAudio）。クロスプラットフォーム対応のための抽象化はしない

## ビルド・実行コマンド

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug   # 初回はFetchContentでJUCE 8.0.9を取得（数分かかる）
cmake --build build
open build/daw_artefacts/Debug/daw.app
```

- JUCEは`CMakeLists.txt`で **8.0.9 に pin** 済み。GOTCHAS.mdの`createWriterFor`（`AudioFormatWriterOptions`版）が実在することは`build/_deps/juce-src/modules/juce_audio_formats/format/juce_AudioFormat.h`で照合済み。**バージョンを上げるときは再照合すること**
- 録音ファイルの出力先: `~/Documents/daw_tier0_recording.wav`（モノラル・16bit WAV）

## ディレクトリ構成方針

```
daw/
├── CMakeLists.txt
├── Source/
│   ├── Main.cpp          # エントリポイント・MainWindow
│   ├── audio/            # オーディオエンジン（リアルタイムスレッド側のコード）
│   ├── ui/               # UIコンポーネント（メッセージスレッド側のコード）
│   └── shared/           # スレッド間で受け渡すデータ構造（FIFO・atomic等）
├── docs/plans/           # 実装計画（/plot で生成）
├── GOTCHAS.md            # JUCE・リアルタイムオーディオの落とし穴集
└── VERIFY.md             # 動作確認手順
```

- **audio/ と ui/ の境界 = スレッド境界**。audio/ のコードはオーディオスレッドで走る前提で書き、ui/ への直接参照を持たない。受け渡しは必ず shared/ の構造を経由する
- スレッド境界に関する注意点は GOTCHAS.md を必ず参照すること
