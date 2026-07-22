# GOTCHAS — JUCE・リアルタイムオーディオの落とし穴集

## JUCE一般の落とし穴

### 非ASCIIの文字列リテラルは必ず `juce::String::fromUTF8(u8"...")` を通す

`juce::String(const char*)` はUTF-8を解釈しない。日本語に限らず em-dash（—）や ● などの記号も対象で、生リテラルのまま `String` と連結すると文字化けする（`"daw — "` が「daw â」になった実例あり）。UI文言・タイトル・ダイアログの全てで `fromUTF8` を徹底する。

### 警告ゼロ基準で引っかかりやすいコンパイラ警告

- **浮動小数の `==` / `!=` は `-Wfloat-equal` で弾かれる**: 等値判定は `juce::approximatelyEqual (a, b)` を使う
- **ネスト内部クラス（RulerContent等）のローカル変数が外側クラスのフィールドと同名だと `-Wshadow`**: 内部クラスからは外側クラスのメンバが名前探索で見えるため、`pxPerBar` のような外側のフィールド名をローカル変数に使わない

### Shift併用ショートカットは getTextCharacter がシフト後の文字を返す

`Shift+,` は `KeyPress::getTextCharacter()` が `<` になる（レイアウト依存）。`switch (getTextCharacter())` で拾う場合は、素の文字＋`isShiftDown()` と、シフト後文字（`<` `>` 等）の両方のcaseを用意する。

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
- **LookAndFeelデフォルトのコントロールフォントはHIG（13px）より大きい**: TextButton = `min(16, 高さ×0.6)`、ComboBox = `min(16, 高さ×0.85)`、Label = 15px。全角を目一杯使う日本語で特に大きく見える。本プロジェクトは `AppLookAndFeel` + `ui/Fonts.h` で13pxに統一済み。**新しいコントロール種を使うときはデフォルトフォントを確認すること**
- **`getLabelFont` はoverrideしない**: 基底実装が「Label自身のフォントを返す」ため、無条件overrideすると `setFont` で設定した `Fonts::mono` 等を壊す。Labelは生成側で `setFont` する
- **ToggleButtonはフォント取得フックがない**: `drawToggleButton` 内に `min(15, 高さ×0.75)` がハードコードされており、変えるには描画メソッドごとコピーしてoverrideするしかない
- **ツールチップは13px boldハードコード＋bounds計算も太字前提**: `LookAndFeel_V2::drawTooltip` はフォントフックがなく内部ヘルパー（匿名namespaceで再利用不可）で13px boldを使う。日本語はヒラギノ太字になり主張が強い。スタイル変更は `drawTooltip` と `getTooltipBounds` の**両方**を同じ自前レイアウト関数でoverrideする（片方だけだと箱サイズと描画が食い違う）。本プロジェクトは `AppLookAndFeel` で11px regular化済み

### `juce::Colours::*` から名前空間スコープの色定数を初期化しない

`Colours::white` 等はヘッダ内の**非constexprなTUローカルconst**（`const Colour white { 0xffffffff };`）。これを自前ヘッダの `inline const juce::Colour` の初期化子に使うと、静的初期化順序次第で**ゼロ初期化（透明黒）のColourを拾う**。実害確認済み: `Theme::playhead { juce::Colours::white }` で再生ヘッドと小節番号が描画されなくなった（コンパイルは通り、実行時も例外なく「静かに消える」）。`ui/Theme.h` の定数は必ず16進リテラル（`{ 0xffffffff }`）で書く。関数内・描画コード内での `juce::Colours::white.withAlpha(...)` の直接使用は問題ない（使用時点では初期化済み）。

### レベルメーターの表示はdBスケールに写す

リニア振幅をそのままバー幅にすると、実用レベル（振幅0.1 = -20dB）が全幅の2%にしか見えず機能しない。`(20*log10(v) + 60) / 60` で -60dB..0dB を 0..1 に写してから描く（実DAWのメーターと同じ）。閾値判定（クリップ警告0.9等）はリニアのままでよい。なお `juce::Decibels` は juce_audio_basics 所属なので、juce_gui_basics しか include しないヘッダでは `std::log10` で直接計算する。

### PopupMenuは「全画面×画面端」に隣接表示できない

PopupMenuの表示位置はOSの「使用可能画面領域」（Dock除け）にクランプされる。全画面表示ではウィンドウがDock領域まで覆うのに、画面最下部のボタン直上には出せない: デスクトップウィンドウ方式（既定）はDock上端まで押し上げられて余白ができ、`withParentComponent` でもターゲット矩形が使用可能領域との交差で空になり左上に飛ぶ。画面端に置いたボタンのメニューは、ウィンドウ内に自前描画するオーバーレイで作る（`ui/AddTrackOverlay.h` が実例。位置計算にOSの画面情報を使わないので全画面/通常で挙動が一致する）。なおPopupMenuはアプリ非アクティブになると即閉じるため、AXPressで開いて背面スクショで確認する検証もできない（自前オーバーレイなら可能）。

- **メニュー項目のショートカット表記は `PopupMenu::Item::shortcutKeyDescription`**: setterのない公開フィールドに直接代入して `addItem (item)`。ApplicationCommandManager無しで表示できる（描画はLookAndFeel任せ）。`setShortcutKeyDescription` という setter は存在しない（8.0.9で確認）

### `JUCE_APPLICATION_NAME_STRING` は JuceHeader 生成時のみ定義される

