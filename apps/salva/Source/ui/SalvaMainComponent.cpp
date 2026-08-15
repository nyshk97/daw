#include "SalvaMainComponent.h"

#include <unistd.h> // getpid（キャッシュ削除時のlock owner記録）

#include "shared/AudioFileTypes.h"
#include "shared/BpmMath.h"
#include "shared/Log.h"
#include "shared/RecordingCheck.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

// パレットはアプリアイコン（make_icon.swift）基準: 盤面のチャコール地＋スリーブのクリーム文字
// ＋レーベルwarmグラデ由来のオレンジをアクセントに使う
const juce::Colour windowBg { 0xff201f24 };
const juce::Colour headerBg { 0xff242328 };
const juce::Colour panelBorder { 0xff34323a };
const juce::Colour emptyBg { 0xff17161a };
const juce::Colour textColour { 0xfff2e8d5 };
const juce::Colour textDim { 0xff97908a };
const juce::Colour accent { 0xffff7a2e }; // アイコンのレーベル中間色
const juce::Colour accentTextDark { 0xff2b1507 }; // オレンジ地の上の文字
const juce::Colour buttonBg { 0xff2e2d34 };
const juce::Colour buttonBorder { 0xff46444e };
const juce::Colour hoverFill { 0xff3a3842 };
const juce::Colour recordTextColour { 0xffd98a76 };
// 盤面はアプリアイコン（Assets/make_icon.swift）と同じデザイン・同じ比率で描く
const juce::Colour discBase { 0xff232228 };
const juce::Colour discGroove { 0xff3b3a44 };
const juce::Colour discHole { 0xfff2e5c6 };
const juce::Colour discBracket { 0xfff6ecd4 };
const juce::Colour labelTop { 0xffffd23f };
const juce::Colour labelMid { 0xffff7a2e };
const juce::Colour labelBottom { 0xffe02fb0 };
const juce::Colour warnColour { 0xffff4500 };

constexpr int headerHeight = 40;
constexpr int transportHeight = 44;
constexpr int bottomHeight = 40;
constexpr int recentRowHeight = 40;

juce::String formatTime (double seconds)
{
    const int m = (int) (seconds / 60.0);
    const double s = seconds - m * 60.0;
    return juce::String (m) + ":" + juce::String (s, 1).paddedLeft ('0', 4);
}

// 最近ファイル行の従属表示用: 親ディレクトリをホーム省略形で（例: ~/Downloads）
juce::String shortDirPath (const juce::File& file)
{
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
    auto dir = file.getParentDirectory().getFullPathName();
    if (dir.startsWith (home))
        dir = "~" + dir.substring (home.length());
    return dir;
}
} // namespace

