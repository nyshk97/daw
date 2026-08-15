// Salva — レコード/ローカル音源のサンプリング素材化アプリ（apps/salva/CLAUDE.md参照）

#include <juce_gui_extra/juce_gui_extra.h>

#include "shared/AudioFileTypes.h"
#include "shared/Log.h"
#include "ui/SalvaMainComponent.h"

namespace
{
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
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Colour (0xff2e2e33), DocumentWindow::allButtons)
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
