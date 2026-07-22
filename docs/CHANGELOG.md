---
build-info: このファイルが source-of-truth。scripts/release.sh が [Unreleased] を [X.Y.Z] - YYYY-MM-DD にリネームし、該当セクションを GitHub Release notes と Sparkle appcast の <description> に注入する。
---

# Changelog

LaLa（旧名 daw）の更新履歴。形式は [Keep a Changelog](https://keepachangelog.com/ja/1.1.0/) ベース、バージョニングは [SemVer](https://semver.org/lang/ja/)。

## 書き方

このセクションは AI が release.sh の pause 中に `[Unreleased]` を埋めるときの判断基準でもある。

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
### ✨ Added
- ミキサーを追加（`X` で表示/非表示。トラックごとの Pan・音量・M/S・send 3種、Reverb A / Reverb B / Delay バスと Master のフェーダー・メーター。FX 本体は今後追加予定で、バスは当面素通しのため send を上げると原音が重なって音量が上がる）
- WAV 書き出し（バウンス）を追加（`File > 書き出し…` / `⌘B`。聞こえているままをステレオ 24bit WAV へ書き出し）
- メニューバーに File メニューを追加（保存 `⌘S` / 書き出し… `⌘B` / プロジェクトを閉じる）

### 📝 Changed
- アプリ名を LaLa に変更

## [0.2.0] - 2026-07-22
### ✨ Added
- 初回公開リリース（マルチトラック録音・小節ルーラー・メトロノーム・プロジェクト保存/読み込み）
- アプリメニューの `Check for Updates…` からアプリ内アップデートが可能に
