#include "PitchCorrection.h"

#include <algorithm>
#include <cmath>

namespace
{
juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }

juce::var refToVar (const BoundaryRef& r)
{
    switch (r.kind)
    {
        case BoundaryRef::Kind::domainStart: return juce::var ("start");
        case BoundaryRef::Kind::domainEnd: return juce::var ("end");
        case BoundaryRef::Kind::node: break;
    }
    return juce::var (r.index);
}

std::optional<BoundaryRef> refFromVar (const juce::var& v)
{
    if (v.isString())
    {
        if (v.toString() == "start") return BoundaryRef::domainStart();
        if (v.toString() == "end") return BoundaryRef::domainEnd();
        return std::nullopt;
    }
    if (v.isInt() || v.isInt64() || v.isDouble())
    {
        const auto d = (double) v;
        if (! std::isfinite (d) || d < 0 || d > 1e7 || std::floor (d) != d)
            return std::nullopt;
        return BoundaryRef::node ((int) d);
    }
    return std::nullopt;
}

juce::int64 roundMul (juce::int64 delta, double ratio)
{
    return (juce::int64) std::llround ((double) delta * ratio);
}

// timeNodes に index を挿入/削除したあと、ノートの参照 index を付け替える
void shiftNodeRefs (PitchCorrection& pc, int insertedAt, int delta)
{
    for (auto& n : pc.notes)
    {
        if (n.start.isNode() && n.start.index >= insertedAt) n.start.index += delta;
        if (n.end.isNode() && n.end.index >= insertedAt) n.end.index += delta;
    }
}
} // namespace

// ---- PitchCorrection ----

juce::var PitchCorrection::toJson() const
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("curveDigest", curveDigest.toHex());
    switch (scaleMode)
    {
        case PitchScaleMode::projectKey: obj->setProperty ("scale", "projectKey"); break;
        case PitchScaleMode::chromatic: obj->setProperty ("scale", "chromatic"); break;
        case PitchScaleMode::custom:
        {
            auto* k = new juce::DynamicObject();
            k->setProperty ("root", customKey.root);
            k->setProperty ("mode", ProjectKeys::modeName (customKey.mode));
            obj->setProperty ("scale", juce::var (k));
            break;
        }
    }
    obj->setProperty ("strength", (double) strength);
    obj->setProperty ("speedMs", (double) speedMs);
    juce::Array<juce::var> nodes;
    for (const auto& n : timeNodes)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("source", n.sourceSample);
        if (n.timingDeltaSamples != 0)
            o->setProperty ("delta", n.timingDeltaSamples);
        nodes.add (juce::var (o));
    }
    obj->setProperty ("timeNodes", nodes);
    juce::Array<juce::var> ns;
    for (const auto& n : notes)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("start", refToVar (n.start));
        o->setProperty ("end", refToVar (n.end));
        o->setProperty ("target", n.targetMidi);
        if (n.bypass)
            o->setProperty ("bypass", true);
        if (n.pinned)
            o->setProperty ("pinned", true);
        ns.add (juce::var (o));
    }
    obj->setProperty ("notes", ns);
    return juce::var (obj);
}

