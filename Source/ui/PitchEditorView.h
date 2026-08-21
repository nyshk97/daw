#pragma once

#include <functional>
#include <optional>
#include <set>
#include <juce_gui_basics/juce_gui_basics.h>

#include "IconButton.h"
#include "../shared/PitchEditorSession.h"
#include "../shared/Project.h"

// ボーカルのピッチ補正エディタ（独立ウィンドウの中身）。鍵盤＋グリッドにノートをブロブ（矩形＝目標音・
// 薄い線＝元ピッチ・濃い線＝補正後）で描く。判断ロジックは持たない: 編集は PitchCorrections の関数を
// session->mutableWorking() に適用して onEdited を呼ぶだけ。確定・プレビュー・undo は MainComponent が配線する。
// 見た目は scratchpad のモック（2026-08-21 確定・全て B 案: バナー＋Enable／左＝音の決め方・右＝確定系／
// 吸収区間に斜線＋ゴースト）
class PitchEditorView : public juce::Component,
                        private juce::Timer
{
public:
    PitchEditorView();
    ~PitchEditorView() override;

    // ---- MainComponent が配線する ----
    PitchEditorSession* session = nullptr;
    std::function<const Clip*()> getClip;                  // 対象クリップ（id で引き直す。無ければ nullptr）
    std::function<double()> getSampleRate;
    std::function<double()> getBpm;
    std::function<std::optional<ProjectKey>()> getProjectKey;
    std::function<juce::int64()> getPlayheadSample;          // タイムラインの再生ヘッド位置
    std::function<bool()> isRendering;                      // プレビュー／本レンダーの待ち中（ブロブを減光）
    std::function<float()> getAnalysisProgress;             // 解析中の進捗 0..1
    std::function<juce::String()> getBlockedMessage;        // サイドカー書込失敗などの理由（空なら無し）

    std::function<void()> onBeginEdit;  // 離散編集の直前（undo 区切り・初回プレビューの確定）
    std::function<void()> onEdited;     // 編集後（モデルへ反映・レンダー要求・dirty）
    std::function<void()> onEnable;
    std::function<void (PitchScaleMode, ProjectKey)> onScaleChanged; // Scale 選択＝合わせ直しのプレビュー開始
    std::function<void()> onReanalyze;
    std::function<void()> onApply;
    std::function<void()> onCancel;
    std::function<void()> onReset;
    std::function<void()> onRetrySidecar;
    std::function<void()> onSetProjectKey;       // 推定キーを「プロジェクトに設定」
    std::function<void (int noteIndex)> onAudition;
    std::function<void (int noteIndex)> onDragAuditionStart; // 上下ドラッグ開始（その音の分解をキャッシュ）
    std::function<void()> onDragAuditionUpdate;             // 目標音が変わった（合成して鳴らし直す）
    std::function<void()> onDragAuditionEnd;
    std::function<bool (const juce::KeyPress&)> onKey; // メインへ転送（Space / ⌘Z / , .）

    void refresh(); // session の状態が変わったら呼ぶ（バーの表示を更新して再描画）
    void showStatus (const juce::String& text); // 上部バー右側に数秒だけ出す通知（再解析の結果など。メインのトーストはエディタからは見えない）

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent&) override { hoverNote = -1; hoverSplit = false; repaint(); }
    void modifierKeysChanged (const juce::ModifierKeys& mods) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify (const juce::MouseEvent& e, float scaleFactor) override;

    static constexpr int barHeight = 36;
    static constexpr int bannerHeight = 26;
    static constexpr int keyboardWidth = 48;
    static constexpr int rulerHeight = 18;

