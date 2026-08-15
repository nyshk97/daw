// Salva — レコード/ローカル音源のサンプリング素材化アプリ（apps/salva/CLAUDE.md参照）

#include <juce_gui_extra/juce_gui_extra.h>

#include "mac/SparkleBridge.h"
#include "shared/AudioFileTypes.h"
#include "shared/Log.h"
#include "ui/SalvaMainComponent.h"

namespace
{
juce::String jpText (const char* text) { return juce::String::fromUTF8 (text); }

// メニューバーはアプリメニューの "Check for Updates…" のためだけに立てる
// （SalvaにFileメニュー等は無い。更新チェックのUIはSparkleが出す）
class SalvaMenuModel : public juce::MenuBarModel
{
public:
    juce::StringArray getMenuBarNames() override { return {}; }
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override { return {}; }
    void menuItemSelected (int, int) override {}
};

// コマンドライン（起動引数 / 既存インスタンスへの受け渡し）から開くファイルを探す
juce::File fileFromCommandLine (const juce::String& commandLine)
{
    for (const auto& token : juce::StringArray::fromTokens (commandLine, true))
    {
        const juce::File f (token.unquoted());
        if (f.existsAsFile() && AudioFileTypes::isSupported (f))
            return f;
    }
    return {};
}
} // namespace

class SalvaApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return SALVA_APP_NAME; }
    const juce::String getApplicationVersion() override { return SALVA_APP_VERSION; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& commandLine) override
    {
        Log::init (SALVA_APP_VERSION, "salva");
        // Sparkle: 手動チェックのみ（SUEnableAutomaticChecks=false）。
        // canCheckの変化でアプリメニューの項目を作り直す
        SparkleBridge::init ([this] (bool canCheck) { rebuildMacMainMenu (canCheck); });
        rebuildMacMainMenu (false); // コールバックを待たずメニューを出す
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
        if (const auto file = fileFromCommandLine (commandLine); file.existsAsFile())
        {
            mainWindow->content().openFile (file);
#if JUCE_DEBUG
            // dev版の自動動作確認用（ヘッドレスに近い再生経路の検証）:
            // --select <start> <end> で区間選択、--autoplay で再生開始
            const auto tokens = juce::StringArray::fromTokens (commandLine, true);
            const int selIdx = tokens.indexOf ("--select");
            if (selIdx >= 0 && selIdx + 2 < tokens.size())
                mainWindow->content().selectForVerification (tokens[selIdx + 1].getLargeIntValue(),
                                                             tokens[selIdx + 2].getLargeIntValue());
            const int groupIdx = tokens.indexOf ("--stemgroup");
            if (groupIdx >= 0 && groupIdx + 1 < tokens.size())
                mainWindow->content().selectStemGroupForVerification (tokens[groupIdx + 1].getIntValue());
            if (tokens.contains ("--separate"))
                mainWindow->content().separateForVerification();
            if (tokens.contains ("--autoplay"))
                mainWindow->content().autoPlayForVerification();
            if (tokens.contains ("--export"))
                mainWindow->content().exportForVerification();
#endif
        }
#if JUCE_DEBUG
        // ポップアップの見た目確認（ファイル引数なしでも使う。表示完了を待ってから開く。
        // 注意: JUCEのポップアップはアプリが前面でないと即dismissされるため、open -g では確認できない）
        {
            const auto tokens = juce::StringArray::fromTokens (commandLine, true);
            if (tokens.contains ("--popup-cache"))
                juce::Timer::callAfterDelay (600, [this] { mainWindow->content().showCacheMenuForVerification(); });
            if (tokens.contains ("--popup-output"))
                juce::Timer::callAfterDelay (600, [this] { mainWindow->content().showOutputPopupForVerification(); });
        }
#endif
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (mainWindow == nullptr)
            return;
        if (const auto file = fileFromCommandLine (commandLine); file.existsAsFile())
            mainWindow->content().openFile (file);
        mainWindow->toFront (true);
    }

    void shutdown() override
    {
        // 先にSparkleのコールバックを無効化してからメニューを片付ける
        // （queue済みコールバックが破棄後のメニューを再構築するのを防ぐ）
        SparkleBridge::shutdown();
        juce::MenuBarModel::setMacMainMenu (nullptr);
        menuModel.reset();
        mainWindow = nullptr;
        Log::shutdown();
    }

    void systemRequestedQuit() override { quit(); }

    void unhandledException (const std::exception* e, const juce::String& sourceFilename, int lineNumber) override
    {
        Log::error ("app.unhandled_exception",
                    "what=" + juce::String (e != nullptr ? e->what() : "unknown")
                        + " source=" + sourceFilename + ":" + juce::String (lineNumber));
        Log::shutdown();
        std::terminate();
    }

private:
    void rebuildMacMainMenu (bool canCheckForUpdates)
    {
        juce::PopupMenu extraAppleMenuItems;
        juce::PopupMenu::Item checkItem (jpText (u8"Check for Updates…"));
        checkItem.isEnabled = canCheckForUpdates;
        checkItem.action = [] { SparkleBridge::checkForUpdates(); };
        extraAppleMenuItems.addItem (std::move (checkItem));

        if (menuModel == nullptr)
            menuModel = std::make_unique<SalvaMenuModel>();
        juce::MenuBarModel::setMacMainMenu (menuModel.get(), &extraAppleMenuItems);
    }

    std::unique_ptr<SalvaMenuModel> menuModel;

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Colour (0xff201f24), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new SalvaMainComponent(), true);
            setResizable (true, true);
            setResizeLimits (640, 360, 4096, 2160);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        SalvaMainComponent& content()
        {
            return *static_cast<SalvaMainComponent*> (getContentComponent());
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (SalvaApplication)