SalvaMainComponent::SalvaMainComponent()
{
    setLookAndFeel (&salvaLnf); // 配色スキームはSalvaLookAndFeelのコンストラクタで設定済み

    settings = SalvaSettings::load();
    engine.initialiseDevice (settings.outputDeviceName);

    addAndMakeVisible (waveform);
    waveform.onSelectionChanged = [this] (juce::int64 s, juce::int64 e) { selectionChanged (s, e); };
    waveform.onSelectionCleared = [this] { selectionCleared(); };
    waveform.onSeek = [this] (juce::int64 pos)
    {
        engine.seek (pos);
        Log::info ("transport.seek", "sample=" + juce::String (pos));
    };

    addAndMakeVisible (playButton);
    playButton.onClick = [this] { togglePlay(); };

    addAndMakeVisible (timeLabel);
    timeLabel.setJustificationType (juce::Justification::centred);
    timeLabel.setColour (juce::Label::textColourId, textColour);
    timeLabel.setText ("0:00.0", juce::dontSendNotification);

    addAndMakeVisible (barsButton);
    barsButton.getProperties().set ("fontSize", 12.5);
    barsButton.onClick = [this]
    {
        if (! waveform.hasSelection())
            return;
        beatsOverride = BpmMath::nextBeats (currentBeats());
        updateBpmDisplay();
    };

    addAndMakeVisible (bpmLabel);
    bpmLabel.setColour (juce::Label::textColourId, textColour);
    bpmLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));

    addAndMakeVisible (exportNameLabel);
    exportNameLabel.setColour (juce::Label::textColourId, textDim);
    exportNameLabel.setJustificationType (juce::Justification::centredRight);
    exportNameLabel.setFont (juce::FontOptions (11.0f));

    addAndMakeVisible (outputLabel);
    outputLabel.setText (jp (u8"出力:"), juce::dontSendNotification);
    outputLabel.setColour (juce::Label::textColourId, textDim);

    addAndMakeVisible (outputDeviceBox);
    rebuildOutputDeviceBox();
    outputDeviceBox.onChange = [this]
    {
        const auto name = outputDeviceBox.getText();
        if (name.isEmpty() || name == engine.currentOutputDeviceName())
            return;
        const auto error = engine.setOutputDevice (name);
        if (error.isNotEmpty())
        {
            Log::warn ("device.output.error", "name=" + name + " error=" + error);
            rebuildOutputDeviceBox(); // 失敗したら実際のデバイスへ表示を戻す
            return;
        }
        settings.outputDeviceName = engine.currentOutputDeviceName();
        settings.save();
        Log::info ("device.output.set", "name=" + settings.outputDeviceName);
    };

    addAndMakeVisible (recordModeButton);
    recordModeButton.setButtonText (jp (u8"● 録音モード"));
    recordModeButton.getProperties().set ("fontSize", 12.5);
    recordModeButton.setColour (juce::TextButton::textColourOffId, recordTextColour);
    recordModeButton.onClick = [this] { toggleRecordMode(); };

    // 空状態の主要アクション（D&D以外の入口と録音への導線）
    addChildComponent (openFileButton);
    openFileButton.setButtonText (jp (u8"ファイルを選択…"));
    openFileButton.setColour (juce::TextButton::buttonColourId, accent);
    openFileButton.setColour (juce::TextButton::textColourOffId, accentTextDark);
    openFileButton.setColour (juce::ComboBox::outlineColourId, accent); // LnFの枠線を地色に合わせる
    openFileButton.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    openFileButton.onClick = [this] { openFileChooser(); };

    addChildComponent (recordEntryButton);
    recordEntryButton.setButtonText (jp (u8"● 録音モードへ"));
    recordEntryButton.setColour (juce::TextButton::buttonColourId, emptyBg);
    recordEntryButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffb6afa4));
    recordEntryButton.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    recordEntryButton.onClick = [this] { toggleRecordMode(); };

    addChildComponent (recordView);
    recordView.onRecordToggle = [this] { toggleRecording(); };
    recordView.onInputDeviceChanged = [this] (juce::String name)
    {
        if (engine.getRecorder().isRecording())
            return; // 録音中のデバイス変更は不可
        const auto error = engine.setInputDevice (name, settings.inputChannelPairStart);
        if (error.isNotEmpty())
            Log::warn ("device.input.error", "name=" + name + " error=" + error);
        settings.inputDeviceName = engine.currentInputDeviceName();
        settings.save();
        recordView.setInputDevices (engine.inputDeviceNames(), engine.currentInputDeviceName());
        recordView.setChannelPairs (engine.currentInputChannelCount(), settings.inputChannelPairStart);
        Log::info ("device.input.set", "name=" + settings.inputDeviceName);
    };
    recordView.onChannelPairChanged = [this] (int pairStart)
    {
        if (engine.getRecorder().isRecording())
            return;
        settings.inputChannelPairStart = pairStart;
        settings.save();
        engine.setInputDevice (engine.currentInputDeviceName(), pairStart);
        Log::info ("device.input.pair", "start=" + juce::String (pairStart));
    };

    addChildComponent (starvedLabel);
    starvedLabel.setColour (juce::Label::textColourId, warnColour);
    starvedLabel.setFont (juce::FontOptions (11.0f));

    // --- ステム分離（ヘッダー）＋M/Sパネル＋書き出し ---
    addAndMakeVisible (separateButton);
    separateButton.setButtonText (jp (u8"STEM分離"));
    separateButton.getProperties().set ("fontSize", 12.5);
    separateButton.onClick = [this] { startSeparation(); };

    addChildComponent (separateProgressLabel);
    separateProgressLabel.setColour (juce::Label::textColourId, textDim);
    separateProgressLabel.setFont (juce::FontOptions (11.0f));
    separateProgressLabel.setText (jp (u8"分離中…"), juce::dontSendNotification);

    addAndMakeVisible (cacheSizeLabel);
    cacheSizeLabel.setColour (juce::Label::textColourId, textDim);
    cacheSizeLabel.setFont (juce::FontOptions (11.0f));
    cacheSizeLabel.setJustificationType (juce::Justification::centredRight);
    cacheSizeLabel.setInterceptsMouseClicks (true, false);
    cacheSizeLabel.addMouseListener (this, false); // クリックで削除メニュー

    addAndMakeVisible (stemPanel);
    stemPanel.onConfigChanged = [this] { applyStemConfig(); };

    addAndMakeVisible (exportButton);
    exportButton.setButtonText (jp (u8"書き出し"));
    exportButton.getProperties().set ("fontSize", 12.5);
    exportButton.onClick = [this] { startExport(); };
    exportButton.setVisible (false);

    addChildComponent (toastLabel);
    toastLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xf0252429));
    toastLabel.setColour (juce::Label::textColourId, textColour);
    toastLabel.setColour (juce::Label::outlineColourId, buttonBorder);
    toastLabel.setJustificationType (juce::Justification::centred);
    toastLabel.setFont (juce::FontOptions (12.0f));

    // 起動時掃除: 死んだlockの解放＋非参照・ロックなしの中途runの削除
    StemCache::sweep (StemCache::stemsRoot(), (juce::int64) getpid(), StemCache::defaultPidAlive,
                      StemCache::defaultPgidAlive, juce::Time::getCurrentTime());
    updateCacheSizeLabel();

    // スペース=再生/停止を常に効かせる: ボタン類にフォーカスを渡さない
    // （フォーカスされたTextButtonはスペースをクリックとして食ってしまう）
    for (auto* c : { (juce::Component*) &playButton, (juce::Component*) &barsButton,
                     (juce::Component*) &recordModeButton, (juce::Component*) &outputDeviceBox,
                     (juce::Component*) &openFileButton, (juce::Component*) &recordEntryButton })
        c->setWantsKeyboardFocus (false);

    setWantsKeyboardFocus (true);
    setSize (860, 480);
    startTimerHz (30);
    updateHeader();
    updateBpmDisplay();
    updateSeparateButtonState();
}

SalvaMainComponent::~SalvaMainComponent()
{
    setLookAndFeel (nullptr); // メンバのsalvaLnfより先に参照を外す
}

void SalvaMainComponent::openFile (const juce::File& file)
{
    if (! AudioFileTypes::isSupported (file) || ! file.existsAsFile())
        return;
    // 同一ファイルの再オープンは無視する（macOSはargvのファイルパスをopenFileイベントでも
    // 届けるため、起動時に二重オープンになり再生・選択状態がリセットされる）
    if (engine.hasFile() && engine.fileInfo().file == file)
        return;
    if (! engine.openFile (file))
    {
        Log::warn ("file.open.failed", "path=" + file.getFullPathName());
        return;
    }
    const auto& fi = engine.fileInfo();
    waveform.setFile (file, fi.sampleRate, fi.lengthSamples);
    waveform.setVisible (true);
    beatsOverride = 0;
    settings.addRecentFile (file.getFullPathName());
    settings.save();
    refreshStemCacheState(); // ステムキャッシュ自動検出（manifest検証済みのもののみ）
    updateHeader();
    updateBpmDisplay();
    updateSeparateButtonState();
    repaint();
    Log::info ("file.open", "path=" + file.getFullPathName()
                                + " sr=" + juce::String (fi.sampleRate, 0)
                                + " length=" + juce::String (fi.lengthSamples)
                                + " ch=" + juce::String (fi.numChannels));
}

void SalvaMainComponent::togglePlay()
{
    if (! engine.hasFile())
        return;

    if (engine.isPlaying())
    {
        engine.stop();
        Log::info ("transport.stop", "sample=" + juce::String (engine.uiPlayheadSample()));
    }
    else
    {
        if (waveform.hasSelection())
        {
            engine.play (waveform.selectionStart(), true, waveform.selectionStart(), waveform.selectionEnd());
        }
        else
        {
            auto from = engine.uiPlayheadSample();
            if (from >= engine.fileInfo().lengthSamples)
                from = 0; // 終端で止まっていたら先頭から
            engine.play (from, false, 0, 0);
        }
        Log::info ("transport.play",
                   "loop=" + juce::String (waveform.hasSelection() ? 1 : 0)
                       + " from=" + juce::String (engine.uiPlayheadSample()));
    }
    playButton.setButtonText (engine.isPlaying() ? jp (u8"■") : jp (u8"▶"));
}

