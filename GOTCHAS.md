# GOTCHAS — JUCE・リアルタイムオーディオの落とし穴集

## このファイルの読み方（2層構造）

- **必読コア**: 直後の2セクション「オーディオコールバック内の禁止事項」「UIスレッドとオーディオスレッドの分離パターン」。`audio/`・`shared/` やスレッド境界に触れる作業の前に通読する
- **個別の罠**: それ以外のセクション（DSP・JUCE一般・分析パイプライン）は**全読みしない**。`grep "^###" GOTCHAS.md` で見出し一覧を出し、触る対象（ウィジェット・機能・ツール）に該当する見出しのセクションだけを読む
- 追記時は該当する層のセクションへ。必読コアには「常に効く不変条件」だけを足し、特定の部品・場面でしか効かない罠は個別の層に置く

## オーディオコールバック内の禁止事項

### 前提: なぜ厳しいのか

`processBlock()` / `getNextAudioBlock()` / `audioDeviceIOCallback()` はOSの高優先度リアルタイムスレッド（macOSではCoreAudioのI/Oスレッド）から呼ばれる。デッドラインは1ブロック分の時間しかない:

```
バッファ128サンプル @ 48kHz → 約2.7ms
バッファ64サンプル  @ 48kHz → 約1.3ms
```

1回でもデッドラインを落とすと出力バッファが埋まらず、クリック/ドロップアウト（グリッチ）として即座に聞こえる。「平均的には速い」では不十分で、**最悪ケースの実行時間が有界（bounded）** であることが唯一の基準。以下の禁止事項はすべて「最悪ケースが有界でない」ことが理由。

### 禁止リスト

| 禁止 | 理由 |
|---|---|
| `malloc` / `new` / `free` / `delete` | アロケータは内部でロックを取る可能性があり、ヒープ探索の時間も非有界。OSにページを要求すればシステムコールになる |
| ミューテックスのロック（`std::mutex`, `juce::CriticalSection`） | **優先度逆転**: ロックを持った低優先度スレッド（UI等）がスケジューラに追い出されると、高優先度のオーディオスレッドがそれを待ってブロックする。待ち時間は非有界 |
| `juce::SpinLock` も同様に不可 | ビジーウェイトでも、ロック保持側が実行されない限り待ち続ける。優先度逆転の構造は同じで、CPUを焼きながら待つ分さらに悪い |
| 例外を投げる（`throw`） | 例外オブジェクトの生成にヒープ確保が入りうる上、スタック巻き戻しのコストが非有界。オーディオコードは実質 `noexcept` で書く |
| ファイル/ネットワーク/コンソールI/O | ブロッキングシステムコール。ディスクの応答時間は非有界 |
| `sleep` / 条件変数待ち / スレッド生成 | ブロック・スケジューラ依存の待ちそのもの |
| `juce::MessageManagerLock` の取得 | メッセージスレッドとの相互待ちで**デッドロック**の定番。絶対に使わない |
| Objective-C / GCD の呼び出し | autorelease pool や内部ロックでアロケーション・ブロックが起きうる |

### JUCE特有の「隠れた違反」

見た目は無害でも内部でヒープ確保やロックをするもの:

- **`juce::String` の生成・連結** — ヒープ確保する。ログ用の文字列整形もNG
- **`DBG()` / `juce::Logger`** — String生成＋I/O。デバッグ中でもコールバック内では使わない（値を`std::atomic`に書いてUI側で表示する）
- **`AudioBuffer::setSize()`** — 再確保。サイズ変更は`prepareToPlay()`でのみ行う。`processBlock`内でバッファサイズが必要なら`buffer.getNumSamples()`を信じる
- **`std::vector::push_back` / `resize`** — 容量超過で再確保。コールバックから触るコンテナは事前に容量固定
- **`std::function` へのラムダ代入** — キャプチャが大きいとヒープ確保
- **`juce::ChangeBroadcaster::sendChangeMessage()` / `AsyncUpdater::triggerAsyncUpdate()`** — 「どのスレッドからでも呼べる」とドキュメントにあるが、内部でOSのメッセージキューに触るため厳密にはリアルタイム安全ではない。通知はpush型でなく、UI側のTimerによるpull型（後述）にする
- **関数内static（マジックスタティック）の初回初期化と重い係数計算** — 係数テーブルを `static const auto table = []{...}()` で持つと、初回のコールバックで初期化本体（sin/cos計算）＋スレッドセーフガードの取得が走る。テーブルはコンストラクタで触って初期化を済ませ、参照をメンバに保持する（TruePeakDetectorの形）。K-weighting等のSR依存係数（tan/pow）も再生開始エッジでなく `prepareToPlay` で計算する。**RT安全テストをウォームアップしてから測ると、まさにこの経路が計測から漏れる** — 開始エッジ込みで測ること

### AudioSourcePlayer / ResamplingAudioSource は使わない（コールバック内ロック・確保）

BufferingAudioSource（前述）だけでなく、`AudioSourcePlayer` はコールバックで CriticalSection を取り
（`juce_AudioSourcePlayer.cpp:81`）、`ResamplingAudioSource` も CriticalSection＋SpinLock を取った上で、
比率が上がると `buffer.setSize()` の再確保に到達する（`juce_ResamplingAudioSource.cpp:92/:109`。
96kHz音源→48kHzデバイス等）。代替は**直接 `AudioIODeviceCallback` を実装**し、`LagrangeInterpolator`＋
`audioDeviceAboutToStart` で事前確保したバッファでリサンプルする（`audioDeviceAboutToStart` は
コールバック開始前に呼ばれる＝prepareToPlay相当で確保してよい）。ただし4点Lagrangeは帯域制限
しないので、**ダウンサンプリング時は補間前・アップ時は補間後に2次ローパス**を入れる
（JUCEのcreateLowPassと同じバイリニアButterworth。実装は `apps/salva/Source/shared/ResampleStage.h`。
エイリアス抑圧はGoertzelで回帰テストできる）。

### 判断に迷ったら

「この関数の最悪実行時間は有界か？」を問う。答えられない関数（自分が書いていないライブラリ関数を含む）はコールバックから呼ばない。

## UIスレッドとオーディオスレッドの分離パターン

### 原則

- JUCEのUI（`Component`, `repaint`, `Timer`）はすべて**メッセージスレッド**で動く。オーディオコールバックは別の**リアルタイムスレッド**で動く
- 通信は必ずロックフリー構造（atomic / FIFO）を経由する。**オーディオスレッドは決して待たない**。UI側が待つ・取りこぼすのは許容する
- 方向で使い分ける:
  - 単一の値（レベルメーター、再生位置、パラメータ）→ `std::atomic`
  - 連続データの流れ（波形サンプル、録音データ）→ `juce::AbstractFifo` + 事前確保バッファ
  - UI→オーディオへの通知はatomicフラグ、オーディオ→UIへの通知はUI側のTimerポーリング

### パターン1: `std::atomic` で単一値を渡す

```cpp
// shared/ に置く。両スレッドから見える
std::atomic<float> currentPeakLevel { 0.0f };
std::atomic<juce::int64> playheadSamplePos { 0 };
std::atomic<bool> shouldRecord { false };

// オーディオスレッド: 書くだけ
currentPeakLevel.store(buffer.getMagnitude(0, buffer.getNumSamples()));

// UIスレッド: Timer（30〜60Hz）で読んで描画
void timerCallback() override
{
    meter.setLevel(currentPeakLevel.load());
    repaint();
}
```

注意:
- `static_assert(std::atomic<T>::is_always_lock_free)` を書いておく。lock-freeでない型（大きなstruct）を`std::atomic`に入れるとミューテックスにフォールバックして禁止事項違反になる
- 複数の値をまとめて整合性を持って渡したい場合はatomicを並べてもダメ（バラバラに読まれる）。その場合はFIFOで構造体ごと送る

### 「要求」と「結果」の2つのatomicは、更新順序を中間状態が安全な向きに倒す

`seekRequest`（UI→オーディオの要求）と `playheadSamplePos`（結果）のように**対になる2つのatomic**は一体で更新できない。必ずどちらか一方だけが新しい瞬間があり、UI がそこを読む。**どちらに転んでも正しい方の順序を選ぶ**こと。

```cpp
// NG: 先に要求を消すと「要求なし＋旧位置」の一瞬が生まれ、UIが旧位置を読む
const auto seek = seekRequest.exchange (kNoSeek);
if (seek != kNoSeek) playheadSamplePos.store (seek);

// OK: 先に結果を公開してから、自分が読んだ要求だけをCASで消す
const auto seek = seekRequest.load();
if (seek == kNoSeek) return false;
playheadSamplePos.store (seek);          // 中間状態は「要求あり＋新位置」＝どちらを読んでも新位置
auto expected = seek;
seekRequest.compare_exchange_strong (expected, kNoSeek);
```

`exchange` でなく CAS にするのは、適用中に UI が**新しい**要求を積んだ場合にそれを消さないため（CASが失敗し、次のコールバックで適用される）。UI 側は「要求があればそれ、なければ結果」を返すヘルパー（`TransportState::uiPositionSample()`）を経由して読み、生の結果atomicを直接見ない。

**症状**: クリック直後に ⌘T で切ると、まれに1つ前の位置で切れる。窓が数命令ぶんしかないので再現しづらく、テストも「両方の更新後」しか観測できないため取りこぼす。設計時に順序で潰すしかない。

### パターン2: `juce::AbstractFifo` で波形データを渡す

`AbstractFifo`はロックフリーFIFOの**インデックス管理だけ**を行うクラス（データは持たない）。**シングルライター・シングルリーダー専用**——「オーディオスレッドが書き、UIスレッドが読む」の1対1でのみ使う。リングバッファなので読み書きが2ブロックに分割されることがあり、`blockSize1` / `blockSize2` の両方を必ず処理する。

```cpp
// shared/WaveformFifo.h — 事前確保したAudioBufferと組で使う
class WaveformFifo
{
public:
    // オーディオスレッドから呼ぶ。ロックもアロケーションもしない
    void push(const float* data, int numSamples)
    {
        const auto scope = fifo.write(numSamples); // ScopedWrite: スコープ終了時にfinishedWrite相当が走る
        if (scope.blockSize1 > 0)
            buffer.copyFrom(0, scope.startIndex1, data, scope.blockSize1);
        if (scope.blockSize2 > 0)
            buffer.copyFrom(0, scope.startIndex2, data + scope.blockSize1, scope.blockSize2);
        // FIFOが満杯なら blockSize1+blockSize2 < numSamples になり、余りは黙って捨てる。
        // オーディオスレッドを待たせないためのトレードオフで、これで正しい
    }

    // UIスレッド（Timerコールバック）から呼ぶ
    int pull(float* dest, int maxSamples)
    {
        const auto scope = fifo.read(juce::jmin(maxSamples, fifo.getNumReady()));
        if (scope.blockSize1 > 0)
            std::copy_n(buffer.getReadPointer(0, scope.startIndex1), scope.blockSize1, dest);
        if (scope.blockSize2 > 0)
            std::copy_n(buffer.getReadPointer(0, scope.startIndex2), scope.blockSize2, dest + scope.blockSize1);
        return scope.blockSize1 + scope.blockSize2;
    }

private:
    static constexpr int capacity = 1 << 15; // 電源投入時に確保し、以後サイズ変更しない
    juce::AbstractFifo fifo { capacity };
    juce::AudioBuffer<float> buffer { 1, capacity };
};
```

