#include "Project.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "GainScale.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }
}

void Clip::buildPeakCache()
{
    peakCache.clear();
    if (audio == nullptr)
        return;

    // 参照範囲 [offsetSamples, offsetSamples + lengthSamples) のみをキャッシュする
    // （index 0 = クリップ先頭。描画側はクリップ相対位置でそのまま引ける）。
    // ステレオはL/Rのmaxを取って1本にする（波形表示は1本のまま）
    const int numSamples = (int) juce::jlimit ((juce::int64) 0,
                                               (juce::int64) audio->getNumSamples() - offsetSamples,
                                               lengthSamples);
    const int numChannels = juce::jmin (2, audio->getNumChannels());
    peakCache.reserve ((size_t) (numSamples / samplesPerPeak + 1));

    for (int i = 0; i < numSamples; i += samplesPerPeak)
    {
        const int count = juce::jmin (samplesPerPeak, numSamples - i);
        float peak = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = audio->getReadPointer (ch, (int) offsetSamples);
            for (int j = 0; j < count; ++j)
                peak = juce::jmax (peak, std::abs (data[i + j]));
        }
        peakCache.push_back (peak);
    }
}

std::vector<PeakRange> buildFullPeakCache (const juce::AudioBuffer<float>& audio)
{
    std::vector<PeakRange> peaks;
    const int numSamples = audio.getNumSamples();
    const int numChannels = juce::jmin (2, audio.getNumChannels());
    if (numSamples <= 0 || numChannels <= 0)
        return peaks;

    peaks.reserve ((size_t) (numSamples / Clip::samplesPerPeak + 1));
    for (int i = 0; i < numSamples; i += Clip::samplesPerPeak)
    {
        const int count = juce::jmin (Clip::samplesPerPeak, numSamples - i);
        // 0で初期化しない（片側にしか振れない区間まで中央線を跨がせないため）
        float lo = std::numeric_limits<float>::max();
        float hi = std::numeric_limits<float>::lowest();
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = audio.getReadPointer (ch);
            for (int j = 0; j < count; ++j)
            {
                lo = juce::jmin (lo, data[i + j]);
                hi = juce::jmax (hi, data[i + j]);
            }
        }
        peaks.push_back ({ lo, hi });
    }
    return peaks;
}

bool splitClip (const Clip& clip, juce::int64 splitSample, Clip& left, Clip& right)
{
    if (splitSample <= clip.startSample || splitSample >= clip.startSample + clip.lengthSamples)
        return false;

    const auto leftLength = splitSample - clip.startSample;

    left = clip; // fileName/audio は共有参照
    left.lengthSamples = leftLength;
    // ループは解除する（左右どちらに繰り返しを引き継ぐか自明でない）。**フェードのクランプより
    // 先に**0にすること: 元の loopCount 込みの全長でクランプしてから呼び出し側が解除すると、
    // フェードが分割後の全長を超えて残る（描画は未クランプ値・再生は防御クランプ値を使うため
    // 見た目と音が食い違い、保存→再読込でも値が変わる）
    left.loopCount = 0;
    // フェードは外側だけ継承する（内側＝分割点側は0）。丸ごとコピーのままだと
    // 左が元のフェードアウトを、右が元のフェードインを引き継ぎ、分割点にフェードが移動してしまう
    left.fadeOutSamples = 0;
    left.clampFades(); // 継承した fadeIn を左の長さへ収める
    left.buildPeakCache();

    right = clip;
    right.startSample = splitSample;
    right.offsetSamples = clip.offsetSamples + leftLength;
    right.lengthSamples = clip.lengthSamples - leftLength;
    right.loopCount = 0;
    right.fadeInSamples = 0;
    right.clampFades(); // 継承した fadeOut を右の長さへ収める
    right.buildPeakCache();
    return true;
}

void appendRegionNotes (const MidiRegion& region, int fixedPitch, std::vector<MidiNotePlayback>& out)
{
    const int reps = 1 + juce::jmax (0, region.loopCount);
    for (int r = 0; r < reps; ++r)
    {
        const auto repStart = region.startPpq + (juce::int64) r * region.lengthPpq;
        const auto repEnd = repStart + region.lengthPpq; // マスクは反復ごと（次の反復へ持ち越さない）
        for (const auto& note : region.notes)
        {
            const auto absStart = repStart + note.startPpq;
            const auto absEnd = juce::jmin (absStart + note.lengthPpq, repEnd);
            if (absEnd <= absStart)
                continue;
            out.push_back ({ absStart, absEnd,
                             fixedPitch >= 0 ? fixedPitch : note.pitch, note.velocity });
        }
    }
}

void appendClipPlaybacks (const Clip& clip, std::vector<ClipPlayback>& out)
{
    if (clip.audio == nullptr)
        return;
    // オーディオスレッドの範囲外読みを防ぐ最終防衛線（モデル側の不変条件が正なら素通し）
    const auto bufferLength = (juce::int64) clip.audio->getNumSamples();
    const auto offset = juce::jlimit ((juce::int64) 0, bufferLength, clip.offsetSamples);
    const auto length = juce::jlimit ((juce::int64) 0, bufferLength - offset, clip.lengthSamples);
    if (length <= 0)
        return;

    // フェードは「連なり全体」の両端に掛かる（繰り返しの間はシームレス）。絶対位置で持つので
    // 反復をまたぐ長さでもそのまま表現でき、全反復に同じ値を載せてよい。
    // モデル側の不変条件（fadeIn + fadeOut <= 全長）が破れていても区間が交差しないよう頭打ちする
    const auto chainStart = clip.startSample;
    const auto chainEnd = chainStart + clip.totalLengthSamples();
    const auto fadeIn = juce::jlimit ((juce::int64) 0, chainEnd - chainStart, clip.fadeInSamples);
    const auto fadeOut = juce::jlimit ((juce::int64) 0, chainEnd - chainStart - fadeIn, clip.fadeOutSamples);
    const auto fadeInEnd = chainStart + fadeIn;
    const auto fadeOutStart = chainEnd - fadeOut;

    // 反復の間隔はクランプ後の length でなくモデルの lengthSamples。描画も lengthSamples 基準なので、
    // 異常値でクランプが効いたときに見た目と鳴りがズレないようにする
    const int reps = 1 + juce::jmax (0, clip.loopCount);
    for (int r = 0; r < reps; ++r)
        out.push_back ({ clip.audio, clip.startSample + (juce::int64) r * clip.lengthSamples,
                         offset, length, clip.gain,
                         chainStart, fadeInEnd, fadeOutStart, chainEnd });
}