std::optional<PitchCorrection> PitchCorrection::fromJson (const juce::var& v)
{
    auto* obj = v.getDynamicObject();
    if (obj == nullptr)
        return std::nullopt;
    PitchCorrection pc;
    auto digest = ContentDigest::fromHex (obj->getProperty ("curveDigest").toString());
    if (! digest.has_value() || digest->isNull())
        return std::nullopt; // curveDigest は必須
    pc.curveDigest = *digest;

    const auto scale = obj->getProperty ("scale");
    if (scale.isString())
    {
        if (scale.toString() == "chromatic") pc.scaleMode = PitchScaleMode::chromatic;
        else if (scale.toString() == "projectKey") pc.scaleMode = PitchScaleMode::projectKey;
        else return std::nullopt;
    }
    else if (auto* k = scale.getDynamicObject())
    {
        KeyMode mode = KeyMode::minor;
        const auto root = k->getProperty ("root");
        if (! root.isInt() || (int) root < 0 || (int) root > 11
            || ! ProjectKeys::modeFromName (k->getProperty ("mode").toString(), mode))
            return std::nullopt;
        pc.scaleMode = PitchScaleMode::custom;
        pc.customKey = { (int) root, mode };
    }
    else if (! scale.isVoid())
        return std::nullopt;

    const auto strength = obj->getProperty ("strength");
    const auto speed = obj->getProperty ("speedMs");
    if (! strength.isVoid())
    {
        const double s = (double) strength;
        if (! std::isfinite (s) || s < 0.0 || s > 1.0) return std::nullopt;
        pc.strength = (float) s;
    }
    if (! speed.isVoid())
    {
        const double s = (double) speed;
        if (! std::isfinite (s) || s < 0.0 || s > 10000.0) return std::nullopt;
        pc.speedMs = (float) s;
    }

    if (auto* nodes = obj->getProperty ("timeNodes").getArray())
    {
        for (const auto& nv : *nodes)
        {
            auto* o = nv.getDynamicObject();
            if (o == nullptr) return std::nullopt;
            const auto src = o->getProperty ("source");
            const auto delta = o->getProperty ("delta");
            if (! (src.isInt() || src.isInt64())) return std::nullopt;
            if (! delta.isVoid() && ! (delta.isInt() || delta.isInt64())) return std::nullopt; // NaN 等は拒否
            pc.timeNodes.push_back ({ (juce::int64) src, delta.isVoid() ? 0 : (juce::int64) delta });
        }
    }
    if (auto* ns = obj->getProperty ("notes").getArray())
    {
        for (const auto& nv : *ns)
        {
            auto* o = nv.getDynamicObject();
            if (o == nullptr) return std::nullopt;
            auto s = refFromVar (o->getProperty ("start"));
            auto e = refFromVar (o->getProperty ("end"));
            const auto target = o->getProperty ("target");
            if (! s.has_value() || ! e.has_value() || ! target.isInt()) return std::nullopt;
            if ((int) target < 0 || (int) target > 127) return std::nullopt;
            const bool bypass = (bool) o->getProperty ("bypass");
            pc.notes.push_back ({ *s, *e, (int) target, bypass, ! bypass && (bool) o->getProperty ("pinned") }); // bypass ⇒ pinned=false に正規化
        }
    }
    return pc;
}

ContentDigest PitchCorrection::digest() const
{
    DigestBuilder d;
    d.add ((std::uint64_t) curveDigest.a);
    d.add ((std::uint64_t) curveDigest.b);
    // scaleMode / customKey は含めない: スナップの「次回の設定」であって音には影響しない（固定方式。
    // 音を決めるのは targetMidi）。含めると設定を変えただけで再レンダーが走る
    d.add (strength);
    d.add (speedMs);
    d.add ((std::int32_t) timeNodes.size());
    for (const auto& n : timeNodes)
    {
        d.add ((std::int64_t) n.sourceSample);
        d.add ((std::int64_t) n.timingDeltaSamples);
    }
    d.add ((std::int32_t) notes.size());
    for (const auto& n : notes)
    {
        d.add ((std::int32_t) n.start.kind); d.add ((std::int32_t) n.start.index);
        d.add ((std::int32_t) n.end.kind); d.add ((std::int32_t) n.end.index);
        d.add ((std::int32_t) n.targetMidi);
        d.add ((std::int32_t) (n.bypass ? 1 : 0));
        d.add ((std::int32_t) (n.pinned && ! n.bypass ? 1 : 0)); // bypass 中の pinned は音に出ない（旧 JSON の正規化で指紋を変えない）
    }
    return d.finish();
}