juce::File SalvaMainComponent::resolveVenvPython() const
{
    const auto venv = settings.venvPathOverride.isNotEmpty()
                          ? juce::File (settings.venvPathOverride)
                          : juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                .getChildFile ("daw/tools/reference/.venv");
    return venv.getChildFile ("bin/python");
}

void SalvaMainComponent::startSeparation()
{
    if (! engine.hasFile() || separator.status() == StemSeparator::Status::running)
        return;

    // venvの解決はアプリ側の責務。見つからなければ構築手順を出す（release版でも機能する診断）
    const auto python = resolveVenvPython();
    if (! python.existsAsFile())
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon, "Salva",
            jp (u8"ステム分離用のPython環境が見つかりません:\n") + python.getFullPathName()
                + jp (u8"\n\n構築手順:\n1. dawリポジトリで `mise run ref:setup` を実行（venvを作成）\n"
                      u8"2. 別の場所に作った場合は設定ファイル\n"
                      u8"   ~/Library/Application Support/salva/settings.json の\n"
                      u8"   venvPathOverride にvenvのパスを指定"));
        return;
    }

    const auto script = juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                            .getChildFile ("Contents/Resources/separate.sh");
    if (! script.existsAsFile())
    {
        showToast (jp (u8"separate.sh がバンドルに見つかりません"));
        return;
    }

    StemSeparator::Request request;
    request.input = engine.fileInfo().file;
    request.identity = StemCache::SourceIdentity::forFile (request.input);
    request.script = script;
    request.python = python;
    // 元音源のSR/長さはアプリ側（CoreAudio）が読んで渡す契約（libsndfileはm4aを読めない）
    request.sampleRate = engine.fileInfo().sampleRate;
    request.lengthSamples = engine.fileInfo().lengthSamples;

    if (! separator.startSeparation (request))
    {
        showToast (jp (u8"別プロセスがこのファイルを分離中です"));
        return;
    }
    separateButton.setEnabled (false);
    separateProgressLabel.setVisible (true);
    Log::info ("separate.start", "path=" + request.input.getFullPathName()
                                     + " identity=" + request.identity.hash());
}

void SalvaMainComponent::refreshStemCacheState()
{
    currentManifest = {};
    if (engine.hasFile())
    {
        const auto identity = StemCache::SourceIdentity::forFile (engine.fileInfo().file);
        const auto dir = StemCache::identityDir (identity);
        const auto manifest = StemCache::parseManifest (dir.getChildFile ("manifest.json"));
        if (StemCache::manifestUsable (manifest, identity, dir))
            currentManifest = manifest;
    }
    stemPanel.setManifest (currentManifest);
    applyStemConfig();
    updateCacheSizeLabel();
    resized();
}

void SalvaMainComponent::applyStemConfig()
{
    const auto* group = stemPanel.currentGroup();
    if (group == nullptr)
    {
        if (engine.isStemMode())
            engine.clearStems();
        activeGroupId.clear();
        resized(); // パネル高さがグループ有無で変わる
        return;
    }

    if (group->id != activeGroupId)
    {
        const auto identity = StemCache::SourceIdentity::forFile (engine.fileInfo().file);
        const auto dir = StemCache::identityDir (identity);
        std::vector<std::pair<juce::String, juce::File>> files;
        for (const auto& s : group->stems)
            files.emplace_back (s.name, dir.getChildFile (s.relPath));
        if (! engine.setStemVoices (files))
        {
            showToast (jp (u8"ステムWAVを開けません（キャッシュを再分離してください）"));
            stemPanel.clear();
            activeGroupId.clear();
            return;
        }
        activeGroupId = group->id;
        Log::info ("stems.group", "id=" + group->id);
    }

    const auto audible = stemPanel.audibleStems();
    for (size_t i = 0; i < audible.size(); ++i)
        engine.setStemGain ((int) i, audible[i] ? 1.0f : 0.0f);
    resized();
}

void SalvaMainComponent::updateCacheSizeLabel()
{
    const auto bytes = StemCache::totalCacheBytes (StemCache::stemsRoot());
    const auto text = bytes <= 0
                          ? jp (u8"キャッシュ 0")
                          : jp (u8"キャッシュ ") + juce::File::descriptionOfSizeInBytes (bytes);
    cacheSizeLabel.setText (text + jp (u8" ▾"), juce::dontSendNotification);
}

void SalvaMainComponent::showCacheDeleteMenu()
{
    juce::PopupMenu menu;
    // 手動生成のPopupMenuはターゲットのLnFを継承しない（デフォルトLnFに落ちる）ので明示する
    menu.setLookAndFeel (&getLookAndFeel());
    menu.addItem (1, jp (u8"現在ファイルのステムを削除"), engine.hasFile());
    menu.addItem (2, jp (u8"全ステムキャッシュを削除"));
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&cacheSizeLabel),
                        [this] (int result)
                        {
                            if (result == 0)
                                return;
                            // 現在再生中のidentityは再生停止・reader解放後に削除（planの契約）
                            engine.stop();
                            if (engine.isStemMode())
                            {
                                engine.clearStems();
                                stemPanel.clear();
                                activeGroupId.clear();
                            }
                            engine.drainRetired (500);

                            const auto myPid = (juce::int64) getpid();
                            StemCache::DeleteResult r;
                            if (result == 1)
                            {
                                const auto identity = StemCache::SourceIdentity::forFile (engine.fileInfo().file);
                                r = StemCache::deleteIdentity (StemCache::identityDir (identity), myPid,
                                                               StemCache::defaultPidAlive,
                                                               StemCache::defaultPgidAlive,
                                                               juce::Time::getCurrentTime());
                            }
                            else
                            {
                                r = StemCache::deleteAll (StemCache::stemsRoot(), myPid,
                                                          StemCache::defaultPidAlive,
                                                          StemCache::defaultPgidAlive,
                                                          juce::Time::getCurrentTime());
                            }
                            auto message = jp (u8"削除: ") + juce::String (r.deleted) + jp (u8"件");
                            if (r.skippedInUse > 0)
                                message += jp (u8"（使用中のためスキップ: ") + juce::String (r.skippedInUse) + jp (u8"件）");
                            showToast (message);
                            Log::info ("stems.delete", "deleted=" + juce::String (r.deleted)
                                                           + " skipped=" + juce::String (r.skippedInUse));
                            refreshStemCacheState();
                        });
}