bool splitMidiRegion (const MidiRegion& region, juce::int64 splitPpq, MidiRegion& left, MidiRegion& right)
{
    if (splitPpq <= region.startPpq || splitPpq >= region.startPpq + region.lengthPpq)
        return false;

    const auto leftLength = splitPpq - region.startPpq;

    left = region;
    left.lengthPpq = leftLength;
    left.notes.clear();

    right = region;
    right.id = 0; // 呼び出し側で採番する
    right.startPpq = splitPpq;
    right.lengthPpq = region.lengthPpq - leftLength;
    right.notes.clear();

    for (const auto& note : region.notes)
    {
        if (note.startPpq < leftLength)
        {
            left.notes.push_back (note); // またぎノートもフル長のまま残す（Keep。再生は境界マスクが止める）
        }
        else
        {
            auto moved = note;
            moved.startPpq -= leftLength;
            right.notes.push_back (moved);
        }
    }
    return true;
}

juce::String ProjectKeys::modeName (KeyMode mode)
{
    return mode == KeyMode::major ? "major" : "minor";
}

bool ProjectKeys::modeFromName (const juce::String& name, KeyMode& out)
{
    if (name == "major")
    {
        out = KeyMode::major;
        return true;
    }
    if (name == "minor")
    {
        out = KeyMode::minor;
        return true;
    }
    return false;
}

namespace
{
// 表示は♯表記固定（Logicの初期設定と同じ。♭併記はUIの情報量を増やすだけなので引き算）
constexpr const char* keyRootNamesSharp[12] = { "C",  "C\xe2\x99\xaf", "D",  "D\xe2\x99\xaf",
                                                "E",  "F",             "F\xe2\x99\xaf", "G",
                                                "G\xe2\x99\xaf", "A",  "A\xe2\x99\xaf", "B" };
constexpr const char* keyRootNamesAscii[12] = { "C", "C#", "D", "D#", "E", "F",
                                                "F#", "G", "G#", "A", "A#", "B" };
}

juce::String ProjectKeys::rootName (int root)
{
    return root >= 0 && root < 12 ? juce::String::fromUTF8 (keyRootNamesSharp[root]) : juce::String();
}

bool ProjectKeys::rootFromName (const juce::String& name, int& out)
{
    const auto trimmed = name.trim();
    if (trimmed.isEmpty())
        return false;

    static constexpr int letterPc[7] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    const auto letter = (juce::juce_wchar) trimmed[0];
    if (letter < 'A' || letter > 'G')
        return false;

    int pc = letterPc[letter - 'A'];
    const auto accidental = trimmed.substring (1);
    if (accidental == "#" || accidental == juce::String::fromUTF8 (u8"♯"))
        pc += 1;
    else if (accidental == "b" || accidental == juce::String::fromUTF8 (u8"♭"))
        pc -= 1;
    else if (accidental.isNotEmpty())
        return false;

    out = (pc + 12) % 12;
    return true;
}

juce::String ProjectKeys::displayName (const ProjectKey& key)
{
    return rootName (key.root) + (key.mode == KeyMode::minor ? "m" : "");
}

juce::String ProjectKeys::cliText (const ProjectKey& key)
{
    return juce::String (key.root >= 0 && key.root < 12 ? keyRootNamesAscii[key.root] : "C")
           + ":" + modeName (key.mode);
}

std::optional<ProjectKey> ProjectKeys::fromCardText (const juce::String& text)
{
    // "F# major" — 末尾の空白区切りがモード、残りがルート（card.py の parse_key と同じ分け方）
    const auto trimmed = text.trim();
    const int lastSpace = trimmed.lastIndexOfChar (' ');
    if (lastSpace <= 0)
        return std::nullopt;

    ProjectKey key;
    if (! rootFromName (trimmed.substring (0, lastSpace), key.root)
        || ! modeFromName (trimmed.substring (lastSpace + 1), key.mode))
        return std::nullopt;
    return key;
}

juce::String SectionMarkers::typeName (SectionType type)
{
    switch (type)
    {
        case SectionType::intro:  return "intro";
        case SectionType::verse:  return "verse";
        case SectionType::hook:   return "hook";
        case SectionType::bridge: return "bridge";
        case SectionType::outro:  return "outro";
        case SectionType::other:  return "other";
    }
    return "other";
}

bool SectionMarkers::typeFromName (const juce::String& name, SectionType& out)
{
    for (auto type : allTypes)
    {
        if (typeName (type) == name)
        {
            out = type;
            return true;
        }
    }
    return false;
}

void SectionMarkers::set (std::vector<SectionMarker>& markers, int startBeats, SectionType type)
{
    if (startBeats < 0)
        return;

    const auto pos = std::lower_bound (markers.begin(), markers.end(), startBeats,
                                       [] (const SectionMarker& m, int beats) { return m.startBeats < beats; });
    if (pos != markers.end() && pos->startBeats == startBeats)
        pos->type = type; // 同一位置への追加は種別変更として扱う
    else
        markers.insert (pos, { startBeats, type });
}

void SectionMarkers::removeAt (std::vector<SectionMarker>& markers, int index)
{
    if (index >= 0 && index < (int) markers.size())
        markers.erase (markers.begin() + index);
}

int SectionMarkers::clampStartBeats (const std::vector<SectionMarker>& markers, int index, int newStartBeats)
{
    if (index < 0 || index >= (int) markers.size())
        return newStartBeats;

    const int lo = index > 0 ? markers[(size_t) (index - 1)].startBeats + 1 : 0;
    const int hi = index + 1 < (int) markers.size() ? markers[(size_t) (index + 1)].startBeats - 1
                                                    : std::numeric_limits<int>::max();
    return juce::jlimit (lo, juce::jmax (lo, hi), newStartBeats);
}

juce::String SectionMarkers::displayName (const std::vector<SectionMarker>& markers, int index)
{
    if (index < 0 || index >= (int) markers.size())
        return {};

    const auto type = markers[(size_t) index].type;
    int total = 0, ordinal = 0;
    for (int i = 0; i < (int) markers.size(); ++i)
    {
        if (markers[(size_t) i].type != type)
            continue;
        ++total;
        if (i <= index)
            ++ordinal;
    }
    // Logic式: 同種別が1個だけなら番号なし、2個以上で出現順に採番
    return total >= 2 ? typeName (type) + juce::String (ordinal) : typeName (type);
}

juce::File Project::projectsRoot()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("daw");
}

