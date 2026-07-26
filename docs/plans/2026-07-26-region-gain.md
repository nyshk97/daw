# オーディオリージョン単位のゲイン調整

## 概要・やりたいこと

オーディオリージョン（`Clip`）ごとにゲイン（トリム）を持たせ、右クリック → 吹き出しのスライダーで調整できるようにする。

### なぜ必要か

現状のゲインは2層あるが、**1トラックの中の時間方向の音量差**を直す手段がない。

```
サンプル素材 →[ Track::sampleGain ]→ 音源 →[ TrackParams::gain ]→ 曲
                 素材のばらつき                  曲の中でのバランス

録音/取り込み →[      ？      ]→ ────────→[ TrackParams::gain ]→ 曲
                リージョン単位（今回追加）
```

- **オートメーションを持たない方針の穴埋め**になる。曲の途中で音量を変える手段が今ゼロで、「⌘Tで切ってそのリージョンだけ下げる」が唯一の代替手段になる。⌘T分割・リージョン移動・ミュートは実装済みなので土台はある
- **本命はボーカルの部分補正**。破裂音・語尾の張り・小さすぎる息継ぎを、コンプに入る前に均す。トラックフェーダーでは1本の中の一部だけを下げられず、ミュートは「消す」だけで「下げる」ができず、フェードでも短い破裂音は狙えない。代替で逃がせない
- テイクごとの音量ばらつき（別日録り・マイク距離違い）を、同一トラックに並べたまま揃えられる

`docs/design/region-settings.md` の項目表に「リージョンゲイン（オーディオ専用）／波形の描画振幅をゲイン込みでスケール」として既に載っており、未決だった「具体操作」を今回決めた。

## 前提・わかっていること

### 決定事項（2026-07-26 の dig-lite で確定）

| 項目 | 決定 | 根拠 |
|---|---|---|
| 対象 | オーディオリージョン（`Clip`）のみ。MIDIリージョンには持たせない | `region-settings.md` で「オーディオ専用」と決着済み |
| 値域 | ±12dB（`GainScale` をそのまま流用・新規スケール定義なし） | スケールが2種類に増えない。破裂音を叩く（-4〜-8dB）・テイクを揃える（±2〜3dB）は範囲内 |
| モデルの保持 | 線形倍率（`Track::sampleGain` と同じ流儀） | 既存の変換・クランプ・表示文字列がそのまま使える |
| 操作 | 右クリック →「ゲイン…」→ `CallOutBox` のスライダー | 下記「操作方式の選定」 |
| undo | 対象にする。粒度は**ドラッグ開始で1回**積む | 同じ `Clip` のフィールドである `muted` がundo対象（`TimelineView.cpp:1352`）。粒度は `InstrumentDetailView.cpp:281` の前例に揃える |
| 複数選択への一括適用 | なし | `ClipSelection` は単数（track+clip）で、複数選択の概念自体が無い |
| 反映範囲 | 再生・⌘Bバウンス・⌘Eリージョン書き出しすべて。**ドラッグ中の再生音にも即反映する** | 「波形の見た目＝出る音」を崩さない。ドラッグ中だけ見た目と音がずれるのは、波形を見ながら調整するという案Bの選定理由そのものに反する |
| リージョン側の痕跡 | 波形の描画振幅をゲイン込みでスケール ＋ 0dB以外のとき**本体の右上**にdBバッジ（幅56px未満のリージョンでは出さない） | 常設パネルを作らない方針（`region-settings.md`）。振幅だけでは -1dB のような微差が読めないのでバッジで補う。右下は**ループハンドルの場所**（`loopHandleRect`）なので避ける |
| ショートカット | 今回は入れない | 右クリックで足りるか使ってから判断する。後から足せる（`Shortcuts.h` に1行） |

### 操作方式の選定（案A: メニュー埋め込み を見送った理由）

モックで両案を比較（`scratchpad/region-gain-popup-mock.html`）した上で、JUCE 8.0.9 の実装を読んで判断した。

