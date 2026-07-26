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
    // ループハンドル（選択中アイテムの下辺・右寄り）。右端リサイズ（本体右端8px）とは辺を分けて
    // 「同じ辺の数px違いで機能が変わる」誤操作を避ける。選択中しか出ないので、未選択アイテムを
    // 掴もうとしてループが伸びることは起きない（1クリック目が必ず選択になる）
    static constexpr int loopHandleHeight = 6;
    static constexpr int loopHandleWidth = 40;
    // フェードハンドル（選択中アイテムの上辺・左右）。ループハンドルと同じ「選択中のみ」規則。
    // ヒット領域10×14px・見えている部分は上辺の8pxで、モックを実マウスでドラッグして決めた寸法
    // （合成クリックでは操作性を検証できないため）
    static constexpr int fadeHandleWidth = 10;
    static constexpr int fadeHandleHeight = 14;
    static constexpr int fadeHandleVisibleHeight = 8;
    // 連なり全体（ループ終端まで）がこれ未満の幅ならフェードハンドルを出さない
    // （10pxハンドル2つ＋移動用の余白20pxを確保。ゲインバッジの56px閾値と同じ考え方）
    static constexpr int fadeHandleMinWidth = 40;
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
    std::function<void()> onCycleChanged;            // サイクル範囲の変更（undo対象外。Transport同期・dirty用）
    std::function<void (int, int)> onOpenRegion;     // リージョンをダブルクリック（track, region）
    std::function<void (int, int)> onDeleteItemRequested; // 右クリックメニューの削除（track, クリップorリージョンindex）
    std::function<void (int, int)> onExportItemRequested; // 右クリックメニューの書き出し（同上）
    // 「クリップのオーディオ値」（リージョンゲイン・フェード）専用の編集通知。通常の
    // onModelEdited（pushSnapshot）を使うとMIDIが消音＋再発音されてしまうため、経路を分けている
    // （MainComponent::pushAudioValueSnapshot へ繋ぐ）
    std::function<void()> onWillEditClipValue; // ドラッグ開始直前（undoは1操作=1件）
    std::function<void()> onClipValueEdited;   // 値の変更ごと（ドラッグ中も呼ばれる）
    // 構造編集を受け付けてよいか（録音中は不可）。TimelineViewはPlaybackEngineを知らず
    // TransportState::recordArmed は録音待機であって録音中と別物なので、MainComponentから渡してもらう。
    // 未設定なら常に編集可（TrackHeadersView::canReorder と同じ形）
    std::function<bool()> canEdit;

    // オーディオファイルのD&D取り込み。対応拡張子のファイルだけを渡す（先頭のみ処理するかは受け手が決める）。
    // trackIndex = -1 は空白ゾーンへのドロップ（新規トラックを作成して配置）。
    // startSampleは表示グリッドへスナップ済み。MIDIトラック上の不受理はTimelineView側で弾く
    std::function<void (const juce::StringArray&, int, juce::int64)> onImportFilesDropped;
    // MIDIトラックの行へのドロップ＝サンプル音源の割り当て（配置位置は使わない）。
    // トラックヘッダーへのドロップ（TrackHeadersView）と同じ受け口へ繋ぐ
    std::function<void (const juce::StringArray&, int)> onAssignInstrumentDropped;
    static bool isImportableAudioFile (const juce::String& path); // 拡張子判定（D&D受理と共用）

    // リージョン単位の操作。対象は引数で明示する（「現在の選択」を暗黙に読まない）。
    // itemIndex はオーディオトラックなら clips、MIDIトラックなら midiRegions のindex
    void toggleMuteAt (int trackIndex, int itemIndex);
    void duplicateAt (int trackIndex, int itemIndex);      // 複製を元の終端直後に置いて選択する
    void splitAtPlayhead (int trackIndex, int itemIndex);  // 再生ヘッド位置で2分割（範囲外はno-op）。左側を選択する
    // 外部（⌘Vのペースト等）がモデルへ追加したアイテムを選択する。isMidi = midiRegions か clips か
    void selectItem (int trackIndex, int itemIndex, bool isMidi);
    void clearLoopAt (int trackIndex, int itemIndex);      // 右クリックメニューの「ループ解除」
    // リージョンゲインの吹き出しを閉じる。表示中はモーダルなのでユーザー操作でクリップは動かないが、
    // 録音の終了ではクリップが増えて保持中のindexが失効するため、録音開始時に閉じてもらう
    void dismissGainCallout();

    // ループハンドルの矩形（描画とヒットテストで同じものを使う）。itemX/itemRight はループ終端まで
    // 含めたアイテムの左右端、laneY はそのトラック行の上端（いずれもコンテンツ座標）
    juce::Rectangle<int> loopHandleRect (int itemX, int itemRight, int laneY) const;

    // フェードハンドルの矩形（描画とヒットテストで同じものを使う）。基準は本体ではなく
    // **連なり全体**（フェードは連なりの両端に掛かるため）。laneY はトラック行の上端。
    // 連なりが fadeHandleMinWidth 未満なら false（ハンドルを出さない）
    bool fadeHandleRects (const Clip& clip, int laneY, juce::Rectangle<int>& fadeInHandle,
                          juce::Rectangle<int>& fadeOutHandle) const;

    // ハンドル1つの矩形。フェード終端（fadeEndX）の上にセンタリングし、連なりの内側へクランプする
    static juce::Rectangle<int> fadeHandleRectAt (int chainX, int chainRight, int laneY, int fadeEndX)
    {
        const int x = juce::jlimit (chainX, chainRight - fadeHandleWidth,
                                    fadeEndX - fadeHandleWidth / 2);
        return { x, laneY + 4, fadeHandleWidth, fadeHandleHeight };
    }

    // 2つのハンドル矩形とクリック位置から掴んだ方を返す（0 = フェードイン / 1 = フェードアウト / -1 = 外れ）。
    // fadeIn + fadeOut が全長に近づくと2つの矩形は重なり、上限では完全に一致するので
    // **クリック位置に近い方（等距離ならフェードイン）**で決める。固定順だと片方が掴めなくなる。
    // 詰まないことの保証: フェードインを縮めればフェードアウトのハンドルが必ず露出する
    static int pickFadeHandle (const juce::Rectangle<int>& fadeInHandle,
                               const juce::Rectangle<int>& fadeOutHandle,
                               juce::Point<int> pos)
    {
        const bool hitIn = fadeInHandle.contains (pos);
        const bool hitOut = fadeOutHandle.contains (pos);
        if (! hitIn && ! hitOut)
            return -1;
        if (hitIn != hitOut)
            return hitIn ? 0 : 1;
        const auto distance = [&pos] (const juce::Rectangle<int>& handle)
        { return std::abs (handle.getCentreX() - pos.x); };
        return distance (fadeInHandle) <= distance (fadeOutHandle) ? 0 : 1;
    }

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
    juce::int64 snapSampleToVisibleGrid (juce::int64 sample) const;

    // 拍（曲頭から0始まり・4/4固定）⇔ サンプル・ピクセル。セクションマーカーの描画・追加・
    // シークは必ずここを経由する（1始まり小節との換算を呼び出し側に書かせない）
    juce::int64 beatStartSample (int beats) const;
    int beatToX (int beats) const;
    int snapUnitBeats() const;                       // マーカーのスナップ刻み（表示グリッド準拠・上限=拍）: 4/2/1拍
    int xToMarkerBeats (int x) const;                // xを含むスナップ枠の頭の拍位置（floor）。>= 0

    // 16分音符（曲頭から0始まり・サイクル範囲の単位）⇔ サンプル。
    // MainComponentのTransport同期・再生開始ジャンプからも使う
    juce::int64 sixteenthStartSample (int sixteenths) const;

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
    juce::int64 snapSampleToGrid (juce::int64 sample) const; // 表示中の最小グリッドの最近傍へ（リージョン移動と同じ規則）
    int hitTestRegion (int trackIndex, int x) const; // 見つからなければ-1（重なりは後勝ち）
    int hitTestClip (int trackIndex, int x) const;   // 同上（オーディオトラック用）
    // フェードハンドルのヒットテスト。0 = フェードイン / 1 = フェードアウト / -1 = 外れ。
    // 2つの矩形は fadeIn + fadeOut が全長に近づくと重なり、上限では完全に一致するので
    // 「クリック位置に近い方（等距離ならフェードイン）」で決める（固定順だと片方が掴めなくなる）
    int hitTestFadeHandle (const Clip& clip, int laneY, juce::Point<int> pos) const;
    void showItemMenu (int trackIndex, int itemIndex); // 右クリックメニュー（ミュート/複製/削除）
    void showClipGainCallout (int trackIndex, int itemIndex); // メニューの「ゲイン…」→ 吹き出し
    void applyClipGain (int trackIndex, int itemIndex, float gain); // 値の適用（index範囲を再検証する）

    // サイクル範囲（ルーラーの操作。クリック=シークは維持し、ドラッグだけが範囲を作る）
    int sixteenthToX (int sixteenths) const;
    int xToCycleSixteenths (int x) const;            // 最近傍の表示グリッド線へスナップ（1/16上限）。>= 0
    int hitTestCycleEdge (int x) const;              // 端±4px。0=開始端・1=終了端・-1=なし
    void handleRulerMouseDown (const juce::MouseEvent& e);
    void handleRulerMouseDrag (const juce::MouseEvent& e);
    void handleRulerMouseUp (const juce::MouseEvent& e);
    void handleRulerMouseMove (const juce::MouseEvent& e);

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
        enum class Mode { none, move, resize, loop, fadeIn, fadeOut };
        Mode mode = Mode::none;
        bool isMidi = false;        // true = midiRegions / false = clips を対象にする
        int track = -1, item = -1;  // item は clips or midiRegions のindex
        juce::int64 origStartPpq = 0, origLengthPpq = 0; // MIDI用
        juce::int64 origStartSample = 0;                 // オーディオ用
        int origLoopCount = 0;      // ループ伸縮の基準（ドラッグはループ終端を掴んでいる）
        // フェード長の基準。ループのドラッグでも使う: ループを縮めると連なり全長が縮んで
        // フェードの再クランプが必要になるが、毎イベント「現在値」からクランプすると
        // 縮めすぎてから戻したときにフェードが復元されない（1ジェスチャー内で値が削られる）。
        // 毎イベント「元値を代入 → clampFades()」で再計算する
        juce::int64 origFadeInSamples = 0, origFadeOutSamples = 0;
        int startX = 0;
        bool duplicateOnDrag = false; // ⌥ドラッグ: 最初の移動時に複製してから動かす
        // 実際に動いた時点で編集開始通知を一度だけ呼ぶ（フェードは onWillEditClipValue、
        // それ以外は onWillEditModel）
        bool edited = false;

        // フェード長のドラッグか（undo種別・スナップショットの経路が構造編集と別）
        bool isFadeDrag() const { return mode == Mode::fadeIn || mode == Mode::fadeOut; }
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

    // サイクル範囲のドラッグ状態。ルーラー上のドラッグで作成（create）、既存範囲の端で
    // リサイズ、内側で移動。動かさず離したときだけシーク（マーカーレーンと同じ mouseUp 判定）。
    // サイクルはundo対象外なので onWillEditModel は呼ばず、確定時に onCycleChanged を呼ぶ
    struct CycleDrag
    {
        enum class Mode { none, create, moveRange, resizeStart, resizeEnd };
        Mode mode = Mode::none;
        int startX = 0;
        int anchorSixteenths = 0; // create: ドラッグ開始点のスナップ位置
        int origStart = 0, origEnd = 0;
        bool edited = false;      // 実際に値が変わった時点でtrue（クリック判定と repaint 抑制用）
    };
    CycleDrag cycleDrag;

    // オーディオファイルD&Dのドロップ先状態（LaneContentがインジケータを描く）。
    // 座標はLaneContentのコンテンツ空間で受ける（スクロール換算不要）
    struct FileDropState
    {
        bool active = false;
        bool rejected = false;      // 不受理（現状は該当なし。将来の受理不能ケース用に残す）
        bool instrument = false;    // MIDIトラック上＝サンプル音源の割り当て（クリップ配置ではない）
        int track = -1;             // -1 = 空白ゾーン（新規トラック）
        juce::int64 startSample = 0;

        bool operator== (const FileDropState& other) const
        {
            return active == other.active && rejected == other.rejected
                && instrument == other.instrument
                && track == other.track && startSample == other.startSample;
        }
    };
    FileDropState fileDrop;
    void updateFileDrop (int contentX, int contentY);
    void clearFileDrop();

    std::unique_ptr<RulerContent> ruler;
    std::unique_ptr<MarkerLaneContent> markerLane;
    // リージョンゲインの吹き出し（非同期に閉じられるので SafePointer で追う）
    juce::Component::SafePointer<juce::CallOutBox> gainCallout;

    std::unique_ptr<LaneViewport> viewport;
    std::unique_ptr<LaneContent> lanes;

    juce::int64 lastPaintedPlayhead = -1;
    juce::uint64 lastPaintedTrackDimMask = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};
