# ミキサー基盤（エンジンのステレオ化＋Pan＋固定バス＋Master＋ミキサーオーバーレイ）

## 概要・やりたいこと

- ミキサー・FX領域（全6スライス）の**スライス1**。エンジンをステレオ化し、Pan・send用固定バス3本・Masterチャンネルを導入して、`X` で開くミキサーオーバーレイから操作できるようにする
- このスライスで「音が変わる」ところまで作る: Pan・send量・バス音量・Master音量は実際に再生音へ反映される。FXのDSP（EQ/Comp/Reverb/Delay/Limiter）は後続スライスで、バスは当面素通し
- 全体設計はメモリ `mixer-fx-ui-direction` を参照（dig 2026-07-22で確定）

## 前提・わかっていること

### 仕様（会話で決定済み）

| 項目 | 決定内容 |
|---|---|
| ミキサーの出し方 | オーバーレイ（`X` トグル・Escでも閉じる）。タイムライン領域だけを覆い、下部（ピアノロール等）は隠さない。表示中もSpace再生・シーク等の再生系ショートカットは有効 |
| ストリップ構成 | 全トラック＋Reverb A＋Reverb B＋Delay＋Master の順に横並び。トラック: 名前・メーター・フェーダー・Pan・M/S・sendノブ3個。バス: 名前・メーター・フェーダー（リターン量）・M。Master: 名前・メーター・フェーダー |
| ストリップに載せないもの | FXチェーンの一覧（下部エディタ＝スライス2と重複するため。ON/OFF豆ランプ程度は将来可） |
| Panの置き場所 | ミキサーのみ。トラックヘッダーには追加しない（見送りリスト「ヘッダーへの情報追加はしない」と整合） |
| 固定バス | Reverb A / Reverb B / Delay の3本固定。作成・削除・命名UIなし。sendはpost-fader（gain・panの後）固定 |
| Master | 1本（LogicのStereo Out相当）。今回はフェーダー＋メーターのみ（Limiterはスライス5） |
| ストリップクリック | 今回はトラックストリップクリック＝タイムラインのトラック選択に連動するだけ。バス/Masterクリックで下部エディタを切り替える動作はスライス2 |
| Pan法則 | モノソース→ステレオ: 等パワー・センター補正型（センター0dB・両端で残る側+3dB。既存プロジェクトの音量がセンターで変わらないことを優先）。MIDIトラック（シンセはステレオ出力）: バランス型（センター0dB・振った反対側だけ減衰） |
| メトロノーム | Masterフェーダーの後（post-master）に加算。Masterを絞ってもカウントイン/クリックは聞こえる |
| 保存 | project.json v4。トラックに pan・sends[3]、プロジェクトに buses（gain/mute×3）・master（gain）を追加。旧バージョン読込時は全部デフォルト値（pan=0・send=0・busGain=1・masterGain=1） |
| Undo | フェーダー・Pan・send・M/Sの操作はUndo対象外（既存の音量・M/Sと同じ扱い） |
| UI文言 | 固定文言は英語（MIXER / Reverb A / Reverb B / Delay / Master）。Theme.h のアクセント運用ルールに従う |

### スコープ外（後続スライス）

- スライス2: 下部FXエディタの器（チェーン表示・ON/OFF・ピアノロールとの排他・ミキサーストリップクリックでバス/Master表示）
- スライス3〜5: EQ / Comp / Reverb（種類セレクタ）+Delay+Limiter の実体。**当面バスは素通しなので、sendを上げると原音が二重加算されて音量が上がる（=send配線の動作確認に使える。仕様通りの一時挙動）**
- スライス6: 外部AUスロット（RX。レイテンシ補償の要否はここで検討）

### 技術的な前提（コード調査済み）