- **スライダーの幅を制御できない** — `addCustomItem` のdocに「the items will be stretched to have a uniform width」と明記。他の項目（"再生ヘッド位置で分割 ⌘T"等）の文字幅でGAINの解像度が決まってしまう
- **モーダルループ中にモデルを書き換える初のパターン**になり、`TimelineView.cpp:1324` の「メニュー表示中はモーダルで他の編集操作が発生しない前提」というコメントが崩れる
- **未知の挙動が残る** — ドラッグでマウスがメニュー外に出ても閉じないことはソースで確認できた（トップレベルは `hideOnExit = false`（`juce_PopupMenu.cpp:952`）／`addMouseListener (&parent, false)`（同`147`）でnested childのイベントは親に転送されない）が、実機検証するまで確証がない
- モックで見えた実害: **メニューがリージョンを覆って波形の変化が見えない**

`CallOutBox` は位置決めと矢印の向きをJUCEが自動でやる（`updatePosition`）ので、渡すのは指し示す矩形だけ。

なお `CallOutBox::launchAsynchronously` は**非同期だがモーダル**（`juce_CallOutBox.cpp:71` が `callout.enterModalState (true, this)` を呼ぶ）。表示中はキー入力も⌘Zも `CallOutBox` が取り、外部クリックはボックスを閉じてから下のコンポーネントへ渡る。ドラッグ中の再描画・タイマーは通常どおり走る。

### 既存資産（そのまま流用する）

`InstrumentDetailView` のサンプル音源GAIN（`InstrumentDetailView.cpp:273-296`）が必要なものを持っている。

```cpp
gainSlider.setRange (-GainScale::rangeDb, GainScale::rangeDb, 0.1);
gainSlider.setDoubleClickReturnValue (true, 0.0);
gainSlider.textFromValueFunction = [] (double v) { return GainScale::text (v); };
gainSlider.setPopupDisplayEnabled (true, false, nullptr);   // ドラッグ中のdB表示
gainSlider.getProperties().set ("centerFill", true);        // 0dB起点で左右に伸びる帯
gainSlider.onDragStart = [this] { if (onWillEdit) onWillEdit (true); };  // undoはドラッグ開始で1回
```

- `GainSlider`（`snapValue` で0dB吸着）と `GainValueLabel`（クリックで0dBリセット・ホバー枠）は `InstrumentDetailView` のprivate inner classなので、共有するには切り出しが要る
- 描画は `AppLookAndFeel::drawHorizontalVolumeBar` の `centerFill` 分岐がそのまま効く

### 変更が必要な箇所（調査済み）

行番号は `a8a1f20`（リージョンのリピート/ループ追加）時点。

| レイヤ | 場所 | 内容 |
|---|---|---|
| モデル | `Project.h:20` `Clip` | `float gain = 1.0f;` を追加 |
| 保存 | `Project.cpp:323` 付近 | `clipObj->setProperty ("gain", (double) clip.gain)` |
| 読込 | `Project.cpp:515` 付近 | `GainScale::clampLinear` を通す。キー欠落時は 1.0 |
| バージョン | `Project.h:210` `currentVersion` | **9 → 10**（v9はループで使用済み） |
| スナップショット | `PlaybackSnapshot.h:50` `ClipPlayback` | `float gain = 1.0f;` を追加 |
| 〃 構築 | `Project.cpp:110` `appendClipPlaybacks` | **ここ1箇所**で `gain` を埋めれば再生・⌘E・⌘Bの全経路に効く（ループ展開の共通ヘルパー。⌘Bは `buildSnapshot` の結果を move して使う＝`MainComponent.cpp:1625`） |
| 再生 | `PlaybackEngine.cpp:275`（モノ経路）/ `:348`（ステレオ経路） | `addFrom` のゲイン引数に乗算 |
| 〃 差し替え検出 | `PlaybackEngine.cpp:113` `snapshotChanged` | ポインタ比較 → MIDI世代の比較（Phase 3） |
| 〃 世代 | `PlaybackSnapshot.h` `PlaybackSnapshot` | `juce::uint64 midiGeneration` を追加（Phase 3） |
| 〃 push | `MainComponent.cpp:2249` `pushSnapshot()` | オーディオのみ更新モードを追加（Phase 3） |
| undo種別 | `UndoStack.h:25` `EditKind` | `clipValue` を追加。`afterHistoryRestore`（`MainComponent.cpp:1072`）の分岐に第3の枝（Phase 3） |
| スケール定義 | `GainScale.h` の説明コメント | 「サンプル音源のGAIN」専用の記述を素材トリム共通へ一般化（Phase 1） |
| 書き出し | `BounceRenderer.cpp:261` 付近の合成ループ | `ClipPlayback` を読む側なので、ここでも乗算（2経路構造は既存コメントのとおり） |
| 描画 | `TimelineView.cpp:538-551` `drawClip` の波形ループ | ピーク値にゲインを掛ける（`peakCache` 自体は触らない）＋dBバッジ |
| 操作 | `TimelineView.cpp:1418` `showItemMenu` | 「ゲイン…」項目＋`CallOutBox` 起動 |

