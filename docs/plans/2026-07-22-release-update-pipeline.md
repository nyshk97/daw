# リリース・アップデート基盤（Sparkle + DMG 配布）

## 概要・やりたいこと

- daw を外部公開（「こんなの作れるぜ」の自慢用途）できるようにし、~/ide (PolePole) で便利だった **アプリ内メニューからのアップデート（Sparkle）** を daw にも導入する
- リリース作業は `scripts/release.sh <version>` のワンコマンドで完結させる（PolePole の release.sh を CMake 用に移植）
- 将来本体 repo を非公開にする可能性があるため、配信は **別 repo（`nyshk97/daw-releases`、public）** の GitHub Releases に分離する

## 前提・わかっていること

### ~/ide (PolePole) から流用するもの
- `scripts/build.sh` の骨格: Release ビルド → Developer ID 署名 → notarize（keychain profile `ide-notary` を流用）→ staple → create-dmg（dmg 自体も codesign + notarize + staple）
- `scripts/release.sh` の骨格: preflight → CHANGELOG 編集ポーズ → `[Unreleased]` リネーム + commit → リリースノート/Sparkle description 生成 → fresh build → EdDSA 署名 → バージョン整合チェック → appcast 累積生成 → `gh release create`
- Sparkle 運用ノウハウ: `CFBundleVersion` で比較・`pubDate` は `LC_ALL=C`・appcast は累積方式・EdDSA 秘密鍵は Keychain + Dropbox バックアップ

### daw 側の現状と差分
- daw は CMake ビルド（PolePole は xcodegen）。JUCE/C++ なので Sparkle は SwiftPM でなく **prebuilt Sparkle.framework を CMake で埋め込み + ObjC++（.mm）ブリッジ** で呼ぶ
- 現状の署名は TCC（マイク）維持用の Apple Development（`CMakeLists.txt` の POST_BUILD）。配布用の Developer ID 署名・hardened runtime・notarize はまだない → 配布ビルドは build.sh 側で inside-out 再署名する
- bundle id が placeholder（`com.yourcompany.daw`）のまま。公開前の今が変え時 → `local.d0ne1s.daw` / `local.d0ne1s.daw.dev`（PolePole の命名に合わせる）。**変更後の初回起動でマイク許可の再付与が一度必要**
- menu bar は未実装（JUCE デフォルトのアプリメニューのみ）。`juce::MenuBarModel::setMacMainMenu` の extraAppleMenuItems で "Check for Updates…" をアプリメニューに足す

### 決定事項（会話で確定）
- 更新チェックは **手動メニューのみ**（`SUEnableAutomaticChecks=false`。PolePole と同じ）
- 配布形式は **DMG**
- 配信 repo は **`nyshk97/daw-releases`**（public）。`SUFeedURL` は `https://github.com/nyshk97/daw-releases/releases/latest/download/appcast.xml` 直参照（Worker プロキシなし）
- CHANGELOG は **ja のみ**（PolePole の ja/en ペア形式は使わない）
- バージョンの source-of-truth は `CMakeLists.txt` の `project(daw VERSION x.y.z)`

### 意図的に見送るもの
- 起動時自動チェック / DL 統計プロキシ（Cloudflare Worker）/ ライセンス / Homebrew cask / en changelog
- AppRelocator 相当（App Translocation 対応）: DMG に Applications リンクがあるので手動ドラッグ前提。DMG 直起動ユーザーの問題が実際に出たら移植を検討

## 実装計画

