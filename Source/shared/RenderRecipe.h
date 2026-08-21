#pragma once

#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>

#include "PitchCorrection.h"
#include "PitchCurve.h"
#include "TimeMap.h"

// ピッチ補正付きレンダーの**不変の入力**。ClipDomains::Request が強参照で持ち、ワーカーはこれだけを読む
// （可変な Clip・サイドカーキャッシュ・エディタ状態をワーカーが読まない＝編集中・undo・再解析・削除との
// 競合を構造で塞ぐ）。RenderedDomain も同じ digest を保持し、装着・pending 判定・巻き戻しの一致判定に使う
struct RenderRecipe
{
    std::shared_ptr<const juce::AudioBuffer<float>> sourceAudio;
    double sampleRate = 0.0;
    juce::int64 domainOffset = 0;
    juce::int64 domainLength = 0;
    int transposeSemitones = 0;
    double stretchRatio = 1.0;
    std::shared_ptr<const PitchCurve> curve; // 補正の元カーブ（有声マスク・元ピッチ）
    PitchCorrection correction;              // 値コピー（不変）

    ContentDigest digest() const { return correction.digest(); }

    TimeMap timeMap() const
    {
        return PitchCorrections::buildTimeMap (correction, domainOffset, domainLength, stretchRatio, sampleRate);
    }
    PitchCorrections::TargetCurve target() const
    {
        return PitchCorrections::targetCurve (correction, *curve, domainOffset, domainLength, transposeSemitones);
    }
};
