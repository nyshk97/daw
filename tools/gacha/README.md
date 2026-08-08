# MIDI生成ガチャ

[docs/design/reference-beat.md](../../docs/design/reference-beat.md) の「生成」側（Phase 2）。
分析パイプライン（`tools/reference/`）が作る制約カード `card.json` を読み、パーツの候補を
ルールベースで振る。card.json を挟んで分析とは疎結合 — 生成器はカードしか読まない。

現状はドラムとベース（`drums.py` / `bass.py`）。
venv は分析側の `tools/reference/.venv` を再利用する（依存が分析側の部分集合のため）。

## 使い方

```sh
mise run gacha:drums ~/Music/daw/references/<名前>        # 8候補を <名前>/gacha/ に生成
mise run gacha:bass  ~/Music/daw/references/<名前> -- --key A:minor   # ベース（キー必須）
mise run gacha:test                                       # 回帰テスト（fixture のみ・数十秒）
```

ベースの**ループ追従モード**（採用ループの実進行に従い、リズムだけ振る）:

```sh
mise run lib:roots -- <loop.wav> --out roots.json         # ループからルート列の契約を抽出
mise run gacha:bass -- <フォルダ> --key A:minor --roots roots.json
```

契約 JSON の形式は `tools/library/looproots.py` の docstring が真実の源。
不正な契約は黙って退化せず即エラーで止まる。

```
drums.py <リファレンスフォルダ|card.json>
    --seed N       全体 seed（省略時は乱数で決めて表示。--count は seed, seed+1, … と回す）
    --count N      候補数（既定 8）
    --bars N       書き出す小節数（1小節パターンの繰り返し。既定 4）
    --lock ...     レーン seed の固定: kick=HEX8,snare=HEX8,hat=HEX8
    --out DIR      出力先（既定はカードと同じフォルダの gacha/）
drums.py --from <候補の .json>       サイドカーから1候補を完全再現（カード不要）
```

出力は候補ごとに3点セット:

```
drums-k<kick seed>-s<snare seed>-h<hat seed>-<設定ハッシュ6桁>.mid   GMドラム(36/38/42・ch10)
                                                            .wav   耳チェック用の合成音
                                                            .json  サイドカー（再現情報の全部）
```

## ガチャの回し方

1. `mise run gacha:drums <フォルダ>` → `gacha/` を Finder で開いて QuickLook（スペース連打）で一周
2. キックが気に入った候補があったら、ファイル名の `k` の8桁を控えて固定し、他を振り直す:

   ```sh
   mise run gacha:drums <フォルダ> -- --lock kick=8f3a21bc
   ```

3. 気に入った候補の `.mid` をピアノロールで手直しする（採用は特別な操作ではない）

- **同じ seed＋同じカード＝バイト単位で同じ出力**。ファイル名が同じ候補は再生成されない
  （スキップと申告される）。カードを分析し直す等で設定が変われば名前のハッシュ部が変わる
- サイドカー `.json` には全体 seed・レーン seed・**補完済みの実効設定そのもの**が入っており、
  カードが消えても `--from <候補の.json>` だけで同じ .mid/.wav をバイト単位で再現できる。
  生成器のバージョンが変わっていた場合は cfg ハッシュの再計算が合わず、黙って別物を出さずに
  エラーで止まる

## カードのどの値がどう効くか

聴きたいのはグルーヴ（位置・強弱・ハネ）であって音色ではない。wav の音色は仮
（kick=サインのピッチスイープ / snare=ノイズ＋胴鳴り / hat=高域ノイズ）で、本番の音は
LaLa の Drum Kit（GM ch10）が出す。

| カードの値 | 生成での意味 |
|---|---|
| `global.bpm` | テンポ（.mid のテンポトラックと .wav の実時間） |
| `*_profile_by_*` | **平均オンセット強度**。0.35 以上は骨格（毎回鳴る）、未満は装飾（強度を確率としてサンプリング）。ガチャの振れは装飾層から出る。0.35 は分析側 `active_16ths` と同じ線 |
| `swing_ratio` | 8分裏を `(ratio−0.5)×4分音符` だけ動かす（符号付き。0.482 なら裏拍が約10ms突っ込む） |
| `quantize_dev_ms` | マイクロタイミングの散らし幅（\|ズレ\|の平均→ガウスσに換算。±3σと16分の40%でクリップ） |

フィールドが**無い**カード（ゲート落ちの正当な省略）は既定値で補完して申告する
（snare→2・4拍バックビート、hat→8分刻み、kick→拍1・3、swing→ストレート）。
**在るのに壊れている**値（card_version 違い・BPM 範囲外・0〜1 外のプロファイル）は
エラーで停止する — 正規カードでは起きないので、起きたら手編集かバージョン不整合。

## 設計上の注意

- パターンそのものはコピーしない。プロファイルはヒット列ではなく強度の統計なので、
  「統計的な性格だけ借りる」（設計文書）と実装が一致している
- seed 導出は SHA-256（Python の `hash()` は PYTHONHASHSEED でプロセスごとに変わる）。
  **導出式を変えると過去候補の seed 再現が全滅する**ため、テストが既知値で固定している
- wav のノイズも専用 seed で決定的（未 seed の RNG だと同じ MIDI でも wav が毎回変わり
  「同名なら同内容」が崩れる）
- 候補の書き出しは 一時ファイル→検算→リネーム の原子的公開。サイドカーは最後に書く
  完成マーカーで、途中失敗の残骸は次回実行時に再生成される
- 自分の出力は検算する: wav の peak が 0dBFS を超えたら（3声同時のクリップ）・ほぼ無音なら
  非ゼロ終了。peak は **PCM 化で黙ってクリップされる前の float 配列**で見る
