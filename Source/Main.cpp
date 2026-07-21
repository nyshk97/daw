#include <exception>
#include <juce_gui_basics/juce_gui_basics.h>

#include "shared/Log.h"
#include "ui/MainComponent.h"
#include "ui/ProjectChooserComponent.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }
}

class DawApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "daw"; }
    const juce::String getApplicationVersion() override { return "0.2.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        Log::init (getApplicationVersion());
        mainWindow = std::make_unique<MainWindow>();
    }

    void shutdown() override
    {
        mainWindow.reset();
        Log::shutdown();
    }

    // JUCE_CATCH_UNHANDLED_EXCEPTIONS=1 でメッセージループまで漏れた例外がここに届く。
    // ログを残した上でterminateし、OS標準のクラッシュレポート（.ips）も生成させる
    void unhandledException (const std::exception* e,
                             const juce::String& sourceFilename,
                             int lineNumber) override
    {
        Log::error ("app.unhandled_exception",
                    juce::String ("what=") + (e != nullptr ? e->what() : "(unknown)")
                        + " source=" + sourceFilename + ":" + juce::String (lineNumber));
        std::terminate();
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            mainWindow->attemptQuit();
        else
            quit();
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow()
            : DocumentWindow ("daw",
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, true);
            showChooser();
            setVisible (true);
        }

        void showChooser()
        {
            mainComp = nullptr;
            auto* chooser = new ProjectChooserComponent();
            chooser->onProjectOpened = [this] (std::unique_ptr<Project> project)
            {
                openProject (std::move (project));
            };
            setContentOwned (chooser, true);
            centreWithSize (getWidth(), getHeight());
        }

        void openProject (std::unique_ptr<Project> project)
        {
            auto* component = new MainComponent (std::move (project));
            component->onTitleChanged = [this] (const juce::String& title) { setName (title); };
            mainComp = component;
            setContentOwned (component, true);
            setName (component->windowTitle());
            centreWithSize (getWidth(), getHeight());
        }

        void closeButtonPressed() override
        {
            attemptQuit();
        }

        void attemptQuit()
        {
            if (mainComp != nullptr && mainComp->hasUnsavedChanges())
            {
                juce::NativeMessageBox::showAsync (
                    juce::MessageBoxOptions()
                        .withIconType (juce::MessageBoxIconType::QuestionIcon)
                        .withTitle (jp (u8"未保存の変更があります"))
                        .withMessage (jp (u8"プロジェクトを保存しますか？"))
                        .withButton (jp (u8"保存して終了"))
                        .withButton (jp (u8"保存せず終了"))
                        .withButton (jp (u8"キャンセル")),
                    [this] (int result)
                    {
                        if (result == 0)
                        {
                            if (mainComp != nullptr && mainComp->trySave())
                                juce::JUCEApplication::getInstance()->quit();
                        }
                        else if (result == 1)
                        {
                            juce::JUCEApplication::getInstance()->quit();
                        }
                    });
                return;
            }
            juce::JUCEApplication::getInstance()->quit();
        }

    private:
        MainComponent* mainComp = nullptr; // 所有はsetContentOwned側

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (DawApplication)
