---
build-info: このファイルが source-of-truth。scripts/release-salva.sh が [Unreleased] を [X.Y.Z] - YYYY-MM-DD にリネームし、該当セクションを GitHub Release notes と Sparkle appcast の <description> に注入する。
---

# Changelog (Salva)

Salva（レコード/ローカル音源のサンプリング素材化アプリ）の更新履歴。形式は [Keep a Changelog](https://keepachangelog.com/ja/1.1.0/) ベース、バージョニングは [SemVer](https://semver.org/lang/ja/)。

## 書き方

このセクションは AI が release-salva.sh の pause 中に `[Unreleased]` を埋めるときの判断基準でもある。

### フォーマット

- 1 項目 = 1 行の `- ` bullet。インライン Markdown は `` `code` ``、`**strong**`、`[label](url)` のみサポート（Sparkle description の HTML 変換が対応するのはこれだけ）
- ユーザー目視で気づく変更だけ書く（内部リファクタ・ドキュメント変更は除く）
- 文体は体言止め（「〜を追加」「〜問題を修正」。「〜しました」体は使わない）

### カテゴリ

```
✨ Added       — 新しい機能・ボタン・画面・ショートカット
📝 Changed     — 既存機能の挙動・デフォルト値・配置・配布形式の変更
🐛 Fixed       — 「期待通りに動かなかった」のが直った
🗑️ Removed     — UI 要素・機能・ショートカットの削除
```

迷ったら **ユーザーが画面でどう感じるか** で選ぶ。

---

## [Unreleased]

## [0.1.0] - 2026-08-15
### ✨ Added

- 初回リリース。レコード/ローカル音源から欲しい区間を素材化するアプリ
- オーディオファイルの読み込み（D&D・最近開いたファイル。wav/aiff/flac/mp3/m4a）と波形表示・区間ループ再生・横ズーム（`⌘←/→`）
- レコード録音モード（入力デバイス＋ステレオペア選択・24bit WAVストリーミング書き込み・レベルメーター。`r` = 録音）
- ステム分離（demucs 4/6ステム）とステムごとのミュート/ソロ再生・レベルメーター
- 区間の書き出し（聴こえている構成のミックス・24bit・元サンプルレート・ファイル名にBPMを焼き込み）
- 区間からのBPM逆算表示（4/4固定・小節数はクリックで手動修正可）
- Sparkle によるアプリ内アップデート（アプリメニューの Check for Updates…）
