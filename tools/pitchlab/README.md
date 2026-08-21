# pitchlab — ボーカルのピッチ補正ラボ（Phase 0）

「検出（YIN 自作で足りるか）」と「再合成（どのエンジンが自然か）」を実素材・実耳で確定するラボ。
真実の源は `docs/plans/2026-08-20-2244-vocal-pitch-correction.md`（Phase 0 の判定基準）。
記録は `docs/labs/pitch-correction.md`（追記専用）。

```sh
./setup.sh            # venv（mise の Python 3.12）＋ signalsmith CLI（work/ssstretch）のビルド
./run_all.sh          # 全工程（約10分）。個別は下記
.venv/bin/python synth.py                                   # 既知 f0 の合成素材 → work/synth/
.venv/bin/python analyze.py <wav> <name> [--truth t.json] [--crop a b]   # YIN/pYIN/CREPE 比較 → work/<name>/
.venv/bin/python notes.py <name>                            # f0 → ノート列（C++ 移植の原型）
.venv/bin/python resynth.py <name> [--scale auto|chromatic|<root>:<major|minor>]  # 4 エンジン × ケース Z/A/B/C/D
.venv/bin/python blindprep.py && .venv/bin/python answer_template.py        # listen/ と回答テンプレート
```

- C++ へ移植する仕様の原型: `yin.py`（検出）・`notes.py`（ノート分割）・`correction.py`（目標カーブ・時間写像）・
  `engines.py` の `render_psola`（TD-PSOLA＋無声素通し）
- `ssstretch.cpp` は LaLa 本体と同じ pin の signalsmith-stretch を時変シフト＋区分線形時間写像で駆動する CLI
- Rubber Band（`rubberband` CLI・GPL）は**品質の天井の参照のみ**。採用対象外
- コミット対象はスクリプトのみ。`work/`（中間）・`listen/`（ブラインド）・`.venv/` は gitignore 済み。
  **実声 WAV を `docs/` や repo 直下へ出さない**（PUBLIC リポジトリ）