### 注意点

- **`PlaybackEngine.cpp:250` の「このブロックは演算順序を含め一切変えないこと」**（ビット一致の回帰コメント）。`gain * clipGain` で `clipGain == 1.0f` のときは IEEE754 上ゲイン値が完全一致するため、既存プロジェクトの出力ハッシュは変わらない**はず**。`testMonoRenderRegressionHash` の変更前後比較で必ず裏を取る
- **再生と書き出しは2経路**（`PlaybackEngine` と `BounceRenderer` が別実装）。片方だけ直すと「聴こえ方と書き出しが違う」になる
- **`pushSnapshot()` はMIDIを消音＋再発音させる** — `PlaybackEngine.cpp:113` がスナップショットのポインタ差し替えを検出して `snapshotChanged` を立て、`:186,188` で `silenceTransport` と `resound` に渡している（再生中の編集でノートオフが失われて鳴りっぱなしになるのを防ぐ既存の安全機構）。ドラッグ中に毎イベント `pushSnapshot()` を呼ぶと、鳴っているMIDIが連打される。`InstrumentDetailView` のGAINが再pushを避けて `SamplerEngine` のatomicミラーだけ更新しているのも同じ理由（`MainComponent.cpp:219-226` のコメント）
- **`CallOutBox` 表示中のindex失効** — 表示中はモーダルなので、ユーザー操作で `Clip` が削除・分割されることはない（キーも外部クリックも `CallOutBox` が先に取る）。残る脅威は**録音の終了でクリップが増える**ケースと、プロジェクト破棄。`Clip` には `MidiRegion` のような id が無くインデックス参照しかできないため、この2つだけを防御する

## 実装計画

### 事前準備 [人間👨‍💻]
- [x] なし（既存の開発環境で完結する）

### Phase 1: モデル層 [AI🤖]
- [x] `Clip` に `float gain = 1.0f;`（線形倍率・値域は `GainScale`）を追加し、コメントで `sampleGain` と同じ流儀であることを示す
- [x] `Project::currentVersion` を 10 に上げ、ヘッダのバージョン履歴コメントに「v10: リージョン/クリップのゲイン」を追記
- [x] 保存に `gain` を追加（`muted` と同じく常に書く）
- [x] 読込で `GainScale::clampLinear` を通す（キー欠落＝v9以前は 1.0）
- [x] `GainScale.h` の説明コメントを一般化する。現在は「サンプル音源のGAIN（トリム）のスケール定義」「モデル（`Track::sampleGain`）= 線形倍率」「`sampleGain` への代入経路はスライダー・新規作成の1.0f・読み込みの3つだけ」と書いてあり、`Clip::gain` と共有すると事実と食い違う。**素材トリム共通**（サンプル音源のGAIN＋オーディオリージョンのゲイン）の記述に書き換え、`clampLinear` の「保存側では使わない」根拠にある代入経路の列挙に `Clip::gain` の分（スライダー・新規作成・読み込み・split/duplicateのコピー＝新たな値を生まない）も加える
- [x] テスト追加: 保存→読込のラウンドトリップ・範囲外のクランプ・キー欠落時の既定値（`testClipOffsetsV2Migration` 近辺の流儀に合わせる）

