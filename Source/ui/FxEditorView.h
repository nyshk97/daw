#pragma once

#include <atomic>
#include <functional>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "FxSlotLayout.h"
#include "SendRow.h"
#include "SlotPill.h"
#include "StereoMeter.h"
#include "../shared/Project.h"

// 左のFXパネル（Logicのインスペクタ相当・基本常設で I トグル）。
// 選択チャンネルをLogicのチャンネルストリップ準拠の縦並びで表示する:
//   トラック = EQサムネイル → スロット [EQ][Comp][Ext(空き)] → Sends → Panノブ＋dB数値 → フェーダー/メーター
//   バス     = [Reverb] / [Delay]、Master = [Limiter]（サムネイル・Sends・Panなし）
// スロットはLogic風のピル（ON=青・OFF=グレー）。hoverで「電源｜エディタ」の2分割に変わり、
// 電源=ON/OFFトグル・エディタ側クリックで下部詳細（FxDetailView）が開く（Logicのフローティングの代替）。
// SendsもLogicの行スタイル（バス名ピル＋右に小ノブ。ピルはsend>0で点灯、ノブのドラッグ中はポップアップで送り量0〜100）。
// トラックヘッダーのさらに左に置かれ、ピアノロール（下部）とは独立。
class FxEditorView : public juce::Component
{
public:
    static constexpr int preferredWidth = 156;

    FxEditorView();

    void setProject (Project* p) { project = p; }

    bool isOpen() const { return open; }
    void openView();  // レイアウト（resized）は呼び出し側（MainComponent）が行う
    void closeView();

    // 表示対象。showTrackはトラック追従（選択変更のたびに呼ばれる）。
    // バス/Masterはミキサーのストリップクリックから（ミキサーを閉じると追従に戻る）
    void showTrack (int trackIndex);
    void showBus (int busIndex);
    void showMaster();

    // モデル変更（リネーム・undo・トラック削除）後の防御的同期＋表示値のrebind
    void refreshFromModel (int selectedTrack);
    void refreshValues();

    // トラック並び替え時のindex引き直し用。shownTrackは表示中のトラックindex（バス/Master表示は-1）、
    // remapTrackは表示対象のトラック自体を変えずにindexだけ差し替える（バス/Master表示はno-op）
    int shownTrack() const { return target == Target::track ? targetTrack : -1; }
    // 表示中のバスindex（バス表示以外は-1）。下部詳細（Reverb/Delayエディタ）の対象解決用
    int shownBus() const { return target == Target::bus ? targetBus : -1; }
    void remapTrack (int newIndex);

    // ---- 下部詳細エディタとの連携（状態管理はMainComponent側）----
    // スロット番号の意味と並びは FxSlotLayout.h（FxSlots）が単一の真実の源。
    // 下部エリアの履歴（BottomPanelHistory）が、表示対象を変えずにスロット種別を判定するために参照する
    static constexpr int maxSlots = FxSlots::maxSlots;
    static constexpr int instrumentSlot = FxSlots::instrument;

    // スロット番号が現在の表示対象で有効か（IDは非連続なので個数の上限比較では判定できない。
    // BottomPanelHistory の復元・詳細ビューの追従が使う）
    bool isValidSlot (int slot) const { return slotName (slot).isNotEmpty(); }
    juce::String slotName (int slot) const;   // "EQ" / "Comp" / "Reverb" 等（範囲外・未使用は空）
    FxVisualKind slotKind (int slot) const;   // 固有色・タイトル書式用の表示ID（範囲外・未使用は neutral）
    // Instrumentスロットか（下部エディタに載せる中身を切り替えるためMainComponentが見る）。
    // 「いま表示しているチャンネル」基準の判定なので、バス/Master表示中は常にfalse
    bool isInstrumentSlot (int slot) const { return target == Target::track && slot == instrumentSlot; }
    juce::String channelName() const { return titleName; }
    juce::String targetKey() const;           // "track" / "bus0".."bus2" / "master"（詳細の追従判定用）
    void setActiveSlot (int slot);            // 詳細を開いているスロットのハイライト（-1=なし）

