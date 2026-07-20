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

    static constexpr double minPxPerBar = 20.0;   // これ以下は小節線のみの俯瞰表示
    static constexpr double maxPxPerBar = 640.0;  // 1/16音符グリッドが40px間隔になる上限
    static constexpr int trackHeight = 64;
    static constexpr int rulerHeight = 26;

    explicit TimelineView (TransportState& transportState);
    ~TimelineView() override;

    void setProject (Project* p);
    void setSelectedTrack (int index);
    void refresh(); // モデル変更後に呼ぶ（サイズ再計算＋再描画）

    const ClipSelection& getSelection() const { return selection; }
    void clearSelection();

    std::function<void()> onSelectionChanged;
    std::function<void (int)> onTrackSelected;       // レーンクリックでトラック選択
    std::function<void (juce::int64)> onSeek;        // 小節スナップ済みサンプル位置
    std::function<void (int)> onVerticalScroll;      // ヘッダ同期用（viewYを渡す）

    void scrollVertically (float wheelDeltaY);       // ヘッダ上のホイールを転送してもらう
    void zoomBy (double factor);                     // 横ズーム（アンカー: 再生ヘッド or ビュー中央）

    // 小節⇔サンプル⇔ピクセルの換算（4/4固定）。MainComponentの表示・録音位置計算からも使う
    double effectiveSampleRate() const;
    double barLengthSamples() const;
    double samplesPerPixel() const;
    int sampleToX (juce::int64 samplePos) const;
    juce::int64 xToSample (int x) const;

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
    void seekFromX (int x);

    TransportState& transport;
    Project* project = nullptr;
    double pxPerBar = 80.0;   // 1小節の表示幅（ズームで可変）
    ClipSelection selection;
    int selectedTrack = 0;

    std::unique_ptr<RulerContent> ruler;
    std::unique_ptr<LaneViewport> viewport;
    std::unique_ptr<LaneContent> lanes;

    juce::int64 lastPaintedPlayhead = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};