- UI側はTimer（40msごと等）で`pull`し、波形の描画用データ（ピーク値の配列など）に集約して`repaint()`する
- 旧API（`prepareToWrite`/`finishedWrite`を手で呼ぶ）も同じもの。Scoped APIの方が終了処理を忘れないので推奨
- 容量は2のべき乗にし、「UIのポーリング間隔 × サンプルレート」より十分大きく取る（40ms @ 48kHz ≈ 1920サンプルに対して32768なら余裕）

### 自作SPSCリングは「上書き型」にしない（1周遅れの読み手とのデータ競合 = UB）

「書き手はスロットを上書きして進み、読み手は追い越されたら捨てる」方式は、読み手が非atomicのstructをコピーしている最中に書き手が同一スロットへ到達し得て、C++上は**未定義動作**になる（コピー後にwriteCountを再確認しても、コピーと上書きの同時進行そのものがUB）。満杯時は**新規エントリを破棄してatomicフラグで通知**する有界キュー（writeCount/readCount両方をatomicで持つ）にすれば、同一スロットへの並行アクセスが構造上起きない（実装: `shared/MasterMeterStats.h`）。破棄の付帯情報（世代等）が単調増加なら「最後の値以前は全部無効」と解釈でき、連続破棄で中間の記録が上書きされても安全側に倒せる。そもそも `juce::AbstractFifo`（パターン2）で書けるならそちらを使う。

### パターン3: メモリ確保のタイミングを分離する

- **`prepareToPlay()` で全確保、`releaseResources()` で解放**。この2つはコールバックが走っていない状態で呼ばれることが保証されている
- コールバック内で必要になるバッファ・FIFO・一時領域は、想定される最大ブロックサイズ（`prepareToPlay`の引数`samplesPerBlockExpected`）を基準に確保しておく
- 「再生開始ボタンでバッファを作る」のような遅延確保はしない。UIイベント→確保→atomicポインタ差し替え、という形にする場合も、**解放は必ずメッセージスレッド側**で行う（オーディオスレッドが触っている可能性のあるメモリを即deleteしない）

### スナップショットのpushはMIDIの消音＋再発音を伴う

`PlaybackEngine` はスナップショットの差し替えを検出すると全ノートオフ＋跨ぎノート再発音を行う（再生中の編集でノートオフが失われて鳴りっぱなしになるのを防ぐ安全機構）。そのため「値を1つ変えたので push」を毎イベント行うと、鳴っているMIDIが連打される。オーディオ側の値だけが変わったときは `Project::SnapshotChange::audioValuesOnly` でMIDI構成の世代を据え置く。

- **こうしたフラグは既定を安全側にして、呼び出し側が例外を明示する形にする**。世代カウンタを `Project` に持たせて `buildSnapshot()` の既定引数で進めているのは、カウンタを呼び出し側（`MainComponent`）に持たせると `buildSnapshot()` を直接使う経路（テスト等）が世代を進めず、安全機構が黙って壊れるため
- **エンジンへ渡さない構築（バウンス用）は世代に触らない**（`offlineRender`）。ここで進めると、次の据え置きpushが「世代が変わった」と誤認される
- **テストで同じ `SnapshotExchange` に3回以上pushするなら、間に `deleteRetired()` を挟む**。`acquire()` は retired が空のときだけ pending を取り込むため（`PlaybackSnapshot.h`）、掃除しないと3回目以降が反映されず「実装が効いていない」ように見える

### パターン4: 録音のディスク書き込みは第3のスレッドへ

Tier 0の録音で必要になる。オーディオスレッドからファイルに直接書くのは禁止事項（I/O）なので:

```
オーディオスレッド → (FIFO) → バックグラウンドスレッド → ディスク
```

JUCEには**`juce::AudioFormatWriter::ThreadedWriter`**がまさにこの用途で用意されている。`juce::TimeSliceThread`と組み合わせ、コールバックからは`threadedWriter->write(...)`を呼ぶだけでよい（内部がFIFO渡しになっている）。自前でFIFO+書き込みスレッドを組む前にこれを使う。

以下は公式`examples/Audio/AudioRecordingDemo.h`（`AudioRecorder`クラス）を実際に読んで確認したライフサイクル管理。実装時はこの形をそのまま踏襲する。

#### メンバ構成（4点セット）

```cpp
juce::TimeSliceThread backgroundThread { "Audio Recorder Thread" };
std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter; // 所有権（メッセージスレッド専用）
juce::CriticalSection writerLock;
std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr }; // コールバックが見るのはこれだけ
```

- **コンストラクタで`backgroundThread.startThread()`を忘れない**。忘れてもコンパイルは通り、`write()`は成功し続けるがディスクに何も書かれず、内部FIFOが満杯になって静かにデータが落ちる
- 所有（`unique_ptr`）と使用（atomicな生ポインタ）を分離するのがこのパターンの本体

#### 録音開始（メッセージスレッド）

```cpp
void startRecording (const juce::File& file)
{
    stop(); // 必ず先に前回分を止める
    file.deleteFile();
    if (std::unique_ptr<juce::OutputStream> fileStream { file.createOutputStream() })
    {
        juce::WavAudioFormat wavFormat;
        using Opts = juce::AudioFormatWriterOptions; // JUCE 8.0.9で導入（同時に位置引数版createWriterForはdeprecated化）。
                                                     // 8.0.8以前を使うなら位置引数版に読み替える。CMakeでJUCEのバージョンを
                                                     // 確定したら、この例が実バージョンのAPIと一致するか要確認
        if (auto writer = wavFormat.createWriterFor (fileStream,
                Opts{}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (16)))
        {
            threadedWriter.reset (new juce::AudioFormatWriter::ThreadedWriter (
                writer.release(), backgroundThread, 32768)); // 32768 = 内部FIFOのサンプル数
            const juce::ScopedLock sl (writerLock);
            activeWriter = threadedWriter.get(); // ポインタの差し替えだけをロック内で
        }
    }
}
```

#### 録音停止（メッセージスレッド）— 順序が核心

```cpp
void stop()
{
    // 1. まずactiveWriterをnullにして、コールバックにwriterを使わせなくする
    {
        const juce::ScopedLock sl (writerLock);
        activeWriter = nullptr;
    }
    // 2. その後、ロックの外でwriterを破棄する。
    //    reset()は残データのディスクflushを待つため時間がかかる。
    //    これをロック内でやるとその間オーディオコールバックがブロックされる
    threadedWriter.reset();
}
```

この「破棄はロックの外」は公式デモのコメントにも明記されている意図的な順序。逆にすると録音停止のたびにグリッチが出る。

#### オーディオコールバック側

```cpp
const juce::ScopedLock sl (writerLock);
if (activeWriter.load() != nullptr)
    activeWriter.load()->write (inputChannelData, numSamples);
```

- **atomicのnullチェックだけではダメでロックが必須**な理由: `load()`がnullでなかった直後にメッセージスレッドが`threadedWriter.reset()`すると、破棄済みwriterへの`write()`（use-after-free）になる。ロックが「コールバックが`write()`を終えるまで破棄側を待たせる」役割を担う
- `write()`が`false`を返したら内部FIFO満杯（ディスクが追いついていない）。公式デモは無視しているが、atomicのドロップカウンタに記録してUIに出す価値はある

#### 「ロック禁止」の原則との折り合い

これは禁止事項セクションと矛盾して見えるが、公式が許容しているのは**ロックの相手側（メッセージスレッド）がロック内で行うのがポインタ代入だけ**だから。最悪の待ち時間が数命令分に有界化されており、「非有界な待ちの禁止」という本来の基準には反していない。この形を維持する条件:

- `writerLock`の中では**ポインタの読み書き以外何もしない**。flush・確保・破棄・ログは絶対にロック内に入れない
- 守れない設計変更をするなら、完全ロックフリー版（遅延破棄キュー等）に移行する。Tier 0では公式パターン踏襲で十分

### 通知はpush型でなくpull型（Timerポーリング）

- オーディオスレッドから「UIに知らせたい」ことがあっても、`sendChangeMessage()`等でpushしない（上記の通り厳密にはRT安全でない）
- UI側が`juce::Timer`（30〜60Hz）でatomic/FIFOを覗きに行くpull型に統一する。レイテンシは最大1ポーリング周期分だが、画面表示用途では問題にならない
- `juce::AudioThumbnail`を波形表示に使う場合も同じ扱い: 公式デモではコールバックから`addBlock()`を呼んでいるが、内部実装への依存になるので、自前FIFOで渡してUI側で`addBlock`する方が原則に忠実

### 複数ストリームにまたがる遷移は epoch（begin/end）で囲み、重なったブロックを無効化する

「シークした」を別のatomic通知で知らせる方式は、**通知と適用（各ストリームのseek採用）が別atomic
である限り競合窓が残る**（通知を見ずに一部ストリームだけ適用されたブロックができる。masterだけ
監視しても非masterだけ変わる順序を取りこぼす — Salvaでレビュー3往復の実例）。正解はエンジン側で
遷移全体を epoch で囲むこと:

- begin で `active++`（release）→ `version++`（release）、requestSeekの列、end で `active--`
- オーディオ側はブロック前に version→active の順で acquire 読み（versionのacquireが
  active++ の可視性を保証する）、ブロック後に version を読み直す
- **開始時 active>0、または処理中に version が進んだブロックは丸ごと無音化＋DSP状態リセット**
  （最悪1ブロックの無音。シーク自体が音の断絶なので実用上は聞こえない）
- 履歴を持つDSP（リサンプラー・フィルタ）は不連続のたびに reset しないと旧位置のサンプルが混ざる

実装は `apps/salva/Source/shared/DiscontinuityGuard.h`（真理値表テスト付き）。

### 再生開始プライミング（シーク直後の頭欠け・複数ストリーム同時開始）の設計要点

「枯渇＝無音を出して位置を進める（時間を保つ）」の契約は再生**中**の途切れには正しいが、シーク直後の未配信状態に適用すると選択頭の数百サンプルを毎回スキップする（ループ頭のキックのアタックが削れる）。Salvaで実装した解（`PlayerEngine::audioDeviceIOCallbackWithContext` のゲート＋`ReadAheadStream::hasPendingData`）の要点:

