#pragma once

#include <atomic>

#include <juce_audio_utils/juce_audio_utils.h>

#include "audio/SalvaRecorder.h"
#include "shared/DiscontinuityGuard.h"
#include "shared/ReadAheadStream.h"
#include "shared/ResampleStage.h"

// Salvaの再生エンジン（出力のみ。録音モードはPhase 4で排他切り替え）。
//
// 構成: ReadAheadStream（ディスク先読み・オーディオ側ロックなし）
//       → PlaybackCallback（自前のAudioIODeviceCallback: ミックス＋リサンプル）
//       → AudioDeviceManager
//
// AudioSourcePlayer / ResamplingAudioSource を使わない理由（JUCE 8.0.9 実装を読んで確認）:
//   - AudioSourcePlayer はコールバック内で CriticalSection を取る（juce_AudioSourcePlayer.cpp:81）
//   - ResamplingAudioSource はコールバック内で CriticalSection＋SpinLock を取り、
//     さらに比率が上がると buffer.setSize() の再確保に到達する
//     （juce_ResamplingAudioSource.cpp:92/:109。96kHz音源→48kHzデバイス等）
//   どちらも GOTCHAS.md の禁止事項（優先度逆転・コールバック内確保）に当たる。
//   代わりに LagrangeInterpolator（ロック・確保なし）＋ audioDeviceAboutToStart で
//   事前確保したバッファでリサンプルする。
//
// スレッド:
//   - メッセージスレッド: openFile / play / stop / seek / デバイス切り替え
//   - read-aheadスレッド（このクラスが所有する juce::Thread）: ディスク読み
//   - オーディオスレッド: ReadAheadStream::readAudio＋補間のみ（ロック・確保なし）
//
// AudioFormatReader の受け渡し: メッセージスレッドが pendingReader に置き、
// read-aheadスレッドが自分のループ先頭で受け取ってから stream.prepare()（世代++）を行う。
// 世代の切り替えを受け取りと同じスレッドで行うことで「新世代なのに旧ファイルを読む」窓を塞ぐ
class PlayerEngine : private juce::Thread,
                     private juce::ChangeListener
{
public:
    struct FileInfo
    {
        juce::File file;
        double sampleRate = 0.0;
        juce::int64 lengthSamples = 0;
        int numChannels = 0;
    };

    PlayerEngine();
    ~PlayerEngine() override;

    // デバイスを開く（出力のみ）。preferredOutputDevice が見つからなければ既定デバイス
    void initialiseDevice (const juce::String& preferredOutputDevice);

    juce::StringArray outputDeviceNames() const;
    juce::String currentOutputDeviceName() const;
    juce::String setOutputDevice (const juce::String& name); // 空文字=成功、それ以外はエラーメッセージ

    bool openFile (const juce::File& file); // 成功時 fileInfo を更新し位置0へ
    void closeFile(); // 停止＋ステム解除＋hasFile()=falseへ（スタート画面へ戻る用）
    bool hasFile() const { return info.lengthSamples > 0; }
    const FileInfo& fileInfo() const { return info; }

    void play (juce::int64 fromSample, bool loop, juce::int64 loopStart, juce::int64 loopEnd);
    void stop() { playing.store (false); }
    bool isPlaying() const { return playing.load(); }
    void seek (juce::int64 positionSample);
    // 再生中の選択変更: ループ範囲を差し替え、現在位置が範囲外なら先頭へ・範囲内なら現在位置で再同期
    void updateLoop (bool loop, juce::int64 loopStart, juce::int64 loopEnd);

    juce::int64 uiPlayheadSample() const;  // ステムモード時は先頭ステムが位置マスター
    juce::uint64 starvedSamples() const;   // 全アクティブストリームの合計
    // 非ループ再生の終端到達を1回だけ拾う（UIのTimerから呼び、trueなら停止処理する）
    bool consumeReachedEnd();

    // --- ステム同時再生（Phase 5） ---
    // グループのステムWAV群へ切り替える（name,fileの列）。位置・ループは現状を引き継ぐ。
    // 全ファイルが開けなければfalse（モードは変わらない）
    bool setStemVoices (const std::vector<std::pair<juce::String, juce::File>>& stems);
    void clearStems(); // オリジナル再生へ戻す（セットは引退リストへ。破棄はpurgeRetired）
    bool isStemMode() const { return stemMode.load(); }
    int numStems() const;
    void setStemGain (int index, float gain); // 0/1（M/S競合の計算はUI側=StemMix）
    float stemPeak (int index) const;         // 直近コールバックのポストゲインピーク（メーター用）
    // 引退したステムセットの破棄（UIのTimerから呼ぶ。オーディオ・ライター両スレッドが
    // 旧ポインタを手放したことをカウンタで確認してからdeleteする）
    void purgeRetired();
    // 引退セットが空になるまで待つ（削除前のreader解放待ち。メッセージスレッド専用・短時間ブロック）
    void drainRetired (int timeoutMs);

    // --- 録音モード（再生と排他。デバイスを入力専用で開き直す） ---
    // 空文字=成功。inputChannelPairStart は0-basedのステレオペア先頭ch
    juce::String enterRecordMode (const juce::String& inputDeviceName, int inputChannelPairStart);
    void exitRecordMode (const juce::String& outputDeviceName); // 出力専用で開き直して再生系へ戻す
    bool isRecordMode() const { return recordMode; }
    juce::String setInputDevice (const juce::String& name, int inputChannelPairStart); // 録音モード中のみ
    juce::StringArray inputDeviceNames() const;
    juce::String currentInputDeviceName() const;
    int currentInputChannelCount() const; // 現在の入力デバイスの総入力ch数（ペア候補の構築用）
    double currentDeviceSampleRate() const;
    SalvaRecorder& getRecorder() { return recorder; }

private:
    void run() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // デバイス変更→SR atomicの更新
    void updateSampleRates();

    // 再生コールバック: ミックス（オリジナル/ステムM/S）→ デバイスSRへのリサンプル。
    // ロック・アロケーションなし（バッファは audioDeviceAboutToStart で事前確保）
    class PlaybackCallback : public juce::AudioIODeviceCallback
    {
    public:
        explicit PlaybackCallback (PlayerEngine& e) : engine (e) {}
        void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
        void audioDeviceStopped() override {}
        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                               float* const* outputChannelData, int numOutputChannels,
                                               int numSamples, const juce::AudioIODeviceCallbackContext&) override;

    private:
        // ソースSR基準のnサンプルをミックスして書く（オリジナル or ステム加算）
        void readMix (float* left, float* right, int numSamples);

        PlayerEngine& engine;
        static constexpr double maxRatio = 8.0; // 384kHz音源→48kHzデバイスまで（超える比率は無音）
        ResampleStage resampleStage;            // 帯域制限＋補間＋キャリー（shared/ResampleStage.h）
        DiscontinuityGuard guard;               // 通知/適用の競合検出（shared/DiscontinuityGuard.h）
    };

    // 録音モードのコールバック: 入力をレコーダーへ渡し、出力は無音（モニタリングはMPC側の領分）
    class RecorderCallback : public juce::AudioIODeviceCallback
    {
    public:
        explicit RecorderCallback (SalvaRecorder& r) : recorder (r) {}
        void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
        void audioDeviceStopped() override {}
        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                               float* const* outputChannelData, int numOutputChannels,
                                               int numSamples, const juce::AudioIODeviceCallbackContext&) override
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
            recorder.pushBlock (inputChannelData, numInputChannels, numSamples);
        }

    private:
        SalvaRecorder& recorder;
    };

    // ステム1本ぶん（reader=ライタースレッド専用・stream=SPSC・gain/peak=atomic）
    struct StemVoice
    {
        juce::String name;
        std::unique_ptr<juce::AudioFormatReader> reader;
        std::unique_ptr<ReadAheadStream> stream;
        std::atomic<float> gain { 1.0f };
        std::atomic<float> peak { 0.0f };
    };

    struct StemSet
    {
        std::vector<std::unique_ptr<StemVoice>> voices;
    };

    // 現在の再生対象ストリーム群にfnを適用（ステムモードなら全ステム、でなければ本流）
    template <typename Fn>
    void forEachActiveStream (Fn&& fn)
    {
        if (stemMode.load())
        {
            if (auto* set = audioStemSet.load())
                for (auto& v : set->voices)
                    fn (*v->stream);
            return;
        }
        fn (stream);
    }
    ReadAheadStream* masterStream() const;
    void retireCurrentStemSet();

    juce::AudioFormatManager formatManager;
    juce::AudioDeviceManager deviceManager;
    PlaybackCallback playbackCallback { *this };
    SalvaRecorder recorder;
    RecorderCallback recorderCallback { recorder };
    bool recordMode = false;

    // リサンプル比の材料（コールバックが毎ブロック読む。比率そのものは計算で出す）
    std::atomic<double> sourceSampleRateAtomic { 0.0 };
    std::atomic<double> deviceSampleRateAtomic { 0.0 };

    // 状態遷移（複数ストリームへのrequestSeek列）をepochで囲む（DiscontinuityGuard.h参照）。
    // 遷移とコールバックが重なったブロックは、一部ストリームだけ新位置の音を混ぜ得るため
    // ガードが検出してSRに関係なく無音化する。ネスト可（activeはカウント）
    std::atomic<juce::uint32> transitionVersion { 0 };
    std::atomic<int> transitionsActive { 0 };
    struct TransitionScope
    {
        explicit TransitionScope (PlayerEngine& e) : engine (e)
        {
            // active++ → version++ の順（versionのacquireがactive++の可視性を保証する契約）
            engine.transitionsActive.fetch_add (1, std::memory_order_release);
            engine.transitionVersion.fetch_add (1, std::memory_order_release);
        }
        ~TransitionScope()
        {
            engine.transitionsActive.fetch_sub (1, std::memory_order_release);
        }
        PlayerEngine& engine;
    };

    // ステムセットの所有と公開。破棄はカウンタ確認後（audio/writerが旧ポインタを手放してから）
    std::unique_ptr<StemSet> currentStemSet;                 // メッセージスレッド所有
    std::atomic<StemSet*> audioStemSet { nullptr };          // audio/writerが読む
    std::atomic<bool> stemMode { false };
    std::atomic<juce::uint64> callbackCounter { 0 };         // audioコールバックごと++
    std::atomic<juce::uint64> writerEpoch { 0 };             // ライターループごと++
    struct RetiredSet
    {
        std::unique_ptr<StemSet> set;
        juce::uint64 callbackAtRetire = 0;
        juce::uint64 writerAtRetire = 0;
    };
    std::vector<RetiredSet> retiredSets;                     // メッセージスレッド専用
    juce::AudioBuffer<float> stemMixTemp { 2, 4096 };        // audioコールバック専用

    ReadAheadStream stream;
    std::atomic<bool> playing { false };

    // メッセージスレッド ⇄ read-aheadスレッドの受け渡し（オーディオスレッドは触らない）
    juce::CriticalSection readerHandoffLock;
    std::unique_ptr<juce::AudioFormatReader> pendingReader;
    juce::int64 pendingLengthSamples = 0;
    std::unique_ptr<juce::AudioFormatReader> activeReader; // read-aheadスレッド専用

    FileInfo info; // メッセージスレッド専用

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayerEngine)
};
