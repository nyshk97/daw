# Salva — レコード/ローカル音源のサンプリング素材化アプリ

レコード/ローカル音源から欲しい区間を素材化するフロントアプリ。出口はMPC（USBオーディオ実時間 or microSDファイル渡し）とLaLa（WAVファイル取り込み）。仕様の真実の源は `docs/plans/2026-08-14-1912-salva-app.md`。

## LaLaとの境界（このディレクトリの掟）

- Salvaは**おもちゃレーン寄り**の別アプリ。LaLaのTierスコープ・`docs/design/feature-scope.md` の引き算基準・`ui-principles.md` の見送りリストで**裁かない**（機能の要否はSalvaのplanと本人の判断で決める）
- LaLaと共有してよいのはリポジトリルート `Source/shared/` の**汎用部品**（`AudioFileTypes`・`GainScale`・`Log`・`SpawnedProcess`・`TempDirSweep` 等）まで。LaLa固有コード（`Project` モデル・ルートの `Source/ui/`・`Source/audio/`）には依存しない
- スレッド境界の流儀はLaLaと同じ（audio/ui/shared の3分割・`GOTCHAS.md` 必読。audio/ のコードはオーディオスレッド前提・ui/ への直接参照禁止）
- **判断ロジックをUIに書かない**: 区間丸め・BPM逆算・ファイル名生成・マニフェスト検証・クリップ判定などは `salva_tests` でテストできる場所に置く（LaLaと同じ基準）

## ビルド・実行

```sh
mise run build:salva   # dev版（Salva-dev.app）をビルド
mise run start:salva   # 起動
mise run test:salva    # salva_tests（C++回帰テスト）
```

- Debug = dev版（`Salva-dev.app`・bundle id `local.d0ne1s.salva.dev`・DEVリボン付きアイコン）、Release = 常用版（`Salva.app`・`local.d0ne1s.salva`）。LaLaと同じbuildディレクトリを共有する（ターゲット名 `salva`）
- アイコンは `apps/salva/Assets/make_icon.swift` で生成（生成済みPNGはコミット済み。差し替えたらconfigure再実行）

## リリース（LaLaと完全分離）

- 配信repoは `salva-releases`（`daw-releases` に相乗りしない）・ソースrepoのタグは `salva-vX.Y.Z`
- バージョンは `apps/salva/CMakeLists.txt` の `SALVA_VERSION`（LaLaのproject VERSIONと独立）
- リリーススクリプトは `scripts/release-salva.sh`・変更履歴は `CHANGELOG-salva.md`（Phase 7で整備）
- LaLa側の `release.sh`・appcast URLには触れない