- **ゲートは全ストリーム一斉**: ストリーム単体で位置を止めると、ステム間で採用タイミングが割れて恒久ずれ（フラム）を作る。「全員にリサンプラー初回要求量が揃うまで誰も消費しない」
- **必要量は `ceil(numOut×ratio)+8`**（ResampleStageの初回要求と同式）。1ブロックでは高比率×大バッファで足りない。上限はリング総容量。非ループのファイル終端・リング満杯（短いループはブロックがループ長で切られる）では早期に開始
- **遷移がコールバックと重なったブロックの後始末は「世代だけ進める」**（`reissueSeek`。位置を書き直すと並行する新シークを上書きする競合窓ができる）。全ストリームが同じ要求位置へ収束し、ライターも書き直す
- **枯渇カウンタは2段公開**（ブロック中は一時値→確定でcommit・破棄でdiscard）。atomicを直接加算してから巻き戻すと、UIが一時値を観測でき単調性も崩れる

## DSP・信号経路の落とし穴

### 非線形DSPを既存チェーンに挿すときは「線形だから偶然成立していた順序」を疑う

EQ（線形）はゲイン・panと可換なので、pan焼き込み後に掛けても出力が同じ＝経路の順序ミスが露見しない。コンプ（検波レベル依存＝非線形）を同じ位置に足した途端、pan位置で圧縮量が変わる実バグになる（実例: track-comp実装でモノ経路のバウンスとステレオ経路のRT/バウンス両方で発生）。非線形FX（コンプ・サチュレーション・リミッター）を挿すときは、仕様の処理順（EQ→FX→gain/pan）と実装の掛け算の位置を経路ごと（モノ/ステレオ/MIDI × RT/バウンス）に突き合わせる。

注意: **RT/バウンス一致テストは両側が同じ誤った順序だと通る**。一致テストが検出するのは経路差だけで、仕様適合（pan前に掛かっているか）は「panを振っても圧縮量が変わらない」ことを直接主張するテストで別途固定する（`testEngineCompPrePanDetection` の形: pan後検波なら効かない条件を作り、効くことを主張する）。

2026-08-16 のバッチ1リファクタで、FXチェーンの「active判定・適用順（EQ→タップ→Comp。バッチ3で →Sat→Lo-fi）・テール対象」は `Source/audio/TrackFxChain.h` に集約された。新FXの追加は evaluateActivity / process / producesTail の3点を触れば全6経路に効く＝「経路ごとに突き合わせる」作業は掛け算の位置（gain/panの後段移動・monoFx/prePanFx）の確認だけに減った。ただし上記の仕様テスト（pan前検波の直接主張）は集約後も残すこと — 集約自体が仕様の順序を保証するわけではない。変更時のビット一致確認は `scripts/check-render-hashes.sh`（VERIFY.md 参照）。

補足（バッチ3）: 集約後も残っていたコピー箇所が **バウンスの `TrackRender` へのFX設定詰め**で、テスト側の手書きコピーが Sat/Lo-fi を落としてハッシュが「不変＝未検出」のまま通っていた実例あり（FXなし系と値が同じなので見た目で気づけない）。FX設定の固定は `TrackRender::loadFxFrom()` に一元化したので、**新FX追加時はこの関数に足し、個別コピーを書かない**。状態機械（serial・リセット3分岐・ON/OFFクロスフェード・settled・snapTo骨格）も `TrackFxBase`（CRTP）に集約済み — 新FXは4フック（fxResetSmootherRates / fxSnapToTargets / fxResetHistory / fxSampleRateChanged）を実装する。

### 素通し高速パスを持つFXの「中立境界」は wet==dry（ビット一致）に収束させてから切り替える

needsActivePath が false になると呼び出し側は完全素通しに切り替えるため、active経路の最終出力が「dryにほぼ等しい」だけでは切替瞬間に段差が出る。バッチ3で同型の欠陥が2件出た実例: ①SatのDrive→0（DCブロッカを通った出力→raw入力の即時切替で低域ポップ）②Lo-fiのTone=0（開放20kHzのLPFは恒等ではない）。対策の型: **中立目標のときwet経路をdryへフェードする SmoothedValue**（Satの neutralFade / Lo-fiの toneMix）を持ち、フェード=0で出力が数式上 dry とビット一致（`dry + mix·0·(…) = dry`）してから settled を立てる（settled 条件にそのフェードの isSmoothing を含める）。回帰テストの型は「ノブを0へ落とし、整定ブロック**末尾**のサンプルが入力とビット一致＋高速パス境界（最終処理サンプル→raw初サンプル）込みで跳躍なし」（整定はブロック途中で完了するので、ブロック全体の一致を要求すると誤検知する）。

### DSPテストの合格閾値は実装前に数値評価で固定し、音決め定数に従属させない

閾値を後から決める（or きつく置く）と「テストを通すためにキャラクター定数を選ぶ」逆転が起きる（実例: Satの-6dBFS補償誤差にきつい絶対値を要求すると、βの音決めが試験に従属する。1次ADAAに12dB改善を要求すると実力6〜9dBに対して設計変更を強いられる）。手順の型: ①伝達式が決まった時点で**Pythonの数値評価**により実力値を測る（β掃引の倍音バランス・補償誤差・エイリアス改善量）②閾値は実力に余裕を持たせてplanに事前固定し「実測後に動かさない」と明記 ③その閾値で保証**できない**量（実素材での音量一定など）は機械判定から外し、人間の耳確認項目へ明示的に寄せる。

### 波形整形（ADAA・伝達関数）の内部計算は float だと桁落ちで巨大スパイクが出る

1次ADAAは不定積分の差分商 `(F(x)−F(x0))/(x−x0)` を使うが、`F` に含まれる `logcosh(z) = |z| + log1p(exp(−2|z|)) − log2` は z が小さいとき「≈0.693 同士の引き算」になり、float では絶対誤差 ~1e-7 が残る。小さいドライブ g では正規化係数 `1/(g·sech²b)` が最大 1/ε 倍（LaLaでは1e4）に誤差を増幅し、**Drive が 0 付近を平滑通過する瞬間に ±100 超のスパイク**が出た（Drive急変テストで検出。定常状態では出ないので聴感スイープだけでは見つけにくい）。伝達関数 f と不定積分 F の内部は double で計算し、float へ落とすのは差分商を取った後にする。

もう1つ: **線形カーブ（g→0）への1次ADAAは恒等でなく中点平均 `(x+x0)/2`＝半サンプルLPFになる**。高速パス（完全素通し）と切り替わる境界に高域差が残るため、線形域は明示的に `wet = x` へ分岐する。

### レイテンシを持つDSP（lookahead等）をエンジンに挿すときの波及は4系統ある

Master Limiter（2ms先読み）導入時の実例。出力がLサンプル遅れるDSPを信号経路に入れると、影響は「音が遅れる」だけでは済まない:

1. **サンプル位置を検証する既存テストが全部追従対象**（実例: エンジン系テスト42件が一斉fail。hash-engine系の変化だけを見積もっていた）。テスト側はエンジン出力を+Lで読む/余分に1ブロック回して先頭Lを詰める形へ書き換える（TestsMainの `engineLimiterLatency` 参照。blockSize < L のときは予備をブロック倍数へ切り上げないとコピー範囲が溢れる）
2. オフライン書き出しは**先頭Lサンプル破棄＋末尾Lサンプルの無音flush**で出力長・頭出しを不変に保つ（bounce系ハッシュが素材天井以下でビット一致することが検証になる）
3. 後段の位置依存処理（曲末フェード等）は評価位置をLだけ引き、セグメント境界も+L側でも切る（ランプが折れ点をまたがないように）
4. **リセット契約を明文化する**: ループ折り返しで内部状態を切らない（切ると毎周L分の無音が入る）。代わりに「鳴らさない区間（フェード終端以降等）でも入力を無音に差し替えてDSPを回し続け、状態を常に進めておく」— 止めると凍結した古いLサンプルが折り返し後の先頭へ漏れる（`testEngineFadeCycleLimiterState` で回帰固定）

### ポリフェーズ補間カーネルの中心は整数タップに置く（(N-1)/2 の慣例は罠）

4倍オーバーサンプリングのwindowed-sincで、FIR設計の慣例どおり中心を `(N-1)/2`（半整数）に置くと、補間点が {0, 1/4, 2/4, 3/4} でなく {1/8, 3/8, 5/8, 7/8} の格子になる。fs/4の正弦のサンプル間ピーク（真値1.0）を常に0.125サンプル外して**約-0.2dB過小評価**し、タップを増やしても0.98で頭打ちになる（TruePeakDetectorで実測）。中心は `N/2`（整数）にし、窓関数も中心対称形で書く。タップ数は正弦だけでなく**広帯域過渡（クリック列）で独立実装（ffmpeg等）と照合**して決める（12タップ/相はクリックのISPを0.9dB取りこぼした。単発インパルス系は帯域制限解釈がフィルタ長依存で厳密な正解が無いことも知っておく — scripts/check-loudness.sh の注記参照）。

### メーター系の集計はdBで平均せず、スレッド間には「十分統計量」を渡す

LUFSのような窓集計値をdB（対数）領域で平均すると数学的に誤る。窓の合成は**線形エネルギー（二乗和＋サンプル数）を合算してから1回だけdB化**する。オーディオ→UIへは完成値（LUFS・相関係数）でなく十分統計量（Σx²・ΣLR・区間max等）を渡すと、UI側で任意の窓（short-term 3s / integrated全区間 / 相関300ms）を後から正しく組み立てられる（実装: `shared/MasterMeterStats.h` / `shared/LoudnessMeter.h`）。窓の判定と計算範囲は必ず**同じ集合**で行う（「揃った判定は31件全体・計算は末尾30件」のようなずれは、停止時の部分ブロックが混ざる境界ケースで3秒未満窓の値を3秒窓として出す）。

### SmoothedValueの `skip(segLen)` 流用はブロックサイズ依存になる

TrackEqの「セグメント頭で `skip(segLen)` → 戻り値で係数再計算」は**係数計算が重いフィルタ専用の最適化**。`skip()` は区間末尾の値を返すだけなので、その値をゲインとして区間全体へ掛けるDSPに流用するとブロックサイズで結果が変わる（512サンプルなら最初から目標値・64なら段階的。速いAttack級の時定数では聴こえ方自体が変わる）。サンプル単位のループを既に持つDSP（コンプのゲイン計算等）は素直に `getNextValue()` をループ内で呼ぶ — ブロック分割に依存せずビット一致になり、「ブロックサイズを変えても出力一致」の回帰テストが書ける。

### ディレイライン部品は read/write の呼び出し順を契約としてヘッダに書く

`read(D)` の意味は呼び出し順で変わる: サンプルループ内で **write の前に read すれば遅延ちょうどD**、write の後だと D-1（実例: BusDelay は read 先行で正しく、BusReverb の pre-delay が write 先行で1サンプル短かった）。部品側のヘッダに「readはwriteの前に呼ぶ」等の契約を明記し、利用側は**遅延量のサンプル単位一致テスト**（0ms設定と比較した出だし位置の差 == round(ms×SR)）で固定する。「±数サンプルの許容」でテストを書くとこのバグは永久に通過する。