std::shared_ptr<juce::AudioBuffer<float>> Project::loadWav (const juce::File& file,
                                                           double* sourceSampleRate)
{
    if (sourceSampleRate != nullptr)
        *sourceSampleRate = 0.0;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return nullptr;

    // SRを要求されたのに取れない（0以下）ファイルは読込失敗として扱う。
    // 正常なWAVならリーダーから必ず取得できるので、デバイスSRで代用するフォールバックは置かない
    // （代用の責務がデバイスSRを知る SynthBank 側に移るだけで経路が増える）
    if (sourceSampleRate != nullptr)
    {
        if (reader->sampleRate <= 0.0)
            return nullptr;
        *sourceSampleRate = reader->sampleRate;
    }

    // 1chはモノ、2ch以上は先頭2chのみ（バッファのch数がreadの読み取りch数を決める）
    const int numChannels = reader->numChannels >= 2 ? 2 : 1;
    auto buffer = std::make_shared<juce::AudioBuffer<float>> (numChannels, (int) reader->lengthInSamples);
    reader->read (buffer.get(), 0, (int) reader->lengthInSamples, 0, true, numChannels >= 2);
    return buffer;
}

bool Project::save (juce::String& error, const juce::StringArray& keepReferencedWavs)
{
    directory.createDirectory();

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", currentVersion);
    root->setProperty ("nextId", (juce::int64) nextId);
    root->setProperty ("bpm", bpm);
    root->setProperty ("sampleRate", sampleRate);
    root->setProperty ("memo", memo);

    // キー（v13）。未設定はJSONを汚さない（loopCountと同じ流儀）
    if (key.has_value())
    {
        auto* keyObj = new juce::DynamicObject();
        keyObj->setProperty ("root", key->root);
        keyObj->setProperty ("mode", ProjectKeys::modeName (key->mode));
        root->setProperty ("key", juce::var (keyObj));
    }

    // 採用ループのアンカー（v14）。未採用はJSONを汚さない。
    // 不正な値は書き込まない（読込側の検証と対にして、壊れたアンカーを永続化の外で殺す）
    if (loopAnchor.has_value() && loopAnchor->isValid())
    {
        auto* anchorObj = new juce::DynamicObject();
        anchorObj->setProperty ("libraryPath", loopAnchor->libraryPath);
        anchorObj->setProperty ("bpm", loopAnchor->bpm);
        auto* anchorKey = new juce::DynamicObject();
        anchorKey->setProperty ("root", loopAnchor->key.root);
        anchorKey->setProperty ("mode", ProjectKeys::modeName (loopAnchor->key.mode));
        anchorObj->setProperty ("key", juce::var (anchorKey));
        anchorObj->setProperty ("loopBars", loopAnchor->loopBars);
        anchorObj->setProperty ("slotsPerBar", loopAnchor->slotsPerBar);
        juce::Array<juce::var> rootsArray, confArray;
        for (int r : loopAnchor->roots)
            rootsArray.add (r);
        for (float c : loopAnchor->confidence)
            confArray.add ((double) c);
        anchorObj->setProperty ("roots", rootsArray);
        anchorObj->setProperty ("confidence", confArray);
        anchorObj->setProperty ("degraded", loopAnchor->degraded);
        root->setProperty ("loopAnchor", juce::var (anchorObj));
    }

    juce::Array<juce::var> tracksArray;
    for (auto& track : tracks)
    {
        auto* trackObj = new juce::DynamicObject();
        trackObj->setProperty ("id", (juce::int64) track.id);
        trackObj->setProperty ("type", track.type == TrackType::midi ? "midi" : "audio");
        trackObj->setProperty ("name", track.name);
        trackObj->setProperty ("mute", track.params->mute.load());
        trackObj->setProperty ("solo", track.params->solo.load());
        trackObj->setProperty ("volume", (double) track.params->gain.load());
        trackObj->setProperty ("pan", (double) track.params->pan.load());

        juce::Array<juce::var> sendsArray;
        for (int b = 0; b < numSendBuses; ++b)
            sendsArray.add ((double) track.params->sends[b].load());
        trackObj->setProperty ("sends", sendsArray);

        // 固定ストリップFX。後続スライスで各FXのパラメータを同じオブジェクトに足す
        auto* eqObj = new juce::DynamicObject();
        eqObj->setProperty ("enabled", track.params->eqEnabled.load());
        auto* compObj = new juce::DynamicObject();
        compObj->setProperty ("enabled", track.params->compEnabled.load());
        auto* fxObj = new juce::DynamicObject();
        fxObj->setProperty ("eq", juce::var (eqObj));
        fxObj->setProperty ("comp", juce::var (compObj));
        trackObj->setProperty ("fx", juce::var (fxObj));

        if (track.type == TrackType::audio)
        {
            juce::Array<juce::var> clipsArray;
            for (auto& clip : track.clips)
            {
                auto* clipObj = new juce::DynamicObject();
                clipObj->setProperty ("file", clip.fileName);
                if (clip.name.isNotEmpty()) // 表示名は取り込みクリップのみ（録音クリップのJSONを汚さない）
                    clipObj->setProperty ("name", clip.name);
                if (clip.loopSource.isNotEmpty()) // ループ由来クリップのみ（v14。出自表示専用）
                    clipObj->setProperty ("loopSource", clip.loopSource);
                clipObj->setProperty ("startSample", clip.startSample);
                clipObj->setProperty ("offsetSamples", clip.offsetSamples);
                clipObj->setProperty ("lengthSamples", clip.lengthSamples);
                clipObj->setProperty ("muted", clip.muted);
                if (clip.loopCount > 0) // ループなしはJSONを汚さない（旧形式と同じ見た目のまま）
                    clipObj->setProperty ("loopCount", clip.loopCount);
                if (clip.gain != 1.0f) // ユニティはJSONを汚さない（loopCountと同じ流儀）
                    clipObj->setProperty ("gain", (double) clip.gain);
                if (clip.fadeInSamples > 0) // フェードなしはJSONを汚さない（同上）
                    clipObj->setProperty ("fadeInSamples", clip.fadeInSamples);
                if (clip.fadeOutSamples > 0)
                    clipObj->setProperty ("fadeOutSamples", clip.fadeOutSamples);
                clipsArray.add (juce::var (clipObj));
            }
            trackObj->setProperty ("clips", clipsArray);
        }
        else
        {
            trackObj->setProperty ("instrument",
                                   track.instrument == InstrumentKind::sample ? "sample" : "gm");
            trackObj->setProperty ("gmProgram", track.gmProgram);
            trackObj->setProperty ("drums", track.drums);
            trackObj->setProperty ("drumPitch", track.drumPitch);

            // サンプル音源の設定（instrument が gm に戻っていても保持する＝プルダウンで戻せる）。
            // sampleSourceRate はWAV実体から復元できるが、ファイル欠損時のログ・将来の診断のためJSONにも書く
            if (track.sampleFile.isNotEmpty())
            {
                trackObj->setProperty ("sampleFile", track.sampleFile);
                trackObj->setProperty ("sampleName", track.sampleName);
                trackObj->setProperty ("samplePitchFollow", track.samplePitchFollow);
                trackObj->setProperty ("sampleMono", track.sampleMono);
                trackObj->setProperty ("sampleRootNote", track.sampleRootNote);
                trackObj->setProperty ("sampleGain", (double) track.sampleGain);
                trackObj->setProperty ("sampleStartOffset", track.sampleStartOffset);
                trackObj->setProperty ("sampleSourceRate", track.sampleSourceRate);
            }

            juce::Array<juce::var> regionsArray;
            for (auto& region : track.midiRegions)
            {
                auto* regionObj = new juce::DynamicObject();
                regionObj->setProperty ("id", (juce::int64) region.id);
                regionObj->setProperty ("startPpq", region.startPpq);
                regionObj->setProperty ("lengthPpq", region.lengthPpq);
                regionObj->setProperty ("muted", region.muted);
                if (region.loopCount > 0) // ループなしはJSONを汚さない
                    regionObj->setProperty ("loopCount", region.loopCount);

                juce::Array<juce::var> notesArray;
                for (auto& note : region.notes)
                {
                    auto* noteObj = new juce::DynamicObject();
                    noteObj->setProperty ("id", (juce::int64) note.id);
                    noteObj->setProperty ("pitch", note.pitch);
                    noteObj->setProperty ("startPpq", note.startPpq);
                    noteObj->setProperty ("lengthPpq", note.lengthPpq);
                    noteObj->setProperty ("velocity", note.velocity);
                    notesArray.add (juce::var (noteObj));
                }
                regionObj->setProperty ("notes", notesArray);
                regionsArray.add (juce::var (regionObj));
            }
            trackObj->setProperty ("regions", regionsArray);
        }
        tracksArray.add (juce::var (trackObj));
    }
    root->setProperty ("tracks", tracksArray);

    juce::Array<juce::var> markersArray;
    for (auto& marker : markers)
    {
        auto* markerObj = new juce::DynamicObject();
        markerObj->setProperty ("bar", marker.bar());
        markerObj->setProperty ("beat", marker.beat());
        markerObj->setProperty ("type", SectionMarkers::typeName (marker.type));
        markersArray.add (juce::var (markerObj));
    }
    root->setProperty ("markers", markersArray);

    // サイクル（ループ）範囲。16分音符単位・[start, end)
    auto* cycleObj = new juce::DynamicObject();
    cycleObj->setProperty ("start", cycleStartSixteenths);
    cycleObj->setProperty ("end", cycleEndSixteenths);
    cycleObj->setProperty ("enabled", cycleEnabled);
    root->setProperty ("cycle", juce::var (cycleObj));

    // 曲末フェードアウト（v12）。単位・書式はサイクルと同じ
    auto* fadeOutObj = new juce::DynamicObject();
    fadeOutObj->setProperty ("start", fadeOutStartSixteenths);
    fadeOutObj->setProperty ("end", fadeOutEndSixteenths);
    root->setProperty ("fadeOut", juce::var (fadeOutObj));

    // 固定バス3本（並びは SendBuses::names と対応）とMaster
    juce::Array<juce::var> busesArray;
    for (int b = 0; b < numSendBuses; ++b)
    {
        auto* busObj = new juce::DynamicObject();
        busObj->setProperty ("gain", (double) busParams[b]->gain.load());
        busObj->setProperty ("mute", busParams[b]->mute.load());
        busesArray.add (juce::var (busObj));
    }
    root->setProperty ("buses", busesArray);

    auto* masterObj = new juce::DynamicObject();
    masterObj->setProperty ("gain", (double) masterParams->gain.load());
    root->setProperty ("master", juce::var (masterObj));

    const auto jsonFile = directory.getChildFile ("project.json");
    if (! jsonFile.replaceWithText (juce::JSON::toString (juce::var (root))))
    {
        error = jp (u8"project.json の書き込みに失敗しました");
        return false;
    }

    // どのクリップ・サンプル音源からも参照されていないWAVを掃除する。
    // クリップ削除は「モデルから外すだけ」で、実ファイルの削除は保存時にここでまとめて行う
    // （未保存のまま終了→再読込しても欠損しないようにするため）。
    // keepReferencedWavs（undo/redo履歴が参照するファイル）はredoでの復元に備えて消さない
    juce::StringArray referenced (keepReferencedWavs);
    for (auto& track : tracks)
    {
        for (auto& clip : track.clips)
            referenced.add (clip.fileName);
        if (track.sampleFile.isNotEmpty()) // GMへ戻していても参照は残す（プルダウンで戻せるため）
            referenced.add (track.sampleFile);
    }

    for (const char* pattern : { "clip-*.wav", "instr-*.wav" })
        for (auto& file : directory.findChildFiles (juce::File::findFiles, false, pattern))
            if (! referenced.contains (file.getFileName()))
                file.deleteFile();

    return true;
}

