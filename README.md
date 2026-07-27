# LaLa

自分が本当に使う機能だけに絞った個人用DAW（macOS専用・JUCE製）。

- 内部識別子は旧名 `daw` のまま（CMakeターゲット `daw`・bundle id `local.d0ne1s.daw`・プロジェクト置き場 `~/Music/daw/`・ログ `~/Library/Logs/daw/`）
- プロジェクトの方針・スコープは [CLAUDE.md](CLAUDE.md)、実装の落とし穴は [GOTCHAS.md](GOTCHAS.md)、動作確認手順は [VERIFY.md](VERIFY.md) を参照

## 必要なソフト

### 実行時

| ソフト | 用途 | 必須度 |
| --- | --- | --- |
| macOS | CoreAudio・内蔵GM音源（DLSMusicDevice）をそのまま使う。クロスプラットフォーム対応はしない | 必須 |
| `yt-dlp` | 「URLから読み込む…」（⌥⌘I）で音源をダウンロードする | URL取り込みを使うときのみ |
| `ffmpeg` | yt-dlp が落とした音声をWAV(PCM)化する | 同上 |

- yt-dlp は `.app` が launchd 起動でPATHを持たないため、`/opt/homebrew/bin/yt-dlp` → `/usr/local/bin/yt-dlp` の順に**絶対パスで探索**する（`Source/audio/UrlDownloader.cpp`）
- ffmpeg は yt-dlp の親ディレクトリを `--ffmpeg-location` に渡している。**yt-dlp と同じ prefix**（＝両方Homebrew）に入っている必要がある
- yt-dlp が古いとYouTube側の変更に追随できず失敗する。エラー時はまず `brew upgrade yt-dlp`

これ以外の実行時依存はない。GM音源はmacOS内蔵のDLSMusicDeviceをAUとしてホストするだけ（追加音源のインストール不要）、FX（Reverb / Delay / Limiter）は自作DSPでサードパーティ製プラグインを使わない。MP3・M4Aの読み込みはJUCE経由のCoreAudioで解決するため ffmpeg は不要。

### ビルド時

| ソフト | 用途 |
| --- | --- |
| Xcode / Command Line Tools | clang・`codesign`・`security`・`ditto`。アイコン再生成には `swiftc`（`Assets/make_icon.swift`） |
| CMake 3.22+ | ビルド |
| git | JUCE 8.0.9 を FetchContent で取得 |
| curl・tar（OS標準） | configure時に `scripts/fetch-sparkle.sh` が Sparkle 2.9.1 を `.sparkle/` へ自動取得する |
| ネットワーク | 上記2つの初回取得のみ |
| Apple Development 証明書 | 任意だが実質必須。無いと ad-hoc 署名にフォールバックし、**リビルドのたびにマイク許可（TCC）がリセットされる** |

JUCE と Sparkle は自動取得なので手動インストール不要。Sparkle の取得に失敗すると configure が `FATAL_ERROR` で止まる。

### リリース・配布時（`mise run release` → `scripts/release.sh` → `scripts/build.sh`）

| ソフト・資格情報 | 用途 |
| --- | --- |
| `gh`（認証済み） | GitHub Release の作成・配信repo `nyshk97/daw-releases` の確認 |
| `create-dmg` | DMGの作成・署名・notarize |
| `python3` | CHANGELOG の書き換え・appcast.xml へのアイテム挿入 |
| `claude` CLI | CHANGELOG `[Unreleased]` の自動生成（任意。無ければ手動編集にフォールバック） |
| Developer ID Application 証明書 | 配布用の再署名（hardened runtime） |
| notarytool の keychain profile `ide-notary` | 公証。**Claude Code の Bash からは keychain に届かないため、リリースはユーザーのTerminalで実行する** |
| Sparkle の EdDSA 秘密鍵（Keychain アカウント `daw`） | appcast の署名。バックアップは `~/Library/CloudStorage/Dropbox/secrets/sparkle-ed25519-daw-private.key` |
| `mise`（任意） | `start` / `stop` / `release` タスクのランナー |

Sparkle の `sign_update` は `scripts/fetch-sparkle.sh` が `.sparkle/bin/` に取得するので手動インストール不要。

### インストール

Homebrew 由来のもの（`yt-dlp` / `ffmpeg` / `cmake` / `gh` / `create-dmg`）はすべて Brewfile に記載済み。`brew install` を直接叩かず Brewfile 経由で入れる。

```sh
brew bundle --file=~/Library/CloudStorage/Dropbox/Brewfile
```

## ビルド・実行

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug   # 初回はJUCE 8.0.9の取得で数分かかる
cmake --build build
open build/daw_artefacts/Debug/LaLa-dev.app   # または mise run start
```

- **Debug = dev版**（`LaLa-dev.app`・bundle id `local.d0ne1s.daw.dev`・DEVリボン付きアイコン）。開発中の動作確認はこちら
- **Release = 常用版**（`LaLa.app`）: `cmake -B build-release -DCMAKE_BUILD_TYPE=Release`
- マルチ構成ジェネレータ（Xcode等）は非対応（configureで拒否する）
- アイコンPNGを差し替えたら configure から実行し直す（`.icns` はconfigure時に生成されるため）

## テスト

```sh
cmake --build build --target daw_tests && ctest --test-dir build --output-on-failure
```

通常の `ctest` はネットワーク不要（外部ツールも使わない）。URL取り込みの実走テストは環境変数を渡したときだけ走り、そのときだけ `yt-dlp` / `ffmpeg` とネット接続が要る。

```sh
LALA_VERIFY_URL='https://www.youtube.com/watch?v=jNQXAC9IVRw' build/daw_tests_artefacts/Debug/daw_tests
```

## リリース

```sh
mise run release           # 自動bump（minor）
mise run release patch     # 刻み指定
mise run release 1.2.3     # 明示指定
```

notarytool が keychain を参照するため、**ユーザーのTerminalで実行すること**。