### フィードバック系FXのモード切替は出力フェードだけでは閉じない（書き込み側の不連続がDサンプル後に出る）

ディレイ網のトポロジ（Ping-pongのL/R交差など）を切り替えるとき、出力ゲインを一瞬落とすだけでは不十分。**書き込み式が変わった点がリング内容の不連続として残り、ディレイ時間Dの後＝出力フェードが戻った後に段差（クリック）として現れる**。無音まで落とした時点でリング・フィルタ状態を組み直す（エコーは切替時に積み直し）か、書き込み側もクロスフェードする。

検出は「全パラメータをスイープしながら**隣接サンプル段差の最大値**に上限を置く」テストが安くて効く（NaN・発散チェックだけではこの種のクリックは素通しする。実測: 修正前0.46 → 修正後0.22=自然なエコー立ち上がり相当。閾値は入力信号の自然な段差の数倍で切る）。

### テールの無音打ち切り窓は「FXが無音から最初の音を出すまでの遅延」で決める

「1ブロック無音なら終了」は、出力が遅れて始まるFXで**音が出る前に打ち切る**: ディレイはエコー間の無音ギャップ（窓=1周期）、リバーブは Pre-delay＋初期反射（juce::Reverbの最短コム≈25ms）まで完全無音。窓は**有効なFXごとの出力遅延の最大値＋1ブロック**を取る。また終端フェードで閉じた後の**後処理経路（Limiterのflush等）も同じ契約に含める** — flushがバスFXを再処理するとリングに残った信号が未フェードで復活してクリックになる（閉じたらゼロ保証）。この種の穴は経路を1本足すたびに再発する（Delay窓→Reverb窓→flushと3回指摘された）ので、「無音から音を出す経路・書き込む経路」を列挙してから窓と契約を決める。

## 移調・タイムストレッチ（signalsmith-stretch / RenderedDomain）の落とし穴

設計の真実の源は `docs/plans/2026-08-18-1028-audio-transpose-stretch.md`。触る前にそちらの
「データモデルの方針」を読むこと。ここは grep で引く要点のみ。

### signalsmith-stretch は固定シードのコンストラクタを使う（既定は起動ごとに音が変わる）

`SignalsmithStretch()` は `std::random_device` でシードするため、「値のみ保存して読込時に再生成」の設計（毎回同じ音になる前提）と回帰テストの両方が壊れる。必ず `SignalsmithStretch(long seed)` を固定値で使う（`ClipStretcher.cpp` の `fixedSeed`）。

### signalsmith-stretch のレイテンシ補償は「前後パディング＋exact()」で吸う

`exact()` は先頭 `outputSeekLength()` 分の入力をプリロールに使い、入力がそれより短いと**無音を返して false**（20〜50ms のチョップが該当）。範囲の前後に原音を助走として足し（バッファ端は無音埋め）、出力の対応区間だけを切り出す（`ClipStretcher::render`）。頭が無音になったらこの補償を疑う（回帰テストあり）。

### 原音バッファは共有参照 — 絶対に書き換えない・識別はアドレス＋強参照で守る

`Clip::audio` は分割・複製・ペーストで共有される。加工は必ず新バッファへ（in-place 禁止）。レンダー結果の指紋は原音の**アドレス**を含むため、`RenderedDomain` と実行中の request が原音の `shared_ptr` を持つことでアドレス再利用による誤ヒットを防いでいる — この強参照を「メモリ節約」で weak にしないこと。

### 要求値（transposeSemitones/stretchRatio）と実効値（activeDomain）を混ぜない

描画・ヒットテスト・再生・終端計算は **activeDomain 由来の実効長だけ**を見る（`renderedLengthSamples()` 系）。要求値から見かけ長を計算すると、レンダー完了前に再生長と実バッファ長が食い違う（1.5倍なら 0.5N の無音、0.5倍なら反復の重なり）。原音側の不変条件（`clampFades`・分割・保存）は `sourceTotalLengthSamples()`。activeDomain の有効契約は「原音・SR一致＋ドメインがクリップ範囲を包含」で、**要求値との一致は求めない**（求めると「完了までは古い音」が壊れる）。

### レンダードメインは分割で継承・値変更でリセット。指紋の範囲はクリップでなくドメイン

分割は再レンダーしない（子は親の `RenderedDomain` を共有し view だけが変わる＝境界の音が厳密に一致し、再起動後も保たれる）。値を変えたらドメインをクリップ自身の範囲へリセットする（チョップ1個のために8小節を再レンダーしない）。undo state には activeDomain を積まない（`UndoStack::stripClipDomains`。100件×約69MBで数GBを抱えるため）。

### 原音座標と実効座標をまたぐ計算は必ず mapBoundary を通す（× ratio / ÷ ratio 直書き禁止）

view の両端・分割境界・再生 offset は**絶対境界の差**（`RenderedDomain::mapBoundary` / `sourceForBoundary`）で求める。相対値に `round(x × ratio)` を独立に使うと、隣接 view の境界が一致せず**バッファ終端を越えて読む**（domainLength=2 / ratio=0.5 の退化ケース）。ループをまたぐフェードは「完了した本体数＋本体内の端数」の chain 変換（`Clip::renderedFadeLength` / `sourceFadeFromRendered`）— 単純な `× ratio` は反復ごとの丸めが積み上がって連なり全長と合わない。範囲外読み・ループフェードのずれ・分割境界のずれ・再生 offset の欠落（`audioBaseOffset` 落とし）は、すべてこの規律を外したときに出る。

## JUCE一般の落とし穴

### 非ASCIIの文字列リテラルは必ず `juce::String::fromUTF8(u8"...")` を通す

`juce::String(const char*)` はUTF-8を解釈しない。日本語に限らず em-dash（—）や ● などの記号も対象で、生リテラルのまま `String` と連結すると文字化けする（`"daw — "` が「daw â」になった実例あり）。UI文言・タイトル・ダイアログの全てで `fromUTF8` を徹底する。

文字化けだけでなく **Debug assertion も発火する**（`String(const char*)` に非ASCIIバイト検査の `jassert` がある）。デバッガ無しの実行では静かに通過するので気づきにくい — `lldb -b -o run -o quit ./daw_tests` で走らせて `stop reason = EXC_BREAKPOINT` が出ないかを見る。また `const char*` を受け取るAPI（`daw_tests` の `expect` の説明文など）は `juce::String` を経由させず**UTF-8リテラルをそのまま渡す**（`std::cout` にはそのまま流れる）。

### 警告ゼロ基準で引っかかりやすいコンパイラ警告

- **浮動小数の `==` / `!=` は `-Wfloat-equal` で弾かれる**: 等値判定は `juce::approximatelyEqual (a, b)` を使う
  - 同じ `-Wfloat-equal` でも**リテラルとの比較（`gain != 1.0f`）では警告が出ない**（clangの例外）。警告は変数同士の比較で出る
  - **「厳密同値」が仕様の箇所は `juce::exactlyEqual (a, b)`**（ビット一致の検証・エンベロープ端点の0/1など）。`approximatelyEqual` に寄せると回帰を検出できなくなる
  - 平坦/傾斜のような**分岐はゲイン値の比較で決めない**（値がたまたま一致したかで経路が変わる）。区間種別のフラグを持たせる
  - **エンジン出力の「素の振幅」を厳密一致で判定してはいけない**: panセンターのモノゲインは `cos(π/4) * √2` で、floatでは 1.0 にわずかに満たない（乗算1回ぶんの誤差が乗る）。許容誤差 `1e-6` 程度で見る
- **ネスト内部クラス（RulerContent等）のローカル変数が外側クラスのフィールドと同名だと `-Wshadow`**: 内部クラスからは外側クラスのメンバが名前探索で見えるため、`pxPerBar` のような外側のフィールド名をローカル変数に使わない

### Shift併用ショートカットは getTextCharacter がシフト後の文字を返す

`Shift+,` は `KeyPress::getTextCharacter()` が `<` になる（レイアウト依存）。`switch (getTextCharacter())` で拾う場合は、素の文字＋`isShiftDown()` と、シフト後文字（`<` `>` 等）の両方のcaseを用意する。

### 修飾なし1文字のショートカットは「そのとき誰がフォーカスを持つか」で可否が決まる

macのpeerは**⌘押下時に `textCharacter` を0にする**（`juce_NSViewComponentPeer_mac.mm` の `handleKeyEvent`）。そのため⌘付きキーは `TextEditor::keyPressed` の文字挿入分岐（`getTextCharacter() >= ' '`）に落ちず `false` で親へ伝播する＝**入力欄にフォーカスがあっても効く**。逆に修飾なし1文字はそのまま入力されるので、入力欄を含むパネルのトグルには使えない（Escも `setEscapeAndReturnKeysConsumed (true)` なら吸われる）。⌘系でも `c/x/v/a/z/y` は `TextEditorKeyMapper` が消費する。一方 `ListBox` は↑↓/Page/Home/End/Return/Delete/⌘Aだけ処理して**文字キーは親へ流す**ので、リストだけのパネルなら修飾なし1文字でトグルできる。実例: メモ（TextEditor）は`⌘N`、ファイルパネル（ListBoxのみ）は`F`。

### AUホスティング（DLSMusicDevice）の落とし穴

- **同期インスタンス化**: `AudioUnitPluginFormat::findAllTypesForFile (found, "AudioUnit:Synths/aumu,dls ,appl")` → `createInstanceFromDescription`（同期版）で取得できる。AUv2なのでメッセージスレッドから同期生成してよい。コンソール（テスト）から使うときは `ScopedJuceInitialiser_GUI` でMessageManagerを先に初期化する
- **出力は2バス計4ch**: DLSは `getTotalNumOutputChannels() == 4`（ステレオ×2バス）を報告し、`disableNonMainBuses()` でも減らない。`processBlock` に渡すバッファのチャンネル数が足りないと `getWritePointer` のチャンネル範囲assertを踏む。**全チャンネル分のバッファを渡し、ミックスにはメインバスのch0/1だけを使う**
- **プログラムチェンジは公開前に適用**: 生成直後（他スレッドから見えるようになる前）ならメッセージスレッドから `processBlock` を直接1回呼んでプログラムチェンジを流せる。楽器変更は既存インスタンスへのイベント送信でなくインスタンス差し替えにすると、発音中ノートの後始末（All Notes Off）そのものが不要になる
- **AUの寿命はスナップショットの `shared_ptr` 共有で守る**: `ClipPlayback::audio` と同じパターン。オーディオスレッドは snapshot 内の参照を辿るだけ（shared_ptrのコピーはしない）。参照カウントの増減（構築・破棄）は必ずメッセージスレッド側（`buildSnapshot` / `deleteRetired`）で起きる

### ログとクラッシュ処理の落とし穴