`juce_add_gui_app` の `PRODUCT_NAME` を渡しただけでは定義されず、`juce_generate_juce_header` を使うプロジェクトでのみ使えるマクロ。本プロジェクトはJuceHeaderを使わないため、アプリ名をコードから参照するときは `target_compile_definitions` の自前定義（`DAW_APP_NAME`）を経由する。

### コールバック実行中のComponentを自己破棄しない（callAsync＋SafePointerで遅延）

ボタン・リスト・`keyPressed` のコールバック内で `setContentOwned` 等により自コンポーネントを差し替えると、実行中オブジェクトの自己破棄になる（スタック上のメソッドの持ち主が消える）。画面遷移は `MessageManager::callAsync` でコールスタックを抜けてから実行する。ラムダには `Component::SafePointer` を捕捉し、実行時に生存確認する（キュー済み遷移や `NativeMessageBox::showAsync` のコールバックが shutdown 後に走る競合も同時に防げる）。連打対策の再入ガードフラグ（開始で立て、キャンセル・失敗・遷移完了で戻す）もセットで入れる。

### AudioAppComponent はアプリ内1個が前提

ヘッダに「An application should only create one global instance of this object and multiple classes should not inherit from this」と明記されている（`juce_AudioAppComponent.h`）。実体は「`AudioDeviceManager`＋`AudioSourcePlayer`」の組。複数の再生系を並べたくなったら、継承をやめてアプリ所有の `AudioDeviceManager` 1個に各自の `AudioSourcePlayer` を `addAudioCallback` する（複数コールバックの出力は合算・入力は全コールバックに配られる）。デバイスSRはグローバル1個なので、SR変更の主導権をどこが持つかの設計も必要になる。

### PopupMenuの角丸カスタム化は背景色を透明にする

LookAndFeelで `drawPopupMenuBackground` を角丸にしても、`PopupMenu::backgroundColourId` が不透明のままだとメニューウィンドウ自体がopaqueになり角の外が黒く残る。`juce_PopupMenu.cpp` のMenuWindowがこの色の `isOpaque()` で `setOpaque` を決めているため、`setColour (backgroundColourId, transparentBlack)` にして全描画を自前で行う。もう1つの罠: `getIdealPopupMenuItemSize` には**ショートカット表記文字列が渡ってこない**（本文textのみ）。ショートカット付き項目の幅は右余白の決め打ちで吸収するしかない（AppLookAndFeel.hでは+44px）。

### 起動直後のgrabKeyboardFocusはisShowing()が偽で空振りする

MainWindowのコンストラクタは `setContentOwned`（→ `parentHierarchyChanged` 発火）→ `setVisible(true)` の順で走るため、初回表示時のフックで `isShowing()` を条件にしたフォーカス取得は実行されない。`MessageManager::callAsync`＋SafePointerでコールスタックを抜けてから再試行する（画面遷移で戻ってきた場合はウィンドウ表示済みなので即時パスが通る）。

### ListBoxのカスタムUI化で先に知っておく3つの制約

① `paintListBoxItem` にはhover状態が渡ってこない → ListBoxに `addMouseListener (this, true)` してmouseMoveで `getRowContainingPosition` → 行indexを自前保持し `repaintRow` で更新（ProjectChooserComponent参照）② 行高は `setRowHeight` の全行共通のみ。可変高の行（ヒーローカード等）はListBoxの外に別Componentとして置く ③ ListBoxがフォーカスを持つと↑↓Returnは素のListBox処理に奪われる。リスト外の要素を含む選択遷移を作るなら `listBox.setWantsKeyboardFocus (false)` にして親の `keyPressed` で処理する。

### ネイティブメニューバー（setMacMainMenu）の3つの罠

① **keyEquivalent（⌘S等の表記）は `ApplicationCommandManager` 経由でのみ設定される**。`PopupMenu::Item::shortcutKeyDescription` はNSMenu変換で無視される（`juce_MainMenu_mac.mm` の `addMenuItem` 参照）。コマンド登録＋`commandManager.getKeyMappings()->addKeyPress()` が必須 ② **NSMenuのkeyEquivalentは `keyPressed` より先にイベントを取る**。メニューに載せたキーの実処理は `ApplicationCommandTarget::perform` に一本化する（keyPressed側の同判定はデッドパス化するがフォールバックとして無害）③ **メニューのenabled状態はNSMenu構築時のスナップショット**で古くなる（disabled表示のままでもキー押下は素通りし得る）。`perform` 側でも必ず状態ガードし、enable条件が変わったら `MenuBarModel::getMacMainMenu()->menuItemsChanged()` で組み直す（Main.cpp / MainComponent::refreshMacMenu 参照）。

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

### パターン3: メモリ確保のタイミングを分離する

- **`prepareToPlay()` で全確保、`releaseResources()` で解放**。この2つはコールバックが走っていない状態で呼ばれることが保証されている
- コールバック内で必要になるバッファ・FIFO・一時領域は、想定される最大ブロックサイズ（`prepareToPlay`の引数`samplesPerBlockExpected`）を基準に確保しておく
- 「再生開始ボタンでバッファを作る」のような遅延確保はしない。UIイベント→確保→atomicポインタ差し替え、という形にする場合も、**解放は必ずメッセージスレッド側**で行う（オーディオスレッドが触っている可能性のあるメモリを即deleteしない）

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