void SalvaMainComponent::startExport()
{
    if (! engine.hasFile() || ! waveform.hasSelection() || exportWorker.isBusy())
        return;

    // 書き出し先はユーザー選択＋記憶（初回のみダイアログ）
    if (settings.exportDirectory.isEmpty() || ! juce::File (settings.exportDirectory).isDirectory())
    {
        recordFileChooser = std::make_unique<juce::FileChooser> (
            jp (u8"書き出し先フォルダ"),
            juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        recordFileChooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                const auto dir = fc.getResult();
                if (dir == juce::File {} || ! dir.isDirectory())
                    return;
                settings.exportDirectory = dir.getFullPathName();
                settings.save();
                startExport();
            });
        return;
    }

    // 聴こえている音を1ファイル: ステム構成中はM/S結果、オリジナル中は元ファイル
    std::vector<RegionExport::Source> sources;
    if (engine.isStemMode())
    {
        const auto* group = stemPanel.currentGroup();
        const auto audible = stemPanel.audibleStems();
        const auto identity = StemCache::SourceIdentity::forFile (engine.fileInfo().file);
        const auto dir = StemCache::identityDir (identity);
        for (size_t i = 0; group != nullptr && i < group->stems.size(); ++i)
            if (i < audible.size() && audible[i])
                sources.push_back ({ dir.getChildFile (group->stems[i].relPath), 1.0f });
        if (sources.empty())
        {
            showToast (jp (u8"鳴っているステムがありません"));
            return;
        }
    }
    else
    {
        sources.push_back ({ engine.fileInfo().file, 1.0f });
    }

    const double sec = selectionSeconds();
    const auto name = BpmMath::exportFileName (engine.fileInfo().file.getFileNameWithoutExtension(),
                                               currentBeats(), sec);
    const auto outFile = juce::File (settings.exportDirectory).getChildFile (name).getNonexistentSibling();

    if (! exportWorker.startExport (std::move (sources), waveform.selectionStart(),
                                    waveform.selectionEnd(), engine.fileInfo().sampleRate, outFile))
        return;
    exportButton.setEnabled (false);
    Log::info ("export.start", "out=" + outFile.getFullPathName());
}

void SalvaMainComponent::showToast (const juce::String& message)
{
    toastLabel.setText (message, juce::dontSendNotification);
    toastLabel.setVisible (true);
    toastTicks = 75; // 30Hzで約2.5秒
    resized();
}

void SalvaMainComponent::toggleRecordMode()
{
    if (recordMode && engine.getRecorder().isRecording())
        return; // 録音中はモードを抜けられない（停止が先）

    recordMode = ! recordMode;
    if (recordMode)
    {
        if (engine.isPlaying())
            togglePlay(); // 録音と再生は排他
        const auto error = engine.enterRecordMode (settings.inputDeviceName, settings.inputChannelPairStart);
        if (error.isNotEmpty())
            Log::warn ("record.enter.error", "error=" + error);
        recordView.setInputDevices (engine.inputDeviceNames(), engine.currentInputDeviceName());
        recordView.setChannelPairs (engine.currentInputChannelCount(), settings.inputChannelPairStart);
        Log::info ("record.mode.enter", "input=" + engine.currentInputDeviceName()
                                            + " sr=" + juce::String (engine.currentDeviceSampleRate(), 0));
    }
    else
    {
        engine.exitRecordMode (settings.outputDeviceName);
        rebuildOutputDeviceBox();
        Log::info ("record.mode.exit");
    }

    recordModeButton.setColour (juce::TextButton::buttonColourId,
                                recordMode ? juce::Colour (0xff8e2a26) : buttonBg);
    recordModeButton.setColour (juce::TextButton::textColourOffId,
                                recordMode ? textColour : recordTextColour);
    playButton.setEnabled (! recordMode);
    barsButton.setEnabled (! recordMode);
    outputDeviceBox.setEnabled (! recordMode);
    updateSeparateButtonState();
    updateBpmDisplay();
    resized();
    repaint();
}

void SalvaMainComponent::toggleRecording()
{
    auto& recorder = engine.getRecorder();
    if (recorder.isRecording())
    {
        finishRecording();
        return;
    }

    // 保存先＋ファイル名は録音開始前にダイアログで指定（テイク破棄は手動削除。キャンセル経路なし）
    const auto defaultDir = settings.recordDirectory.isNotEmpty()
                                ? juce::File (settings.recordDirectory)
                                : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    const auto defaultName = "record-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M") + ".wav";
    recordFileChooser = std::make_unique<juce::FileChooser> (
        jp (u8"録音の保存先"), defaultDir.getChildFile (defaultName), "*.wav");
    recordFileChooser->launchAsync (
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file == juce::File {})
                return; // ダイアログのキャンセル（録音自体が始まっていないので副作用なし）
            settings.recordDirectory = file.getParentDirectory().getFullPathName();
            settings.save();
            const auto sr = engine.currentDeviceSampleRate();
            if (sr <= 0.0 || ! engine.getRecorder().start (file, sr))
            {
                Log::error ("record.start.failed", "path=" + file.getFullPathName());
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                        "Salva", jp (u8"録音を開始できませんでした"));
                return;
            }
            Log::info ("record.start", "path=" + file.getFullPathName() + " sr=" + juce::String (sr, 0));
        });
}

void SalvaMainComponent::finishRecording()
{
    auto& recorder = engine.getRecorder();
    const auto expected = recorder.attemptedSamples();
    const auto sr = recorder.recordingSampleRate();
    const auto file = recorder.recordedFile();
    const auto actual = recorder.stop(); // ヘッダ確定

    Log::info ("record.stop", "path=" + file.getFullPathName()
                                  + " expected=" + juce::String (expected)
                                  + " actual=" + juce::String (actual)
                                  + " dropped=" + juce::String ((juce::int64) recorder.droppedWrites()));

    // 期待サンプル数と実WAV長の照合（欠けた録音を正常テイクとして黙って開かない）
    const auto warning = RecordingCheck::mismatchWarning (expected, actual, sr);
    if (actual < 0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Salva",
                                                jp (u8"録音ファイルの確定に失敗しました"));
        return;
    }
    if (warning.isNotEmpty())
    {
        Log::warn ("record.mismatch", "missing=" + juce::String (expected - actual));
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Salva", warning);
    }

    // 停止 → 再生系へ戻して、そのWAVを現在ファイルとして開く
    toggleRecordMode();
    openFile (file);
}