std::unique_ptr<Project> Project::load (const juce::File& dir,
                                        juce::StringArray& warnings,
                                        juce::String& error)
{
    const auto jsonFile = dir.getChildFile ("project.json");
    const auto parsed = juce::JSON::parse (jsonFile.loadFileAsString());

    if (! parsed.isObject())
    {
        error = jp (u8"project.json を読み込めませんでした: ") + jsonFile.getFullPathName();
        return nullptr;
    }

    auto project = std::make_unique<Project>();
    project->directory = dir;

    const int version = (int) parsed.getProperty ("version", 1);
    if (version > currentVersion)
        warnings.add (jp (u8"より新しいバージョンで保存されたプロジェクトです（一部読み飛ばす可能性があります）"));

    project->bpm = juce::jlimit (30.0, 300.0, (double) parsed.getProperty ("bpm", 120.0));
    project->sampleRate = juce::jmax (0.0, (double) parsed.getProperty ("sampleRate", 0.0));
    project->memo = parsed.getProperty ("memo", "").toString();

    // キー（v12以前は無い → 未設定）。壊れた値（root範囲外・未知mode）は未設定化して警告する
    // — プロジェクト全体を開けなくするほどの破損ではなく、既存データを救う側に倒す
    if (const auto keyVar = parsed.getProperty ("key", {}); keyVar.isObject())
    {
        const int keyRoot = (int) keyVar.getProperty ("root", -1);
        const auto modeText = keyVar.getProperty ("mode", "").toString();
        KeyMode keyMode;
        if (keyRoot >= 0 && keyRoot < 12 && ProjectKeys::modeFromName (modeText, keyMode))
            project->key = ProjectKey { keyRoot, keyMode };
        else
            warnings.add (jp (u8"キーの値が不正なため未設定として読み込みました: root=")
                          + juce::String (keyRoot) + " mode=" + modeText);
    }

    // 採用ループのアンカー（v13以前は無い → 未採用）。壊れた値は未採用化して警告する
    // — キーと同じ「既存データを救う側」の方針。生成入力としての厳密検証（即エラー）は
    // bass.py 側の契約で行われるので、ここで捨てておけば壊れた値が生成に届くことはない。
    // オブジェクトですらない値（"broken" 等）も「欠損」と区別して警告する
    if (const auto anchorVar = parsed.getProperty ("loopAnchor", {});
        ! anchorVar.isVoid() && ! anchorVar.isObject())
        warnings.add (jp (u8"採用ループの値が不正なため未採用として読み込みました: ")
                      + anchorVar.toString());
    if (const auto anchorVar = parsed.getProperty ("loopAnchor", {}); anchorVar.isObject())
    {
        // 整数フィールドは**型も**契約どおりに要求する（2.0 のような浮動小数を黙って切り捨てて
        // 正常値に化けさせない — 契約は looproots.py の「0..11 の整数」）
        const auto strictInt = [] (const juce::var& v, int& out) -> bool
        {
            if (! v.isInt() && ! v.isInt64())
                return false;
            // 64bit値は int の範囲確認をしてから落とす（4294967296 が環境依存で 0 へ化けるのを防ぐ）
            const auto raw = (juce::int64) v;
            if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max())
                return false;
            out = (int) raw;
            return true;
        };

        LoopAnchor anchor;
        bool typesOk = true;

        // 文字列・数値・boolも型を要求する（"85" の bpm・数値の libraryPath 等が
        // toString/キャストで正常値に化けるのを防ぐ）
        const auto libVar = anchorVar.getProperty ("libraryPath", {});
        typesOk = libVar.isString() && typesOk;
        anchor.libraryPath = libVar.toString();
        const auto bpmVar = anchorVar.getProperty ("bpm", {});
        typesOk = (bpmVar.isDouble() || bpmVar.isInt() || bpmVar.isInt64()) && typesOk;
        anchor.bpm = (double) bpmVar;
        typesOk = strictInt (anchorVar.getProperty ("loopBars", {}), anchor.loopBars) && typesOk;
        typesOk = strictInt (anchorVar.getProperty ("slotsPerBar", {}), anchor.slotsPerBar) && typesOk;
        const auto degVar = anchorVar.getProperty ("degraded", {});
        typesOk = degVar.isBool() && typesOk;
        anchor.degraded = (bool) degVar;

        bool keyOk = false;
        if (const auto akVar = anchorVar.getProperty ("key", {}); akVar.isObject())
        {
            int akRoot = -1;
            auto akMode = KeyMode::major;
            if (strictInt (akVar.getProperty ("root", {}), akRoot) // 11.9 を 11 に化けさせない
                && akRoot >= 0 && akRoot < 12
                && ProjectKeys::modeFromName (akVar.getProperty ("mode", "").toString(), akMode))
            {
                anchor.key = ProjectKey { akRoot, akMode };
                keyOk = true;
            }
        }
        if (auto* rootsArray = anchorVar.getProperty ("roots", {}).getArray())
        {
            for (auto& r : *rootsArray)
            {
                int value = -1;
                typesOk = strictInt (r, value) && typesOk;
                anchor.roots.push_back (value);
            }
        }
        if (auto* confArray = anchorVar.getProperty ("confidence", {}).getArray())
        {
            for (auto& c : *confArray)
            {
                typesOk = (c.isDouble() || c.isInt() || c.isInt64()) && typesOk;
                anchor.confidence.push_back ((float) (double) c);
            }
        }

        if (keyOk && typesOk && anchor.isValid())
            project->loopAnchor = std::move (anchor);
        else
            warnings.add (jp (u8"採用ループの値が不正なため未採用として読み込みました: ")
                          + anchor.libraryPath);
    }
    project->nextId = (juce::uint64) juce::jmax ((juce::int64) 1, (juce::int64) parsed.getProperty ("nextId", 1));

    // 同一WAVを参照する複数クリップ（分割・複製後）が別々の全量バッファを持たないよう、
    // fileName 単位でロード結果を共有する（読めなかったWAVも nullptr を記録して再デコードを避ける）
    std::map<juce::String, std::shared_ptr<juce::AudioBuffer<float>>> wavCache;

    if (auto* tracksArray = parsed.getProperty ("tracks", {}).getArray())
    {
        for (auto& trackVar : *tracksArray)
        {
            if (! trackVar.isObject())
                continue;

            Track track;
            track.id = (juce::uint64) juce::jmax ((juce::int64) 0, (juce::int64) trackVar.getProperty ("id", 0));
            track.type = trackVar.getProperty ("type", "audio").toString() == "midi" ? TrackType::midi
                                                                                     : TrackType::audio;
            track.name = trackVar.getProperty ("name", jp (u8"トラック")).toString();
            track.params->mute.store ((bool) trackVar.getProperty ("mute", false));
            track.params->solo.store ((bool) trackVar.getProperty ("solo", false));
            track.params->gain.store (juce::jlimit (0.0f, 1.0f,
                                                    (float) (double) trackVar.getProperty ("volume", 0.8)));
            // v3以前は pan/sends が無い（デフォルト: pan=0・send=0）
            track.params->pan.store (juce::jlimit (-1.0f, 1.0f,
                                                   (float) (double) trackVar.getProperty ("pan", 0.0)));
            if (auto* sendsArray = trackVar.getProperty ("sends", {}).getArray())
                for (int b = 0; b < numSendBuses && b < sendsArray->size(); ++b)
                    track.params->sends[b].store (juce::jlimit (0.0f, 1.0f,
                                                                (float) (double) (*sendsArray)[b]));

            // 固定ストリップFXのON/OFF（欠損＝旧形式はON補完）
            const auto fxVar = trackVar.getProperty ("fx", {});
            track.params->eqEnabled.store ((bool) fxVar.getProperty ("eq", {}).getProperty ("enabled", true));
            track.params->compEnabled.store ((bool) fxVar.getProperty ("comp", {}).getProperty ("enabled", true));

            if (track.type == TrackType::audio)
            {
                if (auto* clipsArray = trackVar.getProperty ("clips", {}).getArray())
                {
                    for (auto& clipVar : *clipsArray)
                    {
                        if (! clipVar.isObject())
                            continue;

                        Clip clip;
                        clip.fileName = clipVar.getProperty ("file", "").toString();
                        clip.name = clipVar.getProperty ("name", "").toString(); // v5以前は無い（空=無ラベル）
                        clip.loopSource = clipVar.getProperty ("loopSource", "").toString(); // v13以前は無い（空=ループ由来でない）
                        clip.startSample = (juce::int64) clipVar.getProperty ("startSample", 0);
                        clip.muted = (bool) clipVar.getProperty ("muted", false);
                        // v8以前は無い（欠損＝ループなし）。上限は展開量の暴走を防ぐ安全弁
                        clip.loopCount = juce::jlimit (0, maxLoopCount,
                                                       (int) clipVar.getProperty ("loopCount", 0));
                        // v9以前は無い（欠損＝ユニティ）。UIが表現できない値を残さないようクランプする
                        clip.gain = GainScale::clampLinear (
                            (float) (double) clipVar.getProperty ("gain", 1.0));
                        // v10以前は無い（欠損＝フェードなし）。不変条件の強制は
                        // 参照範囲の確定後に clampFades() で行う（全長が要るため）
                        clip.fadeInSamples = (juce::int64) clipVar.getProperty ("fadeInSamples", 0);
                        clip.fadeOutSamples = (juce::int64) clipVar.getProperty ("fadeOutSamples", 0);

                        const auto cached = wavCache.find (clip.fileName);
                        if (cached != wavCache.end())
                            clip.audio = cached->second;
                        else
                            wavCache[clip.fileName] = clip.audio = loadWav (dir.getChildFile (clip.fileName));

                        if (clip.audio == nullptr)
                        {
                            warnings.add (clip.fileName + jp (u8" を読み込めないためスキップしました"));
                            continue;
                        }

                        // v2以前は offset/length が無い（全長参照）。v3は不正値をクランプする。
                        // 順序が重要: offset を先にバッファ内へ収めてから length を残り範囲へ収める
                        // （offset + length を先に計算すると手編集JSONの極端値でオーバーフローするため）
                        const auto bufferLength = (juce::int64) clip.audio->getNumSamples();
                        clip.offsetSamples = juce::jlimit ((juce::int64) 0, bufferLength,
                                                           (juce::int64) clipVar.getProperty ("offsetSamples", 0));
                        clip.lengthSamples = juce::jlimit ((juce::int64) 0, bufferLength - clip.offsetSamples,
                                                           (juce::int64) clipVar.getProperty ("lengthSamples",
                                                                                              bufferLength - clip.offsetSamples));
                        if (clip.lengthSamples <= 0)
                        {
                            warnings.add (clip.fileName + jp (u8" の参照範囲が不正なためスキップしました"));
                            continue;
                        }

                        clip.clampFades(); // 全長（ループ込み）が確定した後に不変条件を強制する
                        clip.buildPeakCache();
                        track.clips.push_back (std::move (clip));
                    }
                }
            }
            else
            {
                track.gmProgram = juce::jlimit (0, 127, (int) trackVar.getProperty ("gmProgram", 0));
                track.drums = (bool) trackVar.getProperty ("drums", false);
                track.drumPitch = juce::jlimit (-1, 127, (int) trackVar.getProperty ("drumPitch", -1));

                // サンプル音源（v7以前は無い → instrument = gm）。
                // 設定値は先にクランプし、WAV実体を読めたときだけ instrument = sample を成立させる
                track.sampleFile = trackVar.getProperty ("sampleFile", "").toString();
                track.sampleName = trackVar.getProperty ("sampleName", "").toString();
                track.samplePitchFollow = (bool) trackVar.getProperty ("samplePitchFollow", false);
                track.sampleMono = (bool) trackVar.getProperty ("sampleMono", false); // 欠損=OFF（重ねる）
                track.sampleRootNote = juce::jlimit (0, 127, (int) trackVar.getProperty ("sampleRootNote", 60));
                // 範囲外はここで寄せる（UIが表現できない値をモデルに残すと
                // 「表示は-12dBなのに-20dBで鳴る」という表示と実音の乖離になる）
                track.sampleGain = GainScale::clampLinear (
                    (float) (double) trackVar.getProperty ("sampleGain", 1.0));
                track.sampleStartOffset = juce::jmax ((juce::int64) 0,
                                                     (juce::int64) trackVar.getProperty ("sampleStartOffset", 0));
                // 元SRはまずJSONの記録値を復元しておく（WAVが欠けていても診断用の値を失わず、
                // 開いて保存し直しても0で上書きされない）。読めたら実体の値で上書きする
                track.sampleSourceRate = juce::jmax (0.0, (double) trackVar.getProperty ("sampleSourceRate", 0.0));
                const bool wantsSample =
                    trackVar.getProperty ("instrument", "gm").toString() == "sample";

                if (track.sampleFile.isNotEmpty())
                {
                    // 元SRはWAV実体の値を優先する（JSONの値は診断用）
                    double sourceRate = 0.0;
                    track.sampleAudio = loadWav (dir.getChildFile (track.sampleFile), &sourceRate);
                    if (track.sampleAudio != nullptr)
                    {
                        track.sampleSourceRate = sourceRate;
                        track.sampleStartOffset = juce::jlimit ((juce::int64) 0,
                                                                (juce::int64) track.sampleAudio->getNumSamples() - 1,
                                                                track.sampleStartOffset);
                        track.samplePeakCache = buildFullPeakCache (*track.sampleAudio);
                        track.instrument = wantsSample ? InstrumentKind::sample : InstrumentKind::gm;
                    }
                    else if (wantsSample)
                    {
                        // 勝手にGM音源へ戻すと音が変わって混乱するため、無音のまま警告する
                        warnings.add (track.sampleFile
                                      + jp (u8" を読み込めないため、このトラックは無音になります"));
                        track.instrument = InstrumentKind::sample;
                    }
                }

                if (auto* regionsArray = trackVar.getProperty ("regions", {}).getArray())
                {
                    for (auto& regionVar : *regionsArray)
                    {
                        if (! regionVar.isObject())
                            continue;

                        MidiRegion region;
                        region.id = (juce::uint64) juce::jmax ((juce::int64) 0,
                                                               (juce::int64) regionVar.getProperty ("id", 0));
                        region.startPpq = juce::jmax ((juce::int64) 0,
                                                      (juce::int64) regionVar.getProperty ("startPpq", 0));
                        region.lengthPpq = juce::jmax ((juce::int64) 1,
                                                       (juce::int64) regionVar.getProperty ("lengthPpq", Ppq::ticksPerBar));
                        region.muted = (bool) regionVar.getProperty ("muted", false);
                        // v8以前は無い（欠損＝ループなし）。上限は展開量の暴走を防ぐ安全弁
                        region.loopCount = juce::jlimit (0, maxLoopCount,
                                                         (int) regionVar.getProperty ("loopCount", 0));

                        if (auto* notesArray = regionVar.getProperty ("notes", {}).getArray())
                        {
                            for (auto& noteVar : *notesArray)
                            {
                                if (! noteVar.isObject())
                                    continue;

                                MidiNote note;
                                note.id = (juce::uint64) juce::jmax ((juce::int64) 0,
                                                                     (juce::int64) noteVar.getProperty ("id", 0));
                                note.pitch = (int) noteVar.getProperty ("pitch", 60);
                                note.startPpq = (juce::int64) noteVar.getProperty ("startPpq", 0);
                                note.lengthPpq = (juce::int64) noteVar.getProperty ("lengthPpq", Ppq::ticksPerQuarter);
                                note.velocity = (int) noteVar.getProperty ("velocity", 100);
                                region.clampNote (note); // 範囲外の値はここで不変条件内に丸める
                                region.notes.push_back (note);
                            }
                        }
                        track.midiRegions.push_back (std::move (region));
                    }
                }
            }
            project->tracks.push_back (std::move (track));
        }
    }

    // セクションマーカー。不正値（bar < 1・beat範囲外・未知type・重複位置）は警告付きで捨てる。
    // barの上限は設けない（後方のマーカーはタイムラインのコンテンツ幅を伸ばして表示する）。
    // beat は省略可（旧形式=小節頭のみのプロジェクトは beat 0 として読む）
    if (auto* markersArray = parsed.getProperty ("markers", {}).getArray())
    {
        for (auto& markerVar : *markersArray)
        {
            if (! markerVar.isObject())
                continue;

            const int bar = (int) markerVar.getProperty ("bar", 0);
            const int beat = (int) markerVar.getProperty ("beat", 0);
            const auto typeStr = markerVar.getProperty ("type", "").toString();
            SectionType type;
            if (bar < 1 || beat < 0 || beat > 3 || ! SectionMarkers::typeFromName (typeStr, type))
            {
                warnings.add (jp (u8"不正なマーカーをスキップしました: bar=") + juce::String (bar)
                              + " beat=" + juce::String (beat) + " type=" + typeStr);
                continue;
            }
            const int startBeats = (bar - 1) * 4 + beat;
            const bool duplicate = std::any_of (project->markers.begin(), project->markers.end(),
                                                [startBeats] (const SectionMarker& m)
                                                { return m.startBeats == startBeats; });
            if (duplicate)
            {
                warnings.add (jp (u8"同じ位置のマーカーが重複しているためスキップしました: bar=")
                              + juce::String (bar) + " beat=" + juce::String (beat));
                continue;
            }
            SectionMarkers::set (project->markers, startBeats, type); // 昇順はsetが保つ
        }
    }

    // サイクル範囲（v4以前は無い → 既定値: 範囲なし・OFF）。
    // 不正値（start >= end で範囲が成立しない等）は範囲なしへ落とし、enabledも立てない
    if (const auto cycleVar = parsed.getProperty ("cycle", {}); cycleVar.isObject())
    {
        const int start = juce::jmax (0, (int) cycleVar.getProperty ("start", 0));
        const int end = juce::jmax (0, (int) cycleVar.getProperty ("end", 0));
        if (start < end)
        {
            project->cycleStartSixteenths = start;
            project->cycleEndSixteenths = end;
            project->cycleEnabled = (bool) cycleVar.getProperty ("enabled", false);
        }
    }

    // 曲末フェードアウト（v11以前は無い → 既定値: 未設定）。
    // サイクルと同じく、範囲が成立しない値（start >= end）は未設定へ落とす
    if (const auto fadeVar = parsed.getProperty ("fadeOut", {}); fadeVar.isObject())
    {
        const int start = juce::jmax (0, (int) fadeVar.getProperty ("start", 0));
        const int end = juce::jmax (0, (int) fadeVar.getProperty ("end", 0));
        if (start < end)
        {
            project->fadeOutStartSixteenths = start;
            project->fadeOutEndSixteenths = end;
        }
    }

    // 固定バス・Master（v3以前は無い → unityParams()の初期値 gain=1.0 のまま）
    if (auto* busesArray = parsed.getProperty ("buses", {}).getArray())
    {
        for (int b = 0; b < numSendBuses && b < busesArray->size(); ++b)
        {
            const auto& busVar = (*busesArray)[b];
            if (! busVar.isObject())
                continue;
            project->busParams[b]->gain.store (juce::jlimit (0.0f, 1.0f,
                                                             (float) (double) busVar.getProperty ("gain", 1.0)));
            project->busParams[b]->mute.store ((bool) busVar.getProperty ("mute", false));
        }
    }
    if (const auto masterVar = parsed.getProperty ("master", {}); masterVar.isObject())
        project->masterParams->gain.store (juce::jlimit (0.0f, 1.0f,
                                                         (float) (double) masterVar.getProperty ("gain", 1.0)));

    project->ensureUniqueIds();
    return project;
}

