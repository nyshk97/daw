#include "PlayerEngine.h"

#include <cmath>
#include <cstring>

PlayerEngine::PlayerEngine()
    : juce::Thread ("salva readahead")
{
    formatManager.registerBasicFormats();
    deviceManager.addChangeListener (this);
    startThread();
}

PlayerEngine::~PlayerEngine()
{
    stopThread (2000);
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (&playbackCallback);
    deviceManager.closeAudioDevice();
}

void PlayerEngine::initialiseDevice (const juce::String& preferredOutputDevice)
{
    deviceManager.initialise (0, 2, nullptr, true, preferredOutputDevice);
    deviceManager.addAudioCallback (&playbackCallback);
    updateSampleRates();
}

juce::StringArray PlayerEngine::outputDeviceNames() const
{
    if (auto* type = deviceManager.getCurrentDeviceTypeObject())
    {
        auto* mutableType = const_cast<juce::AudioIODeviceType*> (type);
        mutableType->scanForDevices();
        return mutableType->getDeviceNames (false);
    }
    return {};
}

juce::String PlayerEngine::currentOutputDeviceName() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getName();
    return {};
}

juce::String PlayerEngine::setOutputDevice (const juce::String& name)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.outputDeviceName = name;
    setup.useDefaultOutputChannels = true;
    const auto error = deviceManager.setAudioDeviceSetup (setup, true);
    updateSampleRates();
    return error;
}

bool PlayerEngine::openFile (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false;

    stop();
    // 新しいファイルには旧ステムは付かない（M/S行の点灯はUI側がキャッシュ検出で決める）
    stemMode.store (false);
    retireCurrentStemSet();

    info.file = file;
    info.sampleRate = reader->sampleRate;
    info.lengthSamples = reader->lengthInSamples;
    info.numChannels = (int) reader->numChannels;

    {
        const TransitionScope transition (*this);
        const juce::ScopedLock sl (readerHandoffLock);
        pendingLengthSamples = reader->lengthInSamples;
        pendingReader = std::move (reader);
        updateSampleRates();
    }
    notify(); // read-aheadスレッドが受け取り、stream.prepare()（世代++）を行う
    return true;
}

void PlayerEngine::closeFile()
{
    if (! hasFile())
        return;
    stop();
    if (stemMode.load())
        clearStems();
    updateLoop (false, 0, 0);
    info = {};
    // activeReaderと本流streamは読み上げスレッド所有なのでここでは触らない。
    // hasFile()==false になった時点でUIからの play/seek/書き出しは全て塞がるため、
    // 残っていても鳴らない（次のopenFileの世代付きハンドオフで置き換わる）
}

void PlayerEngine::play (juce::int64 fromSample, bool loop, juce::int64 loopStart, juce::int64 loopEnd)
{
    if (! hasFile())
        return;
    const auto from = juce::jlimit ((juce::int64) 0, info.lengthSamples, fromSample);
    {
        const TransitionScope transition (*this);
        forEachActiveStream ([&] (ReadAheadStream& s)
        {
            s.setLoop (loopStart, loopEnd, loop);
            s.requestSeek (from);
            s.clearEndFlag();
        });
    }
    playing.store (true);
    notify();
}

void PlayerEngine::seek (juce::int64 positionSample)
{
    if (! hasFile())
        return;
    const auto pos = juce::jlimit ((juce::int64) 0, info.lengthSamples, positionSample);
    {
        const TransitionScope transition (*this);
        forEachActiveStream ([&] (ReadAheadStream& s) { s.requestSeek (pos); });
    }
    notify();
}

void PlayerEngine::updateLoop (bool loop, juce::int64 loopStart, juce::int64 loopEnd)
{
    const bool isPlaying = playing.load();
    {
        // setLoopの列も同じscopeで囲む（ストリームごとに新旧ループ範囲が混在したブロックを
        // ガードに検出させる。scope外だとversion不変・active=0に見えて無音化もされない）
        const TransitionScope transition (*this);
        forEachActiveStream ([&] (ReadAheadStream& s) { s.setLoop (loopStart, loopEnd, loop); });
        if (isPlaying)
        {
            // ライターの先読みは旧範囲のままなので必ず再同期する。
            // 現在位置が新範囲内ならそこから継続、外れていれば範囲先頭から
            const auto pos = uiPlayheadSample();
            const auto target = loop && (pos < loopStart || pos >= loopEnd) ? loopStart : pos;
            forEachActiveStream ([&] (ReadAheadStream& s) { s.requestSeek (target); });
        }
    }
    if (isPlaying)
        notify();
}

