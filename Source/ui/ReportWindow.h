#pragma once

#include <functional>
#include <juce_gui_extra/juce_gui_extra.h>

#include "Theme.h"
#include "../shared/Log.h"
#include "../shared/ReferenceReport.h"
#include "../shared/ReferenceTools.h"

// 分析レポート（references/<名前>/report.md）の閲覧ウィンドウ。MixerWindowと同型の
// 1枚使い回し（閉じる＝非表示・位置サイズはセッション内維持）で、開くとき report.md より
// HTML キャッシュが古ければ render_report.py を同期実行してから WKWebView で表示する。
// 読み物専用と割り切り、LaLaショートカットの転送はしない（WKWebViewがキーを食うため。
// テキスト選択・⌘C・スクロールはWKWebView標準で効く。⌘Fの検索UIは無い — Phase 1 実測）。
class ReportWindow : public juce::DocumentWindow
{
public:
    ReportWindow()
        : juce::DocumentWindow (juce::String::fromUTF8 (u8"レポート"),
                                Theme::windowBg, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentNonOwned (&browser, false);
        setResizable (true, false);
        setResizeLimits (480, 400, 4096, 2160);
    }

    ~ReportWindow() override { clearContentComponent(); }

    // 開く（初回はメインウィンドウ中央・以後は前回の位置とサイズ）。
    // 変換が要るのに失敗したときは古い HTML を表示せず、理由をダイアログに出して false を返す
    bool openFor (const juce::File& referenceFolder, juce::Component* alignTo)
    {
        if (ReferenceReport::needsRender (referenceFolder, ReferenceTools::renderScript()))
        {
            juce::String error;
            if (! render (referenceFolder, error))
            {
                Log::error ("report.render.fail", "folder=" + referenceFolder.getFileName()
                                                      + " message=" + error.replace ("\n", " / "));
                // 「失敗時は古いHTMLを表示しない」— 同じフォルダを表示中なら閉じる
                //（書き直し成功→自動再読込→変換失敗、の経路で古い内容が残り続けるのを防ぐ）
                if (showingFolder() == referenceFolder)
                    dismiss();
                juce::NativeMessageBox::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon,
                    juce::String::fromUTF8 (u8"レポートを表示できません"), error);
                return false;
            }
        }

        folder = referenceFolder;
        browser.allowedFile = ReferenceReport::reportHtml (referenceFolder);
        browser.goToURL (juce::URL (browser.allowedFile).toString (false));
        setName (referenceFolder.getFileName() + juce::String::fromUTF8 (u8" — レポート"));

        if (! placed)
        {
            centreAroundComponent (alignTo, 780, 720);
            placed = true;
        }
        setVisible (true);
        toFront (true);
        Log::info ("report.open", "folder=" + referenceFolder.getFileName());
        return true;
    }

    // 生成完了後の自動再読込（同じフォルダを表示中のときだけ呼ばれる想定だが、念のため自分でも確認）
    void reloadIfShowing (const juce::File& referenceFolder)
    {
        if (showingFolder() != referenceFolder)
            return;
        openFor (referenceFolder, nullptr); // placed済みなので位置はそのまま
    }

    // 表示中のフォルダ（非表示なら空File）。ガチャパネルのボタンON判定が使う
    juce::File showingFolder() const { return isVisible() ? folder : juce::File(); }

    // 閉じる＝非表示（クローズボタン経由もここを通る）。onDismissed で通知する
    // （ガチャパネルのボタンON表示を戻すため）
    void dismiss()
    {
        if (! isVisible())
            return;
        setVisible (false);
        if (onDismissed)
            onDismissed();
    }

    std::function<void()> onDismissed;

private:
    // リンクの扱い: 自分の report.html と about:blank だけ WKWebView に描かせ、
    // それ以外（listen/ のwav・analysis/ のJSON・外部URL）は外部デフォルトアプリで開く。
    // 埋め込みリソース（img/script）はここを通らないため、HTML側のCSPが塞ぐ（Phase 1 実測で
    // about:blank が最初に必ず来ることを確認済み — 拒否すると初期化が壊れるので許可する）
    class ReportBrowser : public juce::WebBrowserComponent
    {
    public:
        bool pageAboutToLoad (const juce::String& url) override
        {
            if (url == "about:blank")
                return true;
            const juce::URL parsed (url);
            if (parsed.isLocalFile())
            {
                const auto file = parsed.getLocalFile();
                if (file == allowedFile)
                    return true;
                Log::info ("report.link.file", "path=" + file.getFullPathName());
                file.startAsProcess();
                return false;
            }
            Log::info ("report.link.external", "url=" + url);
            parsed.launchInDefaultBrowser();
            return false;
        }

        juce::File allowedFile;
    };

    // render_report.py の同期実行（数百ms想定・上限10秒）。失敗時は stderr を error に載せる
    static bool render (const juce::File& referenceFolder, juce::String& error)
    {
        juce::ChildProcess process;
        const juce::StringArray args { ReferenceTools::venvPython().getFullPathName(),
                                       ReferenceTools::renderScript().getFullPathName(),
                                       referenceFolder.getFullPathName() };
        if (! process.start (args))
        {
            error = juce::String::fromUTF8 (u8"変換プロセスを起動できません（~/daw のツールを確認してください）");
            return false;
        }
        if (! process.waitForProcessToFinish (10000))
        {
            process.kill();
            error = juce::String::fromUTF8 (u8"変換がタイムアウトしました（10秒）");
            return false;
        }
        if (process.getExitCode() != 0)
        {
            error = process.readAllProcessOutput().trim();
            if (error.isEmpty())
                error = juce::String::fromUTF8 (u8"変換が失敗しました（exit ")
                        + juce::String (process.getExitCode()) + ")";
            return false;
        }
        return true;
    }

    void closeButtonPressed() override
    {
        Log::info ("report.close", "source=windowclose");
        dismiss();
    }

    ReportBrowser browser;
    juce::File folder;
    bool placed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReportWindow)
};
