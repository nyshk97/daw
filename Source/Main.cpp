#include <exception>
#include <juce_gui_basics/juce_gui_basics.h>

#include "mac/SparkleBridge.h"
#include "shared/Log.h"
#include "ui/AppLookAndFeel.h"
#include "ui/MainComponent.h"
#include "ui/ProjectChooserComponent.h"
#include "ui/Shortcuts.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

// Fileメニューのコマンド。ネイティブメニューのkeyEquivalent（⌘S/⌘B/⌘O）は
// ApplicationCommandManagerのKeyPressMappings経由でしか設定されないため（JUCEの
// juce_MainMenu_mac.mmの実装）、Shortcuts.hテーブルのmenuKeyをここで橋渡しする。
// キー押下はNSMenuがMainComponent::keyPressedより先に取り、commandManager→
// DawApplication::perform経由で実行される（keyPressed側の同判定はフォールバック）
namespace MenuCommands
{
    enum : juce::CommandID { save = 1, bounce, closeProject };

    struct Item { juce::CommandID command; Shortcuts::ID shortcut; };
    inline constexpr Item items[] = {
        { save, Shortcuts::ID::save },
        { bounce, Shortcuts::ID::bounce },
        { closeProject, Shortcuts::ID::openChooser },
    };
}

// Fileメニュー＋アプリメニューの "Check for Updates…"（extraAppleMenuItemsは
// setMacMainMenu呼び出し側が渡す）。コマンドの実体・enable判定は
// DawApplication（ApplicationCommandTarget）側にあり、ここは並べるだけ
class AppMenuModel : public juce::MenuBarModel
{
public:
    explicit AppMenuModel (juce::ApplicationCommandManager& cm) : commandManager (cm) {}

    juce::StringArray getMenuBarNames() override { return { "File" }; }

    juce::PopupMenu getMenuForIndex (int, const juce::String& name) override
    {
        juce::PopupMenu menu;
        if (name == "File")
        {
            menu.addCommandItem (&commandManager, MenuCommands::save);
            menu.addCommandItem (&commandManager, MenuCommands::bounce);
            menu.addSeparator();
            menu.addCommandItem (&commandManager, MenuCommands::closeProject);
        }
        return menu;
    }

    // 実行はcommandManager→ApplicationCommandTarget::performで行われる（ここには来るが何もしない）
    void menuItemSelected (int, int) override {}

private:
    juce::ApplicationCommandManager& commandManager;
};
}

class DawApplication : public juce::JUCEApplication
{
public:
    // PRODUCT_NAME 由来（Debug=LaLa-dev / Release=LaLa）。名前は CMakeLists.txt で一元管理
    const juce::String getApplicationName() override    { return DAW_APP_NAME; }
    // CMakeLists.txt の project(VERSION) 由来。Info.plist の CFBundleVersion と常に一致する
    const juce::String getApplicationVersion() override { return DAW_APP_VERSION; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        Log::init (getApplicationVersion());
        lookAndFeel = std::make_unique<AppLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        // Fileメニューのコマンド登録とkeyEquivalent（メニュー構築より先に済ませる）
        commandManager.registerAllCommandsForTarget (this);
        commandManager.setFirstCommandTarget (this);
        for (const auto& item : MenuCommands::items)
            if (const auto key = Shortcuts::menuKey (item.shortcut); key.isValid())
                commandManager.getKeyMappings()->addKeyPress (item.command, key);

        // Sparkle 起動 + アプリメニューに "Check for Updates…" を追加。
        // canCheckForUpdates の変化（チェック進行中は false）でメニューを組み直して
        // enable/disable を反映する。コールバックはメッセージスレッドで呼ばれる
        SparkleBridge::init ([this] (bool canCheck) { rebuildMacMainMenu (canCheck); });
        rebuildMacMainMenu (false); // Sparkleのコールバックを待たずFileメニューを出す
        mainWindow = std::make_unique<MainWindow>();
    }

