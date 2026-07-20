#pragma once

#include <functional>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"
#include "../shared/TransportState.h"

// 下部ペインのピアノロール。MIDIリージョンのダブルクリックで開き、ノートのマウス編集を行う。
// 対象リージョンは trackId / regionId で保持し、undo等でモデルから消えたら自動で閉じる。
// モデルの書き換えはこのクラスが直接行い、undo用スナップショットは onWillEditModel、
// 確定通知は onModelEdited で MainComponent に委ねる（TimelineViewのリージョン操作と同じ流儀）。
class PianoRollView : public juce::Component,
                      private juce::Timer
{
public:
    static constexpr int preferredHeight = 260;
    static constexpr int keyboardWidth = 56;
    static constexpr int headerHeight = 30;
    static constexpr int rowHeight = 12;

    explicit PianoRollView (TransportState& transportState);
    ~PianoRollView() override;

    void setProject (Project* p);
    void openRegion (juce::uint64 trackId, juce::uint64 regionId);
    void close();
    bool isOpen() const { return open; }
    bool isShowingRegion (juce::uint64 trackId, juce::uint64 regionId) const
    {
        return open && shownTrackId == trackId && shownRegionId == regionId;
    }
    juce::uint64 currentTrackId() const { return shownTrackId; }

    // モデル変更（undo・リージョン削除等）後に呼ぶ。対象が消えていたら onCloseRequested を呼ぶ
    void refreshFromModel();

    // ---- キーボード操作（MainComponentから委譲される。処理したらtrue）----
    bool deleteSelectedNotes();               // Delete
    bool transposeSelection (int semitones);  // ↑↓=±1・⌥↑↓=±12（Logic準拠）
    bool copySelection();                     // ⌘C（内部クリップボード）
    bool pasteAtPlayhead();                   // ⌘V（再生ヘッド位置へ。リージョン外なら先頭へクランプ）

    std::function<void()> onWillEditModel;
    std::function<void()> onModelEdited;
    std::function<void (juce::uint64, int, int)> onPreviewNote; // trackId, pitch, velocity
    std::function<void()> onCloseRequested;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class KeyboardContent;
    class GridContent;
    class GridViewport;

    void timerCallback() override;

    Track* findTrack() const;
    MidiRegion* findRegion() const;      // 見つからなければnullptr
    bool isSelected (juce::uint64 noteId) const;
    int forcedPitch() const;             // 固定ピッチ打楽器（Kick等）なら0..127、通常は-1
    void pruneSelection();               // モデルから消えたノートを選択から外す
    juce::int64 playheadRegionPpq() const; // 再生ヘッドのリージョン相対PPQ
    void syncVelocitySlider();

    // PPQ⇔ピクセル（リージョン相対）
    double pxPerTick() const;
    int ppqToX (juce::int64 ppq) const;
    juce::int64 xToPpq (int x) const;
    juce::int64 snapTicks() const;
    void updateContentSize();
    void scrollToPitch (int pitch);
    void preview (int pitch, int velocity);

    // グリッド上のマウス操作（GridContentから転送される）
    void handleGridMouseDown (const juce::MouseEvent& e);
    void handleGridMouseDrag (const juce::MouseEvent& e);
    void handleGridMouseUp (const juce::MouseEvent& e);
    void handleGridDoubleClick (const juce::MouseEvent& e);
    MidiNote* hitTestNote (int x, int y) const;

    TransportState& transport;
    Project* project = nullptr;

    bool open = false;
    juce::uint64 shownTrackId = 0, shownRegionId = 0;
    std::vector<juce::uint64> selectedIds; // 複数選択
    std::vector<MidiNote> clipboard;       // ⌘Cの内部クリップボード（startPpqは先頭ノート基準の相対値）
    double pxPerBar = 320.0;

    // ドラッグ（移動は選択ノート全部・リサイズは掴んだノートのみ）
    struct NoteDrag
    {
        enum class Mode { none, move, resize, rubberBand };
        Mode mode = Mode::none;
        juce::uint64 noteId = 0;               // 掴んだノート
        struct Orig { juce::uint64 id; juce::int64 startPpq; int pitch; };
        std::vector<Orig> origPositions;       // moveで動かす選択ノートの初期位置
        juce::int64 origLengthPpq = 0;
        int startX = 0, startY = 0;
        int lastPreviewedPitch = -1;
        bool edited = false;
        juce::Rectangle<int> rubberRect;       // rubberBand中の矩形（Grid座標）
    };
    NoteDrag noteDrag;

    juce::Label titleLabel;
    juce::ComboBox gridBox;
    juce::Label velocityCaption;
    juce::Slider velocitySlider; // 選択ノートのベロシティ一括変更
    bool velocityDragActive = false;
    juce::TextButton closeButton { juce::String::fromUTF8 (u8"×") };

    juce::Component keyboardHolder; // 鍵盤のクリップ用（縦スクロール追従は中のkeyboardが動く）
    std::unique_ptr<KeyboardContent> keyboard;
    std::unique_ptr<GridContent> grid;
    std::unique_ptr<GridViewport> viewport;

    juce::int64 lastPaintedPlayhead = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollView)
};