### Phase 2: 再生・書き出しへの反映 [AI🤖]
- [x] **先に** `testMonoRenderRegressionHash` の現在のハッシュを採取して控える（`build/daw_tests_artefacts/Debug/daw_tests | grep hash-`）
- [x] `ClipPlayback` に `float gain = 1.0f;` を追加し、`appendClipPlaybacks`（ループ展開の共通ヘルパー）で埋める。再生・⌘E・⌘Bの3経路がこのヘルパー経由なので、モデル→スナップショットの変換はここだけで済む
- [x] `PlaybackEngine` のモノ経路・ステレオ経路それぞれで `addFrom` のゲインに乗算（演算順序を変えず、既存の `gain` に掛ける形にする）
- [x] `BounceRenderer` の合成ループにも同じ乗算を入れる
- [x] テスト追加（正常系だけで済ませず、既存テストに非ユニティのクリップゲインを織り込む）:
  - [x] `buildSnapshot` / `buildItemRender` が値を載せること・読込クランプ後の値が載ること
  - [x] **モノ経路**: -6dBのクリップが約半分の振幅で鳴ること
  - [x] **ステレオ経路**: `testEngineStereoPan` にクリップゲインを加え、L/Rのバランスを保ったままスケールされること
  - [x] **重なりクリップ**: 同一トラック上で重なる2クリップに別々のゲイン（例 -6dB / +6dB）を与え、加算結果が期待どおりになること（クリップ単位に効いていることの証明。トラックゲインでは再現できない）
  - [x] **再生とバウンスの一致**: `testEngineBounceStereoConsistency` にクリップゲインとsendを加え、2経路の出力が一致すること
- [x] `testMonoRenderRegressionHash` のハッシュが**変わっていない**ことを確認（gain=1.0時のビット一致）

### Phase 3: ドラッグ中に音へ反映する経路 [AI🤖]

`pushSnapshot()` はMIDIの消音＋再発音を伴うため、ドラッグ中に呼ぶと鳴っているMIDIが連打される（前提の「注意点」参照）。オーディオのクリップゲインだけが変わったときは、この副作用を起こさずにスナップショットを差し替えられるようにする。

- [x] `PlaybackSnapshot` に「MIDIの構成世代」を持たせる（`juce::uint64 midiGeneration`）。カウンタは **`Project` が所有**し、`buildSnapshot (SnapshotChange)` の引数で進め方を決める（既定は安全側＝進める）:
  - `midiStructure`（既定）… エンジンへ渡す・MIDI構成が変わった可能性あり → 進める
  - `audioValuesOnly` … エンジンへ渡す・オーディオ値だけ → **据え置く**
  - `offlineRender` … エンジンへ渡さない構築（⌘Bのバウンス用）→ 世代に触らない
  - `MainComponent` に持たせない理由: `buildSnapshot()` を直接pushする経路（テストの大半）が世代0のままになり、「再生中のノート削除で音が止まる」安全機構が壊れる