### Phase 1: Sparkle 組み込み（アプリ側） [AI🤖]
- [x] bundle id を `local.d0ne1s.daw` / `local.d0ne1s.daw.dev` に変更（TCC マイク許可の再付与が必要になる旨をユーザーに告知）
- [x] `scripts/fetch-sparkle.sh` 作成: Sparkle 2.9.1 の release tar.xz を DL して `.sparkle/`（gitignore）に展開。`generate_keys` / `sign_update` もここに入る
- [x] CMake: Sparkle.framework のリンク + `Contents/Frameworks/` への埋め込み + rpath 設定。埋め込みは既存 POST_BUILD（`CMakeLists.txt:138` 付近。JUCE リソースを署名前にコピー → codesign）と整合させる: **Sparkle.framework のコピーを署名コマンドより前に同じ POST_BUILD 内へ入れる**（署名後にコピーすると Debug 版の署名シールが壊れる）。コピーは symlink を保持する `ditto` を使う（`cmake -E copy_directory` は symlink を flatten して framework の署名を壊す）
- [x] Info.plist に Sparkle キーを追加（`PLIST_TO_MERGE`）: `SUPublicEDKey`・`SUEnableAutomaticChecks=false`。`SUFeedURL` は **Release のみ** 設定（Debug は空 = 手動チェック時にエラーを返すだけ。PolePole と同じ）。**`SUEnableInstallerLauncherService` は設定しない**（Installer Launcher XPC は Sandbox アプリ用。daw は非 Sandbox なので Sparkle 公式が有効化しないよう案内している。PolePole の設定は踏襲しない）
- [x] ObjC++ ブリッジ（`Source/mac/SparkleUpdater.mm` + ヘッダー）: `SPUStandardUpdaterController` の生成と `checkForUpdates` / `canCheckForUpdates` を C++ に公開
- [x] アプリメニューに "Check for Updates…" を追加（`setMacMainMenu` の extraAppleMenuItems。進行中は disable）
- [x] hardened runtime 用 entitlements ファイル作成（`com.apple.security.device.audio-input` のみ。build.sh の再署名で **本体 .app にだけ** 適用する。`disable-library-validation` は不要 — Sparkle.framework は同じ Team ID で再署名するためライブラリ検証を通る）
- [x] バージョンを CMake に一本化: `PROJECT_VERSION` を `DAW_APP_VERSION` としてコンパイル定義に渡し、`Main.cpp` の `getApplicationVersion()` のハードコード `"0.2.0"` を置き換える。CMakeLists の `project(VERSION)` は現状アプリが表示している **0.2.0 に合わせて bump**（現状 0.1.0 で食い違っている）
- [x] Debug ビルドで検証: メニュー項目が出る・押すと Sparkle が反応する（フィード空エラー表示 or `defaults write` でのフィード上書きで確認）
- [x] **clean Debug build**（`--clean-first`）直後に `codesign --verify --strict <app>` が通ることを確認（Sparkle 埋め込みと POST_BUILD 署名の順序が正しい証明。インクリメンタルビルドでは偶然通るため clean 直後で見る）

### Phase 2前の準備 [人間👨‍💻]
- [x] EdDSA 鍵ペア生成: `.sparkle/bin/generate_keys` を実行（Keychain に保存される。AI がコマンドを提示）
- [x] 秘密鍵のバックアップ: `generate_keys -x <file>` で書き出して `~/Library/CloudStorage/Dropbox/secrets/` に保存（PolePole と同じ運用）
- [x] 公開鍵（`generate_keys -p` の出力）を AI に渡す → Info.plist の `SUPublicEDKey` に反映

### Phase 2: 配布ビルドスクリプト [AI🤖]
- [x] `scripts/build.sh` 作成（PolePole 版の CMake 移植）:
  - `cmake -B build-release -DCMAKE_BUILD_TYPE=Release` でフレッシュビルド
  - Developer ID 署名を inside-out で再署名。順序と entitlements の扱いは Sparkle 公式の手動再署名手順（https://sparkle-project.org/documentation/sandboxing/）に従う:
    1. `Sparkle.framework/Versions/B/XPCServices/Downloader.xpc` — **`--preserve-metadata=entitlements`** で既存 entitlement を保持して署名
    2. `Installer.xpc` → `Autoupdate` → `Updater.app` → `Sparkle.framework` の順に `--options runtime` で署名
    3. 最後に本体 .app を `--options runtime --timestamp --entitlements <audio-input のみ>` で署名
    - **`--deep` と、全体への `--entitlements` 一括適用は禁止**（XPC の entitlement を壊す）
  - notarize（`ide-notary` profile）→ staple
  - create-dmg で DMG 化（dmg も codesign + notarize + staple）
- [x] AI の自走確認は署名検証（`codesign -dvvv --entitlements -` で runtime / Timestamp / get-task-allow 不在）まで。**notarytool は Claude の Bash から Keychain に届かないため、notarize 以降はユーザー Terminal で実行**