private:
    struct Geometry
    {
        juce::Rectangle<int> canvas;   // 鍵盤・ルーラーを除いたグリッド領域
        juce::int64 viewStart = 0, viewEnd = 1; // 表示範囲（render 座標。ズーム・スクロール後）
        juce::int64 clipRenderStart = 0;        // クリップ先頭の render 座標（タイムライン位置 = startSample に対応）
        double pxPerSample = 0.0;
        int midiLo = 48, midiHi = 84;  // 表示する音域 [lo, hi)
        float rowHeight = 1.0f;
        juce::int64 domainOffset = 0, domainLength = 0;
        double stretchRatio = 1.0;
        TimeMap timeMap;
        float xForRender (juce::int64 render) const { return (float) canvas.getX() + (float) (render - viewStart) * (float) pxPerSample; }
        float xForSource (juce::int64 source) const { return xForRender (timeMap.map (source)); }
        float yForMidi (double midi) const { return (float) canvas.getY() + (float) (midiHi - midi) * rowHeight; }
        juce::int64 renderForX (float x) const { return viewStart + (juce::int64) std::llround ((x - (float) canvas.getX()) / pxPerSample); }
        // render 座標 ⇄ タイムライン位置（クリップの startSample 基準）
        juce::int64 timelineForRender (juce::int64 render, juce::int64 clipStartSample) const { return clipStartSample + (render - clipRenderStart); }
        juce::int64 renderForTimeline (juce::int64 timeline, juce::int64 clipStartSample) const { return clipRenderStart + (timeline - clipStartSample); }
    };
    Geometry computeGeometry (const Clip& clip) const;
    int noteAt (const Geometry& g, juce::Point<int> p); // -1 = 無し（描画と同じ目標カーブで判定するため非 const）
    void drawKeyboard (juce::Graphics& g, const Geometry& geo) const;
    void drawGrid (juce::Graphics& g, const Geometry& geo, const Clip& clip) const;
    void drawBlobs (juce::Graphics& g, const Geometry& geo, const Clip& clip);
    void drawAbsorbHatch (juce::Graphics& g, const Geometry& geo) const;
    void updateBar();
    void applyScaleSelection();
    bool canEdit() const { return session != nullptr && session->editable(); }
    bool changingButtonsVisible() const;
    // 横ズーム（1 = クリップ全体が幅に収まる）とスクロール（render 座標・view 先頭からのオフセット）
    void zoomAround (double factor, float anchorX);
    void clampScroll (const Clip& clip);
    double zoom = 1.0;
    juce::int64 scrollRender = 0;

    void timerCallback() override
    {
        if (statusText.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() > statusUntil) { statusText.clear(); updateBar(); }
        repaint();
    }

    // ---- 上部バー（左＝音の決め方 ／ 右＝確定系）----
    juce::Label scaleLabel, strengthLabel, speedLabel, statusLabel;
    juce::ComboBox scaleBox;
    juce::TextButton enableButton, applyButton, cancelButton, retryButton;
    IconButton scaleHighlightButton { IconButton::Icon::keys, juce::String::fromUTF8 (u8"スケール音を表示") };
    bool highlightScale = true;       // スケール音の段を明るく・外を暗く（ON/OFF）
    juce::String statusText;
    double statusUntil = 0.0;
    juce::Slider strengthSlider, speedSlider;
    juce::Label bannerLabel;
    juce::TextButton bannerButton;
    juce::TextButton bannerKeyButton; // キー未設定時の「推定キーをプロジェクトに設定」（バナー側）
    void showContextMenu (juce::Point<int> at); // 右クリック: Reset・Re-analyze・Set project key（頻度の低い保守操作）
    bool bannerVisible = false;
    juce::String bannerText;
    bool sliderEditing = false;

    // ---- ドラッグ状態 ----
    struct Drag
    {
        int noteIndex = -1;
        juce::Point<int> start;
        enum class Mode { undecided, pitch, time } mode = Mode::undecided;
        int targetAtStart = 0;
        bool pinnedAtStart = false;
        juce::int64 appliedDelta = 0;   // 横移動の累積（render）
        juce::int64 prevEndAtStart = 0, nextStartAtStart = 0, startAtStart = 0, endAtStart = 0; // 斜線用（render）
        bool moved = false;
        bool snap = true;
        ContentDigest workingDigestAtStart; // ドラッグ中に working が差し替わった（巻き戻し・再解析の着地）ら確定しない
    };
    std::optional<Drag> drag;
    std::set<int> selected;
    int hoverNote = -1;
    juce::Point<int> hoverPos;
    bool hoverSplit = false; // ⌘ を押してブロブに乗っている（分割位置の点線とハサミカーソル）
    void splitAt (int noteIndex, juce::Point<int> p);
    double hatchFadeUntil = 0.0; // 斜線・ゴーストをドラッグ後しばらく残す（ms）
    Drag lastDragForHatch;
    bool lastDragValid = false;

    // 目標カーブのキャッシュ（working の digest・transpose・domain が変わったら作り直す）。描画とヒットテストで共用
    void ensureTargetCache (const Geometry& geo, const Clip& clip);
    float shiftAtFrame (int k, int transpose) const;
    ContentDigest cachedTargetDigest;
    int cachedTargetTranspose = 0;
    std::pair<juce::int64, juce::int64> cachedTargetDomain { -1, -1 };
    PitchCorrections::TargetCurve cachedTarget;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchEditorView)
};
