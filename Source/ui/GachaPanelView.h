#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/GachaSession.h"
#include "../shared/Project.h"

// 右パネル第3モード「ガチャ」の中身。カード選択・候補8件の一覧・レーンロック・
// 振り直す/残す。モデルへの仮配置・確定は MainComponent（GachaSession 経由）が行い、
// このビューは表示と操作の通知だけを担当する。
class GachaPanelView : public juce::Component,
                       private juce::ListBoxModel
{
public:
    GachaPanelView();

    void setProject (Project* projectToUse);
    void refreshCards();       // <project>/references/*/card.json を列挙し直す（モードを開くたび）
    void refreshAvailability(); // ツール（~/daw）の存在チェック

    std::function<void()> onRoll;        // 振り直す（カード・ロックは getter で読む）
    std::function<void (int)> onPick;    // 候補クリック（index は candidates() の添字）
    std::function<void()> onKeep;        // 残す
    std::function<void()> onCardChanged; // カード変更（仮配置の撤去用）

    juce::File selectedCardFolder() const; // 選択中カードのリファレンスフォルダ（未選択は空File）

    // ロック中レーンの seed（8桁hex。空 = ロックなし）。トグルをONにした瞬間に選択中候補から
    // 確保する（振り直しで選択が外れても、表示がONなら必ずこの seed が --lock に渡る —
    // 「点灯しているのに次の振り直しで変わる」を作らない）
    juce::String lockedKickSeed() const { return lockedKick; }
    juce::String lockedSnareSeed() const { return lockedSnare; }
    juce::String lockedHatSeed() const { return lockedHat; }

    void setCandidates (std::vector<GachaSession::Candidate> list);
    const std::vector<GachaSession::Candidate>& candidates() const { return items; }
    int selectedCandidate() const { return listBox.getSelectedRow(); }
    void clearCandidateSelection() { listBox.deselectAllRows(); }
    // 仮配置の有無（「残す」の有効/無効・選択行の「▶ 仮配置中」バッジ・案内文が連動する）
    void setKeepEnabled (bool enabled);

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class KeepChipRow; // 選択行の「仮配置中」バッジ（ホバーで「残す」ボタンに変わる）

    int getNumRows() override { return (int) items.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
    juce::Component* refreshComponentForRow (int row, bool selected,
                                             juce::Component* existing) override;
    void listBoxItemClicked (int row, const juce::MouseEvent& e) override;
    void selectedRowsChanged (int) override; // ロックトグルの有効/無効が選択に依存する
    void updateControls(); // 全ロック時の「振り直す」無効化・案内文の切り替え
    void clearLocks();     // トグルOFF＋確保済みseedの破棄（カード変更時）
    void handleLockToggle (juce::TextButton& button, juce::String& stored);

    Project* project = nullptr;
    bool toolsAvailable = false;
    bool previewActive = false; // 仮配置中（選択行のバッジ・「候補Nを残す」表示）
    std::vector<GachaSession::Candidate> items;
    std::vector<juce::File> cardFolders; // コンボの並びと対応
    juce::String lockedKick, lockedSnare, lockedHat; // トグルON時に確保した seed（空=ロックなし）

    juce::ComboBox cardBox;
    juce::Label lockCaption;   // トグル行の「ロック」ラベル
    juce::TextButton kickLock { "kick" }, snareLock { "snare" }, hatLock { "hat" };
    juce::TextButton rollButton; // 「残す」は独立ボタンでなく選択行のバッジ（KeepChipRow）が担う
    juce::ListBox listBox { {}, this };
    juce::Label infoLabel;   // ツール不在・カード無しの案内（一覧の代わりに出す）
    juce::Label statusLabel; // 下部の1行ガイド（手順・仮配置中・全ロック・ロック不可の理由）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GachaPanelView)
};
