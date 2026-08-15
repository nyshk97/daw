#pragma once

#include <array>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../shared/Project.h"
#include "../shared/SpectrumAnalyzer.h"

// 下部エディタ（FxDetailView）に載せるトラックEQの操作UI。
// plan: docs/plans/2026-08-15-1506-track-eq.md
//
// 構成: グラフ（対数周波数軸 20Hz〜20kHz・±24dB）＋下部の数値表示行（バンドごとに Hz/dB/Q）。
// カーブはDSPと同じRBJ係数（juce::dsp::IIR::Coefficients）の getMagnitudeForFrequencyArray から
// 描く＝描画と音が同じ数式。ui-principles「操作は削る、概念は隠さない」に従い、
// 数値は実単位（Hz/dB/Q）のまま出し、固定値（HPの12dB/oct）は静的ラベルで見せる。
//
// 操作（バンド別）:
//   HP    = 横ドラッグ（周波数）＋数値行のON/OFFトグル。ゲイン概念なし・Q固定
//   ベル  = 横=周波数・縦=ゲイン・スクロール=Q・ダブルクリック=0dB
//   シェルフ = 横=周波数・縦=ゲイン・ダブルクリック=0dB（Q固定でスクロール無効）
//
// モデル（TrackParams::eqBands の atomic）への書き込みはこのクラスが直接行い
// （必ず Eq::normalized を通す。ドラッグ中も即時反映＝音が追従する）、
// dirty化は onEdited で MainComponent に委ねる（EQはフェーダーと同じくundo対象外）。
//
// スペクトラムアナライザ: 表示中だけ AnalyzerTap を有効化し、30Hz Timer で
// SpectrumAnalyzer（shared/・数値処理はそちら）の結果dBFS配列を**描くだけ**。
// 目盛りはカーブ（±24dB=加工量）と独立（-60〜0dBFS=信号の実量）
class EqEditorView : public juce::Component,
                     private juce::Timer
{
public:
    EqEditorView();

    // 表示対象。nullptr = 空表示（トラック削除・追従切れへの防御）
    void setTrack (Track* trackToShow);
    Track* shownTrack() const { return track; }

    // eqEnabled の切替等、外（スロットピル）からの変更の再描画
    void refreshFromModel() { repaint(); }

    // 全変更経路（ドラッグ確定・Qスクロール・ダブルクリックリセット・HPのON/OFF）で呼ぶ
    std::function<void()> onEdited;
    // カーブ計算に使うSR（デバイス追従。未確定時は呼び出し側が48kへフォールバックする）
    std::function<double()> getSampleRate;

    // アナライザタップ（MainComponent所有・エンジンと共有）。表示対象の切替はsetTrackが行う
    void setAnalyzerTap (AnalyzerTap* tapToUse) { tap = tapToUse; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    double sampleRate() const;
    float xForFreq (float freqHz) const;
    float freqForX (float x) const;
    float yForDb (float db) const;
    float dbForY (float y) const;
    juce::Point<float> pointFor (int band, const Eq::BandValue& value) const;
    int bandAt (juce::Point<float> position) const; // ヒットしたバンド（-1=なし。無効HPは対象外）
    juce::Rectangle<int> readoutColumn (int band) const;
    juce::Rectangle<int> hpToggleArea() const;

    void applyBand (int band, Eq::BandValue value); // normalized→store→repaint（dirty化は呼び出し側）
    void notifyEdited();
    void timerCallback() override; // 30Hz: アナライザの取り込み＋減衰描画

    Track* track = nullptr;
    AnalyzerTap* tap = nullptr;
    SpectrumAnalyzer analyzer;
    // ピークホールド＋減衰付きの表示値（analyzerの生フレームとは分ける。減衰は表示演出）
    std::array<float, SpectrumAnalyzer::numBins> spectrumDb {};
    bool lastSpectrumVisible = false; // 消えた後も1回描いて残像を消すためのフラグ

    juce::Rectangle<int> graphArea, readoutArea;
    int dragBand = -1;   // ドラッグ中のバンド（-1=なし）
    int hoverBand = -1;  // ホバー中のバンド（ポイント強調・スクロールQの対象）
    bool dragMoved = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqEditorView)
};