- [x] `PlaybackEngine` の `snapshotChanged`（現在はポインタ比較）を `midiGeneration` の比較に置き換える。この値は消音・resoundの2箇所（`:186,188`）にしか流れていないことを確認済みなので、オーディオ側の挙動は変わらない
- [x] `MainComponent::pushAudioValueSnapshot()` を足す（`synthBank.sync()` は呼ばない。音源の作り直しは不要で、ドラッグ中に毎イベント走らせたくない）。⌘Bのバウンス用構築は `offlineRender` を渡す
- [x] **確定・リセットでも通常pushを走らせない**。現在の `timeline.onModelEdited`（`MainComponent.cpp:263-268`）は通常pushなので、これを使うとマウスを離した瞬間にMIDIが鳴り直す。`TimelineView` にクリップゲイン専用のコールバックを2本足す:
  - `onWillEditClipGain` … `undoStack.begin (*project, EditKind::clipValue)`
  - `onClipGainEdited` … `setDirty(true)` ＋ オーディオのみ更新push ＋ `lanes->repaint()`。**ドラッグ中・ドラッグ確定・0dBリセットのすべてがこの1本を通る**
  - `onWillEditClipGain` を呼ぶのは **「値が実際に動いた最初の1回」だけ**で、区切りは **`GainSlider::onNewClickSequence`**（`mouseDown` のうち `getNumberOfClicks() <= 1` のもの）で入れる。理由:
    - JUCEのSliderは mouseDown ごとに `ScopedDragNotification` を作り（`juce_Slider.cpp:900`）、ダブルクリック確定時にも別の通知を出す（同`:1121`）ため、`onDragStart` を区切りにすると1回のダブルクリックでundoが2〜3件に割れる
    - つまみ以外をダブルクリックすると1クリック目で値がクリック位置へ飛ぶので、区切ってしまうと**最初の⌘Zが元の値でなく中間値へ戻る**
    - `getNumberOfClicks() >= 2` はJUCE自身が `mouseDoubleClick` のトリガーに使っている判定（`juce_Component.cpp:2325`）なので、その裏返しでクリック列の先頭を拾える
    - `setValue` には同値ガード（`juce_Slider.cpp:218`）があるので、値が動かないクリックではundoが積まれない
    - ホイールでの値変更は `mouseDown` を伴わず区切りが入らない（ドラッグ後にホイールで微調整すると⌘Zがドラッグ前まで戻る）ため、`GainSlider` のコンストラクタで **`setScrollWheelEnabled (false)`**。区切りを増やすより経路を1本に絞る（ドラッグ＋0dBリセットで足りる／パネル上でスクロールして意図せず音量が変わる事故も防げる）。`testGainSliderIgnoresScrollWheel` で固定した
- [x] **undo/redoでもMIDIを乱さない**。`UndoStack::EditKind` に `clipValue` を追加し、`MainComponent::afterHistoryRestore`（`:1079`）に第3の枝を作る。`sampleValue` は「atomicミラー更新で足りる＝pushしない」だが、`clipValue` はスナップショット経由の値なので**オーディオのみ更新pushが要る**（この違いをコメントに残す）
- [x] テスト追加: 再生中にオーディオのみ更新をpushしてもMIDIの発音が途切れない／通常pushでは従来どおり消音＋resoundが走る（`testSnapshotSwapDuringPlayback` を参考にする）／`clipValue` のundo/redoでクリップゲインが戻ること
- [x] 呼び出し側の責任（MIDI構成を変えていないことの保証）をコメントで明記し、使用箇所をクリップゲインの3経路（ドラッグ中・確定・undo/redo復元）のみに限定する

### Phase 4: リージョンの見た目 [AI🤖]
- [x] `drawClip` の波形描画でピーク値にゲインを掛ける（`peakCache` は生のまま。`jlimit` の上限で頭打ちになるので、上げ側は自然に潰れる）
- [x] 0dB以外のときリージョン**右上**にdBバッジ（`+3.0` / `-4.5`）。幅56px未満のリージョンでは出さない。表示名（左上）の描画幅からバッジ分を差し引き、長い取り込みファイル名でdB値が潰れないようにする。将来トランスポーズのバッジが増えたら、ここから左へ並べる
- [x] オフスクリーン描画（VERIFY.md「アプリを起動せずに描画を確認する」）で 0dB / -6dB / +6dB の3状態をPNG化して目視