    // メーター値の配布（30Hz）。peakL/peakR の exchange(0) と maxSincePlay の蓄積・リセットは
    // MainComponent が一元的に行う。表示対象（トラック/バス/Master）のぶんだけ使う
    void updateMeters (const std::vector<MeterFeed>& trackFeeds,
                       const MeterFeed (&busFeeds)[numSendBuses], const MeterFeed& masterFeed);

    // EQサムネイルのカーブ計算に使うSR（デバイス追従。EqEditorViewと同じ流儀で未確定は48kで描く）
    std::function<double()> getSampleRate;
    // EQ編集（下部エディタのドラッグ等）にサムネイルを追従させる（呼び出し側=MainComponentが配線）
    void repaintEqThumbnail()
    {
        if (! eqThumbArea.isEmpty())
            repaint (eqThumbArea);
    }

    std::function<void (int)> onSlotClicked;  // 空きスロット以外の「エディタを開く」操作（行クリック・EQサムネイル）
    std::function<void()> onCloseRequested;   // ✕ボタン
    // send/pan/EQ・CompのON/OFFはいずれもミキサーと同じatomicの表示なので相互refreshが要る。
    // 呼び出し側の同期範囲が違う（音量はヘッダーにも出る）ためコールバックを分けている
    std::function<void()> onSendOrPanChanged;
    std::function<void()> onFxEnabledChanged;
    std::function<void()> onVolumeChanged;    // 音量はヘッダー・ミキサーと同じatomicの表示（両方へ反映が要る）

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

private:
    enum class Target { none, track, bus, master };

    void rebind(); // 表示対象のTrackParamsとタイトルを解決してUIを組み直す
    std::shared_ptr<TrackParams> targetParams() const;

    Project* project = nullptr;
    bool open = false;
    Target target = Target::none;
    int targetTrack = -1;
    int targetBus = 0;
    int activeSlot = -1;

    juce::String titleName;

    // スロット（対話込みの実体はSlotPill。名前はfxDetail連携用にここでも持つ。rebindで構成する）。
    // 配列index = スロット番号（意味は FxSlots::Id）。画面上の並びは slotOrder が決める
    //（FxSlots::panelOrder の投影規則。Instrumentは音源なので一番上）
    SlotPill slotPills[maxSlots];
    juce::String slotNames[maxSlots]; // 未使用スロットは空（isValidSlot の判定材料）
    FxVisualKind slotKinds[maxSlots] {};
    int slotOrder[maxSlots] {}; // 上から並べる配列indexの順
    int numOrderedSlots = 0;
    juce::Rectangle<int> eqThumbArea;       // EQサムネイル（トラックのみ。クリック=EQスロットのエディタを開く）
    juce::Rectangle<int> sendsArea;         // Sends区画（見出し＋行。トラックのみ）
    juce::Rectangle<int> volumeReadoutArea; // dB数値ボックスのペア（設定値・ピーク。Panノブの下）
    float peakMaxDisplay = 0.0f;            // 再生開始からの最大ピーク（dB数値表示用）

    bool hoverThumb = false;
    void updateHover (juce::Point<int> pos);

    juce::TextButton closeButton { juce::String::fromUTF8 (u8"×") };
    juce::Slider volumeSlider;   // Logicのチャンネルストリップと同じ「フェーダー＋メーター分離」配置
    juce::Slider panKnob;        // トラックのみ（Logic風の物理ノブ描画はAppLookAndFeel::drawLogicPanKnob）
    StereoMeter meter;
    SendRow sendRows[numSendBuses] { SendRow (0), SendRow (1), SendRow (2) }; // トラックのみ（ミキサーと共有部品）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxEditorView)
};