- **`SystemStats::setApplicationCrashHandler` からログを書いてはならない**: JUCE 8.0.9 の実装は POSIX シグナルハンドラで、コールバック後に `::kill(getpid(), SIGKILL)` する（`juce_SystemStats.cpp`）。シグナル文脈での `FileLogger`（mutex・String確保・ファイルopen/write）や `getStackBacktrace()`（`backtrace_symbols` が malloc する）は未定義動作。**このプロジェクトでは使わない**。クラッシュのスタックトレースは macOS 標準の `~/Library/Logs/DiagnosticReports/*.ips` に任せ、自前ログ（`shared/Log.h`）は「直前に何をしていたか」の文脈提供に徹する
- **`juce::FileLogger` は1メッセージごとにファイルを開閉し、タイムスタンプもセッション中のサイズ上限も付けない**: 常用ログには薄い自前実装（`shared/Log.cpp` の SessionLogger）を使う。`Logger::setCurrentLogger` は生ポインタ参照なので、shutdown では **`setCurrentLogger(nullptr)` を先に呼んでから logger を破棄**する
- **オーディオスレッドの異常はatomicカウンタに載せてUIのTimerで集約ログする**: `transport.midiDroppedNoteOns` / `transport.recordDroppedBlocks` のパターン（boolフラグだと発生件数が数えられない。発生箇所で `fetch_add`、Timer側で `exchange(0)`）。`ThreadedWriter::write` は FIFO 満杯時に false を返すので録音ドロップもこれで拾える。連続発生に備えてログは2秒に1回・件数付き1行に抑える（`MainComponent::pollAudioAnomalies`）

### フォント解決とLookAndFeelの落とし穴

- **macのTypeface解決は「family名＋style名」**（`juce_Fonts_mac.mm` がCTFontDescriptorに `kCTFontFamilyNameAttribute` + `kCTFontStyleNameAttribute` で問い合わせる）。`.AppleSystemUIFontDemi` のような**font名（PostScript名）指定では引けない**。semibold等のウェイトは `FontOptions(".AppleSystemUIFont", "Semibold", h)` とfamily＋styleで指定する。familyが持つstyle一覧はCoreTextの `CTFontDescriptorCreateMatchingFontDescriptors` で列挙して事前確認できる
- **システムUIフォントの内部名**: 可変幅 = `.AppleSystemUIFont`（SF Pro相当）、等幅 = `.AppleSystemUIFontMonospaced`（SF Mono相当）。アプリ全体への適用は `getTypefaceForFont` の手書きoverrideでなく **`setDefaultSansSerifTypefaceName(".AppleSystemUIFont")`** が正解（基底実装が「デフォルトsans名のときだけ差し替え、明示指定は素通し」なので、`Fonts::mono` 等の明示指定と共存できる）。日本語グリフはSF Proに無いが、JUCE 8のフォールバックでヒラギノに自動解決される（8.0.9で確認済み）
- **日英混植の自由記述欄はフォールバック任せにしない**: `.AppleSystemUIFont` を指定すると英字はSF Pro、日本語はヒラギノへフォールバックするため、同じサイズでも見かけの大きさ・ウェイトが揃わない。メモのように日英が同じ文章内へ入るUIは、`FontOptions("Hiragino Sans", "W3", 13.0f)` のように両言語を描ける同一書体へ固定する。`TextEditor` の外側余白はbounds、文字との内側余白は `setIndents(x, y)` で別々に調整する
- **LookAndFeelデフォルトのコントロールフォントはHIG（13px）より大きい**: TextButton = `min(16, 高さ×0.6)`、ComboBox = `min(16, 高さ×0.85)`、Label = 15px。全角を目一杯使う日本語で特に大きく見える。本プロジェクトは `AppLookAndFeel` + `ui/Fonts.h` で13pxに統一済み。**新しいコントロール種を使うときはデフォルトフォントを確認すること**
- **`getLabelFont` はoverrideしない**: 基底実装が「Label自身のフォントを返す」ため、無条件overrideすると `setFont` で設定した `Fonts::mono` 等を壊す。Labelは生成側で `setFont` する
- **ToggleButtonはフォント取得フックがない**: `drawToggleButton` 内に `min(15, 高さ×0.75)` がハードコードされており、変えるには描画メソッドごとコピーしてoverrideするしかない
- **ツールチップは13px boldハードコード＋bounds計算も太字前提**: `LookAndFeel_V2::drawTooltip` はフォントフックがなく内部ヘルパー（匿名namespaceで再利用不可）で13px boldを使う。日本語はヒラギノ太字になり主張が強い。スタイル変更は `drawTooltip` と `getTooltipBounds` の**両方**を同じ自前レイアウト関数でoverrideする（片方だけだと箱サイズと描画が食い違う）。本プロジェクトは `AppLookAndFeel` で11px regular化済み

### `juce::Colours::*` から名前空間スコープの色定数を初期化しない

`Colours::white` 等はヘッダ内の**非constexprなTUローカルconst**（`const Colour white { 0xffffffff };`）。これを自前ヘッダの `inline const juce::Colour` の初期化子に使うと、静的初期化順序次第で**ゼロ初期化（透明黒）のColourを拾う**。実害確認済み: `Theme::playhead { juce::Colours::white }` で再生ヘッドと小節番号が描画されなくなった（コンパイルは通り、実行時も例外なく「静かに消える」）。`ui/Theme.h` の定数は必ず16進リテラル（`{ 0xffffffff }`）で書く。関数内・描画コード内での `juce::Colours::white.withAlpha(...)` の直接使用は問題ない（使用時点では初期化済み）。

### レベルメーターの表示はdBスケールに写す

リニア振幅をそのままバー幅にすると、実用レベル（振幅0.1 = -20dB）が全幅の2%にしか見えず機能しない。`(20*log10(v) + 60) / 60` で -60dB..0dB を 0..1 に写してから描く（実DAWのメーターと同じ）。閾値判定（クリップ警告0.9等）はリニアのままでよい。なお `juce::Decibels` は juce_audio_basics 所属なので、juce_gui_basics しか include しないヘッダでは `std::log10` で直接計算する。

### drawLinearSliderに渡る矩形はコンポーネント境界でなく「可動域」

x, y, width, height は `getSliderThumbRadius` ぶん両端を詰めたトラック領域で、Sliderコンポーネントのboundsより小さい。スライダーの溝を隣に並べる自前コンポーネント（メーター等）と端揃えするときは、同じ詰め幅を定数で共有する（`StereoMeter::wellInsetY` = つまみ半径、が実例）。片方だけ固定マージンで詰めると上下端がズレる。

### Sliderの値をundo対象にするなら区切りは「クリック列の先頭」で入れる

`onDragStart` を「編集開始」に使うと、1回のダブルクリックでundoが2〜3件に割れる。JUCEは `mouseDown` ごとに `ScopedDragNotification` を作り（`juce_Slider.cpp:900`）、ダブルクリック確定時にも別の通知を出す（同`:1121`）ためで、さらにつまみ以外をクリックすると値がその位置へ飛ぶので、**最初の⌘Zが元の値でなく中間値へ戻る**。

- 区切りは `mouseDown` を override して `e.getNumberOfClicks() <= 1` のときだけ入れる（`>= 2` はJUCE自身が `mouseDoubleClick` の発火条件に使う判定＝`juce_Component.cpp:2325` の裏返し）。実装例は `ui/GainControls.h` の `GainSlider::onNewClickSequence`
- undoを積むのは「値が実際に動いた最初の1回」。`setValue` には同値ガード（`juce_Slider.cpp:218`）があるので、値が動かないクリックでは `onValueChange` が来ない＝ゴミが積まれない
- `onDragEnd` で区切りをリセットしてはいけない（ダブルクリックの2回目の `mouseDown` より前に挟まるため、また2件に割れる）
- **ホイールは `mouseDown` を伴わない**ので区切りが漏れ、「ドラッグ後にホイールで微調整すると⌘Zがドラッグ前まで戻る」。区切りを増やすより `setScrollWheelEnabled (false)` で経路を絞るほうが単純（ホイールは1回転で複数イベントが来るので、素直に区切ると1目盛りごとに積まれる）

### PopupMenuは「全画面×画面端」に隣接表示できない

PopupMenuの表示位置はOSの「使用可能画面領域」（Dock除け）にクランプされる。全画面表示ではウィンドウがDock領域まで覆うのに、画面最下部のボタン直上には出せない: デスクトップウィンドウ方式（既定）はDock上端まで押し上げられて余白ができ、`withParentComponent` でもターゲット矩形が使用可能領域との交差で空になり左上に飛ぶ。画面端に置いたボタンのメニューは、ウィンドウ内に自前描画するオーバーレイで作る（`ui/AddTrackOverlay.h` が実例。位置計算にOSの画面情報を使わないので全画面/通常で挙動が一致する）。なおPopupMenuはアプリ非アクティブになると即閉じるため、AXPressで開いて背面スクショで確認する検証もできない（自前オーバーレイなら可能）。

- **メニュー項目のショートカット表記は `PopupMenu::Item::shortcutKeyDescription`**: setterのない公開フィールドに直接代入して `addItem (item)`。ApplicationCommandManager無しで表示できる（描画はLookAndFeel任せ）。`setShortcutKeyDescription` という setter は存在しない（8.0.9で確認）
- **`withTargetComponent` をスクロールするコンテンツに使わない**: ルーラー・マーカーレーンのように「viewport の外に置いて `setTopLeftPosition (-viewPositionX, …)` で横スクロールへ手動追従させる」コンポーネントは、原点がスクロール量ぶん画面外の左にあり幅も全コンテンツぶんある。これを渡すとメニューがウィンドウの遥か左に出る（メニュー自体は出るので原因が見えにくい）。マウス位置に出すなら**素の `Options()`**（タイムライン内の他メニューと同じ）

### `JUCE_APPLICATION_NAME_STRING` は JuceHeader 生成時のみ定義される

`juce_add_gui_app` の `PRODUCT_NAME` を渡しただけでは定義されず、`juce_generate_juce_header` を使うプロジェクトでのみ使えるマクロ。本プロジェクトはJuceHeaderを使わないため、アプリ名をコードから参照するときは `target_compile_definitions` の自前定義（`DAW_APP_NAME`）を経由する。

### コールバック実行中のComponentを自己破棄しない（callAsync＋SafePointerで遅延）

ボタン・リスト・`keyPressed` のコールバック内で `setContentOwned` 等により自コンポーネントを差し替えると、実行中オブジェクトの自己破棄になる（スタック上のメソッドの持ち主が消える）。画面遷移は `MessageManager::callAsync` でコールスタックを抜けてから実行する。ラムダには `Component::SafePointer` を捕捉し、実行時に生存確認する（キュー済み遷移や `NativeMessageBox::showAsync` のコールバックが shutdown 後に走る競合も同時に防げる）。連打対策の再入ガードフラグ（開始で立て、キャンセル・失敗・遷移完了で戻す）もセットで入れる。

### `DialogWindow::LaunchOptions::launchAsync()` はモーダル（トグル開閉にはできない）

