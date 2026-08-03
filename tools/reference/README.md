# リファレンス分析パイプライン

[docs/design/reference-beat.md](../../docs/design/reference-beat.md) の「分析」側。LaLa本体とは別プロセスで、
リファレンス曲1曲を **BPM・キー・コード進行・グルーヴ統計・編成・構成** に分解する。
曲ごとに1回の低頻度処理なので Python 外部プロセスに置いている（C++移植はしない）。

入出力はすべて**渡された1フォルダの中で完結する**ので、置き場はどこでもよい。
LaLa内から起動する経路（Phase 3）では `<プロジェクト>/references/<リファレンス名>/` に落ちる。

現物: `~/Music/daw/references/rau-def-freeze/`（Phase 0 のラボ第1号）

## セットアップ

```sh
./setup.sh          # mise の Python 3.12 で .venv を作り requirements.txt を入れる
```

Python は 3.12 固定（`.mise.toml` の `[tools]`）。homebrew の 3.14 では torch の wheel が無い。

## 使い方

```sh
REF=~/Music/daw/references/<リファレンス名>   # 置き場は任意。1フォルダで完結する
mkdir -p "$REF" && cd "$REF"

# 1. 音源を取る
yt-dlp -f bestaudio -x --audio-format wav --audio-quality 0 --no-playlist \
       -o 'source.%(ext)s' --write-info-json '<URL>'

# 2. MVエディットの有無を目で見る（頭のSE・尻のフェード/切断）
cd - && .venv/bin/python overview.py "$REF/source.wav" "$REF/analysis/overview.png"
.venv/bin/python overview.py "$REF/source.wav" "$REF/analysis/head.png" --end 40   # 拡大

# 3. 本編だけ切り出す（この判断だけは自動化しない）
ffmpeg -i "$REF/source.wav" -ss <開始> -to <終了> "$REF/track.wav"

# 4. 残りを通しで
./analyze.sh "$REF"
```

3:50 の曲で通し約4分。

> 手順1〜3は Phase 3（LaLa内から起動）で不要になる。取り込みはLaLaの既存機能、
> トリムは**リージョンの範囲そのもの**になるので、リージョンを `track.wav` に書き出して
> 手順4を叩くだけになる。

## スクリプト

| ファイル | 役割 | 主な出力 |
|---|---|---|
| `overview.py` | 波形・RMS・スペクトログラムを1枚に。MVエディットの検出 | `overview.png` |
| `basics.py` | BPM（固定テンポの剛体グリッド）・小節頭・キー・小節ごとのエネルギー | `basics.json` `click.wav` `sections.png` |
| `stems.py` | ステムの音量・帯域バランス・調波打楽器比・**聴きどころの小節番号** | `stems-*.json` `stems-*.png` |
| `arrange.py` | 小節×ステムの在/不在からセクションを切る | `arrangement.json` `arrangement.png` |
| `groove.py` | 16分グリッド上のドラム/ベースのプロファイル・スウィング・マイクロタイミング | `groove.json` `groove.png` |
| `topline.py` | コード進行（ループ畳み込み）・音声→MIDI・音色特徴 | `topline-*.json` `*.mid` `midi_check-*.wav` |
| `excerpts.py` | **耳で確認するための短いクリップ**を確認する順に番号を振って切り出す | `listen/*.wav` `listen/README.md` |
| `report-template.md` | レポートの雛形（節構成・各節に何を書くか・JSONキーの対応・判定語の閾値）。Phase 1 の `report.py` の入力 | — |
| `report-example.md` | 雛形に沿って実際に書いたレポートの見本（第1号のコピー） | — |

## 動作確認（耳での検算）

`analyze.sh` の最後に `excerpts.py` が走り、`<ref>/listen/` に18本前後の短いクリップができる。
Finder で開いてスペースキー（Quick Look）を連打すれば一周できる。

```sh
open "$REF/listen"
# ターミナルで通しで聴くなら
cd "$REF/listen" && for f in *.wav; do echo "$f"; afplay "$f"; done
```