ReadAheadStream* PlayerEngine::masterStream() const
{
    if (stemMode.load())
        if (auto* set = audioStemSet.load(); set != nullptr && ! set->voices.empty())
            return set->voices.front()->stream.get();
    return const_cast<ReadAheadStream*> (&stream);
}

juce::int64 PlayerEngine::uiPlayheadSample() const
{
    return masterStream()->uiPosition();
}

juce::uint64 PlayerEngine::starvedSamples() const
{
    juce::uint64 total = stream.starvedSamples();
    if (auto* set = audioStemSet.load())
        for (auto& v : set->voices)
            total += v->stream->starvedSamples();
    return total;
}

bool PlayerEngine::consumeReachedEnd()
{
    auto* master = masterStream();
    if (! master->reachedEnd())
        return false;
    forEachActiveStream ([] (ReadAheadStream& s) { s.clearEndFlag(); });
    return true;
}

bool PlayerEngine::setStemVoices (const std::vector<std::pair<juce::String, juce::File>>& stems)
{
    auto set = std::make_unique<StemSet>();
    const auto pos = uiPlayheadSample();
    for (const auto& [name, file] : stems)
    {
        auto voice = std::make_unique<StemVoice>();
        voice->name = name;
        voice->reader.reset (formatManager.createReaderFor (file));
        if (voice->reader == nullptr)
            return false; // 1本でも開けなければモードを変えない
        voice->stream = std::make_unique<ReadAheadStream>();
        // 公開前にメッセージスレッドで初期化を済ませる（この時点では並行アクセスなし）
        voice->stream->prepare (info.lengthSamples); // 契約: ステムは元音源と同SR・同長
        voice->stream->requestSeek (pos);
        set->voices.push_back (std::move (voice));
    }

    {
        const TransitionScope transition (*this);
        retireCurrentStemSet();
        currentStemSet = std::move (set);
        audioStemSet.store (currentStemSet.get());
        stemMode.store (true);
        // ループ状態を引き継いで再同期（seek内のスコープはネストとして数えられる）
        seek (pos);
    }
    notify();
    return true;
}

void PlayerEngine::clearStems()
{
    // 位置の取得はモードを切り替える**前**に（切り替え後に読むと、止まったままの
    // 本流streamの古い位置を拾って再生位置が巻き戻る）
    const auto pos = uiPlayheadSample();
    {
        const TransitionScope transition (*this);
        stemMode.store (false);
        retireCurrentStemSet();
        stream.requestSeek (pos); // 本流をステムの現在位置へ再同期
    }
    notify();
}

void PlayerEngine::retireCurrentStemSet()
{
    if (currentStemSet == nullptr)
        return;
    audioStemSet.store (nullptr);
    retiredSets.push_back ({ std::move (currentStemSet), callbackCounter.load(), writerEpoch.load() });
}

int PlayerEngine::numStems() const
{
    if (auto* set = audioStemSet.load())
        return (int) set->voices.size();
    return 0;
}

void PlayerEngine::setStemGain (int index, float gain)
{
    if (auto* set = audioStemSet.load())
        if (index >= 0 && index < (int) set->voices.size())
            set->voices[(size_t) index]->gain.store (gain);
}

float PlayerEngine::stemPeak (int index) const
{
    if (auto* set = audioStemSet.load())
        if (index >= 0 && index < (int) set->voices.size())
            return set->voices[(size_t) index]->peak.load();
    return 0.0f;
}

void PlayerEngine::drainRetired (int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (! retiredSets.empty() && juce::Time::getMillisecondCounter() < deadline)
    {
        purgeRetired();
        if (! retiredSets.empty())
            juce::Thread::sleep (10);
    }
    // タイムアウトしても続行してよい（削除対象ファイルは開いたままでもPOSIX的に消せる。
    // readerは残ったデータを読み終えたら捨てられる）
    purgeRetired();
}

void PlayerEngine::purgeRetired()
{
    for (int i = (int) retiredSets.size(); --i >= 0;)
    {
        const auto& r = retiredSets[(size_t) i];
        // audio・writer両方が引退後に2周以上進んでいれば、旧ポインタは誰も持っていない
        if (callbackCounter.load() > r.callbackAtRetire + 1 && writerEpoch.load() > r.writerAtRetire + 1)
            retiredSets.erase (retiredSets.begin() + i);
    }
}

