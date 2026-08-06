#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/GachaSession.h"
#include "../shared/Project.h"

// 右パネル第3モード「ガチャ」の中身。パーツ（Drums / Bass）切り替え・カード選択・
// 候補8件の一覧・レーンロック・振り直す/残す。カード・候補・ロックは**パーツごとに独立**
// （ドラムは曲A・ベースは曲Bのカード参照が成立する）。モデルへの仮配置・確定は
// MainComponent（GachaSession 経由）が行い、このビューは表示と操作の通知だけを担当する。
class GachaPanelView : public juce::Component,
                       private juce::ListBoxModel
{
public:
    using Part = GachaSession::Part;

    GachaPanelView();

    void setProject (Project* projectToUse);
    void refreshCards();       // <project>/references/*/card.json を列挙し直す（モードを開くたび）
    void refreshAvailability(); // ツール（~/daw）の存在チェック

    Part selectedPart() const { return currentPart; }

    std::function<void()> onRoll;        // 振り直す（パーツ・カード・ロックは getter で読む）
    std::function<void (int)> onPick;    // 候補クリック（index は candidates() の添字）
    std::function<void()> onKeep;        // 残す（全パーツ一括確定）
    std::function<void (Part)> onCardChanged; // カード変更（そのパーツの仮配置の撤去用）
    std::function<void()> onAlignReference; // 「原曲を頭出し」（実行側がクリック時に再解決する）
    // 頭出しの可否問い合わせ（空文字=可、非空=理由。source/groove/gates のファイルI/Oを含むため
    // updateControls 経由でのみ呼ぶ — タイマーから毎tick呼んではいけない）
    std::function<juce::String()> alignUnavailableReason;
    // レポートボタン（report.md 有→開く/閉じる・無→書く、の分岐は実行側が判断する）
    std::function<void()> onReportAction;
    std::function<void()> onRewriteReport; // ボタン右クリック「書き直す」（上書き確認は実行側）
    std::function<void()> onCancelReport;  // 生成中の経過行のキャンセル
    // レポートウィンドウが表示中のフォルダ（非表示は空File。ボタンのON表示判定に使う）
    std::function<juce::File()> reportWindowFolder;
    // レポート生成が走行中のフォルダ（未走行は空File。経過行の表示と多重起動の禁止に使う）
    std::function<juce::File()> reportGeneratingFolder;

    // 頭出しボタンの有効状態を再評価する（構造編集後・録音の開始/終了に MainComponent から呼ぶ）
    void refreshAlignAvailability() { updateControls(); }
    // レポートボタンの表示を再評価する（ウィンドウの開閉・生成の開始/終了に MainComponent から呼ぶ。
    // ボタン⇄経過行の切り替えを含むため resized も打ち直す）
    void refreshReportButton()
    {
        updateControls();
        resized();
    }
    // 生成中の経過行テキストだけを更新する（MainComponent のタイマーから毎tick。
    // updateControls はファイルI/Oを含むのでここから呼ばない）
    void setReportProgress (const juce::String& text)
    {
        reportProgressLabel.setText (text, juce::dontSendNotification);
    }
    // 下部ガイドへ一時メッセージを出す（クリック時の再解決失敗の理由など）
    void showStatus (const juce::String& text);

    juce::File selectedCardFolder() const; // 表示中パーツの選択カード（未選択は空File）
    juce::File selectedCardFolder (Part part) const;

    // ロック中レーンの seed（8桁hex。空 = ロックなし）。トグルをONにした瞬間に選択中候補から
    // 確保する（振り直しで選択が外れても、表示がONなら必ずこの seed が --lock に渡る —
    // 「点灯しているのに次の振り直しで変わる」を作らない）。
    // drums パーツ: kick/snare/hat、bass パーツ: prog/rhythm
    juce::String lockedKickSeed() const { return parts[0].locks[0]; }
    juce::String lockedSnareSeed() const { return parts[0].locks[1]; }
    juce::String lockedHatSeed() const { return parts[0].locks[2]; }
    juce::String lockedProgSeed() const { return parts[1].locks[0]; }
    juce::String lockedRhythmSeed() const { return parts[1].locks[1]; }

    void setCandidates (std::vector<GachaSession::Candidate> list); // 表示中パーツへ反映
    const std::vector<GachaSession::Candidate>& candidates() const
    {
        return parts[(size_t) currentPart].items;
    }
    int selectedCandidate() const { return listBox.getSelectedRow(); }
    void clearCandidateSelection() { listBox.deselectAllRows(); }
    // パーツの仮配置の有無（選択行の「▶ 仮配置中」バッジ・案内文が連動する）。
    // 「残す」は全パーツ一括確定なので、バッジはどのパーツから押しても同じ意味
    void setPreviewActive (Part part, bool active);
    void clearAllPreviewBadges(); // 「残す」確定・全撤去後（両パーツのバッジ・選択を畳む）

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class KeepChipRow; // 選択行の「仮配置中」バッジ（ホバーで「ビートを残す」ボタンに変わる）

    // パーツごとに独立して保持する状態（切り替えでウィジェットに入れ替える）
    struct PartUiState
    {
        juce::File cardFolder;                       // 選択中カード（切替時に復元）
        std::vector<GachaSession::Candidate> items;  // 候補一覧
        juce::String locks[3];                       // 確保済み seed（空=ロックなし）
        bool previewActive = false;                  // 仮配置中（バッジ表示）
    };

    int getNumRows() override { return (int) parts[(size_t) currentPart].items.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
    juce::Component* refreshComponentForRow (int row, bool selected,
                                             juce::Component* existing) override;
    void listBoxItemClicked (int row, const juce::MouseEvent& e) override;
    void selectedRowsChanged (int) override; // ロックトグルの有効/無効が選択に依存する
    void switchPart (Part part);   // タブ切り替え（状態の退避・復元）
    void applyCardSelection (const juce::File& folder); // cardBox へ通知なしで反映
    void updateControls(); // 全ロック時の「振り直す」無効化・案内文の切り替え
    void clearLocks();     // トグルOFF＋確保済みseedの破棄（カード変更時）
    void handleLockToggle (int laneIndex);
    int numLanes() const { return currentPart == Part::drums ? 3 : 2; }
    const char* laneName (int laneIndex) const;

    Project* project = nullptr;
    bool toolsAvailable = false;
    bool bassToolAvailable = false;
    Part currentPart = Part::drums;
    PartUiState parts[GachaSession::numParts];
    std::vector<juce::File> cardFolders; // コンボの並びと対応

    juce::TextButton drumsTab { "Drums" }, bassTab { "Bass" }; // パーツ切り替え（ラジオ動作）
    juce::ComboBox cardBox;
    juce::Label lockCaption;   // トグル行の「ロック」ラベル
    juce::TextButton lockButtons[3]; // drums: kick/snare/hat / bass: 進行/リズム（3つ目は隠す）
    juce::TextButton rollButton; // 「残す」は独立ボタンでなく選択行のバッジ（KeepChipRow）が担う
    juce::TextButton alignButton; // 原曲を頭出し（カード行の右端）
    // 右クリック（書き直しメニュー）を通常クリックと分離するボタン。素の TextButton は
    // 右クリックでも onClick が発火するため、popup 系の押下は Button に渡さない。
    // placeholder = レポート未生成（押すと作る）。破線枠で「まだ無い」を形で示し、
    // 実線の「開く」（押すと読む）とひと目で区別する。文字の沈みは textColourOffId 側で行う
    struct ReportButton final : juce::TextButton
    {
        std::function<void()> onRightClick;
        bool placeholder = false;
        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            if (! placeholder)
            {
                juce::TextButton::paintButton (g, highlighted, down);
                return;
            }
            // 塗り・角丸・hover/押下の変化量は AppLookAndFeel::drawButtonBackground と揃える
            const float cornerSize = 6.0f;
            const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
            auto fill = findColour (buttonColourId).withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f);
            if (down)
                fill = fill.darker (0.25f);
            else if (highlighted)
                fill = fill.brighter (0.08f);
            g.setColour (fill);
            g.fillRoundedRectangle (bounds, cornerSize);

            juce::Path outline;
            outline.addRoundedRectangle (bounds, cornerSize);
            juce::Path dashed;
            const float dashes[] = { 4.0f, 3.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (findColour (juce::ComboBox::outlineColourId)
                             .withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f));
            g.fillPath (dashed);

            getLookAndFeel().drawButtonText (g, *this, highlighted, down);
        }
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu())
            {
                if (onRightClick != nullptr)
                    onRightClick();
                return;
            }
            juce::TextButton::mouseDown (e);
        }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! e.mods.isPopupMenu())
                juce::TextButton::mouseUp (e);
        }
    };

    ReportButton reportButton; // レポートを書く/開く（カード行の下。ウィンドウ表示中はON表示）
    juce::Label reportProgressLabel;      // 生成中の経過行（ボタンと排他表示）
    juce::TextButton reportCancelButton;  // 経過行の右端のキャンセル
    juce::ListBox listBox { {}, this };
    juce::Label infoLabel;   // ツール不在・カード無しの案内（一覧の代わりに出す）
    juce::Label statusLabel; // 下部の1行ガイド（手順・仮配置中・全ロック・ロック不可の理由）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GachaPanelView)
};