void SalvaMainComponent::selectionChanged (juce::int64 start, juce::int64 end)
{
    beatsOverride = 0; // 区間が変わったら小節数は自動選択に戻す
    engine.updateLoop (true, start, end);
    updateBpmDisplay();
}

void SalvaMainComponent::selectionCleared()
{
    beatsOverride = 0;
    engine.updateLoop (false, 0, 0);
    updateBpmDisplay();
}

double SalvaMainComponent::selectionSeconds() const
{
    const auto& fi = engine.fileInfo();
    if (! waveform.hasSelection() || fi.sampleRate <= 0.0)
        return 0.0;
    return (double) (waveform.selectionEnd() - waveform.selectionStart()) / fi.sampleRate;
}

int SalvaMainComponent::currentBeats() const
{
    return beatsOverride > 0 ? beatsOverride : BpmMath::autoBeats (selectionSeconds());
}

void SalvaMainComponent::updateBpmDisplay()
{
    const bool has = waveform.hasSelection() && selectionSeconds() > 0.0;
    barsButton.setVisible (has);
    bpmLabel.setVisible (has);
    exportNameLabel.setVisible (has);
    if (! has)
        return;

    const double sec = selectionSeconds();
    const int beats = currentBeats();
    barsButton.setButtonText (juce::String (beats / 4) + jp (u8"小節 ▾"));
    bpmLabel.setText (BpmMath::bpmDisplayText (beats, sec) + " BPM", juce::dontSendNotification);
    exportNameLabel.setText (
        BpmMath::exportFileName (engine.fileInfo().file.getFileNameWithoutExtension(), beats, sec),
        juce::dontSendNotification);
    exportButton.setVisible (has && ! recordMode);
}

void SalvaMainComponent::updateHeader()
{
    repaint (0, 0, getWidth(), headerHeight);
}

void SalvaMainComponent::rebuildOutputDeviceBox()
{
    outputDeviceBox.clear (juce::dontSendNotification);
    const auto names = engine.outputDeviceNames();
    int id = 1;
    for (const auto& n : names)
        outputDeviceBox.addItem (n, id++);
    const auto current = engine.currentOutputDeviceName();
    const int idx = names.indexOf (current);
    if (idx >= 0)
        outputDeviceBox.setSelectedId (idx + 1, juce::dontSendNotification);
}

void SalvaMainComponent::timerCallback()
{
    // 起動直後のgrabKeyboardFocusはisShowing()が偽で空振りする（GOTCHAS）→ 表示後に1回だけ
    if (! initialFocusDone && isShowing())
    {
        grabKeyboardFocus();
        initialFocusDone = true;
    }

    engine.purgeRetired();

    // ドラッグ中は盤面を回す（モックの約9秒/周に合わせた角速度）
    if (dragOver && ! engine.hasFile() && ! recordMode)
    {
        discAngle += 0.023f;
        repaint (emptyStateArea);
    }

    if (toastTicks > 0 && --toastTicks == 0)
        toastLabel.setVisible (false);

    // 分離ジョブの完了（identity一致時のみM/S行を点灯。実行中に別ファイルへ切り替えても誤接続しない）
    {
        StemCache::SourceIdentity identity;
        bool success = false;
        if (separator.consumeResult (identity, success))
        {
            updateSeparateButtonState();
            separateProgressLabel.setVisible (false);
            updateCacheSizeLabel();
            if (! success)
            {
                showToast (jp (u8"ステム分離に失敗しました（ログ参照）"));
                Log::warn ("separate.failed");
            }
            else if (engine.hasFile()
                     && StemCache::SourceIdentity::forFile (engine.fileInfo().file) == identity)
            {
                refreshStemCacheState();
                showToast (jp (u8"ステム分離が完了しました"));
                Log::info ("separate.done", "identity=" + identity.hash());
            }
            else
            {
                Log::info ("separate.done_other_file", "identity=" + identity.hash());
            }
        }
    }

    // 書き出しの完了
    {
        RegionExport::Result result;
        juce::File outFile;
        if (exportWorker.consumeResult (result, outFile))
        {
            exportButton.setEnabled (true);
            if (! result.ok)
            {
                showToast (jp (u8"書き出しに失敗: ") + result.error);
                Log::warn ("export.failed", "error=" + result.error);
            }
            else
            {
                auto message = jp (u8"書き出しました: ") + outFile.getFileName();
                if (result.appliedGain < 1.0f)
                    message += jp (u8"（0dBFS超のため ")
                               + juce::String (20.0f * std::log10 (result.appliedGain), 1)
                               + jp (u8"dB 下げて収めました）");
                showToast (message);
                Log::info ("export.done", "out=" + outFile.getFullPathName()
                                              + " gain=" + juce::String (result.appliedGain, 4));
            }
        }
    }

    // ステム行のレベルメーター（案B）
    if (stemPanel.hasStems() && engine.isStemMode())
        stemPanel.updatePeaks ([this] (int i) { return engine.stemPeak (i); });

    if (recordMode)
    {
        auto& recorder = engine.getRecorder();
        const double sr = recorder.isRecording() ? recorder.recordingSampleRate() : 0.0;
        recordView.update (recorder.peakL(), recorder.peakR(), recorder.isRecording(),
                           sr > 0.0 ? (double) recorder.attemptedSamples() / sr : 0.0);
        return;
    }

    const bool playing = engine.isPlaying();

    if (playing && engine.consumeReachedEnd())
    {
        engine.stop();
        playButton.setButtonText (jp (u8"▶"));
        Log::info ("transport.end_of_file");
    }

    if (engine.hasFile())
    {
        const auto pos = engine.uiPlayheadSample();
        const auto& fi = engine.fileInfo();
        timeLabel.setText (formatTime ((double) pos / fi.sampleRate), juce::dontSendNotification);
        waveform.setPlayhead (pos, true);
    }

    // read-aheadの枯渇はatomicカウンタで観測し、UI側で表示・ログする（GOTCHAS: pull型）
    const auto starved = engine.starvedSamples();
    if (starved != lastStarved)
    {
        lastStarved = starved;
        starvedLabel.setText (jp (u8"読み込み遅延 ") + juce::String (starved), juce::dontSendNotification);
        starvedLabel.setVisible (true);
        Log::warn ("stream.starved", "totalSamples=" + juce::String (starved));
    }

    playButton.setButtonText (playing ? jp (u8"■") : jp (u8"▶"));
}

