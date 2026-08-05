#include "ReferenceAlign.h"

#include <cmath>

namespace ReferenceAlign
{
namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

juce::var parseJsonFile (const juce::File& file)
{
    return file.existsAsFile() ? juce::JSON::parse (file.loadFileAsString()) : juce::var();
}
}

Info readInfo (const juce::File& referenceFolder)
{
    Info info;

    // 信頼性ゲート: gates.downbeat.ok。カード生成の停止条件は BPM とテンポ安定だけなので、
    // 「カードがある＝小節頭が信頼できる」ではない
    const auto gates = parseJsonFile (referenceFolder.getChildFile ("analysis/gates.json"));
    const auto downbeatOk = gates.getProperty ("downbeat", {}).getProperty ("ok", {});
    if (! downbeatOk.isBool() || ! (bool) downbeatOk)
    {
        info.reason = jp (u8"小節頭の検出が信頼できません（gates.downbeat が落ちています）");
        return info;
    }

    // 位相: groove.json の補正済み first_downbeat_sec のみ（basics へのフォールバックはしない）
    const auto groove = parseJsonFile (referenceFolder.getChildFile ("analysis/groove.json"));
    const auto fd = groove.getProperty ("first_downbeat_sec", {});
    if (! (fd.isDouble() || fd.isInt()))
    {
        info.reason = jp (u8"小節頭の位置情報がありません（analysis/groove.json）");
        return info;
    }
    const double seconds = (double) fd;
    if (! std::isfinite (seconds) || seconds < 0.0)
    {
        info.reason = jp (u8"小節頭の位置情報が不正です（analysis/groove.json）");
        return info;
    }

    info.available = true;
    info.firstDownbeatSec = seconds;
    return info;
}

bool writeSourceDescriptor (const juce::File& referenceFolder, const ClipDescriptor& descriptor)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("file", descriptor.fileName);
    object->setProperty ("offsetSamples", descriptor.offsetSamples);
    object->setProperty ("lengthSamples", descriptor.lengthSamples);
    return referenceFolder.getChildFile ("source.json")
        .replaceWithText (juce::JSON::toString (juce::var (object)) + "\n");
}

ClipDescriptor readSourceDescriptor (const juce::File& referenceFolder)
{
    const auto parsed = parseJsonFile (referenceFolder.getChildFile ("source.json"));
    ClipDescriptor descriptor;
    if (! parsed.isObject())
        return descriptor;
    descriptor.fileName = parsed.getProperty ("file", "").toString();
    descriptor.offsetSamples = (juce::int64) parsed.getProperty ("offsetSamples", 0);
    descriptor.lengthSamples = (juce::int64) parsed.getProperty ("lengthSamples", 0);
    return descriptor;
}

Located locateClip (const Project& project, const ClipDescriptor& descriptor)
{
    Located located;
    if (! descriptor.isValid())
        return located;
    for (int t = 0; t < (int) project.tracks.size(); ++t)
    {
        const auto& track = project.tracks[(size_t) t];
        if (track.type != TrackType::audio)
            continue;
        for (int c = 0; c < (int) track.clips.size(); ++c)
        {
            const auto& clip = track.clips[(size_t) c];
            if (clip.fileName == descriptor.fileName
                && clip.offsetSamples == descriptor.offsetSamples
                && clip.lengthSamples == descriptor.lengthSamples)
            {
                ++located.matches;
                located.trackIndex = t;
                located.clipIndex = c;
            }
        }
    }
    return located;
}

juce::int64 alignedClipStart (juce::int64 currentStart, juce::int64 fdSamples, double barLenSamples)
{
    // 現在位置から最も近い「クリップ内の小節頭が小節線に乗る」位置。
    // startSample >= 0 を保つため、小節番号は ceil(fd / barLen) を下限にする
    const auto minBar = (juce::int64) std::ceil ((double) fdSamples / barLenSamples - 1e-9);
    auto bar = (juce::int64) std::llround (((double) currentStart + (double) fdSamples) / barLenSamples);
    bar = juce::jmax (bar, minBar);
    return (juce::int64) std::llround ((double) bar * barLenSamples) - fdSamples;
}

ApplyResult apply (Project& project, UndoStack& undoStack, const ClipDescriptor& descriptor,
                   double bpm, double firstDownbeatSec)
{
    if (project.sampleRate <= 0.0 || bpm < 30.0 || bpm > 300.0
        || ! std::isfinite (firstDownbeatSec) || firstDownbeatSec < 0.0)
        return ApplyResult::notFound;

    const auto located = locateClip (project, descriptor);
    if (located.matches != 1)
        return ApplyResult::notFound; // 0件（消えた・トリム済み）/ 2件以上（複製）は黙って動かさない

    const auto fdSamples = (juce::int64) std::llround (firstDownbeatSec * project.sampleRate);
    const double barLen = project.sampleRate * 240.0 / bpm; // 60/bpm×4拍（4/4固定）
    const auto& clip = project.tracks[(size_t) located.trackIndex].clips[(size_t) located.clipIndex];
    const auto newStart = alignedClipStart (clip.startSample, fdSamples, barLen);

    // 両方同値なら begin() 前に返す（頭出し済みで再度押したとき、⌘Zの1回目が
    // 「何も戻らない」no-op 履歴にならないように。setProjectBpm の同値ガードと同じ流儀）
    const bool bpmSame = std::abs (project.bpm - bpm) < 1e-9;
    if (bpmSame && newStart == clip.startSample)
        return ApplyResult::noChange;

    // begin の willBegin フック（ガチャ仮配置の撤去）は末尾に追加された仮オブジェクトしか
    // 消さないため、上で解決した track/clip の index は begin 後も有効
    undoStack.begin (project);
    project.bpm = bpm;
    project.tracks[(size_t) located.trackIndex].clips[(size_t) located.clipIndex].startSample = newStart;
    return ApplyResult::applied;
}
}
