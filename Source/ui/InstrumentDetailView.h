#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "GainControls.h"
#include "../shared/Project.h"

// 下部エディタ（FxDetailView）に載せるサンプル音源の設定UI。レイアウトは
// 「波形フル幅（大）＋下に設定1行」（2026-07-25のモックで案Aに確定）。
//
// - 波形: samplePeakCache から描く。頭カット区間は減光し、境界の縦線をドラッグで微調整する
//   （docs/design/region-settings.md の「痕跡が見える」原則。数値は右端に小さく出すだけ）
// - 波形クリックで試聴（下部エディタはピアノロールと排他なので、音を確かめる唯一の手段）
// - 音程モード（固定/追従）と、追従のときだけ有効なルート音
// - Mono（新しい打点で前の音を切る）。長い音の連打で重なって濁るのを避ける
// - サンプル音量（ドラッグ中だけdBポップアップ。ヘッダーの音量スライダーと同じ流儀）
//
// モデル（Track）への書き込みはこのクラスが直接行い、undoスナップショットは onWillEdit、
// 確定通知は onValueEdited / onPitchModeEdited で MainComponent に委ねる
// （PianoRollView・TimelineViewと同じ流儀）。SamplerEngine の atomic への反映は
// 受け手側で走る SynthBank::sync() の責務。
// 確定通知を2本に分けているのは、スナップショットの再pushが「全ノートオフ→跨ぎノート再発音」を
// 引き起こすため: 音量・ルート音・頭カットで再pushすると、追従モードで鳴っている音が頭から鳴り直す。
class InstrumentDetailView : public juce::Component
{
public:
    InstrumentDetailView();
    ~InstrumentDetailView() override; // WaveDisplayが不完全型なのでデストラクタは.cppで定義する

    // 表示対象。nullptr / サンプル未割当なら空表示になる
    void setTrack (Track* trackToShow);
    Track* shownTrack() const { return track; }

    // モデル側が変わったとき（undo/redo・サンプル差し替え）の再同期
    void refreshFromModel();

    // 編集の直前（undoスナップショット用。ドラッグは開始時に1回だけ）。
    // valueOnly = 音量・ルート音・頭カット（＝undo/redoでもスナップショット再pushが不要な編集）
    std::function<void (bool valueOnly)> onWillEdit;
    // 音量・ルート音・頭カットの確定後。**スナップショットは再pushしない**（差し替えを検出した
    // エンジンが全ノートをオフ→跨ぎノート再発音するため、発音中の音が頭から鳴り直してしまう）。
    // 受け手は SamplerEngine の atomic ミラー更新（SynthBank::sync）とdirty化だけを行う
    std::function<void()> onValueEdited;
    // 音程モードの確定後。停止要求（requestStopAll）と resound が必要なので受け手は pushSnapshot する
    std::function<void()> onPitchModeEdited;
    std::function<void (int)> onPreview; // 試聴（ピッチを渡す。固定モードはルート音・追従もルート音）

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class WaveDisplay;
    friend class WaveDisplay; // 波形側から頭カットの書き換え・試聴・undo通知を行う

    // GAINのスライダー（0dB吸着）と現在値表示（クリックで0dBリセット）は
    // オーディオリージョンのゲインと共通（ui/GainControls.h）
    void applyPitchMode (bool follow);
    void applyRootNote (int note);
    void applyMono (bool mono);
    juce::String trimText() const;
    juce::Rectangle<int> labelAreaFor (const juce::Component& c) const; // コントロール上の見出し位置

    Track* track = nullptr;

    std::unique_ptr<WaveDisplay> wave;
    juce::Label sampleNameLabel;
    juce::TextButton fixedButton { juce::String::fromUTF8 (u8"固定") };
    juce::TextButton followButton { juce::String::fromUTF8 (u8"追従") };
    juce::TextButton monoButton { "Mono" };
    juce::ComboBox rootBox;
    GainSlider gainSlider;
    GainValueLabel gainValueLabel;
    bool gainEditBegun = false; // このドラッグでundoを積んだか（ダブルクリックでの二重積みを防ぐ）
    juce::Label trimLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentDetailView)
};