### Phase 3: リリーススクリプト + 配信 repo [AI🤖]
- [x] `gh repo create nyshk97/daw-releases --public` で配信 repo 作成（README に「daw の配信用 repo」と一言。**初回 `gh release create` より前に必ず作る** — release のタグはこの repo の HEAD に打たれるため最低 1 commit が必要）
- [x] `docs/CHANGELOG.md` 新設（Keep a Changelog ベース・ja のみ・体言止め。冒頭に書き方ガイドを含める）
- [x] `scripts/release.sh` 作成（PolePole 版の移植・簡略化）。Git 整合性を保証する流れ:
  - preflight: gh 認証・**clean worktree・カレントブランチが main** であることを確認
  - CHANGELOG 編集ポーズ → `[Unreleased]` → `[X.Y.Z] - 日付` リネーム + commit（再実行ガード付き）
  - GitHub Release notes（md）と Sparkle description（HTML・light/dark 対応 CSS）を CHANGELOG から生成
  - build.sh 実行 → `sign_update` で EdDSA 署名 → ビルド済み .app の `CFBundleShortVersionString` と引数 `<version>` の整合チェック（CMakeLists の VERSION bump し忘れ検知）
  - `git push origin main` → **本体 repo に `v<version>` タグを作成して push**（DMG を作った commit を記録。次回リリース時の「前回タグ以降の commit 一覧」表示にも使う）
  - 既存 appcast を fetch して新 item 追記（累積方式・`LC_ALL=C` で pubDate）
  - `gh release create --repo nyshk97/daw-releases` で dmg + appcast.xml を添付（配信 repo 側のタグは配信 repo の HEAD に打たれる）
  - **再実行ガードは CHANGELOG だけでなく後半のステップにも入れる**（DMG 作成後の appcast 取得・Release 作成での失敗 → 再実行に耐える）:
    - 本体 repo のタグ: 同名 `v<version>` が既に存在し **現在の HEAD を指しているなら続行**（作成をスキップ）、別 commit を指しているならエラーで停止
    - 配信 repo の Release: 同名 Release が既に存在する場合も同様に扱う（前回の中途半端な Release を検出したら、内容を確認して削除・再作成するか asset 差し替えで続行できるようエラーメッセージで案内する）

### 動作確認 [人間👨‍💻]
- [ ] ユーザー Terminal で `scripts/release.sh 0.2.0` を実行して初回リリース
- [ ] アップデートの一連の流れを確認: 一時的に低い VERSION（例: 0.0.1）でビルドした dmg の .app を **必ず `/Applications` にコピーしてから起動**（Translocation 回避）→ "Check for Updates…" → 0.2.0 が offer される → 更新が完走する
- [ ] 更新後の検証: `plutil -extract CFBundleVersion raw /Applications/daw.app/Contents/Info.plist` が新バージョンになっている + `codesign --verify --strict /Applications/daw.app` が通る
- [ ] 別 Mac（または新規ユーザーアカウント）想定: dmg をダウンロード → Gatekeeper を通って起動できる（`spctl --assess --type execute -vv` で `Notarized Developer ID` でも代替可）

## ログ

### 試したこと・わかったこと
- `MenuBarModel` の pure virtual は `menuItemSelected`（`menuItemActivated` ではない）
- ネイティブメニューバーの項目は AX（System Events の `click menu item`）でクリックできる。JUCE の PopupMenu と違い CGEvent 合成は不要。着弾はアプリログの `update.check_started` で裏取りした
- Sparkle の `generate_keys` / `sign_update` は `--account daw` で PolePole の鍵（デフォルトアカウント ed25519）と分離できる。Claude の Bash から Keychain の読み書きとも通った（notarytool と違い制限なし）。公開鍵 `qicnPwvUMYYTOBCfRGm4wmJHdGYbpv+MPsO9n6OZhVk=`、秘密鍵バックアップは Dropbox/secrets/sparkle-ed25519-daw-private.key
- Sparkle 2.9.1 配布物の Downloader.xpc は元から entitlements 空（`[Dict]` のみ）。`--preserve-metadata=entitlements` で再署名して同一を確認
- 配布用 inside-out 再署名を notarize 手前まで実走: `flags=0x10000(runtime)`・`Timestamp=` あり・entitlements は audio-input のみ・`codesign --verify --strict --deep` OK
- Phase 2前の人間タスク（鍵生成・バックアップ）は AI 自走で完了。残る人間作業は release.sh の実行（notarytool が Claude の Bash から Keychain に届かないため）と実アップデートの目視確認のみ

### 方針変更
- 2026-07-22 実装前レビューを反映: ①非 Sandbox のため Installer Launcher XPC と disable-library-validation を削除 ②Sparkle 再署名手順を公式の順序（Downloader.xpc は entitlement 保持）で明記 ③バージョンを CMake `PROJECT_VERSION` に一本化（Main.cpp のハードコード 0.2.0 を解消、CMake 側を 0.2.0 に bump）④release.sh に clean worktree/main 確認・本体 repo への tag push を追加、daw-releases 作成を初回リリース前に明示
- 2026-07-22 実装前レビュー2巡目を反映: ①Sparkle.framework の埋め込みは既存 POST_BUILD の署名より前に ditto でコピー（symlink 保持）し、clean Debug build 直後の `codesign --verify --strict` を検証項目に追加 ②release.sh の再実行ガードをタグ作成・Release 作成まで拡張（同名タグが HEAD を指すなら続行、別 commit ならエラー）