bool PitchCorrection::isAudiblyNeutral() const
{
    if (strength > 0.0f)
        return false;
    for (const auto& n : notes)
        if (n.pinned && ! n.bypass)
            return false;
    for (const auto& n : timeNodes)
        if (n.timingDeltaSamples != 0)
            return false;
    return true;
}

juce::int64 PitchCorrection::resolve (const BoundaryRef& ref, juce::int64 domainOffset, juce::int64 domainLength) const
{
    switch (ref.kind)
    {
        case BoundaryRef::Kind::domainStart: return domainOffset;
        case BoundaryRef::Kind::domainEnd: return domainOffset + domainLength;
        case BoundaryRef::Kind::node: break;
    }
    if (ref.index < 0 || ref.index >= (int) timeNodes.size())
        return -1;
    return timeNodes[(size_t) ref.index].sourceSample;
}

bool PitchCorrection::validate (juce::int64 domainOffset, juce::int64 domainLength, juce::String* why) const
{
    auto fail = [why] (const juce::String& reason)
    {
        if (why != nullptr) *why = reason;
        return false;
    };
    if (curveDigest.isNull()) return fail (jp (u8"curveDigest が無い"));
    if (! std::isfinite (strength) || strength < 0.0f || strength > 1.0f) return fail (jp (u8"strength が値域外"));
    if (! std::isfinite (speedMs) || speedMs < 0.0f) return fail (jp (u8"speedMs が値域外"));
    if (domainLength <= 0) return fail (jp (u8"domain が空"));
    const auto domainEnd = domainOffset + domainLength;
    for (size_t i = 0; i < timeNodes.size(); ++i)
    {
        const auto& n = timeNodes[i];
        if (n.sourceSample <= domainOffset || n.sourceSample >= domainEnd) return fail (jp (u8"time node が開区間の外"));
        if (i > 0 && n.sourceSample <= timeNodes[i - 1].sourceSample) return fail (jp (u8"time node が昇順でない／重複"));
        if (std::llabs (n.timingDeltaSamples) > (juce::int64) 1 << 40) return fail (jp (u8"Δ が巨大"));
    }
    std::vector<std::pair<juce::int64, juce::int64>> ranges;
    for (const auto& n : notes)
    {
        auto check = [&] (const BoundaryRef& r)
        {
            return ! r.isNode() || (r.index >= 0 && r.index < (int) timeNodes.size());
        };
        if (! check (n.start) || ! check (n.end)) return fail (jp (u8"ノートの node index が範囲外"));
        const auto s = resolve (n.start, domainOffset, domainLength), e = resolve (n.end, domainOffset, domainLength);
        if (s >= e) return fail (jp (u8"ノートの開始が終了以降"));
        if (n.targetMidi < 0 || n.targetMidi > 127) return fail (jp (u8"targetMidi が値域外"));
        ranges.push_back ({ s, e });
    }
    for (size_t i = 1; i < ranges.size(); ++i)
        if (ranges[i].first < ranges[i - 1].second) return fail (jp (u8"ノートが重なる／昇順でない"));
    return true;
}

// ---- PitchCorrections ----

