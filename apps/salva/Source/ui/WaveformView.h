#pragma once

#include <functional>

#include <juce_audio_utils/juce_audio_utils.h>

#include "shared/SeparationProgress.h"

// 波形表示＋区間ドラッグ選択＋端ドラッグ調整＋クリックシーク＋横ズーム。
// 時間軸の基準は元音源のサンプル（秒は表示用）。判断はコールバックで上位へ返し、
// 再生状態はこのビューでは持たない（playheadは上位がsetPlayheadで流し込む）
class WaveformView : public juce::Component,
                     private juce::ChangeListener
{
public:
    WaveformView();
    ~WaveformView() override;

    void setFile (const juce::File& file, double sampleRate, juce::int64 lengthSamples);
    void clearFile();

    // 選択区間（サンプル・endは排他的）。無選択は start==end==-1
    juce::int64 selectionStart() const { return selStart; }
    juce::int64 selectionEnd() const { return selEnd; }
    bool hasSelection() const { return selStart >= 0 && selEnd > selStart; }
    void clearSelection();
    void setSelection (juce::int64 start, juce::int64 end); // プログラムからの選択（通知はしない）

    void setPlayhead (juce::int64 sample, bool visible);

    // 横ズーム（Logic準拠で⌘←/→を上位が受けて呼ぶ）。アンカーは選択中心 or 再生ヘッド
    void zoomIn() { zoom (0.5); }
    void zoomOut() { zoom (2.0); }

    std::function<void (juce::int64 start, juce::int64 end)> onSelectionChanged; // ドラッグ中も随時
    std::function<void()> onSelectionCleared;
    std::function<void (juce::int64 sample)> onSeek; // クリック（ドラッグにならなかった）

    // 分離中の表示（2026-09-03確定モック案C）: 走査線が左→右へ進み、通り過ぎた区間の波形が
    // ステム色の積み重ねに変わる。走査位置＝段階内の進捗（曲のどこまで処理したか）。
    // 積み重ねの比率は装飾（実際のステム配分ではない。分離が終わるまで中身は分からない）。
    // 分離中はこのビューへのマウス操作（区間選択・シーク・ズーム）を受け付けない。
    // 上位がTimerごとに呼ぶ（走査線は目標位置へ滑らかに寄せる）
    void setSeparation (bool active, const SeparationProgress& progress, double remainingSeconds);
    bool isSeparating() const { return separating; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    double xToSample (float x) const;
    float sampleToX (juce::int64 sample) const;
    juce::int64 clampToLength (double sample) const;
    void zoom (double factor);
    void clampView();
    void notifySelection();

    enum class Drag { none, newSelection, leftEdge, rightEdge };
    Drag dragMode = Drag::none;
    juce::int64 dragAnchor = 0;

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache { 8 };
    juce::AudioThumbnail thumbnail { 1024, formatManager, thumbnailCache };

    double sourceSampleRate = 0.0;
    juce::int64 lengthSamples = 0;

    // 表示窓（サンプル）
    double viewStart = 0.0;
    double viewLength = 0.0;

    juce::int64 selStart = -1, selEnd = -1;
    juce::int64 playheadSample = 0;
    bool playheadVisible = false;

    // 分離中の表示状態
    void paintSeparation (juce::Graphics& g);
    void paintStemStack (juce::Graphics& g, float fromX, float toX, int numStems, float alpha);
    bool separating = false;
    SeparationProgress separation;
    double separationRemainingSec = -1.0;
    float scanShownX = 0.0f; // 走査線の表示位置（目標へ毎tick寄せる）

    static constexpr int edgeGrabPx = 6;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};