// 未採番(0)・重複のIDを振り直す。v1プロジェクト（ID無し）の移行と、手編集されたJSONへの防御を兼ねる
void Project::ensureUniqueIds()
{
    // まず既存IDの最大値に合わせて採番カウンタを進める（振り直しが既存IDと衝突しないように）
    juce::uint64 maxId = 0;
    for (auto& track : tracks)
    {
        maxId = juce::jmax (maxId, track.id);
        for (auto& region : track.midiRegions)
        {
            maxId = juce::jmax (maxId, region.id);
            for (auto& note : region.notes)
                maxId = juce::jmax (maxId, note.id);
        }
    }
    nextId = juce::jmax (nextId, maxId + 1);

    std::vector<juce::uint64> seen;
    auto assignIfNeeded = [this, &seen] (juce::uint64& id)
    {
        if (id == 0 || std::find (seen.begin(), seen.end(), id) != seen.end())
            id = allocateId();
        seen.push_back (id);
    };

    for (auto& track : tracks)
    {
        assignIfNeeded (track.id);
        for (auto& region : track.midiRegions)
        {
            assignIfNeeded (region.id);
            for (auto& note : region.notes)
                assignIfNeeded (note.id);
        }
    }
}

std::unique_ptr<Project> Project::createNew (const juce::File& dir, juce::String& error)
{
    if (dir.exists())
    {
        error = jp (u8"同名のプロジェクトが既にあります");
        return nullptr;
    }
    if (! dir.createDirectory())
    {
        error = jp (u8"フォルダを作成できませんでした: ") + dir.getFullPathName();
        return nullptr;
    }

    auto project = std::make_unique<Project>();
    project->directory = dir;

    Track track;
    track.id = project->allocateId();
    track.name = jp (u8"トラック 1");
    project->tracks.push_back (std::move (track));

    if (! project->save (error))
        return nullptr;

    return project;
}

