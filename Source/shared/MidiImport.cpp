#include "MidiImport.h"

#include <algorithm>
#include <deque>
#include <map>

namespace MidiImport
{
namespace
{
// マージ・ソート用の中間イベント。order は同時刻の処理順（off を先に処理して
// 隣接ノートの off→on を正しく分ける。同時刻の on/off で自分の on を閉じない）
struct NoteEvent
{
    double tick = 0.0;
    int order = 0;      // 0 = off / 1 = on
    int channel = 1;    // 1..16（JUCE の getChannel と同じ1始まり）
    int pitch = 0;
    int velocity = 0;
    juce::int64 seq = 0; // 安定ソート用（同時刻・同順序はファイル出現順）
};

juce::int64 toLalaPpq (double tick, double scale)
{
    return (juce::int64) std::llround (tick * scale);
}

juce::int64 ceilToBar (juce::int64 ppq)
{
    return ((juce::jmax ((juce::int64) 1, ppq) + Ppq::ticksPerBar - 1) / Ppq::ticksPerBar)
           * Ppq::ticksPerBar;
}

void finishNote (std::vector<MidiNote>& dest, double onTick, double offTick,
                 int pitch, int velocity, double scale)
{
    MidiNote note;
    note.pitch = juce::jlimit (0, 127, pitch);
    note.velocity = juce::jlimit (1, 127, velocity);
    note.startPpq = toLalaPpq (onTick, scale);
    // 開始・終端を別々に換算してから差を取る（(off-on) の一括換算だと丸めで終端の絶対位置がずれる）
    note.lengthPpq = juce::jmax ((juce::int64) 1, toLalaPpq (offTick, scale) - note.startPpq);
    dest.push_back (note);
}
} // namespace

bool parse (juce::InputStream& stream, Result& out, juce::String& error)
{
    juce::MidiFile midiFile;
    // createMatchingNoteOffs=false: 対応付けは下の自前仕様で行う（(channel,pitch) キー・
    // FIFO・velocity 0 = off・未クローズはファイル末尾で閉じる）
    if (! midiFile.readFrom (stream, false))
    {
        error = juce::String::fromUTF8 (u8"MIDIファイルとして読めません");
        return false;
    }

    const short timeFormat = midiFile.getTimeFormat();
    if (timeFormat < 0)
    {
        error = juce::String::fromUTF8 (u8"SMPTE形式のMIDIファイルには対応していません");
        return false;
    }
    if (timeFormat == 0)
    {
        error = juce::String::fromUTF8 (u8"MIDIファイルの時間解像度が不正です");
        return false;
    }
    const double scale = (double) Ppq::ticksPerQuarter / (double) timeFormat;

    // 全トラックのノートイベントを吸い上げてマージ（type 0/1 両対応）。
    // テンポ・拍子等のメタイベントは無視するが、「ファイル末尾」の時刻には含める
    // （未クローズ note_on を閉じる位置 = 最終イベント時刻）
    std::vector<NoteEvent> events;
    double lastTimestamp = 0.0;
    juce::int64 seq = 0;
    for (int t = 0; t < midiFile.getNumTracks(); ++t)
    {
        const auto* track = midiFile.getTrack (t);
        for (int i = 0; i < track->getNumEvents(); ++i)
        {
            const auto& msg = track->getEventPointer (i)->message;
            lastTimestamp = juce::jmax (lastTimestamp, msg.getTimeStamp());

            NoteEvent ev;
            ev.tick = msg.getTimeStamp();
            ev.channel = msg.getChannel();
            ev.pitch = msg.getNoteNumber();
            ev.seq = seq++;
            if (msg.isNoteOn (false)) // velocity 0 の note_on は off として下の分岐で拾う
            {
                ev.order = 1;
                ev.velocity = (int) msg.getVelocity();
                events.push_back (ev);
            }
            else if (msg.isNoteOff (true)) // true = velocity 0 の note_on も off 扱い
            {
                ev.order = 0;
                events.push_back (ev);
            }
        }
    }

    std::sort (events.begin(), events.end(), [] (const NoteEvent& a, const NoteEvent& b)
    {
        return std::tie (a.tick, a.order, a.seq) < std::tie (b.tick, b.order, b.seq);
    });

    // (channel, pitch) ごとの未クローズ note_on（FIFO = 最も古いものから閉じる）。
    // pitch だけをキーにすると ch1 と ch2 の同音で他チャンネルの off が誤って閉じる
    struct OpenNote { double tick; int velocity; };
    std::map<std::pair<int, int>, std::deque<OpenNote>> open;
    out = {};
    for (const auto& ev : events)
    {
        auto& queue = open[{ ev.channel, ev.pitch }];
        if (ev.order == 1)
        {
            queue.push_back ({ ev.tick, ev.velocity }); // 重複 note_on は各々独立ノート
            continue;
        }
        if (queue.empty())
            continue; // 孤立 note_off は無視
        const auto on = queue.front();
        queue.pop_front();
        finishNote (ev.channel == 10 ? out.drumNotes : out.otherNotes,
                    on.tick, ev.tick, ev.pitch, on.velocity, scale);
    }
    // クローズされない note_on はファイル末尾（最終イベント時刻）で閉じる
    for (auto& [key, queue] : open)
        for (const auto& on : queue)
            finishNote (key.first == 10 ? out.drumNotes : out.otherNotes,
                        on.tick, lastTimestamp, key.second, on.velocity, scale);

    if (out.drumNotes.empty() && out.otherNotes.empty())
    {
        error = juce::String::fromUTF8 (u8"ノートが1つも含まれていません");
        return false;
    }

    // 表示・編集の安定のため開始位置順に揃える（未クローズ分が末尾に混ざるため）
    const auto byStart = [] (const MidiNote& a, const MidiNote& b)
    { return std::tie (a.startPpq, a.pitch) < std::tie (b.startPpq, b.pitch); };
    std::sort (out.drumNotes.begin(), out.drumNotes.end(), byStart);
    std::sort (out.otherNotes.begin(), out.otherNotes.end(), byStart);

    // リージョン長 = 最終ノート終端の小節切り上げ（グループごとに独立）
    const auto regionLength = [] (const std::vector<MidiNote>& notes) -> juce::int64
    {
        juce::int64 maxEnd = 0;
        for (const auto& n : notes)
            maxEnd = juce::jmax (maxEnd, n.startPpq + n.lengthPpq);
        return notes.empty() ? 0 : ceilToBar (maxEnd);
    };
    out.drumRegionLengthPpq = regionLength (out.drumNotes);
    out.otherRegionLengthPpq = regionLength (out.otherNotes);
    return true;
}

bool parseFile (const juce::File& file, Result& out, juce::String& error)
{
    juce::FileInputStream stream (file);
    if (! stream.openedOk())
    {
        error = juce::String::fromUTF8 (u8"ファイルを開けません: ") + file.getFullPathName();
        return false;
    }
    return parse (stream, out, error);
}

namespace
{
void addTrackWithRegion (Project& project, const std::vector<MidiNote>& notes,
                         juce::int64 regionLengthPpq, const juce::String& trackName,
                         bool drums, juce::int64 startPpq)
{
    Track track;
    track.id = project.allocateId();
    track.type = TrackType::midi;
    track.instrument = InstrumentKind::gm;
    track.gmProgram = 0;   // ドラムは Drum Kit（drums=true・program 未使用）/ 他は Piano
    track.drums = drums;
    track.drumPitch = -1;

    track.name = trackName;

    MidiRegion region;
    region.id = project.allocateId();
    region.startPpq = juce::jmax ((juce::int64) 0, startPpq);
    region.lengthPpq = juce::jmax ((juce::int64) 1, regionLengthPpq);
    region.notes = notes;
    for (auto& note : region.notes)
    {
        note.id = project.allocateId();
        region.clampNote (note);
    }
    track.midiRegions.push_back (std::move (region));
    project.tracks.push_back (std::move (track));
}
} // namespace

ApplyOutcome apply (Project& project, UndoStack& undoStack, const Result& result,
                    const juce::String& name, juce::int64 startPpq)
{
    ApplyOutcome outcome;
    if (result.drumNotes.empty() && result.otherNotes.empty())
        return outcome;

    undoStack.begin (project); // 混在 SMF の2トラックも全体で undo 1件

    outcome.firstTrackIndex = (int) project.tracks.size();
    const bool mixed = ! result.drumNotes.empty() && ! result.otherNotes.empty();
    if (! result.drumNotes.empty())
    {
        // 混在時だけ接尾辞で区別する（単独ならファイル名そのまま）
        const auto trackName = mixed ? name + juce::String::fromUTF8 (u8"（ドラム）") : name;
        addTrackWithRegion (project, result.drumNotes, result.drumRegionLengthPpq,
                            trackName, true, startPpq);
        ++outcome.numTracksCreated;
    }
    if (! result.otherNotes.empty())
    {
        addTrackWithRegion (project, result.otherNotes, result.otherRegionLengthPpq,
                            name, false, startPpq);
        ++outcome.numTracksCreated;
    }
    return outcome;
}
}