    void shutdown() override
    {
        // 先に Sparkle のコールバックを無効化してから menu を片付ける
        // （queue 済みコールバックが破棄後の menu を再構築するのを防ぐ）
        SparkleBridge::shutdown();
        juce::MenuBarModel::setMacMainMenu (nullptr);
        menuModel.reset();
        // LookAndFeelはウィンドウより長生きさせ、参照を外してから破棄する
        mainWindow.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel.reset();
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

    // ---- Fileメニューのコマンド（ApplicationCommandTarget）----
    // 実体は現在のMainComponentへ委譲する。プロジェクト未オープン（選択画面）や
    // バウンス中はdisabled（enable状態はメニュー再構築時にここから引き直される）

    void getAllCommands (juce::Array<juce::CommandID>& commands) override
    {
        JUCEApplication::getAllCommands (commands);
        for (const auto& item : MenuCommands::items)
            commands.add (item.command);
    }

    void getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info) override
    {
        auto* mainComp = mainWindow != nullptr ? mainWindow->currentMainComponent() : nullptr;
        const bool ready = mainComp != nullptr && ! mainComp->isBouncing();

        switch (id)
        {
            case MenuCommands::save:
                info.setInfo (Shortcuts::name (Shortcuts::ID::save), {}, "File", 0);
                info.setActive (ready);
                return;
            case MenuCommands::bounce:
                info.setInfo (Shortcuts::name (Shortcuts::ID::bounce) + jp (u8"…"), {}, "File", 0);
                info.setActive (ready);
                return;
            case MenuCommands::closeProject:
                info.setInfo (jp (u8"プロジェクトを閉じる"), {}, "File", 0);
                info.setActive (ready);
                return;
            default:
                JUCEApplication::getCommandInfo (id, info);
                return;
        }
    }

    bool perform (const InvocationInfo& info) override
    {
        // メニューのenable状態はNSMenu構築時のもので古いことがあるため、ここでも必ずガードする
        auto* mainComp = mainWindow != nullptr ? mainWindow->currentMainComponent() : nullptr;
        const bool ready = mainComp != nullptr && ! mainComp->isBouncing();

        switch (info.commandID)
        {
            case MenuCommands::save:
                if (ready)
                    mainComp->trySave();
                return true;
            case MenuCommands::bounce:
                if (ready)
                    mainComp->startBounceFlow();
                return true;
            case MenuCommands::closeProject:
                if (ready)
                    mainWindow->closeProjectToChooser();
                return true;
            default:
                return JUCEApplication::perform (info);
        }
    }

private:
    void rebuildMacMainMenu (bool canCheckForUpdates)
    {
        if (menuModel == nullptr)
            menuModel = std::make_unique<AppMenuModel> (commandManager);

        juce::PopupMenu extraAppleMenuItems;
        juce::PopupMenu::Item checkItem (jp (u8"Check for Updates…"));
        checkItem.isEnabled = canCheckForUpdates;
        checkItem.action = []
        {
            Log::info ("update.check_started", "source=menu");
            SparkleBridge::checkForUpdates();
        };
        extraAppleMenuItems.addItem (std::move (checkItem));
        juce::MenuBarModel::setMacMainMenu (menuModel.get(), &extraAppleMenuItems);
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow()
            : DocumentWindow (DAW_APP_NAME,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, true);
            showChooser();
            setVisible (true);
        }

        // フォーカス復帰時に選択画面の一覧を読み直す（Finderでのリネーム・削除・追加を反映）
        void activeWindowStatusChanged() override
        {
            DocumentWindow::activeWindowStatusChanged();
            if (isActiveWindow())
                if (auto* chooser = dynamic_cast<ProjectChooserComponent*> (getContentComponent()))
                    chooser->refreshList();
        }

        // バツ: プロジェクト表示中はプロジェクトだけ閉じて選択画面へ。選択画面表示中はアプリ終了
        void closeButtonPressed() override
        {
            if (mainComp != nullptr)
                closeProjectToChooser();
            else
                juce::JUCEApplication::getInstance()->quit();
        }

        // ⌘Q・メニューからの終了
        void attemptQuit()
        {
            if (mainComp == nullptr)
            {
                juce::JUCEApplication::getInstance()->quit();
                return;
            }
            confirmCloseProject (true, [] (MainWindow&)
            {
                juce::JUCEApplication::getInstance()->quit();
            });
        }

        // Fileメニューのenable判定・コマンド委譲用（所有はsetContentOwned側のまま）
        MainComponent* currentMainComponent() const { return mainComp; }

