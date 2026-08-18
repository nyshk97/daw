# noiselab — ノイズ除去ラボ

宅録ボーカルのノイズ除去機能の**要否と方式**を実素材・実耳で確定するラボ。
真実の源は `docs/plans/2026-08-18-1123-noise-removal-lab.md`（判定基準・拒否条件・手順）。

```sh
./setup.sh                                  # venv 構築（mise の Python 3.12）
.venv/bin/python measure.py <clip.wav>      # Step 0: ノイズフロア実測
```

- コミット対象はスクリプト・設定・環境lockのみ（`.venv/` `listen/` `work/` `models/` `logs/` は gitignore 済み）
- Step 1（候補処理）の依存とスクリプトは、Step 0 のゲートを通過したら追加する
- 結果の記録先: `docs/labs/noise-removal.md`（追記専用）・人間回答は `docs/labs/reference-beat-human-answers/` の流儀