`launchAsync()` は `enterModalState` に入るため、開いている間はメインウィンドウへのクリックが一切届かない。「開いたボタンをもう一度押して閉じる」ができず、閉じ方が×とEscだけになる。トグル開閉にしたいなら `juce::DialogWindow` を継承して `setVisible(true)` + `toFront()` で非モーダルに出す（`Source/ui/DeviceSettingsWindow.h`）。その際:

- 閉じる経路は `closeButtonPressed()`（×）・`escapeKeyPressed()`（Esc）を override して1関数に集約し、所有者へ通知するだけにする。実際の破棄は所有者側が `MessageManager::callAsync` で行う（→「コールバック実行中のComponentを自己破棄しない」）
- **そのウィンドウにフォーカスがある間は `MainComponent::keyPressed` にキーが来ない**。開閉ショートカットはウィンドウ側でも `keyPressed` を override し `Shortcuts::matches()` で拾う。全キーをメインへ転送するかは画面の性格で決める（ミキサーは転送して再生・m/sを効かせる／デバイス設定は⌘,だけ拾い、設定中に `r` で録音が走らないようにする）
- モーダルでなくなる＝危険な同時操作が可能になる。競合する操作（録音開始とデバイス変更等）は開始側から明示的に閉じる

### CallOutBoxは「非同期だがモーダル」・アンカー矩形は可視領域と交差させる

`CallOutBox::launchAsynchronously` は内部で `callout.enterModalState (true, this)` を呼ぶ（`juce_CallOutBox.cpp:71`）。表示中はキー入力も⌘Zもボックスが取り、外部クリックは閉じてから下のコンポーネントへ渡る。つまり表示中にユーザー操作でモデルが変わることはなく、保持したindexが失効する経路は録音完了のような非同期処理とプロジェクト破棄だけに絞れる。ドラッグ中の再描画・タイマーは通常どおり走る。

**CallOutBox 内の TextEditor は外側クリックで閉じると `onFocusLost` が走らない**（パネルごと破棄されるため）。「Return を押さずに閉じる」自然な操作で入力途中の値が黙って捨てられる（移調・伸縮の小節数入力で実際に「打ったのに反映されない」となった）。対策はパネルのデストラクタで未確定入力を commit する（値が変わらなければ no-op の冪等な commit にしておけば、Return / focusLost 済みの経路と二重適用にならない）。

- 位置決めと矢印の向きはJUCEが自動でやる（`updatePosition`）ので、渡すのは「指し示す矩形」だけ。ただし**矩形の中央と各辺が矢印の候補**になるため、Viewport内の長いアイテムを指すときは可視領域（`viewport->getBounds()`）との交差を渡す。そのまま渡すとスクロール状態次第で画面外を指す
- 交差を取る元は「クリックできる範囲全体」にする（ループ反復部分なども含む）。本体だけにすると「本体は画面外・反復だけ表示中」で交差が空になる

### AudioAppComponent はアプリ内1個が前提

ヘッダに「An application should only create one global instance of this object and multiple classes should not inherit from this」と明記されている（`juce_AudioAppComponent.h`）。実体は「`AudioDeviceManager`＋`AudioSourcePlayer`」の組。複数の再生系を並べたくなったら、継承をやめてアプリ所有の `AudioDeviceManager` 1個に各自の `AudioSourcePlayer` を `addAudioCallback` する（複数コールバックの出力は合算・入力は全コールバックに配られる）。デバイスSRはグローバル1個なので、SR変更の主導権をどこが持つかの設計も必要になる。

### PopupMenuの角丸カスタム化は背景色を透明にする

LookAndFeelで `drawPopupMenuBackground` を角丸にしても、`PopupMenu::backgroundColourId` が不透明のままだとメニューウィンドウ自体がopaqueになり角の外が黒く残る。`juce_PopupMenu.cpp` のMenuWindowがこの色の `isOpaque()` で `setOpaque` を決めているため、`setColour (backgroundColourId, transparentBlack)` にして全描画を自前で行う。もう1つの罠: `getIdealPopupMenuItemSize` には**ショートカット表記文字列が渡ってこない**（本文textのみ）。ショートカット付き項目の幅は右余白の決め打ちで吸収するしかない（AppLookAndFeel.hでは+44px）。

### TextEditorのフォーカス枠は2pxのベタ塗り

`LookAndFeel_V4::drawTextEditorOutline` はフォーカス時だけ `focusedOutlineColourId` で**2px**の矩形を描く（非フォーカスは1px）。面積の大きい入力欄では枠が主役になってしまう。細くする・やめるには ①`AppLookAndFeel` で `drawTextEditorOutline` をoverride（全TextEditorに効く）②その欄だけなら `outlineColourId` / `focusedOutlineColourId` を両方 `transparentBlack` にしてサブクラスの `paintOverChildren` で自前描画する（メモ欄 `MemoEditor` はこの方式で、フォーカスは枠でなく地の明度で示している）。

### `Component::setColour` は再描画しない

`colourChanged()` の基底実装が空なので、`setColour` を呼んだだけでは画面に反映されない。フォーカスや状態に応じて色を差し替えるときは明示的に `repaint()` する。`TextEditor::focusGained/focusLost` は内部で `repaint()` を呼ぶが、それは基底呼び出しの時点なので、その**後**に色を変える実装は順序依存になる（自分で `repaint()` を呼ぶこと）。

### 起動直後のgrabKeyboardFocusはisShowing()が偽で空振りする

MainWindowのコンストラクタは `setContentOwned`（→ `parentHierarchyChanged` 発火）→ `setVisible(true)` の順で走るため、初回表示時のフックで `isShowing()` を条件にしたフォーカス取得は実行されない。`MessageManager::callAsync`＋SafePointerでコールスタックを抜けてから再試行する（画面遷移で戻ってきた場合はウィンドウ表示済みなので即時パスが通る）。

### ListBoxのカスタムUI化で先に知っておく5つの制約

① `paintListBoxItem` にはhover状態が渡ってこない → ListBoxに `addMouseListener (this, true)` してmouseMoveで `getRowContainingPosition` → 行indexを自前保持し `repaintRow` で更新（ProjectChooserComponent参照）② 行高は `setRowHeight` の全行共通のみ。可変高の行（ヒーローカード等）はListBoxの外に別Componentとして置く ③ ListBoxがフォーカスを持つと↑↓Returnは素のListBox処理に奪われる。リスト外の要素を含む選択遷移を作るなら `listBox.setWantsKeyboardFocus (false)` にして親の `keyPressed` で処理する ④ **行内に置いたボタンでmouseDownを取ると、その領域だけ選択と `getDragSourceDescription` 由来のドラッグが死ぬ**。行コンポーネント自体は `setInterceptsMouseClicks (false, true)`（自分は透過・子だけ受ける）にして、押したい部分だけを覆う子Componentを1つ置く。押した側では選択が起きないので `selectRow()` を自前で呼ぶ（AudioFileBrowserView参照）⑤ **`refreshComponentForRow` の `existingComponentToUpdate` は「別の行のために作られたもの」が渡ってくる**（スクロールで使い回される。JUCE本体のコメントに明記あり）。作成時の行番号をクリックのlambdaにキャプチャすると、スクロール後に別の行を操作する。呼ばれるたびにコンポーネント側の行番号を書き直す。

### ネイティブメニューバー（setMacMainMenu）の4つの罠

① **keyEquivalent（⌘S等の表記）は `ApplicationCommandManager` 経由でのみ設定される**。`PopupMenu::Item::shortcutKeyDescription` はNSMenu変換で無視される（`juce_MainMenu_mac.mm` の `addMenuItem` 参照）。コマンド登録＋`commandManager.getKeyMappings()->addKeyPress()` が必須 ② **NSMenuのkeyEquivalentは `keyPressed` より先にイベントを取る**。メニューに載せたキーの実処理は `ApplicationCommandTarget::perform` に一本化する（keyPressed側の同判定はデッドパス化するがフォールバックとして無害）③ **メニューのenabled状態はNSMenu構築時のスナップショット**で古くなる（disabled表示のままでもキー押下は素通りし得る）。`perform` 側でも必ず状態ガードし、enable条件が変わったら `MenuBarModel::getMacMainMenu()->menuItemsChanged()` で組み直す（Main.cpp / MainComponent::refreshMacMenu 参照）④ **メニュー項目の追加は4点セット**。コマンドenum・`getCommandInfo`・`perform` を揃えても、`AppMenuModel::getMenuForIndex` に `addCommandItem` を足さないと項目自体が現れない（ショートカットだけ効く中途半端な状態になり、ビルドは通るので気づきにくい）。

### ノートの位置判定をPPQ同士で比べるとグリッド上のノートを取りこぼす

再生位置 `pos` は「グリッド位置をサンプル整数へ丸めた値」（シーク・サイクル範囲・録音位置はすべて
サンプル単位に丸めて保持する）。これを `pos * ticksPerSample` でPPQへ戻すと、元のPPQより
わずかに大きく（または小さく）なるため、`note.startPpq >= pos * tps` のようなPPQ同士の比較では
**境界ちょうどのノートが「過去のノート」に落ちて発音されない**。

実例: 127BPM・44.1kHz の16分音符1つ目は PPQ 240 に対しサンプル 5209。戻すと 240.0156 になり、
その位置へシーク（またはサイクル範囲の開始位置に設定）すると PPQ 240 のノートが発音されない。
持続音は resound（跨ぎノートの再発音）で救われるが、**One Shot（resound対象外）は完全に消える**。

判定・オフセット計算の両方を `llround(ppq / tps)` の整数サンプルに統一する。
PlaybackEngine（RT）とBounceRenderer（オフライン）は同じ規則で書くこと（片方だけ直すと
「再生とバウンスの出力一致」が崩れる）。

### WindowedSincInterpolatorはレイテンシ補償と終端ゼロパディングが必須

① 入力100サンプルのアルゴリズム遅延を持つ（`WindowedSincTraits::algorithmicLatency`）。補償しないと出力全体が100入力サンプル分後ろへずれ、末尾が同じ分欠落する。「先頭の遅延分を捨て、その分余計に生成」で補償する（AudioImporter参照）② `process(available, wrapAround=0)` は「入力が尽きたらゼロを供給」に見えるが、**available==0でも境界判定より先に入力ポインタを1サンプル読む**（`interpolateImpl` が `input[numUsed++]` を先に実行）ため境界外読みになる。ソースバッファ末尾にゼロパディングを確保し、渡すポインタとavailable（≥1）を常にバッファ実体内に収める ③ 検証は「末尾1サンプルのパルスが出力末尾に残るか」＋「中間パルスが換算位置±2サンプルに出るか」のテストで固定する（testAudioImporter参照）

### `addFromWithRamp` の `endGain` は「最終サンプルの次に適用される値」

サンプル `i` のゲインは `startGain + i / numSamples * (endGain - startGain)`（`juce_AudioSampleBuffer.h` のdocに明記）。つまり **`endGain` には「区間の排他端で評価したエンベロープ値」を渡す**。閉区間のフェード（`n` サンプルで先頭が0・`n-1`番目が1）ならフェードイン区間の排他端は `n/(n-1) > 1`、フェードアウトは負になるが、**実際には適用されないのでクランプしてはいけない**（クランプすると傾きが狂う）。