        // Fileメニュー「プロジェクトを閉じる」からも呼ばれる
        void closeProjectToChooser()
        {
            confirmCloseProject (false, [] (MainWindow& w)
            {
                // keyPressed（⌘O）実行中にMainComponent自身を破棄しないよう遷移を遅延する
                juce::Component::SafePointer<MainWindow> safe (&w);
                juce::MessageManager::callAsync ([safe]
                {
                    if (safe != nullptr)
                    {
                        safe->showChooser();
                        safe->flowPending = false;
                    }
                });
            });
        }

    private:
        void showChooser()
        {
            mainComp = nullptr;
            auto* chooser = new ProjectChooserComponent();
            chooser->onProjectOpened = [this] (std::unique_ptr<Project> project)
            {
                // openRow()実行中にchooser自身をsetContentOwnedで破棄しないよう、
                // 遷移はコールスタックを抜けてから行う
                if (flowPending)
                    return;
                flowPending = true;
                pendingProject = std::move (project);
                juce::Component::SafePointer<MainWindow> safe (this);
                juce::MessageManager::callAsync ([safe]
                {
                    if (safe != nullptr)
                        safe->openPendingProject();
                });
            };
            setContentOwned (chooser, true);
            setName (DAW_APP_NAME);
            centreWithSize (getWidth(), getHeight());

            // 選択画面ではFileメニューをdisabledにする（enable判定の引き直し）
            if (auto* model = juce::MenuBarModel::getMacMainMenu())
                model->menuItemsChanged();
        }

        void openPendingProject()
        {
            auto* component = new MainComponent (std::move (pendingProject));
            component->onTitleChanged = [this] (const juce::String& title) { setName (title); };
            component->onOpenChooserRequested = [this] { closeProjectToChooser(); };
            mainComp = component;
            setContentOwned (component, true);
            setName (component->windowTitle());
            centreWithSize (getWidth(), getHeight());
            flowPending = false;

            // プロジェクトが開いたのでFileメニューをenabledにする
            if (auto* model = juce::MenuBarModel::getMacMainMenu())
                model->menuItemsChanged();
        }

        // 未保存確認 → onClosed。録音中は先に録音を確定（クリップ化）してから確認する。
        // quitting はダイアログ文言の切り替え（終了/閉じる）のみ。
        // flowPending は確認〜遷移完了までの再入ガード（バツ/⌘O/⌘Q/ダブルクリックの連打対策）で、
        // キャンセル・保存失敗・遷移完了で戻す
        void confirmCloseProject (bool quitting, std::function<void (MainWindow&)> onClosed)
        {
            if (flowPending || mainComp == nullptr)
                return;
            flowPending = true;

            // バウンス中の閉じる/終了（バツボタン・⌘Q。メニューはdisabled）は
            // キャンセル→ワーカーjoin→一時ファイル削除を待ってから進める
            mainComp->cancelBounceForClose();
            mainComp->finishRecordingForClose();

            if (! mainComp->hasUnsavedChanges())
            {
                onClosed (*this);
                return;
            }

            juce::Component::SafePointer<MainWindow> safe (this);
            juce::NativeMessageBox::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::QuestionIcon)
                    .withTitle (jp (u8"未保存の変更があります"))
                    .withMessage (jp (u8"プロジェクトを保存しますか？"))
                    .withButton (quitting ? jp (u8"保存して終了") : jp (u8"保存して閉じる"))
                    .withButton (quitting ? jp (u8"保存せず終了") : jp (u8"保存せず閉じる"))
                    .withButton (jp (u8"キャンセル")),
                [safe, onClosed] (int result)
                {
                    if (safe == nullptr)
                        return; // shutdownとの競合: ウィンドウは破棄済み
                    if (result == 0)
                    {
                        if (safe->mainComp != nullptr && safe->mainComp->trySave())
                            onClosed (*safe);
                        else
                            safe->flowPending = false; // 保存失敗: 閉じない
                    }
                    else if (result == 1)
                    {
                        onClosed (*safe);
                    }
                    else
                    {
                        safe->flowPending = false; // キャンセル
                    }
                });
        }

        MainComponent* mainComp = nullptr;          // 所有はsetContentOwned側
        std::unique_ptr<Project> pendingProject;    // callAsyncでの遷移待ちプロジェクト
        bool flowPending = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<AppLookAndFeel> lookAndFeel;
    juce::ApplicationCommandManager commandManager; // menuModelより先に構築・後に破棄（menuModelが参照する）
    std::unique_ptr<AppMenuModel> menuModel;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (DawApplication)
