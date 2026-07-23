#pragma once

#include <functional>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

#include "SendKnob.h"
#include "StereoMeter.h"
#include "../shared/Project.h"

// 左のFXパネル（Logicのインスペクタ相当・基本常設で I トグル）。
// 選択チャンネルの固定チェーンを「スロット一覧＋Sendsノブ」の概要として縦に表示する:
//   トラック = [EQ][Comp][Ext(グレーアウト)] ＋ Sends
//   バス     = [Reverb] / [Delay]、Master = [Limiter]
// スロットのクリックで下部に詳細エディタ（FxDetailView）が開く（Logicのフローティングの代替）。
// トラックヘッダーのさらに左に置かれ、ピアノロール（下部）とは独立。
class FxEditorView : public juce::Component
{
public:
    static constexpr int preferredWidth = 232;

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
    void remapTrack (int newIndex);

    // ---- 下部詳細エディタとの連携（状態管理はMainComponent側）----
    int numSlots() const { return (int) slots.size(); }
    juce::String slotName (int slot) const;   // "EQ" / "Comp" / "Reverb" 等（範囲外は空）
    juce::String channelName() const { return titleName; }
    juce::String targetKey() const;           // "track" / "bus0".."bus2" / "master"（詳細の追従判定用）
    void setActiveSlot (int slot);            // 詳細を開いているスロットのハイライト（-1=なし）

    // メーター値の配布（30Hz）。peakL/peakR の exchange(0) と maxSincePlay の蓄積・リセットは
    // MainComponent が一元的に行う。表示対象（トラック/バス/Master）のぶんだけ使う
    void updateMeters (const std::vector<MeterFeed>& trackFeeds,
                       const MeterFeed (&busFeeds)[numSendBuses], const MeterFeed& masterFeed);

    std::function<void (int)> onSlotClicked;  // グレーアウト以外のスロットのクリック
    std::function<void()> onCloseRequested;   // ✕ボタン
    // sendはミキサーと同じatomicを表示するため相互refreshが要るが、EQ/CompのON/OFFは
    // ミキサーに表示がないためdirty化のみでよい。呼び出し側の同期範囲が違うので分ける
    std::function<void()> onSendChanged;
    std::function<void()> onFxEnabledChanged;
    std::function<void()> onVolumeChanged;    // 音量はヘッダー・ミキサーと同じatomicの表示（両方へ反映が要る）

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

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

    // スロット行（resizedで構築、paint/mouseDownで使用）
    struct Slot
    {
        juce::Rectangle<int> bounds;
        juce::String name;
        bool grayed = false;
    };
    std::vector<Slot> slots;
    juce::Rectangle<int> sendsArea;         // Sends区画（見出し＋ノブ。トラックのみ）
    juce::Rectangle<int> volumeArea;        // Volume区画（見出し＋フェーダー/メーター。全チャンネル共通）
    juce::Rectangle<int> volumeReadoutArea; // dB数値ボックスのペア（設定値・ピーク）
    float peakMaxDisplay = 0.0f;            // 再生開始からの最大ピーク（dB数値表示用）

    juce::TextButton closeButton { juce::String::fromUTF8 (u8"×") };
    juce::Slider volumeSlider;   // Logicのチャンネルストリップと同じ「フェーダー＋メーター分離」配置
    StereoMeter meter;
    juce::TextButton eqButton { "ON" };   // トラックのみ（EQ行の右端）
    juce::TextButton compButton { "ON" }; // トラックのみ（Comp行の右端）
    SendKnob sendKnobs[numSendBuses] { SendKnob (0), SendKnob (1), SendKnob (2) }; // トラックのみ

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxEditorView)
};