void PlayerEngine::run()
{
    while (! threadShouldExit())
    {
        bool adopted = false;
        {
            const juce::ScopedLock sl (readerHandoffLock);
            if (pendingReader != nullptr)
            {
                activeReader = std::move (pendingReader);
                adopted = true;
            }
        }
        if (adopted)
        {
            const TransitionScope transition (*this);
            stream.prepare (activeReader->lengthInSamples); // 世代++はリーダー受け取りと同じスレッドで
        }

        writerEpoch.fetch_add (1); // 引退セットの安全な破棄判定用（ループ境界でポインタを取り直す）

        // fillに使ってよい世代の上限を「pendingの新readerが無い」ことと同じロック下で確定する。
        // openFile()はpendingReaderをこのロック下で置くため、ロック内でpending無しなら
        // その時点までの全seek世代は現行readerのファイルに属する。これを超える世代
        // （openFile直後のplay等）はfillOnceが応じない＝旧readerのデータを新世代として
        // 公開しない（切替前のファイルが最大1ブロック鳴る競合の対策）
        bool canFill = false;
        juce::uint32 maxGeneration = 0;
        {
            const juce::ScopedLock sl (readerHandoffLock);
            if (pendingReader == nullptr)
            {
                canFill = true;
                maxGeneration = stream.latestGeneration();
            }
        }

        int written = (canFill && activeReader != nullptr) ? stream.fillOnce (*activeReader, maxGeneration) : 0;
        // ステムはreaderとstreamがセットで不変（セット差し替えは丸ごとatomic公開）なので
        // ソース差し替えの競合はなく、上限は常に最新でよい
        if (auto* set = audioStemSet.load())
            for (auto& v : set->voices)
                written += v->stream->fillOnce (*v->reader);

        if (written == 0)
            wait (5); // 満杯 or 終端 or 未オープン or 世代待ち。シーク・オープンの notify でも起きる
    }
}

juce::String PlayerEngine::enterRecordMode (const juce::String& inputDeviceName, int inputChannelPairStart)
{
    if (recordMode)
        return {};
    stop();
    deviceManager.removeAudioCallback (&playbackCallback);
    deviceManager.closeAudioDevice();
    recordMode = true;
    const auto error = setInputDevice (inputDeviceName, inputChannelPairStart);
    deviceManager.addAudioCallback (&recorderCallback);
    return error;
}

void PlayerEngine::exitRecordMode (const juce::String& outputDeviceName)
{
    if (! recordMode)
        return;
    recorder.stop();
    deviceManager.removeAudioCallback (&recorderCallback);
    deviceManager.closeAudioDevice();
    recordMode = false;
    deviceManager.initialise (0, 2, nullptr, true, outputDeviceName);
    deviceManager.addAudioCallback (&playbackCallback);
    updateSampleRates();
}

juce::String PlayerEngine::setInputDevice (const juce::String& name, int inputChannelPairStart)
{
    if (! recordMode)
        return {};
    // 入力専用で開き直し、指定のステレオペアだけを有効化する
    deviceManager.initialise (2, 0, nullptr, true, name);
    // 指定ペアがデバイスのch数を超えていたら収まる位置へクランプする
    // （既定値はmk5想定の3-4ch。内蔵マイク等1〜2chのデバイスでは0chオープンになってしまう）
    const int channels = currentInputChannelCount();
    const int start = juce::jlimit (0, juce::jmax (0, channels - 2), inputChannelPairStart);
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    setup.inputChannels.setBit (start);
    if (start + 1 < channels)
        setup.inputChannels.setBit (start + 1);
    return deviceManager.setAudioDeviceSetup (setup, true);
}

juce::StringArray PlayerEngine::inputDeviceNames() const
{
    if (auto* type = deviceManager.getCurrentDeviceTypeObject())
    {
        auto* mutableType = const_cast<juce::AudioIODeviceType*> (type);
        mutableType->scanForDevices();
        return mutableType->getDeviceNames (true);
    }
    return {};
}

juce::String PlayerEngine::currentInputDeviceName() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getName();
    return {};
}

int PlayerEngine::currentInputChannelCount() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getInputChannelNames().size();
    return 0;
}

double PlayerEngine::currentDeviceSampleRate() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentSampleRate();
    return 0.0;
}

void PlayerEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateSampleRates();
}

void PlayerEngine::updateSampleRates()
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        deviceSampleRateAtomic.store (device->getCurrentSampleRate());
    sourceSampleRateAtomic.store (info.sampleRate);
}

void PlayerEngine::PlaybackCallback::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // ここはコールバック開始前に呼ばれる＝確保してよい（prepareToPlay相当）。
    // 比率上限まで見込んだバッファを確保し、以後コールバック内では確保しない
    const int deviceBlock = juce::jmax (256, device->getCurrentBufferSizeSamples());
    resampleStage.prepare (deviceBlock * 4, maxRatio); // ブロックサイズ変動の余裕込み
    engine.deviceSampleRateAtomic.store (device->getCurrentSampleRate());
}