- `01-click-*` — 拍クリックが曲頭と曲尾の両方でズレなければ BPM は正しい
- `02-chords-check` — **推定コードをサイン波で重ねてある。合っていれば曲に溶け、違えば濁る**。
  コード名を読むより速くて確実な検算方法（`03-loop-only` が同じ区間のコード無し版）
- `04-stem-*-quiet-*` — 分離の破綻・幽霊音が一番聴こえるのは「鳴っているが小さい」区間
- `05-midi-*` — 左が原音、右が basic-pitch の結果

## 既知の壊れ方

検証したのは「テンポ完全固定・ループ1つ・ハネ無し・打ち込み」の1曲だけ（2026-08-03）。
以下は**確実に壊れると分かっている**条件。壊れたことに気づけるかどうかも併記する。

| 曲の性質 | 何が壊れるか | 気づき方 |
|---|---|---|
| **途中でコード進行が変わる** | `topline.py` は全長を1つのループとして畳むので、複数の進行が平均されて実在しない和音が出る | `loop.similarity_by_lag` がどのラグでも低い / `loop_progression.progression[].stability` が下がる |
| **生ドラム・テンポが揺れる** | `basics.py` の剛体グリッドが破綻し、後半の小節番号が全部ずれる。以降の全分析が道連れ | `tempo.grid_contrast_by_eighth` の後半だけ落ちる / `tempo.local_bpm_std` が大きい |
| **3拍子・6/8系** | 4/4 決め打ちなので小節・16分プロファイル・ループ長が全部無意味になる | **気づけない**（サイレントに壊れる）。拍子は人が確認するしかない |
| **イントロ/アウトロが長い曲** | ループ畳み込みが「本編以外」も混ぜる | `repetitions_folded` の割に `stability` が低い |

前3つは**壊れ方を知るために意図的に試す価値がある**。2曲目以降は素直な曲でなく、
上のどれかに当たる曲を選ぶ方が学びが多い。結果は [docs/labs/reference-beat.md](../../docs/labs/reference-beat.md) に追記する。

## 設計上の注意（実測で踏んだもの）

- **BPM は「テンポ一定」を仮定して剛体グリッドを当てる。** 1拍ずつ追うビートトラッカーは打ち込みでも
  数十ms揺れ、そのまま小節グリッドにすると曲の後半でズレる
- **BPM の粗探索は曲の中ほど45秒だけで行う。** 全長で粗く探すと、BPM が Δ ずれたグリッドは終端で
  `span×Δ/bpm` 秒ぶん歩くため（230秒・97BPM なら 0.25BPM 刻みでも1/3拍）、正解の近傍が谷になる
- **探索レンジは 70-150 に絞る。** HIPHOP は8分ハットが4分のキック/スネアと同じくらいオンセットが立つので、
  レンジを広げると倍テンポ（8分グリッド）が僅差で勝つ。`octave_check` に半分/倍の素点も出している
- **低域のオンセット位置は16分単位では信用しない。** 帯域通過の時間分解能が原理的に足りない
- **ハット帯域にはクラップのアタックが混入する**（クラップは広帯域なので分離できない）
- **コードはループ長で畳んでから当てる。** 1区間ごとの誤検出が繰り返しで打ち消され、進行の骨格だけ残る
- **basic-pitch は持続音を短く刻む。** 統計を借りる用途には足りるが、譜面としてはそのまま使えない
- **閾値の計算をループの条件式に書かない。** `if rise[i:i+step].max() > np.percentile(rise, 99.5)` は
  50msごとに500万要素の percentile を引き直し、この1行だけで2分半かかっていた（全体18分のうち13分）。
  外に出して 0.31秒。分析は全長の配列を回すので、こういう1行が桁で効く
- **書き出した音声は長さでなく RMS/peak を見る。** 「ファイルがある・長さが正しい」だけ確認していたせいで、
  中身が全部無音のクリップをレビューに出したことがある。`excerpts.py` は自分の出力を検算して
  無音があれば非ゼロ終了する