### Phase 5: 操作UI [AI🤖]
- [x] `InstrumentDetailView` の `GainSlider`（0dB吸着）と `GainValueLabel`（クリックで0dBリセット）を `ui/GainControls.h` へ切り出して共有可能にする
- [x] 切り出し後、`InstrumentDetailView` の見た目が変わっていないことをピクセル差分で確認（VERIFY.md「UI変更のピクセル差分検証」。差分ゼロが期待値）
- [x] `CallOutBox` の中身になる `RegionGainPanel`（見出し "GAIN"＋現在値ラベル＋スライダー）を作る。値ラベルのクリックで0dBリセット（`InstrumentDetailView` と同じ作法）
- [x] `showItemMenu` に「ゲイン…」項目を追加（オーディオリージョンのときだけ）。項目には現在値を右側に出す（0dBのときは空）
- [x] `CallOutBox::launchAsynchronously` でリージョン矩形を指して表示。矩形は `lanes` 座標 → `TimelineView` 座標に変換し、**viewportとの交差**を渡す（長いリージョンや横スクロールで大半が画面外だと、矩形の中央＝矢印の候補が画面外になる）
- [x] undoは「値が動いた最初の1回」で `onWillEditClipGain`（区切りは `onNewClickSequence`）、値の反映は `onValueChange` から `onClipGainEdited`（Phase 3 の専用経路。通常の `onModelEdited` は**使わない**）。これで見た目と音が同時に動き、マウスを離してもMIDIが鳴り直さない
- [x] 0dBリセット（値ラベルのクリック・ダブルクリック）もundo 1件として積み、反映は同じ `onClipGainEdited` を通す（リセット経路では `onWillEdit` 相当を直接呼ばず、`onValueChange` 側の1件に任せる）
- [x] 録音開始時に `CallOutBox` を `dismiss()` する（録音終了でクリップが増え、保持中のindexが失効する唯一の現実的な経路）。`TimelineView` に `SafePointer<juce::CallOutBox>` を持って参照する
- [x] 値の適用時にも `trackIndex` / `itemIndex` の範囲を再検証する（`toggleMuteAt` と同じ防御。安価な保険として残す）

### Phase 6: 仕上げ [AI🤖]
- [x] `docs/design/region-settings.md` の項目表を更新（リージョンゲインの「方式」を確定した内容に書き換え、未決リストから外す）
- [x] VERIFY.md にリージョンゲインの確認手順を追記（再利用可能な範囲のみ: CLIでのテストプロジェクト用意＋ゲイン付きproject.jsonの形・オフスクリーン描画での見た目確認）
- [x] `cmake --build build` と `ctest` が通ることを確認

### 動作確認 [人間👨‍💻]
- [ ] 実機で右クリック →「ゲイン…」→ ドラッグ。**吹き出しの出る位置と掴みやすさ**（合成クリックでは操作性を検証できないため）
- [ ] ドラッグ中に波形の振幅が追従して見えるか（案Bを選んだ理由そのものの確認）
- [ ] **再生しながら**ドラッグして、音がリアルタイムに変わるか。同時にMIDIトラックを鳴らして、その発音が乱れないか（Phase 3 の確認）
- [ ] 破裂音のあるテイクを⌘Tで切って下げ、耳で意図どおりか
- [ ] **undoの粒度**: 1回のドラッグが⌘Zで1件だけ戻ること（ドラッグ中の中間値が積み上がっていないこと）
- [ ] **0dBリセットもundoできる**こと（値ラベルのクリック／ダブルクリックの両方）
- [ ] **MIDIを鳴らしたまま**、ドラッグ確定（マウスを離す）・0dBリセット・⌘Z/⌘⇧Z を行って、MIDIの発音が乱れないこと（Phase 3 の3経路すべての確認）
- [ ] ⌘S → プロジェクトを開き直してゲインが復元されるか
- [ ] ⌘Eと⌘Bの書き出しが、聴こえている音と一致しているか

## ログ

### 試したこと・わかったこと

- 変更前の回帰ハッシュ（`a8a1f20` 時点）: `hash-engine: 59c79a3b0352743f` / `hash-bounce: 96f5e41dc31e0732`
- 計画立案中に `a8a1f20`（リージョンのリピート/コピー/ループ）がコミットされ、前提が2つ変わった:
  - **project.json は既に v9**（ループ用）なので、リージョンゲインは **v10**
  - ループ展開が `appendClipPlaybacks`（`Project.cpp:110`）に集約され、再生・⌘E・⌘Bの3経路すべてがこのヘルパー経由になった。当初「2経路それぞれで `push_back` を直す」としていたモデル→スナップショット変換が**1箇所**で済む