void PlayerEngine::PlaybackCallback::readMix (float* left, float* right, int n)
{
    auto* set = engine.stemMode.load() ? engine.audioStemSet.load() : nullptr;
    if (set == nullptr)
    {
        engine.stream.readAudio (left, right, n);
        return;
    }

    // ステム構成: 各ストリームを読み、M/S結果のゲイン（0/1）で加算ミックスする。
    // ミュート中も読む＝位置を進める（M解除で他ステムとずれないための契約）
    juce::FloatVectorOperations::clear (left, n);
    if (right != left)
        juce::FloatVectorOperations::clear (right, n);
    auto& temp = engine.stemMixTemp;
    for (auto& v : set->voices)
    {
        const float gain = v->gain.load();
        float voicePeak = 0.0f;
        for (int offset = 0; offset < n; offset += temp.getNumSamples())
        {
            const int m = juce::jmin (temp.getNumSamples(), n - offset);
            float* tl = temp.getWritePointer (0);
            float* tr = temp.getWritePointer (1);
            v->stream->readAudio (tl, tr, m);
            if (gain > 0.0f)
            {
                juce::FloatVectorOperations::addWithMultiply (left + offset, tl, gain, m);
                juce::FloatVectorOperations::addWithMultiply (right + offset, tr, gain, m);
                const auto lr = juce::FloatVectorOperations::findMinAndMax (tl, m);
                voicePeak = juce::jmax (voicePeak,
                                        gain * juce::jmax (std::abs (lr.getStart()), std::abs (lr.getEnd())));
            }
        }
        v->peak.store (voicePeak);
    }
}

void PlayerEngine::PlaybackCallback::audioDeviceIOCallbackWithContext (
    const float* const*, int, float* const* outputChannelData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    engine.callbackCounter.fetch_add (1);

    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    if (numOutputChannels < 1 || outputChannelData[0] == nullptr)
        return;

    // 状態遷移（シーク・ファイル変更・構成切替）はepochで囲まれている。
    // 前ブロック以降に完了した遷移があれば、処理**前**にリサンプラー状態を破棄する
    // （残すと旧位置の数サンプルが混ざる）。version→activeの順に読む（Guardの契約）
    const auto versionBefore = engine.transitionVersion.load (std::memory_order_acquire);
    const auto activeBefore = engine.transitionsActive.load (std::memory_order_acquire);
    if (guard.preBlock (versionBefore, activeBefore))
        resampleStage.reset();

    if (! engine.playing.load())
    {
        engine.stream.discardStale();
        if (auto* set = engine.audioStemSet.load())
            for (auto& v : set->voices)
                v->stream->discardStale();
        return;
    }

    float* left = outputChannelData[0];
    float* right = (numOutputChannels > 1 && outputChannelData[1] != nullptr) ? outputChannelData[1] : left;

    const double sourceRate = engine.sourceSampleRateAtomic.load();
    const double deviceRate = engine.deviceSampleRateAtomic.load();
    const double ratio = (sourceRate > 0.0 && deviceRate > 0.0) ? sourceRate / deviceRate : 1.0;

    if (! resampleStage.canHandle (ratio))
        return; // 想定外の比率（384kHz超相当）は無音。事前確保の範囲を守る

    resampleStage.process (ratio,
                           [this] (float* l, float* r, int n) { readMix (l, r, n); },
                           left, right, numSamples);

    // 遷移が処理と重なったブロック＝一部のストリームだけ新位置の音を混ぜた可能性がある。
    // SRに関係なく無音化し、状態を破棄する（次のブロックから全ストリーム一貫の音が出る）
    if (guard.postBlock (engine.transitionVersion.load (std::memory_order_acquire)))
    {
        resampleStage.reset();
        juce::FloatVectorOperations::clear (left, numSamples);
        if (right != left)
            juce::FloatVectorOperations::clear (right, numSamples);
    }

    // 実際にデバイスへ渡すバッファのピークを記録（「JUCE側は音を書けているか」の診断）
    {
        const auto mm = juce::FloatVectorOperations::findMinAndMax (left, numSamples);
        const float p = juce::jmax (std::abs (mm.getStart()), std::abs (mm.getEnd()));
        if (p > engine.outputPeakAtomic.load (std::memory_order_relaxed))
            engine.outputPeakAtomic.store (p, std::memory_order_relaxed);
    }
}
