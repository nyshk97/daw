#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "Project.h"

// リージョン右クリック「リファレンスとして分析」の前段: クリップの参照範囲を
// `<プロジェクト>/references/<名前>/track.wav` へ**素のまま**コピーする。
// リージョンゲイン・フェードは適用しない（分析対象は原曲であって LaLa 上の加工結果ではない）。
// ループは1周分（offset〜offset+length）。名前はクリップ名からサニタイズし、衝突は連番。
namespace ReferenceExport
{
// フォルダ名に使えない文字を除いた表示名を作る。空になったら "reference"
juce::String sanitizeName (const juce::String& raw);

// `<projectDir>/references/<sanitized>` の空きフォルダを決める（衝突は -2, -3, ... の連番。
// 作成はしない — 呼び出し側がコピー直前に作る）
juce::File allocateFolder (const juce::File& projectDir, const juce::String& rawName);

// クリップの参照範囲をソースWAVから読み出して folder/track.wav へ書く（folder は作成される）。
// 成功で true。失敗時は error に理由（folder の後片付けは呼び出し側の責任）
bool exportClipRange (const juce::File& projectDir, const Clip& clip,
                      const juce::File& folder, juce::String& error);
}