- 実装後の回帰ハッシュは変更前と**完全一致**（`hash-engine: 59c79a3b0352743f` / `hash-bounce: 96f5e41dc31e0732`）。`gain * 1.0f` が厳密に `gain` と同値なので、既存プロジェクトの出力は変わっていない
- **テストで `SnapshotExchange` に3回以上pushするときは間に `deleteRetired()` が必要**。`acquire()` は retired が空のときだけ pending を取り込む（`PlaybackSnapshot.h:179`）ため、掃除しないと3回目以降の push が反映されず「実装が効いていない」ように見える（実際に一度誤診した）
- レビュー指摘で直したもの: ①**⌘Bのバウンス用構築が世代を進めていた**（その後リージョンゲインを動かすとエンジンが世代変更と誤認してMIDIを消音・再発音する）→ `SnapshotChange::offlineRender` を追加して世代に触らない経路にし、「バウンス構築を挟んでも据え置きが効く」テストを追加（`offlineRender` を `midiStructure` に戻すと実際にFAILすることも確認）②長いクリップ名がバッジを覆う → 名前の描画幅からバッジ分を差し引き、描画順も名前→バッジに ③スクロールで大半が画面外のリージョンで吹き出しが画面外を指す → viewportとの交差を渡す
- レビュー4巡目で直したもの: **ホイール操作がundoの区切りから漏れていた**（`mouseDown` を伴わないため `editBegun` がリセットされず、ドラッグ後のホイール微調整が前のundo件に合体する）→ `GainSlider` でホイールを無効化。3巡目で「実害小」として割り切ったが、⌘Zの戻り先が狂うのは粒度の約束が崩れるのと同じなので撤回した。回帰テスト（`testGainSliderIgnoresScrollWheel`）も追加し、`setScrollWheelEnabled(true)` に戻すと落ちることを確認済み
- レビュー3巡目で直したもの: **つまみ以外のダブルクリックでundoが2件に割れる**（1クリック目のジャンプと0dBリセットが別件になり、最初の⌘Zが中間値へ戻る）→ 区切りを `onDragStart` から `GainSlider::onNewClickSequence`（`getNumberOfClicks() <= 1` の mouseDown）へ移し、クリック列全体を1操作としてまとめた。`InstrumentDetailView` 側も同じ形に揃えた
- レビュー2巡目で直したもの: ①**ダブルクリックでundoが複数件積まれる**（`onDragStart` は1回のダブルクリックで最大3回発火する）→ 「値が実際に動いた最初の1回」で積む方式に変更。同じ作法をコピーしていた `InstrumentDetailView` のGAINも揃えた（今回の変更で壊したものではないが、片方だけ直すと作法が分かれるため）②ループ反復部分だけが表示中のとき吹き出しが的を外す → アンカー矩形をループ終端まで含める
- **バッジ位置は右下でなく右上**に変更（計画時は右下だったが、右下は `loopHandleRect` の場所だった）。吹き出しの見出しも幅が窮屈なので "REGION GAIN" → "GAIN" に短縮
- 描画の目視確認: 0dB→バッジなし / -6dB→振幅が半分＋`-6.0` バッジ / +6dB→頭打ちで潰れる＋`+6.0` バッジ。ループ2回＋選択の行で、バッジ（本体右上）とループハンドル（ループ終端の右下）が衝突しないこと・幅56px未満のクリップでバッジが出ないことも確認済み

### 方針変更

- **MIDI世代のカウンタを `MainComponent` から `Project` へ移した**。当初計画では `MainComponent` が世代を持ち push のたびに進める形だったが、それだと `project.buildSnapshot()` を直接 push する経路（テストの大半）が世代0のままになり、「再生中のノート削除で音が止まる」安全機構が壊れる。`Project::buildSnapshot (SnapshotChange = midiStructure)` の既定引数で**安全側（世代を進める）をデフォルト**にし、据え置きは呼び出し側が明示する形にした。既存の全呼び出しが従来挙動のままになり、呼び忘れの危険もない
- **`InstrumentDetailView` 切り出しの検証をピクセル差分からコード同一性に変えた**。移動のみ（`jp()` を同義の `juce::String::fromUTF8` に置換しただけ）で `git diff` が削除だけであることを確認できるため、オフスクリーン描画のための一時コードを足すより確実。差分は `git diff Source/ui/InstrumentDetailView.cpp` で確認済み
- **描画の確認に `paintEntireComponent` を使った**。プロジェクトを開くには合成ダブルクリックが必要でユーザーの作業を妨げるため、`TimelineView` をオフスクリーンに丸ごと描いて検証した（手順はVERIFY.mdへ追記）
