#pragma once

#include <functional>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"
#include "../shared/TransportState.h"

// 小節ルーラー＋トラックレーン（クリップ・再生ヘッド）の表示とシーク・クリップ選択。
// 横スクロールはルーラーと、縦スクロールはトラックヘッダ（TrackHeadersView）と同期する。
class TimelineView : public juce::Component,
                     private juce::Timer
{
public:
    struct ClipSelection
    {
        int track = -1;
        int clip = -1;
        bool isValid() const { return track >= 0 && clip >= 0; }
        void clear() { track = clip = -1; }
    };

    struct RegionSelection
    {
        int track = -1;
        int region = -1;
        bool isValid() const { return track >= 0 && region >= 0; }
        void clear() { track = region = -1; }
    };

    static constexpr double minPxPerBar = 20.0;   // これ以下は小節線のみの俯瞰表示
    static constexpr double maxPxPerBar = 640.0;  // 1/16音符グリッドが40px間隔になる上限
    static constexpr int trackHeight = 84;        // ヘッダの3行レイアウト（名前/M・S・音量/楽器）に合わせる
    static constexpr int rulerHeight = 26;

    explicit TimelineView (TransportState& transportState);
    ~TimelineView() override;

    void setProject (Project* p);
    void setSelectedTrack (int index);
    void refresh(); // モデル変更後に呼ぶ（サイズ再計算＋再描画）

    const ClipSelection& getSelection() const { return selection; }
    const RegionSelection& getRegionSelection() const { return regionSelection; }
    void clearSelection();

    std::function<void()> onSelectionChanged;
    std::function<void (int)> onTrackSelected;       // レーンクリックでトラック選択
    std::function<void (juce::int64)> onSeek;        // 表示グリッドにスナップ済みのサンプル位置
    std::function<void (int)> onVerticalScroll;      // ヘッダ同期用（viewYを渡す）

    // MIDIリージョン編集（作成・移動・リサイズ・複製はTimelineViewがモデルを直接書く）
    std::function<void()> onWillEditModel;           // 編集の直前（undoスナップショット用）
    std::function<void()> onModelEdited;             // 編集の確定後（pushSnapshot・dirty用）
    std::function<void (int, int)> onOpenRegion;     // リージョンをダブルクリック（track, region）
    std::function<void (int, int)> onDeleteItemRequested; // 右クリックメニューの削除（track, クリップorリージョンindex）

    // リージョン単位の操作。対象は引数で明示する（「現在の選択」を暗黙に読まない）。
    // itemIndex はオーディオトラックなら clips、MIDIトラックなら midiRegions のindex
    void toggleMuteAt (int trackIndex, int itemIndex);
    void duplicateAt (int trackIndex, int itemIndex); // 複製を元の終端直後に置いて選択する

    void scrollVertically (float wheelDeltaY);       // ヘッダ上のホイールを転送してもらう
    void zoomBy (double factor);                     // 横ズーム（アンカー: 再生ヘッド or ビュー中央）

    // 小節⇔サンプル⇔ピクセルの換算（4/4固定）。MainComponentの表示・録音位置計算からも使う
    double effectiveSampleRate() const;
    double barLengthSamples() const;
    double samplesPerPixel() const;
    int sampleToX (juce::int64 samplePos) const;
    juce::int64 xToSample (int x) const;

    // PPQ⇔ピクセル（MIDIリージョン用）
    double samplesPerTick() const;
    int ppqToX (juce::int64 ppq) const;
    juce::int64 xToPpq (int x) const;
    juce::int64 gridPpq() const;                     // 表示中の最小グリッドのPPQ幅

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class RulerContent;
    class LaneContent;
    class LaneViewport;

    void timerCallback() override;
    int gridDivisionsPerBar() const;
    void zoomAroundContentX (double factor, int contentX);
    void updateContentSize();
    void syncScroll();
    void handleLaneMouseDown (const juce::MouseEvent& e);
    void handleLaneMouseDrag (const juce::MouseEvent& e);
    void handleLaneMouseUp (const juce::MouseEvent& e);
    void handleLaneDoubleClick (const juce::MouseEvent& e);
    void seekFromX (int x);
    int hitTestRegion (int trackIndex, int x) const; // 見つからなければ-1（重なりは後勝ち）
    int hitTestClip (int trackIndex, int x) const;   // 同上（オーディオトラック用）
    void showItemMenu (int trackIndex, int itemIndex); // 右クリックメニュー（ミュート/複製/削除）

    TransportState& transport;
    Project* project = nullptr;
    double pxPerBar = 80.0;   // 1小節の表示幅（ズームで可変）
    ClipSelection selection;
    RegionSelection regionSelection;
    int selectedTrack = 0;

    // MIDIリージョンのドラッグ状態（移動 or 右端リサイズ）。
    // ⌥ドラッグの複製は「実際にドラッグが動いた時点」で行う（mouseDownで作ると、
    // クリックしただけで元と完全に重なった見えない複製が残る事故になるため）
    struct RegionDrag
    {
        enum class Mode { none, move, resize };
        Mode mode = Mode::none;
        int track = -1, region = -1;
        juce::int64 origStartPpq = 0, origLengthPpq = 0;
        int startX = 0;
        bool duplicateOnDrag = false; // ⌥ドラッグ: 最初の移動時に複製してから動かす
        bool edited = false;          // 最初に動いた時点で onWillEditModel を一度だけ呼ぶ
    };
    RegionDrag regionDrag;

    std::unique_ptr<RulerContent> ruler;
    std::unique_ptr<LaneViewport> viewport;
    std::unique_ptr<LaneContent> lanes;

    juce::int64 lastPaintedPlayhead = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};