- **現行エンジンは実質モノ設計**: オーディオトラックはモノ `trackScratch` を全出力chへ同一加算（`PlaybackEngine.cpp:150-160`）、MIDIトラックはシンセのステレオ出力を `jmin(outCh, 1)` でch0/1から流用（`:427`）。Pan導入は「全chに同一値」をやめてch0=L/ch1=Rの明示ステレオにする変更。3ch以上のデバイスでは余剰chは無音でよい。モノ出力デバイス（1ch）は L+R を等分ミックス
- **パラメータ共有は `TrackParams` のatomicパターンが確立済み**（`PlaybackSnapshot.h:12`）: gain/mute/solo/peakLevel をUIとオーディオスレッドが shared_ptr で共有し、スナップショット再構築なしに即反映。pan・sends[3] はここに追加する。バス・Masterのパラメータも同じ `TrackParams` を流用（不要フィールドは未使用のまま）し、`Project` が3+1本分の shared_ptr を持ち、`PlaybackSnapshot` に載せて渡す
- **メーターの `peakLevel` は exchange(0) で読むと消費される**（`TrackParams` コメント参照）。現在はトラックヘッダーのメーターが消費者。ミキサーにもメーターを出すので、**消費者を1箇所（MainComponentのTimer）に集約して、読んだ値をヘッダーとミキサーの両方へ配る**形に変える。2箇所で exchange(0) するとピークを取り合って両方のメーターが暴れる
- **バス/Masterのミックス順序**: 各トラックの post-fader 信号（gain・pan適用後のステレオ）を ①メイン加算バッファへ ②sendゲインを掛けて3本のバス蓄積バッファ（prepareToPlayで確保・毎ブロックclear）へ。全トラック処理後、各バスを（素通しで）busGain・mute適用してメイン加算 → 全体にmasterGain → クリック加算。バス・Masterのメーターもこの時点でCAS更新
- **`renderMidiTracks()` はシンセ出力を最終出力バッファへ直接加算している**（`PlaybackEngine.cpp:184` / `:427`。プレビュー発音もこの経路）。通常再生だけをバス化するとプレビュー音だけがPan/send/bus/Masterを迂回するため、**renderMidiTracksの出力先を「トラックごとのステレオ信号→メイン/sendバス→Master」に変更する**（プレビュー含む全MIDI出力が対象）
- **クリックは現状全出力chに加算している**（`PlaybackEngine.cpp:224`）。最終出力ルール「ch0/1にのみ書く・1chデバイスはL+R等分ダウンミックス・2ch超の余剰chはclearのまま」を通常音・バス・Master・クリックのすべてに適用する
- **`TrackParams::gain` のデフォルトは 0.8f**（`PlaybackSnapshot.h:14`）。バス・Masterに流用する際、初期値を明示的に 1.0f にしないと新規・v3移行プロジェクトでバス/Masterが約-1.9dBから始まってしまう。メンバー初期化・`createNew`・旧JSON読込の全経路で 1.0f を保証する
- **バウンスとの相互影響**: `docs/plans/2026-07-22-bounce-export.md` は「聞こえているまま」ミックスを自前で再実装する計画（未実装）。**後に実装される側が、先に入った側の pan・send・バス・master を反映する責任を持つ**。本planが先行するなら、バウンス側planの前提に追記しておく
- **`X`・`B` キーは未使用**（Shortcuts.h確認済み）。`X`＝ミキサートグルを Shortcuts.h のテーブルに追加（カテゴリ・keyLabel・matcher）。⌘?一覧は自動反映
- **オーバーレイの前例あり**: `AddTrackOverlay` / `ShortcutListOverlay` が既存パターン。キーイベントを飲み込まない設計（未処理キーはMainComponentへ流す）と、Escで閉じる動作をこれらに合わせる
- **JUCE 8.0.9 pin**。juce::dsp は今回まだ不要（ゲイン系は掛け算のみ）

## 実装計画

### Phase 1: データモデル＋保存形式v4 [AI🤖]

- [x] `TrackParams` に `pan`（-1..+1）・`sends[3]`（0..1）の atomic を追加（lock-free static_assert 維持）
- [x] `Project` に固定バス3本＋Master の `TrackParams` を追加（`busParams[3]`・`masterParams`）。バス名は定数（Reverb A / Reverb B / Delay）
- [x] バス・Masterの gain を全経路（メンバー初期化・`createNew`・v3以前のJSON読込）で明示的に 1.0f にする（`unityParams()` ヘルパーをメンバー初期化子に使用 = 全生成経路で保証）
- [x] `PlaybackSnapshot` に busParams/masterParams を載せ、`buildSnapshot()` で接続
- [x] project.json v4: save/load に pan・sends・buses・master を追加。v3以前の読込はデフォルト値で補完（currentVersion=4）
- [x] 保存→読込のラウンドトリップを確認（`testMixerParamsRoundtrip` で新規デフォルト・v3移行・v4往復を検証）

### Phase 2: エンジンのステレオ化＋pan/send/バス/Master [AI🤖]

- [x] `prepareToPlay` でステレオのバス蓄積バッファ3本＋ステレオミックスバッファを確保
- [x] オーディオトラック: モノscratch→等パワー補正型panでch0/1へ。MIDIトラック: ステレオ出力にバランス型pan（法則は `shared/Pan.h` に共通化）
- [x] `renderMidiTracks()` の出力先を mixScratch/busScratch に変更し、プレビュー発音を含む全MIDI出力が pan→メイン/sendバス→Master を通るようにする
- [x] post-fader sendを3バスへ蓄積 → バスを素通しでbusGain/mute適用してメイン加算 → masterGain → クリック（post-master）
- [x] 最終出力ルールを一元化: 通常音・バス・Master・クリックのすべてが ch0/1 にのみ書く。1chデバイスはL+R等分ダウンミックス、2ch超の余剰chはclearのまま無音
- [x] バス・Masterのメーター（peakLevel CAS）を追加
- [x] ユニットテストで確認: 1chダウンミックス・既存プロジェクト互換（panセンター=従来同値、`testEngineReadsClipOffsets` が従来どおり通ることでも担保）・停止中プレビューへのMaster適用（`testPreviewThroughMaster`）

