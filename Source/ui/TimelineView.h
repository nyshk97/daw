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
    static constexpr int trackHeight = 92;        // ヘッダの3行レイアウト（名前/M・S・音量/楽器）＋上下余白に合わせる
    static constexpr int rulerHeight = 26;
    static constexpr int markerLaneHeight = 18;   // ルーラー直下のセクションマーカー帯
    static constexpr int topHeight = rulerHeight + markerLaneHeight; // レーン上端（ヘッダ側の高さ合わせ用）

    explicit TimelineView (TransportState& transportState);
    ~TimelineView() override;

    void setProject (Project* p);
    void setSelectedTrack (int index);
    void refresh(); // モデル変更後に呼ぶ（サイズ再計算＋再描画）

    const ClipSelection& getSelection() const { return selection; }
    const RegionSelection& getRegionSelection() const { return regionSelection; }
    void clearSelection();

    // トラック並び替え後の選択の引き直し（並び替えでクリップ/リージョンのindexは変わらないため
    // トラックindexだけ差し替える。-1 = 対象トラックが見つからない → その選択を解除）
    void remapSelectionTracks (int newClipTrack, int newRegionTrack);

    std::function<void()> onSelectionChanged;
    std::function<void (int)> onTrackSelected;       // レーンクリックでトラック選択
    std::function<void (juce::int64)> onSeek;        // 表示グリッドにスナップ済みのサンプル位置
    std::function<void (int)> onVerticalScroll;      // ヘッダ同期用（viewYを渡す）

    // リージョン/クリップ編集（作成・移動・トラック跨ぎ・リサイズ・複製はTimelineViewがモデルを直接書く）
    std::function<void()> onWillEditModel;           // 編集の直前（undoスナップショット用）
    std::function<void()> onModelEdited;             // 編集の確定後（pushSnapshot・dirty用）
    std::function<void (int, int)> onOpenRegion;     // リージョンをダブルクリック（track, region）
    std::function<void (int, int)> onDeleteItemRequested; // 右クリックメニューの削除（track, クリップorリージョンindex）

    // リージョン単位の操作。対象は引数で明示する（「現在の選択」を暗黙に読まない）。
    // itemIndex はオーディオトラックなら clips、MIDIトラックなら midiRegions のindex
    void toggleMuteAt (int trackIndex, int itemIndex);
    void duplicateAt (int trackIndex, int itemIndex);      // 複製を元の終端直後に置いて選択する
    void splitAtPlayhead (int trackIndex, int itemIndex);  // 再生ヘッド位置で2分割（範囲外はno-op）。左側を選択する

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

    // 拍（曲頭から0始まり・4/4固定）⇔ サンプル・ピクセル。セクションマーカーの描画・追加・
    // シークは必ずここを経由する（1始まり小節との換算を呼び出し側に書かせない）
    juce::int64 beatStartSample (int beats) const;
    int beatToX (int beats) const;
    int snapUnitBeats() const;                       // マーカーのスナップ刻み（表示グリッド準拠・上限=拍）: 4/2/1拍
    int xToMarkerBeats (int x) const;                // xを含むスナップ枠の頭の拍位置（floor）。>= 0

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class RulerContent;
    class MarkerLaneContent;
    class LaneContent;
    class LaneViewport;

    void timerCallback() override;
    juce::uint64 trackDimMask() const;               // 各トラック行の減光状態（変更検知用。ミュート＋ソロ由来）
    int gridDivisionsPerBar() const;
    void zoomAroundContentX (double factor, int contentX);
    void updateContentSize();
    void syncScroll();
    void handleLaneMouseDown (const juce::MouseEvent& e);
    void handleLaneMouseDrag (const juce::MouseEvent& e);
    void handleLaneMouseUp (const juce::MouseEvent& e);
    void handleLaneDoubleClick (const juce::MouseEvent& e);
    void seekFromX (int x);
    juce::int64 playheadPpq() const;                 // 再生ヘッド位置をPPQへ換算（最近傍tickへ丸め）
    int hitTestRegion (int trackIndex, int x) const; // 見つからなければ-1（重なりは後勝ち）
    int hitTestClip (int trackIndex, int x) const;   // 同上（オーディオトラック用）
    void showItemMenu (int trackIndex, int itemIndex); // 右クリックメニュー（ミュート/複製/削除）

    // セクションマーカー（マーカーレーンの操作。モデル編集は SectionMarkers ヘルパー経由）
    int hitTestMarker (int x) const;                 // xが属するマーカー区間のindex（最初のマーカーより前は-1）
    int hitTestMarkerEdge (int x) const;             // 開始境界±4pxのマーカーindex。なければ-1
    void handleMarkerLaneMouseDown (const juce::MouseEvent& e);
    void handleMarkerLaneMouseDrag (const juce::MouseEvent& e);
    void handleMarkerLaneMouseUp (const juce::MouseEvent& e);
    void handleMarkerLaneMouseMove (const juce::MouseEvent& e);
    void showAddMarkerMenu (int beats);              // 空白クリック: 6種から選んで追加
    void showMarkerMenu (int markerIndex, int clickedBeats); // 既存マーカー右クリック: 追加(=分割)/種別変更/削除
    void addMarkerAt (int beats, SectionType type);
    void changeMarkerType (int index, SectionType type);
    void removeMarker (int index);

    TransportState& transport;
    Project* project = nullptr;
    double pxPerBar = 80.0;   // 1小節の表示幅（ズームで可変）
    ClipSelection selection;
    RegionSelection regionSelection;
    int selectedTrack = 0;

    // リージョン/クリップのドラッグ状態（移動・同種トラックへの跨ぎ移動・MIDIのみ右端リサイズ）。
    // ⌥ドラッグの複製は「実際にドラッグが動いた時点」で行う（mouseDownで作ると、
    // クリックしただけで元と完全に重なった見えない複製が残る事故になるため）
    struct RegionDrag
    {
        enum class Mode { none, move, resize };
        Mode mode = Mode::none;
        bool isMidi = false;        // true = midiRegions / false = clips を対象にする
        int track = -1, item = -1;  // item は clips or midiRegions のindex
        juce::int64 origStartPpq = 0, origLengthPpq = 0; // MIDI用
        juce::int64 origStartSample = 0;                 // オーディオ用
        int startX = 0;
        bool duplicateOnDrag = false; // ⌥ドラッグ: 最初の移動時に複製してから動かす
        bool edited = false;          // 実際に動いた時点で onWillEditModel を一度だけ呼ぶ
    };
    RegionDrag regionDrag;

    // マーカーのドラッグ状態。本体ドラッグ＝開始位置の相対移動、クリック（動かさず離す）＝セクション頭へシーク。
    // 最初に実際へ動いた時点で onWillEditModel を一度だけ呼ぶ（RegionDragと同じ方針）
    struct MarkerDrag
    {
        int index = -1;
        int origStartBeats = 0;
        int startX = 0;
        bool fromEdge = false;   // 境界を掴んだ（吸着移動。本体は相対移動）
        bool edited = false;
    };
    MarkerDrag markerDrag;

    std::unique_ptr<RulerContent> ruler;
    std::unique_ptr<MarkerLaneContent> markerLane;
    std::unique_ptr<LaneViewport> viewport;
    std::unique_ptr<LaneContent> lanes;

    juce::int64 lastPaintedPlayhead = -1;
    juce::uint64 lastPaintedTrackDimMask = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};