bool SalvaMainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        if (! recordMode)
            togglePlay();
        return true;
    }
    if (key == juce::KeyPress ('r')) // Logic準拠: r = 録音（録音モード中のみ）
    {
        if (recordMode)
            toggleRecording();
        return true;
    }
    if (key == juce::KeyPress ('w', juce::ModifierKeys::commandModifier, 0))
    {
        // ×ボタンと同じ「1段戻る」。スタート画面ではアプリ終了（macOSの⌘W慣習に合わせる）
        if (! handleCloseRequest())
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        return true;
    }
    if (key == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0))
    {
        if (! recordMode)
            openFileChooser();
        return true;
    }
    if (key == juce::KeyPress (juce::KeyPress::leftKey, juce::ModifierKeys::commandModifier, 0))
    {
        waveform.zoomOut();
        return true;
    }
    if (key == juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::commandModifier, 0))
    {
        waveform.zoomIn();
        return true;
    }
    return false;
}

bool SalvaMainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (AudioFileTypes::isSupported (f))
            return true;
    return false;
}

void SalvaMainComponent::fileDragEnter (const juce::StringArray&, int, int)
{
    dragOver = true;
    repaint (emptyStateArea);
}

void SalvaMainComponent::fileDragExit (const juce::StringArray&)
{
    dragOver = false;
    repaint (emptyStateArea);
}

void SalvaMainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    repaint (emptyStateArea);
    for (const auto& f : files)
    {
        if (AudioFileTypes::isSupported (f))
        {
            openFile (juce::File (f));
            return; // 最初の対応ファイルだけ開く
        }
    }
}

void SalvaMainComponent::paint (juce::Graphics& g)
{
    g.fillAll (windowBg);

    // ファイル未オープン時: フルブリードの空状態（ヘッダー・フッターは描かない）
    if (! engine.hasFile() && ! recordMode)
    {
        paintEmptyState (g, emptyStateArea);
        return;
    }

    // ヘッダー
    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (headerBg);
    g.fillRect (header);
    g.setColour (panelBorder);
    g.fillRect (header.removeFromBottom (1));

    // レコードのモチーフは空状態の大きな盤面1つに絞る（アプリアイコンと微妙に違う
    // ミニ盤面を並べない）。ヘッダーはアプリ名の文字だけ
    g.setColour (textColour);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText ("Salva", 14, 0, 80, headerHeight, juce::Justification::centredLeft);

    g.setFont (juce::FontOptions (12.0f));
    if (engine.hasFile())
    {
        const auto& fi = engine.fileInfo();
        const auto infoText = fi.file.getFileName()
                              + jp (u8" — ") + juce::String (fi.sampleRate / 1000.0, 1) + "kHz"
                              + jp (u8"・") + (fi.numChannels >= 2 ? jp (u8"ステレオ") : jp (u8"モノラル"))
                              + jp (u8"・") + formatTime ((double) fi.lengthSamples / fi.sampleRate);
        g.setColour (textDim);
        g.drawText (infoText, 80, 0, juce::jmax (100, getWidth() - 400), headerHeight,
                    juce::Justification::centredLeft, true);
    }
}

void SalvaMainComponent::refreshShownRecentFiles()
{
    shownRecentFiles.clear();
    for (const auto& path : settings.recentFiles)
        if (juce::File (path).existsAsFile())
            shownRecentFiles.add (path);
}

SalvaMainComponent::EmptyLayout SalvaMainComponent::computeEmptyLayout() const
{
    EmptyLayout el;
    auto content = emptyStateArea.reduced (40, 20);

    const int discSize = juce::jlimit (60, 210, juce::jmin (content.getHeight() - 10,
                                                            content.getWidth() * 2 / 5));
    el.disc = content.removeFromLeft (discSize).withSizeKeepingCentre (discSize, discSize);
    content.removeFromLeft (36);

    // 右側ブロックは縦中央寄せ。最近ファイル行はエリアに収まる分だけ（保存は最大8件）
    constexpr int cardH = 118;
    constexpr int fixedH = 20 + 8 + cardH;     // セクションラベル＋間隔＋カード行
    constexpr int recentHeadH = 20 + 24;       // 間隔＋見出し
    el.visibleRows = juce::jlimit (0, shownRecentFiles.size(),
                                   (content.getHeight() - fixedH - recentHeadH) / recentRowHeight);

    const int blockH = fixedH + (el.visibleRows > 0 ? recentHeadH + el.visibleRows * recentRowHeight : 0);
    auto side = content.withSizeKeepingCentre (content.getWidth(),
                                               juce::jmin (blockH, content.getHeight()));
    el.title = side.removeFromTop (20);
    side.removeFromTop (8);
    auto cardsRow = side.removeFromTop (cardH);
    el.cardFile = cardsRow.removeFromLeft ((cardsRow.getWidth() - 12) / 2);
    cardsRow.removeFromLeft (12);
    el.cardRecord = cardsRow;
    if (el.visibleRows > 0)
    {
        side.removeFromTop (20);
        el.recentHeader = side.removeFromTop (24);
        el.rowsTop = side;
    }
    return el;
}

juce::Rectangle<int> SalvaMainComponent::emptyRecentRowRect (const EmptyLayout& el, int index) const
{
    return { el.rowsTop.getX(), el.rowsTop.getY() + index * recentRowHeight,
             el.rowsTop.getWidth(), recentRowHeight };
}