実装は `startGain += increment` の逐次加算なので最終ゲインは厳密に0にならない。**ブロックごとに絶対位置から `startGain`/`endGain` を評価し直す設計なら、誤差はランプ全長ではなくブロック長で頭打ちになる**（2秒のランプでも一括処理なら-60dB残るが block=512 なら-155dB）。実測の最悪は240サンプル（1ブロックに収まる短いランプ）で 2.8e-6（-111dB）。

- **ブロックサイズが変わると出力はビット一致しない**（区間の切れ目で加算順序が変わる）。実測の最大差は 2.4e-5 なので、再生（デバイス依存）とバウンス（1024固定）の一致テストは `1e-4` 程度の許容誤差で見る
- **曲線（S字等）を1ブロック=1直線で近似する場合、この `1e-4` は当てはまらない**。上の 2.4e-5 は「直線ランプを分割したときの加算順序の差」だが、曲線をブロック単位で折れ線にすると**近似そのものの誤差**が乗り、ブロックサイズが変われば折れ点の位置ごと変わる。誤差は**区間長の2乗に反比例**する（区間を4倍にすると誤差は約1/16）。曲末フェード（S字）の実測: 0.3秒フェードで RT(512) vs バウンス(1024) が **1.5e-3**、ブロック512 vs 500 が 3.5e-4。実使用の長さ（数秒〜数十秒）ではこの数百分の1になり聴感差はないが、**短い区間で書いたテストは必ずこの誤差を踏む**ので、許容は実測してから決める
- **「許容誤差いくつ？」は推測せず、JUCEと同じfloat加算を再現して測ってから決める**（nodeの `Math.fround` で足りる）
- 区間の継ぎ目で連続なのは**適用されるゲイン**であってパラメータではない。同一傾斜内の分割ならパラメータも一致するが、傾斜↔平坦↔無音の境界は一致しない（検証は実適用ゲインで行う）
- **ランプの境界（開始・終端）ではブロックを分割する**。分割せずに「ブロック両端のゲイン」で直線ランプを掛けると、境界を越えてゲインが漏れる（例: `[end-256, end+256)` に開始>0・排他端0のランプを掛けると、本来0であるべき `end` 以後の256サンプルに非ゼロが残る）。`PlaybackEngine` はサイクル境界と同じループで切っている
- ⚠️ **境界での分割は `jmin` で書かない**（`segLen = jmin(segLen, boundary - segPos)`）。境界ちょうどに来たとき `segLen = 0` になり、消化サンプル数が進まず**無限ループする**。サイクルが `jmin` で成立しているのは `segPos >= cycleEnd` で範囲頭へ**巻き戻す**からで、巻き戻しのない境界（曲末フェード等）では `if (segPos < boundary && boundary < segPos + segLen)` と「現在位置より厳密に後ろ」を条件にする。`BounceRenderer` 側も同様で、境界で短縮するなら `pos += renderBlockSize` を **`pos += n`** に変えないと境界後のサンプルを飛ばす
- 段差（値の不連続）を消す用途では**閉区間**（`n-1` で割る）にする。半開区間（`n` で割る）だと最終サンプルにゲイン `1/n` が数学的に残り、240サンプルでも -47.6dB で鳴る

### 外部プロセスを起動するなら `juce::ChildProcess` を使わない

`juce_SharedCode_posix.h` の実装に3つの問題がある。① `read()` が内部で `fread()` を呼ぶ**ブロッキング実装**（`:1193`）で、出力が止まるとキャンセル要求を観測できず終了が固まる ② `killProcess()` が `::kill(childPID)` だけで子を `setpgid` しないため、**子が spawn した孫（yt-dlp → ffmpeg 等）が孤児化する**（子はアプリと同じプロセスグループに入るので `killpg` すると自分を巻き込む） ③ デストラクタが子を待たない／killしない（ヘッダにも "deleting this object won't terminate the child process" と明記）。

代わりに `Source/shared/SpawnedProcess` のように `posix_spawn` ＋ `POSIX_SPAWN_SETPGROUP` ＋ `posix_spawnattr_setpgroup(&attr, 0)` で子を新プロセスグループのリーダーにし、`O_NONBLOCK` パイプ＋タイムアウト付き `poll()` で読む。副次効果として **stdout/stderr を別パイプで取れる**（JUCE版は両方を同一パイプにマージするため分離不可）。`posix_spawn` は PATH を検索しない（するのは `posix_spawnp`）ので実行ファイルは絶対パスで渡す。`.app` は launchd 起動で PATH が最小限になるため、Homebrew のツールを呼ぶなら `/opt/homebrew/bin/...` を明示的に探すこと。

**`killpg` する側の注意**: 「直接の子を `waitpid` で回収できたら終わり」にすると、**SIGTERM を無視する孫が残る**（子が先に終了するケースがある）。PGID を `childPid` とは別に保持し、`killpg(pgid, 0)` でグループの消滅を確認しながら SIGTERM → 猶予 → SIGKILL と進める。未回収の子（ゾンビ）もプロセスとして数えられるので、**生存確認の前に必ず `waitpid(WNOHANG)` で回収する**。検証は名前（`pgrep -f ffmpeg` 等）ではなく **PGID** で数える（ユーザーが別用途で動かしている同名プロセスを誤検知する）。

### `getSpecialLocation(tempDirectory)` は `$TMPDIR` を返さない

macOS では **`~/Library/Caches/<実行ファイル名>/`**（`juce_Files_mac.mm`）。`$TMPDIR` を前提にシェルから検証すると空振りする。都合の良い副作用として、dev版（`LaLa-dev`）とRelease版（`LaLa`）で自動的に別ディレクトリになるため、並走しても一時ファイルが混ざらない。

### pull型ワーカー（`juce::Thread` + atomic status）を追加するときの3点

- **`currentStatus.store(terminal)` は `run()` の最後の1文にする**。先に公開すると、まだ `run()` の中に居るうちに呼び出し側が `takeResult()` を呼ぶ
- **`takeResult()` は `waitForThreadToExit(-1)` してから result を move する**。`run()` の最終行で status を公開しても、JUCEがスレッドハンドルを閉じるのは `run()` から戻った後で、その隙間では `isThreadRunning()` がまだ true（`jassert(! isThreadRunning())` だけでは防げない）
- **`startThread()` は `bool` を返す**。無視して `return true` すると、スレッド作成失敗時に status が running のまま残り、進捗オーバーレイが永久に閉じない

既存の `AudioImporter` / `BounceRenderer` は後者2点が未対応（`startThread()` の戻り値を見ておらず、`takeResult()` は `jassert` のみ）。触るときに合わせて直す。

### 標準AboutパネルにObjCから options を渡すなら `WithOptions:` の方

`[NSApp orderFrontStandardAboutPanel: @{...}]` はコンパイルも実行も通るが**辞書は黙って無視される**（このセレクタの引数は sender）。options 版は `orderFrontStandardAboutPanelWithOptions:` で、Swift の `orderFrontStandardAboutPanel(options:)` はこちらにマップされるため、Swiftの小さなプローブで検証すると「効いた」と誤認する。

バージョン行の組み立ては `"Version <A> (<B>)"`（A=`NSAboutPanelOptionApplicationVersion` / B=`NSAboutPanelOptionVersion`。キー名の語感と逆）。挙動:

- **A を指定すると "Version" ラベルごと置き換わる**（ラベルが要るなら自前で `Version %@` と組む）
- **B に空文字を渡すと括弧ごと消える**
- daw は `CFBundleShortVersionString` と `CFBundleVersion` の両方に `PROJECT_VERSION` を入れているため、素のパネルだと `Version 0.7.0 (0.7.0)` と同じ数字が二重に出る。上の2点で1つに畳んでいる（`Source/mac/AboutPanel.mm`）

### `juce::var` の直接キャストは型違反を正常値に化けさせる

`(int) v` は `1.5` を 1 に切り捨て、int範囲外の int64（`4294967296`）を環境依存で 0 に切り詰め、
`toString()` は数値からも文字列を作る。契約のあるJSON（project.json・外部ツールの出力）を読むときは、
`v.isInt() || v.isInt64()`＋int範囲確認・`isBool()`・`isString()`・`isDouble()` で**型を確認してから**
変換する（変換後の値域チェックだけでは元の型違反を検出できない — ループアンカーの実装でレビューに
3周連続で刺された）。欠損は `getProperty` の第2引数のデフォルトでなく `isVoid()` で区別すると、
「無い」と「壊れている」を分けて警告できる。実装例は `Project.cpp` の `strictIntVar` /
`anchorFromContractJson` と `GachaSession::parseRecommendJson`。

### `JSON::parse(...).getDynamicObject()` を一時式のまま使わない

`if (auto* obj = juce::JSON::parse (text).getDynamicObject())` は、条件式の完全式終了で一時 `var` が
破棄され（DynamicObjectは参照カウントで道連れ）、if本体の `obj` がdanglingになる。読める値が返る
こともあり症状が不安定（StemCacheのlock読取で実バグ→テストが検出）。
`const auto parsed = juce::JSON::parse (text);` と名前を付けて生かしてから `getDynamicObject()` する。
なお `DynamicObject::getProperty()` は `const var&` を返すので同パターンでも安全。

### 起動引数のファイルパスは openFile イベントでも二重に届く

macOS（AppKit）はコマンドライン引数のファイルパスを document open イベントとしても配送するため、
`initialise (commandLine)` で自前パースして開くと、直後に `anotherInstanceStarted` 経由で同じファイルが
もう一度届く。openFileが状態リセットを伴う実装だと「開いた直後に再生・選択が巻き戻る」症状になる。
**現在ファイルと同一パスの再オープンはスキップ**して冪等にしておく（Salvaで実例）。

### PopupMenuの落とし穴（カスタムLookAndFeel・角丸・検証）

- **手動生成した `PopupMenu` はターゲットのLookAndFeelを継承しない**: `findLookAndFeel` は `menu.lookAndFeel` しか見ず、未設定ならデフォルトLnFに落ちる（`Options().withTargetComponent()` を渡しても効かない）。`menu.setLookAndFeel (&getLookAndFeel())` を明示する。ComboBoxのポップアップが効くのは内部で明示しているため
- **メニューの角丸は背景色の不透明度で決まる**: MenuWindowは `findColour (PopupMenu::backgroundColourId).isOpaque()` で `setOpaque` を判定する。完全不透明だと角丸の外が白く塗られる → 背景色をわずかに非不透明（alpha 0xfa等）にして `drawPopupMenuBackgroundWithOptions` で角丸を描く（実装例: `apps/salva/Source/ui/SalvaLookAndFeel.h`）
- **アプリが前面でないとポップアップは即dismissされる**（`doesAnyJuceCompHaveFocus` が偽で〜10ms後に閉じる）。`open -g` のバックグラウンド起動では開いた瞬間に消えるため検証不可。見た目確認はユーザーの実操作に頼るか、前面化できるタイミングで行う