namespace
{
struct Placement
{
    std::vector<juce::int64> source;  // 端点込み
    std::vector<juce::int64> initial; // 初期配置
    std::vector<juce::int64> lower;   // lower[i] = 区間 (i-1, i) の出力長の下限（lower[0] は未使用）
    std::vector<juce::int64> placed;  // 射影後
};

Placement place (const PitchCorrection& pc, juce::int64 domainOffset, juce::int64 domainLength,
                 double ratio, double sampleRate)
{
    Placement p;
    const auto minSeg = (juce::int64) std::llround (PitchCorrections::minSegmentMs / 1000.0 * sampleRate);
    p.source.push_back (domainOffset);
    for (const auto& n : pc.timeNodes) p.source.push_back (n.sourceSample);
    p.source.push_back (domainOffset + domainLength);
    const size_t count = p.source.size();
    p.initial.resize (count); p.lower.assign (count, 0); p.placed.resize (count);
    for (size_t i = 0; i < count; ++i)
        p.initial[i] = roundMul (p.source[i] - domainOffset, ratio);
    p.initial.back() = roundMul (domainLength, ratio);
    for (size_t i = 1; i < count; ++i)
        p.lower[i] = juce::jmin (minSeg, p.initial[i] - p.initial[i - 1]);

    // 候補 = 初期 + Δ（端点は固定）
    std::vector<juce::int64> cand = p.initial;
    for (size_t i = 1; i + 1 < count; ++i)
        cand[i] += pc.timeNodes[i - 1].timingDeltaSamples;

    // 左から: 直前の確定位置 + 下限 以上
    p.placed[0] = 0;
    for (size_t i = 1; i + 1 < count; ++i)
        p.placed[i] = juce::jmax (cand[i], p.placed[i - 1] + p.lower[i]);
    p.placed[count - 1] = p.initial.back();
    // 右から: 次の確定位置 − 次の区間の下限 以下
    for (size_t i = count - 1; i-- > 1;)
        p.placed[i] = juce::jmin (p.placed[i], p.placed[i + 1] - p.lower[i + 1]);
    // 射影で左の制約が崩れていないか（初期配置が実現可能なら崩れない）。崩れていたら初期配置へ戻す
    for (size_t i = 1; i < count; ++i)
        if (p.placed[i] - p.placed[i - 1] < p.lower[i])
        {
            p.placed = p.initial;
            break;
        }
    return p;
}

TimeMap toTimeMap (const Placement& p)
{
    TimeMap m;
    for (size_t i = 0; i < p.source.size(); ++i)
        m.nodes.push_back ({ p.source[i], p.placed[i] });
    return m;
}

// ノート index i の両端を「端点込みのノード列」の index に写す（端点 = 0 / last）
int placementIndex (const BoundaryRef& r, size_t count)
{
    switch (r.kind)
    {
        case BoundaryRef::Kind::domainStart: return 0;
        case BoundaryRef::Kind::domainEnd: return (int) count - 1;
        case BoundaryRef::Kind::node: break;
    }
    return r.index + 1;
}
} // namespace

TimeMap PitchCorrections::buildTimeMap (const PitchCorrection& pc, juce::int64 domainOffset, juce::int64 domainLength,
                                        double stretchRatio, double sampleRate)
{
    return toTimeMap (place (pc, domainOffset, domainLength, stretchRatio, sampleRate));
}