void SalvaMainComponent::paintDisc (juce::Graphics& g, juce::Rectangle<float> bounds)
{
    // アプリアイコンと同じ構成（make_icon.swift の比率を盤半径基準に換算）。
    // 回転はレーベルのグラデーション角度で見せ、選択ブラケットは固定
    //（回る盤の上に区間選択が乗っている、というアプリの動作そのものの表現）
    const auto b = dragOver ? bounds.expanded (bounds.getWidth() * 0.02f) : bounds;
    const auto c = b.getCentre();
    const float r = b.getWidth() * 0.5f;

    g.setColour (discBase);
    g.fillEllipse (b);

    // 溝: 26本の同心円（外周ほど密・うっすら明るい線）
    for (int i = 0; i < 26; ++i)
    {
        const float t = (float) i / 25.0f;
        const float gr = r * (0.42f + t * 0.55f);
        g.setColour (discGroove.withAlpha (0.35f + 0.25f * (1.0f - t)));
        g.drawEllipse (c.x - gr, c.y - gr, gr * 2.0f, gr * 2.0f, 1.0f);
    }

    // 照り: 左上からの光（固定＝照明。盤が回ってもここは動かない）
    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addEllipse (b);
        g.reduceClipRegion (clip);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.10f),
                                                 c.x - r, c.y - r,
                                                 juce::Colours::white.withAlpha (0.0f),
                                                 c.x, c.y, false));
        g.fillRect (b);
    }

    // レーベル: アイコンと同じwarmグラデ。ドラッグ中はグラデ方向が回る
    {
        const float lr = r * 0.359f;
        const juce::Point<float> dir { std::sin (discAngle), -std::cos (discAngle) };
        const auto top = c + dir * lr;
        const auto bottom = c - dir * lr;
        juce::ColourGradient grad (labelTop, top.x, top.y, labelBottom, bottom.x, bottom.y, false);
        grad.addColour (0.48, labelMid);
        g.setGradientFill (grad);
        g.fillEllipse (c.x - lr, c.y - lr, lr * 2.0f, lr * 2.0f);
    }

    // 区間選択のブラケット: レーベルを跨ぐ2本の縦線＋間の半透明帯
    {
        const float halfW = r * 0.217f, halfH = r * 0.402f;
        const float barW = juce::jmax (2.0f, r * 0.030f);
        g.setColour (juce::Colours::white.withAlpha (0.16f));
        g.fillRect (juce::Rectangle<float> (c.x - halfW, c.y - halfH, halfW * 2.0f, halfH * 2.0f));
        g.setColour (discBracket.withAlpha (0.9f));
        g.fillRect (juce::Rectangle<float> (c.x - halfW - barW * 0.5f, c.y - halfH, barW, halfH * 2.0f));
        g.fillRect (juce::Rectangle<float> (c.x + halfW - barW * 0.5f, c.y - halfH, barW, halfH * 2.0f));
    }

    // センターホール
    {
        const float hr = r * 0.048f;
        g.setColour (discHole);
        g.fillEllipse (c.x - hr, c.y - hr, hr * 2.0f, hr * 2.0f);
    }

    if (dragOver)
    {
        const auto badge = juce::Rectangle<float> (110.0f, 24.0f).withCentre (c);
        g.setColour (juce::Colour (0xe0121115));
        g.fillRoundedRectangle (badge, 12.0f);
        g.setColour (textColour);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (jp (u8"ここにドロップ"), badge.toNearestInt(), juce::Justification::centred);
    }
}

void SalvaMainComponent::paintEmptyState (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (emptyBg);
    g.fillRect (area);

    refreshShownRecentFiles();
    const auto el = computeEmptyLayout();
    const auto discF = el.disc.toFloat();

    // 盤面の背後の淡いアンバーのグロー
    {
        const auto c = discF.getCentre();
        juce::ColourGradient glow (accent.withAlpha (0.07f), c.x, c.y,
                                   accent.withAlpha (0.0f), c.x + discF.getWidth() * 1.1f, c.y, true);
        g.setGradientFill (glow);
        g.fillRect (area);
    }

    paintDisc (g, discF);

    // 「最近開いたファイル」と同格のセクションラベル（大見出しにすると階層が浮く）
    g.setColour (textDim.withAlpha (0.85f));
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (jp (u8"音源を取り込む"), el.title, juce::Justification::centredLeft);

    // 入口カード×2（「ファイルから」「レコードから」は対等な入口。ドロップは全域で受ける）
    auto drawCard = [&g] (juce::Rectangle<int> r, bool hoveredNow, bool primary,
                                const char* title, const char* line1, const char* line2)
    {
        g.setColour (juce::Colour (0xff1d1c21));
        g.fillRoundedRectangle (r.toFloat(), 10.0f);
        g.setColour (hoveredNow ? (primary ? accent : juce::Colour (0xff6a6774)) : buttonBg);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);

        auto inner = r.reduced (14, 12);
        g.setColour (textColour);
        g.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        g.drawText (jp (title), inner.removeFromTop (18), juce::Justification::centredLeft, true);
        inner.removeFromTop (2);
        g.setColour (textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (jp (line1), inner.removeFromTop (15), juce::Justification::centredLeft, true);
        g.drawText (jp (line2), inner.removeFromTop (15), juce::Justification::centredLeft, true);
    };
    drawCard (el.cardFile, hoveredCard == 0, true, u8"ファイルから",
              u8"ドロップ / クリックで選択", u8"wav / aiff / flac / mp3 / m4a");
    drawCard (el.cardRecord, hoveredCard == 1, false, u8"レコードから",
              u8"ターンテーブルの音を録音して", u8"そのまま素材にする");

    if (el.visibleRows <= 0)
        return;

    g.setColour (textDim.withAlpha (0.85f));
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (jp (u8"最近開いたファイル"), el.recentHeader, juce::Justification::centredLeft);

    for (int i = 0; i < el.visibleRows; ++i)
    {
        const auto row = emptyRecentRowRect (el, i);
        const bool hovered = (i == hoveredRecent);
        if (hovered)
        {
            g.setColour (accent.withAlpha (0.09f));
            g.fillRoundedRectangle (row.toFloat(), 8.0f);
        }
        auto inner = row.reduced (12, 6);

        const juce::File f (shownRecentFiles[i]);
        g.setColour (hovered ? textColour : juce::Colour (0xffe8e0d2));
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (f.getFileName(), inner.removeFromTop (16), juce::Justification::centredLeft, true);
        g.setColour (textDim.withAlpha (0.8f));
        g.setFont (juce::FontOptions (10.5f));
        g.drawText (shortDirPath (f), inner, juce::Justification::centredLeft, true);
    }
}

void SalvaMainComponent::mouseUp (const juce::MouseEvent& e)
{
    if (e.eventComponent == &cacheSizeLabel)
    {
        showCacheDeleteMenu();
        return;
    }
    if (engine.hasFile() || recordMode)
        return;
    // 描画と同じレイアウト計算でヒットテスト（座標ズレ防止）。カード全体がクリック領域
    const auto el = computeEmptyLayout();
    if (el.cardFile.contains (e.getPosition()))
    {
        openFileChooser();
        return;
    }
    if (el.cardRecord.contains (e.getPosition()))
    {
        toggleRecordMode();
        return;
    }
    for (int i = 0; i < el.visibleRows; ++i)
    {
        if (emptyRecentRowRect (el, i).contains (e.getPosition()))
        {
            openFile (juce::File (shownRecentFiles[i]));
            return;
        }
    }
}