### 「デバイス切替後に無音」はアプリのバグと限らない（macOSのデバイス別ミュート）

macOSの消音は**出力デバイスごとに記憶される**。切替先が過去にミュートされたままだと、アプリが正しく出力していても無音になる。切り分けは2点:

1. コールバックが書いた実出力ピークをatomicに記録してUIから毎秒ログ（Salvaのdev版 `debug.outpeak` が実装例。0.8等が出ていればJUCE側は健全）
2. CoreAudioでデバイスの `kAudioDevicePropertyVolumeScalar` / `kAudioDevicePropertyMute`（output scope）を照会（swiftの小ツールで一覧できる。`mute=1` が犯人）

解除はコントロールセンターでそのデバイスを選んで消音解除。

### juce::Reverb は「setParameters→setSampleRate」の順で初期化する

コンストラクタ既定が dry 0.4 / wet 0.33 で、`setParameters()` は内部 SmoothedValue の**ターゲットを変えるだけ**（即時反映しない）。平滑値をターゲットへスナップするのは `setSampleRate()`。full wet で使うつもりで「setSampleRate → setParameters(dry 0)」の順に書くと、**冒頭10msだけ dry 0.4 が漏れる**（`reset()` はバッファを消すだけで平滑値は触らない）。逆順（wet/dry設定 → setSampleRate）なら最初から確定する。聴感では気づきにくく、出だし位置のサンプル一致テストで検出できた（バッチ4のPre-delayテスト）。

### FetchContent で「取得のみ」したい依存は Populate を使う（MakeAvailable は上流の CMake 要求を巻き込む）

`FetchContent_MakeAvailable` は上流を `add_subdirectory` するため、上流の `cmake_minimum_required` が LaLa の宣言（3.22）より新しいと configure できない。**ローカルの CMake が新しいと 3.22 宣言のままでも通ってしまうため、自分の環境では検知できない**（signalsmith-stretch = 3.24 要求で実際に踏んだ）。include だけ使うヘッダライブラリは `FetchContent_Populate`（単引数形。CMP0169 は `if(POLICY ...)` ガード付きで OLD を明示）で**取得のみ**にする。CMakeLists.txt の signalsmith 節が実例。

### ドラッグ可能な点を持つグラフは、値域の端に描画領域の余白を取る

値の上限/下限の点が描画領域の端にぴったり乗ると、点の半分が領域外に出てヒット面積が半減し、外した側のクリックは親や隣のコンポーネントに配られる。ユーザーには「この点だけ動かない」「横に動かしてもカーブが変わらない」に見える（EQ のシェルフ +24dB で実際に起きた。OFF のハイパス点を掴んで「効かない」も同時に混ざっていて切り分けに手間取った）。座標変換（`yForDb` 等）の段階で `点の半径＋数px` の余白を引いておく（`EqEditorView::halfPlotHeight`）。枠線を足す変更（メーター窓化など）で潜在していたものが顕在化するので、グラフに枠を足したら端の点を掴めるか確認する。

## 分析パイプライン（tools/reference）の落とし穴

### demucs（MPS）は同一入力でも出力が微妙に揺れる

- GPU 演算の非決定性により、ステム分離の結果が実行ごとにわずかに変わる。閾値ベースの検査（無音判定・ゲート等）は境界ケースで「一度落ちて再実行では通る」flaky になる（実例: 同じ track.wav で excerpts.py の無音検査が落ち、再実行では素通りした）
- 検査を書くときは「**系統的バグの検出**（例: 切り出しミスで全クリップが無音）」と「**データ由来で正当に閾値を割るケース**（例: そのステムの最静小節が実質無音）」を区別し、後者はエラー停止でなく除外＋続行にする（実例: excerpts.py の quiet 抜粋 → a07992a で修正）
- 「同一入力なら同一出力」を前提にした回帰テスト・キャッシュ判定も demucs 出力そのものには適用できない（ゴールデン比較は drums.py のような決定的な生成器にだけ使う）

### librosa 系プロセスの並走は BLAS スレッドを 1 に絞らないと轢き合う

- macOS の BLAS（Accelerate/OpenBLAS）はデフォルトで論理コア数ぶんスレッドを立てるが、pyin 等の小さい行列演算では**単独実行ですらスレッド1本の方が速い**（52.1→50.1秒）うえ、複数プロセス並走時はスピン待ちで sys time が爆発する（pyin 2本並走: 52→76秒に劣化・sys 97秒。`OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 VECLIB_MAXIMUM_THREADS=1` で 50.7＋13.2秒・sys 0.9秒）
- **数値結果はスレッド数によらずバイト一致**を確認済み（basics/stems/groove の json・png 全比較）。並列化の高速化でスレッド上限を入れても「精度を変えない」は崩れない
- demucs（torch・MPS）も同罪: CPU は 0.3 コアしか使わないのに torch のスレッドプールがスピンし、並走する basics.py を 35→73秒に轢いた。`OMP_NUM_THREADS=2` で解消（demucs 自身は遅くならない）
- この環境変数群は analyze.py（オーケストレータ）が子プロセスに注入している。ステップを単体で手実行して時間を測るときも付けないと並走時と別の数字が出る
- なお `taskpolicy -c utility` で非クリティカルステップを E コアに寄せる案は実測で効果なし（`mediaanalysisd` 等の外部負荷によるブレの方が大きい）。実行時間の計測時は `ps -Ao pcpu,comm -r | head` で外部負荷を先に確認する

### 子プロセスツリーの停止は killpg 一択（ppid 列挙は取りこぼす）

- ppid をたどって子孫を kill する方式は、①失敗した親の子孫は親の死の時点で reparent 済みで列挙不能 ②TERM 無視の孫が親の死で reparent すると次の列挙から消える、の2パターンで漏れる（analyze.py の初版実装で実際に漏れた）。プロセスグループ所属は reparent で変わらないので、ステップを `Popen(process_group=0)` で自グループのリーダーにして `killpg` で止めるのが唯一確実
- ただし別グループの子は起動元（LaLa の SpawnedProcess）の killpg が届かなくなる → orchestrator が SIGTERM/SIGHUP を例外に変換して全ステップグループへ TERM → 0.5秒 → KILL を伝播する（LaLa は TERM の1秒後に KILL を撃つので、その猶予内に完了させる。SIGHUP はターミナルを閉じたとき orchestrator にしか届かないため同じ経路が必要）
- **シグナルハンドラの即 raise は「Popen 完了〜管理リストへの登録」の窓で子を孤児化させる**: ハンドラはその区間だけフラグを立てるに留め、登録完了直後のチェックで raise する。sigmask でブロックする案は fork/exec した子がマスクを継承して TERM を無視する子になる副作用があるので使わない
- **macOS の `killpg(pgid, 0)` は生死判定に使えない**: グループ内に終了処理中のメンバーが1つでもいると EPERM（man kill）。存在確認は `pgrep -g <pgid>` を使い、シグナル送信側は EPERM も握りつぶす（killable なメンバーへの配送自体は行われる）
- 失敗したステップの drain スレッドは **kill 前に join しない**: 生き残った子孫が stdout/stderr パイプの write 端を握っていると EOF が来ず、join のタイムアウトぶん fail-fast が遅れる（join は kill 後）

### 見切りタイムアウト付きの同期CLI呼び出しに「初回だけ重い処理」を持たせない

- LaLa はガチャ系 CLI を同期実行＋デッドラインで見切る（`runGachaTool`）。この呼び出しの中に初回キャッシュ生成のような重い処理があると、**タイムアウト kill → キャッシュ未生成 → リトライも毎回同じ経路で失敗**の恒久ループになる（実例: recommend.py の upper-features 初回計算 13秒 vs 10秒見切り。exit=-15 が延々続いた）
- 初回コストは重い工程のついでに前倒しする（analyze.py の `upper-features` ステップが分析中にキャッシュを温める）。前倒しステップは `fatal=False` にして本来の成果物（カード）を道連れにしない
- 新しいガチャ系ツールにキャッシュや遅延初期化を足すときは「kill されても次回が速くなるか」を確認する

### yt-dlp の YouTube 403 は player client 単位で出る（1動画の検証では判定できない）

- YouTube 側の対策強化で、yt-dlp stable のデフォルト player client（2026-08 時点は android vr）が**動画によっては**ダウンロード段だけ `HTTP Error 403: Forbidden` になる（メタ取得は通る）。`brew upgrade yt-dlp` は stable が追いつくまで効かない（2026-08-18 実測: stable 2026.07.04 = brew 最新で403、nightly 2026.08.17 は修正済み）
- client と動画の相性は交差する: デフォルト client は「Me at the zoo」○ / Rick Astley 403、`web_safari` は Rick Astley ○（HLS・映像込みで重い）/「Me at the zoo」と縦動画 zVmYWuLC4Ek は No video formats found / Requested format is not available、`web_embedded` は3本とも音声のみで○（ただし埋め込み不可動画では使えないはず）。**1動画で通った/落ちたを client 全体の生死と誤認しない**
- 対処は「デフォルトで試行 → 403 のときだけ `--extractor-args "youtube:player_client=web_embedded,web_safari"`（複数指定でフォーマットをマージ・片方が失敗しても続行）でリトライ」（`UrlDownloader::download()` と `tools/reference/analyze-url.sh` に実装済み。403 判定は `YtDlpOutput::isHttp403`）。stable が追いつけば1回目で通りフォールバックは自然に発動しなくなる
- キャッシュ削除（`--rm-cache-dir`）やフォーマット限定（`-f 140`）はこの型の 403 には効かない（同一 client 経由のため共倒れ）
- **web 系 client は JS チャレンジ解決に deno（/opt/homebrew/bin）が必要**。`.app` は launchd 起動で PATH が最小限のため deno が見つからず、`n challenge solving failed` → フォーマット全滅 → `Requested format is not available` になる（デフォルト client はチャレンジ不要なので、この欠落は**リトライ経路でだけ**発火する）。対処は `SpawnedProcess::start` が子の PATH に Homebrew の bin を前置する実装
- **CLI・ターミナル起動の daw_tests で通っても .app で落ちる**のはこの PATH 差が原因のことがある。yt-dlp まわりの E2E は `PATH=/usr/bin:/bin:/usr/sbin:/sbin LALA_VERIFY_URL=… daw_tests` と launchd 相当の最小 PATH で回して初めてアプリ実機と同条件になる

### `File::replaceWithText` の既定改行は CRLF（bash スクリプトを書くと壊れる）

- `juce::File::replaceWithText (text)` は既定引数 `lineEndings = "\r\n"` で `\n` を CRLF に変換して書く。テストから bash スクリプトや `.next` 系の検証ファイルを生成すると `exit 65\r` のような行になり、bash が `numeric argument required` で exit 255 を返す（daw_tests の fake script で実際に踏んだ）
- スクリプト・改行に意味があるファイルは `replaceWithText (text, false, false, "\n")` と LF を明示する