### Phase 3: ミキサーオーバーレイUI [AI🤖]

- [x] `Shortcuts.h` に `toggleMixer`（`X`）を追加
- [x] `MixerOverlay` コンポーネント新設: ヘッダー＋タイムライン領域のみ覆う。Esc/Xで閉じる。キーを奪わない（Space・シーク等はMainComponentが処理）
- [x] ストリップ実装: トラック（名前・メーター・縦フェーダー・Panノブ・M/S・sendノブ3）／バス（名前・メーター・フェーダー・M）／Master（名前・メーター・フェーダー）。トラックはViewportで横スクロール
- [x] メーター消費の集約: MainComponentのTimerで exchange(0) を1回だけ行い、ヘッダーとミキサーへ配布する形にリファクタ
- [x] トラックストリップクリックでタイムラインのトラック選択と連動（選択ハイライトも同期。mキーのミュートも同期）
- [x] 操作は既存の音量スライダーと同じ経路でモデルへ反映（atomic直書き・onChanged→setDirty）

### バウンス連携 [AI🤖]（バウンスが先に実装済みだったため本planで対応）

- [x] `BounceRenderer` に pan・sends・busGain/busMute・masterGain を追加し、RTと同じ法則・順序でミックス（`shared/Pan.h` 共有）。テールの無音判定はMaster適用後の最終出力で行う
- [x] `beginBounce` で開始時のバス・Master値をプレーン値に固定

### 動作確認

#### AI🤖で確認
- [x] ビルド（daw / daw_tests とも成功）＋dev版起動＋ログで例外なし（session.start正常）
- [ ] ~~CGEvent合成ドラッグでPan/send/フェーダー操作 → project.jsonの値で裏取り~~ → **中断**（ユーザーが別Spaceで作業中で合成クリックが他アプリに着弾するため。ログ > 試したこと参照。下の人間確認に委譲）
- [x] v3プロジェクトの読込→保存でv4に移行され、デフォルト値が入ること（`testMixerParamsRoundtrip` で検証。jqの代わりにJSONパースで裏取り）
- [x] ユニットテスト: 4chバッファで通常音・バス・クリックの ch2 以降がゼロのまま／1chダウンミックス（`testEngineOutputChannelRule`）。pan法則・send二重加算・バスM/gain・Masterゲイン・各メーター（`testEnginePanSendsMaster`）。全26テストpass
- [ ] ミキサー表示中にSpace再生・シークが効くこと（実機未確認 → 人間確認に委譲）

#### 人間👨‍💻で確認
- [ ] `X` でミキサーが開閉すること（開いたときのレイアウト・見た目の確認も）
- [ ] ミキサー表示中もSpace再生・`,`/`.`シークが効くこと
- [ ] Panを振って音が左右に動くこと（オーディオ・MIDI両トラック）
- [ ] sendを上げると音量が上がる（素通しバスの二重加算＝配線OK）こと、バスのMで消えること
- [ ] Masterフェーダーで全体音量が変わり、クリック/カウントインの音量は変わらないこと
- [ ] ミキサー操作後に⌘Sで保存し、再起動しても値が残っていること
- [ ] 既存プロジェクトを開いて従来どおりの音がすること

## ログ

### 試したこと・わかったこと

- ミキサー開閉に `mixer.open` / `mixer.close` のログを追加（動作確認の裏取り用）
- 実機のCGEvent検証は中断: `activate` 直後の onscreen チェックは通ったが、実際の表示SpaceはArc（ブラウザ）のままで、合成クリックがArcに着弾していた（`CGWindowListCopyWindowInfo(.optionOnScreenOnly)` を座標で引いて発覚）。CLAUDE.mdの「着弾していなければ即中止」に従い中断し、UI検証は人間確認に委譲
- 既存テスト `testClipOffsetsV2Migration` の「保存バージョンが3」期待を `Project::currentVersion` 比較に変更（バージョンbumpで壊れない形に）
- Masterメーターは CAS max 蓄積のため、テストでシナリオを跨ぐときは `peakLevel.exchange(0)` でリセットしてから計測する
- レビュー対応: 巨大ブロック時のフォールバックでも出力ルール（ch0/1・1chはL+R等分ダウンミックス）とpan・Masterを本編と揃えた。sendとメーターを通らない点だけが縮退として残る（稀ケースの意図的縮退。コメントに明記）

### 方針変更

- ユーザーの実機確認フィードバックでミキサーUIを調整（2026-07-22）: ①パネルをドラッグ移動可能に（MIXERタイトルバー＝ハンドル。隙間でも掴める。位置はセッション内保持・領域内クランプ）②ロータリーノブ描画を自前化（AppLookAndFeel::drawRotarySlider。溝アーク＋値アーク＋ポインタ線。Panは中央起点の双方向表示）③Panノブを38pxに拡大し「PAN」ラベルを追加（何のノブか分からなかったため）