int PitchCorrections::snapToScale (double midi, const std::optional<ProjectKey>& scale)
{
    if (! scale.has_value())
        return (int) std::floor (midi + 0.5);
    static const int majorDegrees[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static const int minorDegrees[7] = { 0, 2, 3, 5, 7, 8, 10 };
    const int* degrees = scale->mode == KeyMode::minor ? minorDegrees : majorDegrees;
    int best = 0;
    double bestDistance = 1e9;
    for (int cand = (int) std::floor (midi) - 6; cand <= (int) std::ceil (midi) + 6; ++cand)
    {
        const int pc = ((cand - scale->root) % 12 + 12) % 12;
        bool inScale = false;
        for (int d = 0; d < 7; ++d) inScale = inScale || degrees[d] == pc;
        if (! inScale)
            continue;
        const double dist = std::abs (cand - midi);
        if (dist < bestDistance - 1e-9 || (std::abs (dist - bestDistance) < 1e-9 && cand > best))
        {
            best = cand;
            bestDistance = dist;
        }
    }
    return best;
}

std::optional<ProjectKey> PitchCorrections::effectiveScale (const PitchCorrection& pc,
                                                            const std::optional<ProjectKey>& projectKey)
{
    switch (pc.scaleMode)
    {
        case PitchScaleMode::chromatic: return std::nullopt;
        case PitchScaleMode::custom: return pc.customKey;
        case PitchScaleMode::projectKey: break;
    }
    return projectKey; // 未設定ならクロマチック
}

std::optional<double> PitchCorrections::noteMedianMidi (const PitchCurve& curve, juce::int64 startSample, juce::int64 endSample)
{
    if (curve.hopSamples <= 0)
        return std::nullopt;
    std::vector<double> v;
    const int k0 = (int) ((startSample + curve.hopSamples - 1) / curve.hopSamples); // 中心が範囲内の最初のフレーム
    const int k1 = (int) juce::jmin ((juce::int64) curve.numFrames(), (endSample + curve.hopSamples - 1) / curve.hopSamples);
    for (int k = juce::jmax (0, k0); k < k1; ++k)
        if (curve.isVoiced (k))
            v.push_back (curve.midiAt (k));
    if (v.empty())
        return std::nullopt;
    std::sort (v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

PitchCorrections::TargetCurve PitchCorrections::targetCurve (const PitchCorrection& pc, const PitchCurve& curve,
                                                             juce::int64 domainOffset, juce::int64 domainLength,
                                                             int transposeSemitones)
{
    TargetCurve out;
    const int hop = curve.hopSamples;
    if (hop <= 0 || curve.numFrames() == 0)
        return out;
    const int k0 = juce::jlimit (0, curve.numFrames(), (int) (domainOffset / hop));
    const int k1 = juce::jlimit (k0, curve.numFrames(), (int) ((domainOffset + domainLength + hop - 1) / hop));
    out.firstFrame = k0;
    out.shiftSemitones.assign ((size_t) (k1 - k0), (float) transposeSemitones);

    const double hopMs = hop / curve.sampleRate * 1000.0;
    const double alpha = pc.speedMs <= 0.0f ? 1.0 : 1.0 - std::exp (-hopMs / (double) pc.speedMs);
    for (const auto& note : pc.notes)
    {
        if (note.bypass)
            continue;
        const auto s = pc.resolve (note.start, domainOffset, domainLength);
        const auto e = pc.resolve (note.end, domainOffset, domainLength);
        if (s < 0 || e <= s)
            continue;
        const auto median = noteMedianMidi (curve, s, e);
        if (! median.has_value())
            continue;
        const double noteStrength = note.pinned ? 1.0 : (double) pc.strength; // 手で置いた音は常に 100%
        double state = note.targetMidi - *median; // 初期値 = 中心のずれ（速さ∞ならこれ一定）
        const int ks = juce::jmax (k0, (int) ((s + hop - 1) / hop));
        const int ke = juce::jmin (k1, (int) ((e + hop - 1) / hop));
        for (int k = ks; k < ke; ++k)
        {
            if (! curve.isVoiced (k))
                continue; // 有声マスク: 無声フレームは transpose のみ
            const double err = note.targetMidi - curve.midiAt (k);
            state += alpha * (err - state);
            out.shiftSemitones[(size_t) (k - k0)] = (float) (noteStrength * state + transposeSemitones);
        }
    }
    return out;
}

PitchCorrection PitchCorrections::autoSnap (const PitchCurve& curve, const std::vector<DetectedPitchNote>& detected,
                                            juce::int64 domainOffset, juce::int64 domainLength,
                                            const std::optional<ProjectKey>& scale, PitchScaleMode mode,
                                            const ProjectKey& customKey)
{
    PitchCorrection pc;
    pc.curveDigest = curve.digest();
    pc.scaleMode = mode;
    pc.customKey = customKey;
    const auto domainEnd = domainOffset + domainLength;
    const juce::int64 hop = curve.hopSamples;

    // 境界（原音サンプル）を集めてノード化。domain 端に一致する境界はノードにしない
    struct Range { juce::int64 s, e; int target; };
    std::vector<Range> ranges;
    for (const auto& d : detected)
    {
        const auto s = juce::jmax (domainOffset, (juce::int64) d.startFrame * hop);
        const auto e = juce::jmin (domainEnd, (juce::int64) d.endFrame * hop);
        if (e <= s)
            continue; // 交差が空
        ranges.push_back ({ s, e, snapToScale (d.medianMidi, scale) });
    }
    std::vector<juce::int64> boundaries;
    for (const auto& r : ranges)
    {
        if (r.s > domainOffset && r.s < domainEnd) boundaries.push_back (r.s);
        if (r.e > domainOffset && r.e < domainEnd) boundaries.push_back (r.e);
    }
    std::sort (boundaries.begin(), boundaries.end());
    boundaries.erase (std::unique (boundaries.begin(), boundaries.end()), boundaries.end());
    for (auto b : boundaries)
        pc.timeNodes.push_back ({ b, 0 });
    auto refFor = [&] (juce::int64 sample)
    {
        if (sample <= domainOffset) return BoundaryRef::domainStart();
        if (sample >= domainEnd) return BoundaryRef::domainEnd();
        const auto it = std::lower_bound (boundaries.begin(), boundaries.end(), sample);
        return BoundaryRef::node ((int) (it - boundaries.begin()));
    };
    for (const auto& r : ranges)
        pc.notes.push_back ({ refFor (r.s), refFor (r.e), r.target, false });
    return pc;
}

void PitchCorrections::resnap (PitchCorrection& pc, const PitchCurve& curve, juce::int64 domainOffset,
                               juce::int64 domainLength, const std::optional<ProjectKey>& scale)
{
    for (auto& n : pc.notes)
    {
        if (n.pinned)
            continue; // 手で置いた音は付け直さない（手直し済みノートを勝手に変えない）
        const auto s = pc.resolve (n.start, domainOffset, domainLength);
        const auto e = pc.resolve (n.end, domainOffset, domainLength);
        if (auto median = noteMedianMidi (curve, s, e))
            n.targetMidi = snapToScale (*median, scale);
    }
}

bool PitchCorrections::setNoteTarget (PitchCorrection& pc, int noteIndex, int targetMidi, int targetAtStart, bool pinnedAtStart)
{
    if (noteIndex < 0 || noteIndex >= (int) pc.notes.size())
        return false;
    auto& n = pc.notes[(size_t) noteIndex];
    if (n.bypass)
        return false; // bypass 中は音程を置けない（UI・debug とも同じ規則）
    targetMidi = juce::jlimit (0, 127, targetMidi);
    n.targetMidi = targetMidi;
    n.pinned = pinnedAtStart || targetMidi != targetAtStart;
    return targetMidi != targetAtStart || n.pinned != pinnedAtStart;
}

void PitchCorrections::toggleNoteBypass (PitchCorrection& pc, int noteIndex)
{
    if (noteIndex < 0 || noteIndex >= (int) pc.notes.size())
        return;
    setNoteBypass (pc, noteIndex, ! pc.notes[(size_t) noteIndex].bypass);
}

void PitchCorrections::setNoteBypass (PitchCorrection& pc, int noteIndex, bool bypass)
{
    if (noteIndex < 0 || noteIndex >= (int) pc.notes.size())
        return;
    auto& n = pc.notes[(size_t) noteIndex];
    n.bypass = bypass;
    if (bypass)
        n.pinned = false;
}

juce::int64 PitchCorrections::moveNote (PitchCorrection& pc, int noteIndex, juce::int64 deltaRender,
                                        juce::int64 domainOffset, juce::int64 domainLength,
                                        double stretchRatio, double sampleRate)
{
    if (noteIndex < 0 || noteIndex >= (int) pc.notes.size() || deltaRender == 0)
        return 0;
    const auto& note = pc.notes[(size_t) noteIndex];
    if (! note.start.isNode() || ! note.end.isNode())
        return 0; // 端点は固定
    const auto p = place (pc, domainOffset, domainLength, stretchRatio, sampleRate);
    const size_t count = p.source.size();
    const int is = placementIndex (note.start, count), ie = placementIndex (note.end, count);
    if (is <= 0 || ie >= (int) count - 1 || is >= ie)
        return 0;
    // 手前の区間 (is-1, is) と後ろの区間 (ie, ie+1) の下限でクランプ
    const auto lo = -(p.placed[(size_t) is] - p.placed[(size_t) is - 1] - p.lower[(size_t) is]);
    const auto hi = p.placed[(size_t) ie + 1] - p.placed[(size_t) ie] - p.lower[(size_t) ie + 1];
    const auto d = juce::jlimit (juce::jmin ((juce::int64) 0, lo), juce::jmax ((juce::int64) 0, hi), deltaRender);
    if (d == 0)
        return 0;
    // 保存値は「射影後の位置 + d − 初期位置」（表示と一致させる）
    pc.timeNodes[(size_t) note.start.index].timingDeltaSamples = p.placed[(size_t) is] + d - p.initial[(size_t) is];
    pc.timeNodes[(size_t) note.end.index].timingDeltaSamples = p.placed[(size_t) ie] + d - p.initial[(size_t) ie];
    return d;
}

bool PitchCorrections::splitNote (PitchCorrection& pc, int noteIndex, juce::int64 sourceSample,
                                  juce::int64 domainOffset, juce::int64 domainLength,
                                  double stretchRatio, double sampleRate)
{
    if (noteIndex < 0 || noteIndex >= (int) pc.notes.size())
        return false;
    const auto note = pc.notes[(size_t) noteIndex];
    const auto s = pc.resolve (note.start, domainOffset, domainLength);
    const auto e = pc.resolve (note.end, domainOffset, domainLength);
    if (sourceSample <= s || sourceSample >= e)
        return false;
    const auto before = buildTimeMap (pc, domainOffset, domainLength, stretchRatio, sampleRate);
    const auto outputAt = before.map (sourceSample);
    const auto initial = roundMul (sourceSample - domainOffset, stretchRatio);
    // 挿入位置（昇順を保つ）
    int at = 0;
    while (at < (int) pc.timeNodes.size() && pc.timeNodes[(size_t) at].sourceSample < sourceSample)
        ++at;
    if (at < (int) pc.timeNodes.size() && pc.timeNodes[(size_t) at].sourceSample == sourceSample)
        return false;
    pc.timeNodes.insert (pc.timeNodes.begin() + at, TimeNode { sourceSample, outputAt - initial });
    shiftNodeRefs (pc, at, +1);
    auto& left = pc.notes[(size_t) noteIndex];
    PitchNote right = left;
    left.end = BoundaryRef::node (at);
    right.start = BoundaryRef::node (at);
    pc.notes.insert (pc.notes.begin() + noteIndex + 1, right);
    return true;
}

bool PitchCorrections::mergeNotes (PitchCorrection& pc, int leftIndex, juce::int64 domainOffset, juce::int64 domainLength)
{
    if (leftIndex < 0 || leftIndex + 1 >= (int) pc.notes.size())
        return false;
    auto& left = pc.notes[(size_t) leftIndex];
    const auto right = pc.notes[(size_t) leftIndex + 1];
    const auto s = pc.resolve (left.start, domainOffset, domainLength);
    const auto e = pc.resolve (right.end, domainOffset, domainLength);
    if (s < 0 || e <= s)
        return false;
    const auto leftLength = pc.resolve (left.end, domainOffset, domainLength) - s;
    const auto rightLength = e - pc.resolve (right.start, domainOffset, domainLength);
    // 目標: 片側だけ pinned ならその側（手で選んだ音程を残す）。両方 or どちらも pinned でなければ長い側
    const int target = left.pinned != right.pinned ? (left.pinned ? left.targetMidi : right.targetMidi)
                                                   : (rightLength > leftLength ? right.targetMidi : left.targetMidi);
    const bool bypass = left.bypass && right.bypass;
    const bool pinned = left.pinned || right.pinned;
    const auto endRef = right.end;
    pc.notes.erase (pc.notes.begin() + leftIndex + 1);
    auto& merged = pc.notes[(size_t) leftIndex];
    merged.end = endRef;
    merged.targetMidi = target;
    merged.bypass = bypass;
    merged.pinned = pinned && ! bypass;
    // (s, e) の開区間にあるノードをすべて削除（後ろから消して index を付け替える）
    for (int i = (int) pc.timeNodes.size(); i-- > 0;)
    {
        const auto src = pc.timeNodes[(size_t) i].sourceSample;
        if (src > s && src < e)
        {
            pc.timeNodes.erase (pc.timeNodes.begin() + i);
            shiftNodeRefs (pc, i + 1, -1);
        }
    }
    return true;
}

void PitchCorrections::detachToDomain (PitchCorrection& pc, juce::int64 oldOffset, juce::int64 oldLength,
                                       juce::int64 newOffset, juce::int64 newLength)
{
    const auto newEnd = newOffset + newLength;
    // ノートを解決済み座標に展開してから範囲へ切る
    struct Resolved { juce::int64 s, e; int target; bool bypass; bool pinned; };
    std::vector<Resolved> resolved;
    for (const auto& n : pc.notes)
    {
        auto s = pc.resolve (n.start, oldOffset, oldLength);
        auto e = pc.resolve (n.end, oldOffset, oldLength);
        s = juce::jmax (s, newOffset);
        e = juce::jmin (e, newEnd);
        if (e > s)
            resolved.push_back ({ s, e, n.targetMidi, n.bypass, n.pinned });
    }
    // ノードは開区間内だけ残す
    std::vector<TimeNode> kept;
    for (const auto& n : pc.timeNodes)
        if (n.sourceSample > newOffset && n.sourceSample < newEnd)
            kept.push_back (n);
    pc.timeNodes = kept;
    auto refFor = [&] (juce::int64 sample)
    {
        if (sample <= newOffset) return BoundaryRef::domainStart();
        if (sample >= newEnd) return BoundaryRef::domainEnd();
        for (size_t i = 0; i < pc.timeNodes.size(); ++i)
            if (pc.timeNodes[i].sourceSample == sample)
                return BoundaryRef::node ((int) i);
        // ノードが無い境界（あり得ない: 元のノートの境界はノードか端点）→ 新しいノードを足す
        int at = 0;
        while (at < (int) pc.timeNodes.size() && pc.timeNodes[(size_t) at].sourceSample < sample) ++at;
        pc.timeNodes.insert (pc.timeNodes.begin() + at, TimeNode { sample, 0 });
        return BoundaryRef::node (at);
    };
    pc.notes.clear();
    for (const auto& r : resolved)
        pc.notes.push_back ({ refFor (r.s), refFor (r.e), r.target, r.bypass, r.pinned });
    // refFor がノードを足した場合に備えて index を付け直す（昇順なので再解決で一意）
    std::vector<PitchNote> fixed;
    for (auto& n : pc.notes)
    {
        auto fix = [&] (BoundaryRef& ref, juce::int64 sample)
        {
            if (! ref.isNode()) return;
            for (size_t i = 0; i < pc.timeNodes.size(); ++i)
                if (pc.timeNodes[i].sourceSample == sample) { ref.index = (int) i; return; }
        };
        const auto s = n.start.isNode() ? pc.timeNodes[(size_t) n.start.index].sourceSample : 0;
        const auto e = n.end.isNode() ? pc.timeNodes[(size_t) n.end.index].sourceSample : 0;
        fix (n.start, s);
        fix (n.end, e);
        fixed.push_back (n);
    }
    pc.notes = fixed;
}