juce::File Project::nextClipFile() const
{
    for (int i = 1; i < 10000; ++i)
    {
        auto file = directory.getChildFile (juce::String::formatted ("clip-%03d.wav", i));
        if (! file.existsAsFile())
            return file;
    }
    jassertfalse;
    return directory.getChildFile ("clip-overflow.wav");
}

juce::File Project::nextInstrumentFile() const
{
    // 既存トラックが参照している名前も避ける（保存前＝実ファイルがまだ無い場合の衝突防止）
    for (int i = 1; i < 10000; ++i)
    {
        const auto name = juce::String::formatted ("instr-%03d.wav", i);
        auto file = directory.getChildFile (name);
        if (file.existsAsFile())
            continue;
        const bool referenced = std::any_of (tracks.begin(), tracks.end(),
                                             [&name] (const Track& t) { return t.sampleFile == name; });
        if (! referenced)
            return file;
    }
    jassertfalse;
    return directory.getChildFile ("instr-overflow.wav");
}

int Project::lastItemEndSixteenths (double sr) const
{
    // 16分音符1つ分の長さ。オーディオはサンプル、MIDIはPPQで持っているので単位ごとに割る
    const double sixteenthSamples = sr > 0.0
                                        ? sr * 60.0 / juce::jlimit (20.0, 400.0, bpm) * 4.0 / 16.0
                                        : 0.0;
    constexpr double sixteenthTicks = (double) Ppq::ticksPerQuarter / 4.0;

    int result = 0;
    const auto bump = [&result] (double sixteenths)
    {
        // 端数は切り上げる（最近傍だと終端が手前へ吸着して最後の音を切る）
        result = juce::jmax (result, (int) std::ceil (sixteenths));
    };

    for (const auto& track : tracks)
    {
        // トラックのミュート・ソロは見ない（一時的なモニター状態であって曲の長さではない）
        for (const auto& clip : track.clips)
            if (! clip.muted && sixteenthSamples > 0.0)
                bump ((double) (clip.startSample + clip.totalLengthSamples()) / sixteenthSamples);

        if (track.type == TrackType::midi)
            for (const auto& region : track.midiRegions)
                if (! region.muted)
                    bump ((double) (region.startPpq + region.totalLengthPpq()) / sixteenthTicks);
    }

    return result;
}

