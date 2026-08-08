# tools/library — サンプルライブラリのインデックスとおすすめ

リファレンス駆動ビートメイクの「上モノはループ素材を検索で引く」側の道具。
設計は [docs/design/reference-beat.md](../../docs/design/reference-beat.md) の「音色の調達」節と
[docs/plans/2026-08-07-2319-loop-track.md](../../docs/plans/2026-08-07-2319-loop-track.md)。

## ライブラリの置き場

実体は iCloud Drive（マシン間同期）、参照は symlink 経由:

```
~/Music/daw/library → ~/Library/Mobile Documents/com~apple~CloudDocs/daw-library/
    ├── loops/<パック名>/       # ループ素材。ファイル名はリネームしない（Am_82bpm 等が主メタデータ）
    ├── oneshots/<パック名>/    # ワンショット。現状は置き場のみ（インデックス対象外）
    ├── _contrast/loops/…       # レコメンド検証用のジャンル外素材
    ├── _contrast/oneshots/…
    └── index.json              # index.py の出力（同期される。特徴量の再計算はマシン間で不要）
```

- セットアップ: `mise run lib:setup`（冪等）。実行後に Finder で daw-library に
  「ダウンロードを保持」を指定する（iCloud の最適化がローカル実体を退避すると試聴が止まる）
- 市販サンプルは royalty-free でも**素材のままの再配布は禁止**。アプリ成果物や公開リポジトリに
  入れない（ライブラリはあくまで私物）
- **Cymatics Hub で落としたパックは一部フォルダが平坦化される**: zip内のWindows形式エントリが
  `Melodics\Melody Loops\xxx.wav` のような**バックスラッシュ入りの1ファイル**として展開される
  （正常なサブフォルダと混在する）。振り分けるときは `\` で分割した末尾（本来のファイル名）を
  復元してから `loops/` 等へ置く。`Loop Stems` 系のフォルダは楽器別分解版なので通常はライブラリに
  入れない

## スクリプト

venv は `tools/reference/.venv` を共用する（gacha と同じ方式）。

| スクリプト | 役割 | mise |
|---|---|---|
| `setup.sh` | 置き場と symlink の冪等セットアップ | `lib:setup` |
| `index.py` | loops/ をスキャンして index.json を生成（差分更新・原子的書き込み） | `lib:index` |
| `recommend.py` | おすすめ5 — キー±2半音/BPM±10%で足切り→特徴量距離で並べる（決定的・ページング型） | `lib:recommend` |
| `evaluate.py` | 効きの検証レポート（対照群が沈むか・リファレンス間で上位が入れ替わるか）。CI対象外 | `lib:evaluate` |
| `looproots.py` | 採用ループからルート列の契約 JSON を抽出（`bass.py --roots` の入力。**契約は docstring が真実の源**） | `lib:roots` |

- `index.py` はキー/BPM を**ファイル名から**読み、無いときだけ音声から推定する
  （`*_source` フィールドで出所が分かる。推定は filename より信頼度が落ちる）
- パック追加・削除のたびに `mise run lib:index` を回す。size+mtime が同じファイルは
  再分析しないので2回目以降は速い
- `recommend.py` は**同曲のバリエーションを1枠に集約**する（テイク番号 `_1`〜・`_LOFI` を
  剥がした名前＋パック＋BPM＋キーが一致したら距離最小の1本だけ出す。`group_size` に隠れた
  本数。APS ギターパックが1曲最大10ファイルで枠を占領した実害への対処）
- `recommend.py` の距離は index と同じ `compute_features` を通した
  リファレンスの上モノ合算（6分割 piano/guitar/other。`analysis/upper-features.json` にキャッシュ）
  と比べる。BPM フィルタは**±10%のみ**（倍/半分テンポ表記の許容は逆コピーの意味論が
  未定義になるため撤回 — 経緯は `recommend.py` の `tempo_relation` コメント）
- ループ追従の使い方: `mise run lib:roots -- <loop.wav> --out roots.json` →
  `mise run gacha:bass -- <カード> --key ROOT:MODE --roots roots.json`
  （進行がループで固定され、振り直しで変わるのはリズムだけ）

## テスト

```
mise run lib:test   # 合成 wav のみ・数十秒
```