void SalvaMainComponent::mouseMove (const juce::MouseEvent& e)
{
    if (e.eventComponent != this)
        return; // cacheSizeLabelのリスナー経由は座標系が違うので無視
    int hitRow = -1, hitCard = -1;
    if (! engine.hasFile() && ! recordMode)
    {
        const auto el = computeEmptyLayout();
        if (el.cardFile.contains (e.getPosition()))
            hitCard = 0;
        else if (el.cardRecord.contains (e.getPosition()))
            hitCard = 1;
        for (int i = 0; hitCard < 0 && i < el.visibleRows; ++i)
            if (emptyRecentRowRect (el, i).contains (e.getPosition()))
            {
                hitRow = i;
                break;
            }
    }
    if (hitRow != hoveredRecent || hitCard != hoveredCard)
    {
        hoveredRecent = hitRow;
        hoveredCard = hitCard;
        setMouseCursor ((hitRow >= 0 || hitCard >= 0) ? juce::MouseCursor::PointingHandCursor
                                                      : juce::MouseCursor::NormalCursor);
        repaint (emptyStateArea);
    }
}

void SalvaMainComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredRecent != -1 || hoveredCard != -1)
    {
        hoveredRecent = -1;
        hoveredCard = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint (emptyStateArea);
    }
}

void SalvaMainComponent::openFileChooser()
{
    const auto downloads = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                               .getChildFile ("Downloads");
    recordFileChooser = std::make_unique<juce::FileChooser> (
        jp (u8"オーディオファイルを開く"),
        downloads.isDirectory() ? downloads
                                : juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a");
    recordFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            if (const auto f = fc.getResult(); f.existsAsFile())
                openFile (f);
        });
}

bool SalvaMainComponent::handleCloseRequest()
{
    if (recordMode)
    {
        if (engine.getRecorder().isRecording())
        {
            showToast (jp (u8"録音を停止してから閉じてください"));
            return true; // 録音中の×で黙ってテイクを失わせない
        }
        toggleRecordMode(); // ファイル画面 or スタート画面へ戻る
        return true;
    }
    if (engine.hasFile())
    {
        closeFileToStart();
        return true;
    }
    return false; // スタート画面での×はアプリ終了
}

void SalvaMainComponent::closeFileToStart()
{
    engine.closeFile();
    waveform.clearFile();
    beatsOverride = 0;
    playButton.setButtonText (jp (u8"▶"));
    timeLabel.setText ("0:00.0", juce::dontSendNotification);
    refreshStemCacheState(); // manifest・ステムパネルを空へ（resized込み）
    updateBpmDisplay();
    updateSeparateButtonState();
    updateHeader();
    repaint();
    Log::info ("file.close");
}

void SalvaMainComponent::updateSeparateButtonState()
{
    separateButton.setEnabled (engine.hasFile() && ! recordMode
                               && separator.status() != StemSeparator::Status::running);
}

void SalvaMainComponent::openRecentAt (int index)
{
    if (index >= 0 && index < shownRecentFiles.size())
        openFile (juce::File (shownRecentFiles[index]));
}

void SalvaMainComponent::resized()
{
    auto area = getLocalBounds();
    const bool emptyState = ! engine.hasFile() && ! recordMode;

    // 空状態はヘッダー・トランスポート・フッターを丸ごと隠す（載っているものが全部
    // 死んでいるか空状態のボタンと重複のため）。フルブリードの選択画面にする
    // ヘッダー右側: [分離中…] [ステム分離] [キャッシュ x GB ▾]
    auto header = emptyState ? juce::Rectangle<int>()
                             : area.removeFromTop (headerHeight).reduced (10, 8);
    cacheSizeLabel.setBounds (header.removeFromRight (130));
    header.removeFromRight (8);
    separateButton.setBounds (header.removeFromRight (90));
    header.removeFromRight (8);
    separateProgressLabel.setBounds (header.removeFromRight (60));

    auto bottom = emptyState ? juce::Rectangle<int>()
                             : area.removeFromBottom (bottomHeight).reduced (10, 6);
    auto transport = emptyState ? juce::Rectangle<int>()
                                : area.removeFromBottom (transportHeight).reduced (10, 7);
    for (auto* c : { (juce::Component*) &playButton, (juce::Component*) &timeLabel,
                     (juce::Component*) &separateButton, (juce::Component*) &cacheSizeLabel,
                     (juce::Component*) &outputLabel, (juce::Component*) &outputDeviceBox,
                     (juce::Component*) &recordModeButton })
        c->setVisible (! emptyState);

    // ステムM/Sパネル（案B: 縦リスト。グループ未選択時はタブだけの高さ）
    const int stemHeight = (! recordMode && engine.hasFile()) ? stemPanel.preferredHeight() : 0;
    stemPanel.setBounds (area.removeFromBottom (stemHeight));
    stemPanel.setVisible (stemHeight > 0);

    emptyStateArea = area;
    waveform.setBounds (area);
    recordView.setBounds (area);
    waveform.setVisible (engine.hasFile() && ! recordMode);
    recordView.setVisible (recordMode);

    // 空状態の主要アクション（描画と同じレイアウト計算でボタンを置く）
    openFileButton.setVisible (emptyState);
    recordEntryButton.setVisible (emptyState);
    if (emptyState)
    {
        refreshShownRecentFiles(); // 行数で縦中央が変わるため先に更新
        const auto el = computeEmptyLayout();
        auto cf = el.cardFile.reduced (14, 12);
        openFileButton.setBounds (cf.removeFromBottom (28).withWidth (juce::jmin (128, cf.getWidth())));
        auto cr = el.cardRecord.reduced (14, 12);
        recordEntryButton.setBounds (cr.removeFromBottom (28).withWidth (juce::jmin (140, cr.getWidth())));
    }

    playButton.setBounds (transport.removeFromLeft (44));
    transport.removeFromLeft (10);
    timeLabel.setBounds (transport.removeFromLeft (76));
    transport.removeFromLeft (16);
    barsButton.setBounds (transport.removeFromLeft (86));
    transport.removeFromLeft (8);
    bpmLabel.setBounds (transport.removeFromLeft (110));
    starvedLabel.setBounds (transport.removeFromLeft (120));
    exportButton.setBounds (transport.removeFromRight (78));
    transport.removeFromRight (8);
    exportNameLabel.setBounds (transport);

    outputLabel.setBounds (bottom.removeFromLeft (44));
    outputDeviceBox.setBounds (bottom.removeFromLeft (260));
    recordModeButton.setBounds (bottom.removeFromRight (120));

    toastLabel.setBounds (getLocalBounds().withSizeKeepingCentre (juce::jmin (520, getWidth() - 40), 28)
                              .withY (getHeight() - (emptyState ? 0 : bottomHeight + transportHeight) - 40));
}