int Project::resolveSongFadeEnd (int startSixteenths, double sr) const
{
    // 鳴るアイテムが無ければフェードを引く対象がない。**既存フェードの有無より先に見る**
    // （後で見ると、全アイテムを消した後でも既存終端を使って開始点だけ動かせてしまう）
    if (lastItemEndSixteenths (sr) <= 0)
        return 0;

    const int end = hasFadeOut() ? fadeOutEndSixteenths : lastItemEndSixteenths (sr);
    return startSixteenths < end ? end : 0;
}

std::unique_ptr<PlaybackSnapshot> Project::buildSnapshot (SnapshotChange change)
{
    // 既定（midiStructure）は世代を進める＝エンジンに消音＋跨ぎノート再発音をさせる。
    // audioValuesOnly は据え置き（鳴っているMIDIを乱さずにオーディオ側の値だけ差し替える）、
    // offlineRender はエンジンへ渡さない構築なので世代に触らない（進めると次のaudioValuesOnlyが誤認される）
    if (change == SnapshotChange::midiStructure)
        ++midiGeneration;

    auto snapshot = std::make_unique<PlaybackSnapshot>();
    snapshot->midiGeneration = midiGeneration;
    snapshot->tracks.reserve (tracks.size());

    for (int b = 0; b < numSendBuses; ++b)
        snapshot->busParams[b] = busParams[b];
    snapshot->masterParams = masterParams;
    snapshot->fadeOutStartSixteenths = fadeOutStartSixteenths;
    snapshot->fadeOutEndSixteenths = fadeOutEndSixteenths;

    for (auto& track : tracks)
    {
        TrackPlayback trackPlayback;
        trackPlayback.trackId = track.id;
        trackPlayback.params = track.params;

        if (track.type == TrackType::audio)
        {
            for (auto& clip : track.clips)
            {
                if (clip.audio == nullptr || clip.muted)
                    continue;
                if (clip.audio->getNumChannels() >= 2)
                    trackPlayback.hasStereoClip = true; // エンジンのトラック経路選択（モノのみ=現行経路）
                appendClipPlaybacks (clip, trackPlayback.clips); // ループ展開・範囲クランプはヘルパー側
            }
        }
        else
        {
            // ノートを絶対PPQへフラット化し、（ループの各反復ごとに）リージョン境界でマスクする。
            // synth の参照は呼び出し側（MainComponent::pushSnapshot）が SynthBank から埋める。
            //
            // 固定ピッチ置換（Kick等）は**GM音源専用の規則**。GMのドラムマップへ「その打楽器の
            // ピッチ」を送る必要があるためで、サンプラーの固定モードでは置換しない
            // （サンプラーはピッチを見ずに等速で鳴らすので置換が無意味＝規則が増えるだけ）。
            // 同じ規則が BounceRenderer::buildItemRender にもあるので、変えるときは両方を見ること
            const bool fixedPitch = track.instrument == InstrumentKind::gm
                                    && track.drums && track.drumPitch >= 0;
            for (auto& region : track.midiRegions)
            {
                if (region.muted) // ミュート除外はここ（展開ヘルパーはミュートを判断しない）
                    continue;
                appendRegionNotes (region, fixedPitch ? track.drumPitch : -1, trackPlayback.notes);
            }
            // stable_sort: 同時刻ノートの順序をリージョン/ノートの並びで決定的にする（イベント上限時の挙動を再現可能に）
            std::stable_sort (trackPlayback.notes.begin(), trackPlayback.notes.end(),
                              [] (const MidiNotePlayback& a, const MidiNotePlayback& b)
                              { return a.startPpq < b.startPpq; });
        }
        snapshot->tracks.push_back (std::move (trackPlayback));
    }

    return snapshot;
}
