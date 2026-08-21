#include "PitchCurve.h"

#include <cmath>
#include <limits>

// ---- ContentDigest / DigestBuilder ----

namespace
{
constexpr std::uint64_t fnvOffsetA = 0xcbf29ce484222325ULL;
constexpr std::uint64_t fnvOffsetB = 0x84222325cbf29ce4ULL; // 2本目は別の初期値（独立したハッシュ列）
constexpr std::uint64_t fnvPrime = 0x100000001b3ULL;

std::uint64_t parseHex16 (const juce::String& s)
{
    std::uint64_t v = 0;
    for (auto c : s)
    {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (std::uint64_t) (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (std::uint64_t) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (std::uint64_t) (c - 'A' + 10);
    }
    return v;
}

bool isHexString (const juce::String& s)
{
    for (auto c : s)
        if (! ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    return s.isNotEmpty();
}
} // namespace

juce::String ContentDigest::toHex() const
{
    return juce::String::toHexString ((juce::int64) a).paddedLeft ('0', 16)
         + juce::String::toHexString ((juce::int64) b).paddedLeft ('0', 16);
}

std::optional<ContentDigest> ContentDigest::fromHex (const juce::String& hex)
{
    if (hex.length() != 32 || ! isHexString (hex))
        return std::nullopt;
    return ContentDigest { parseHex16 (hex.substring (0, 16)), parseHex16 (hex.substring (16, 32)) };
}

DigestBuilder::DigestBuilder() : a (fnvOffsetA), b (fnvOffsetB) {}

void DigestBuilder::add (const void* data, size_t bytes)
{
    const auto* p = static_cast<const unsigned char*> (data);
    for (size_t i = 0; i < bytes; ++i)
    {
        a = (a ^ p[i]) * fnvPrime;
        b = (b ^ (std::uint64_t) (p[i] + 0x5a)) * fnvPrime; // 2本目は入力を少しずらす
    }
}

void DigestBuilder::add (const juce::String& s)
{
    const auto utf8 = s.toRawUTF8();
    add (utf8, std::strlen (utf8));
    add ((std::int32_t) 0); // 区切り（"ab"+"c" と "a"+"bc" を区別）
}

// ---- SourceIdentity ----

SourceIdentity SourceIdentity::of (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    SourceIdentity id;
    id.frames = audio.getNumSamples();
    id.channels = audio.getNumChannels();
    id.sampleRate = sampleRate;
    DigestBuilder d;
    d.add ((std::int64_t) id.frames);
    d.add ((std::int32_t) id.channels);
    d.add (sampleRate);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        d.add (audio.getReadPointer (ch), sizeof (float) * (size_t) audio.getNumSamples());
    id.digest = d.finish();
    return id;
}

// ---- PitchCurve ----

double PitchCurve::midiAt (int k) const
{
    if (! isVoiced (k))
        return std::numeric_limits<double>::quiet_NaN();
    return 69.0 + 12.0 * std::log2 ((double) f0[(size_t) k] / 440.0);
}

ContentDigest PitchCurve::digest() const
{
    DigestBuilder d;
    d.add (algoId);
    d.add (sampleRate);
    d.add ((std::int32_t) hopSamples);
    d.add ((std::int32_t) numFrames());
    d.add (f0.data(), sizeof (float) * f0.size());
    d.add (voicing.data(), sizeof (float) * voicing.size());
    d.add (rms.data(), sizeof (float) * rms.size());
    return d.finish();
}

bool PitchCurve::validate (juce::String* why) const
{
    auto fail = [why] (const juce::String& reason)
    {
        if (why != nullptr)
            *why = reason;
        return false;
    };
    if (algoId.isEmpty()) return fail (juce::String::fromUTF8 (u8"algoId が空"));
    if (! (sampleRate > 0.0) || ! std::isfinite (sampleRate)) return fail (juce::String::fromUTF8 (u8"sampleRate が不正"));
    if (hopSamples <= 0) return fail (juce::String::fromUTF8 (u8"hop が不正"));
    if (voicing.size() != f0.size() || rms.size() != f0.size()) return fail (juce::String::fromUTF8 (u8"配列長が不一致"));
    if (source.frames <= 0 || source.channels <= 0) return fail (juce::String::fromUTF8 (u8"source 識別子が不正"));
    for (size_t k = 0; k < f0.size(); ++k)
    {
        const float f = f0[k], v = voicing[k], r = rms[k];
        if (! std::isfinite (f) || ! std::isfinite (v) || ! std::isfinite (r)) return fail (juce::String::fromUTF8 (u8"非有限値"));
        if (f != 0.0f && (f < 30.0f || f > 2000.0f)) return fail (juce::String::fromUTF8 (u8"f0 が値域外"));
        if (v < 0.0f || v > 1.0f) return fail (juce::String::fromUTF8 (u8"有声度が値域外"));
        if (r < 0.0f) return fail (juce::String::fromUTF8 (u8"rms が負"));
    }
    return true;
}

// ---- サイドカー ----

namespace
{
constexpr char magic[4] = { 'L', 'L', 'P', 'C' };
constexpr std::int32_t formatVersion = 1;
const char* const extension = ".pitch";

juce::String wavStem (const juce::File& wavFile) { return wavFile.getFileNameWithoutExtension(); }
} // namespace

juce::File PitchSidecar::fileFor (const juce::File& wavFile, const ContentDigest& curveDigest)
{
    return wavFile.getSiblingFile (wavStem (wavFile) + "." + curveDigest.toHex() + extension);
}

std::vector<juce::File> PitchSidecar::listFor (const juce::File& wavFile)
{
    std::vector<juce::File> out;
    const auto prefix = wavStem (wavFile) + ".";
    for (const auto& entry : juce::RangedDirectoryIterator (wavFile.getParentDirectory(), false,
                                                            prefix + "*" + extension, juce::File::findFiles))
        if (digestFromFile (entry.getFile()).has_value())
            out.push_back (entry.getFile());
    std::sort (out.begin(), out.end(),
               [] (const juce::File& x, const juce::File& y) { return x.getFileName() < y.getFileName(); });
    return out;
}

std::optional<ContentDigest> PitchSidecar::digestFromFile (const juce::File& sidecar)
{
    if (sidecar.getFileExtension() != extension)
        return std::nullopt;
    const auto stem = sidecar.getFileNameWithoutExtension(); // "clip-001.<hex32>"
    const auto dot = stem.lastIndexOfChar ('.');
    if (dot < 0)
        return std::nullopt;
    return ContentDigest::fromHex (stem.substring (dot + 1));
}

bool PitchSidecar::write (const PitchCurve& curve, const juce::File& wavFile, juce::String* error)
{
    auto fail = [error] (const juce::String& reason)
    {
        if (error != nullptr)
            *error = reason;
        return false;
    };
    juce::String why;
    if (! curve.validate (&why))
        return fail (juce::String::fromUTF8 (u8"カーブが不正: ") + why);

    const auto digest = curve.digest();
    const auto target = fileFor (wavFile, digest);
    if (target.existsAsFile())
        return true; // 同一内容の世代は既にある（世代不変なので書き直さない）

    juce::MemoryOutputStream out;
    out.write (magic, 4);
    out.writeInt (formatVersion);
    out.writeString (curve.algoId);
    out.writeDouble (curve.sampleRate);
    out.writeInt (curve.hopSamples);
    out.writeInt (curve.numFrames());
    out.writeInt64 (curve.source.frames);
    out.writeInt (curve.source.channels);
    out.writeDouble (curve.source.sampleRate);
    out.writeInt64 ((juce::int64) curve.source.digest.a);
    out.writeInt64 ((juce::int64) curve.source.digest.b);
    for (auto v : curve.f0) out.writeFloat (v);
    for (auto v : curve.voicing) out.writeFloat (v);
    for (auto v : curve.rms) out.writeFloat (v);
    out.writeInt64 ((juce::int64) digest.a);
    out.writeInt64 ((juce::int64) digest.b);

    // 一時ファイル → atomic replace（途中まで書かれたファイルを読ませない）
    juce::TemporaryFile temp (target);
    if (! temp.getFile().replaceWithData (out.getData(), out.getDataSize()))
        return fail (juce::String::fromUTF8 (u8"一時ファイルへ書けない: ") + temp.getFile().getFullPathName());
    if (! temp.overwriteTargetFileWithTemporary())
        return fail (juce::String::fromUTF8 (u8"置き換えに失敗: ") + target.getFullPathName());
    return true;
}

std::optional<PitchCurve> PitchSidecar::read (const juce::File& sidecar, juce::String* why)
{
    auto fail = [why] (const juce::String& reason)
    {
        if (why != nullptr)
            *why = reason;
        return std::optional<PitchCurve>();
    };
    juce::MemoryBlock data;
    if (! sidecar.loadFileAsData (data))
        return fail (juce::String::fromUTF8 (u8"読めない"));
    juce::MemoryInputStream in (data, false);
    char m[4] = {};
    if (in.read (m, 4) != 4 || std::memcmp (m, magic, 4) != 0)
        return fail (juce::String::fromUTF8 (u8"magic 不一致"));
    if (in.readInt() != formatVersion)
        return fail (juce::String::fromUTF8 (u8"未知のフォーマット版"));
    PitchCurve c;
    c.algoId = in.readString();
    c.sampleRate = in.readDouble();
    c.hopSamples = in.readInt();
    const int frames = in.readInt();
    c.source.frames = in.readInt64();
    c.source.channels = in.readInt();
    c.source.sampleRate = in.readDouble();
    c.source.digest.a = (std::uint64_t) in.readInt64();
    c.source.digest.b = (std::uint64_t) in.readInt64();
    if (frames < 0 || frames > 50'000'000)
        return fail (juce::String::fromUTF8 (u8"frame count が不正"));
    const auto bytesNeeded = (juce::int64) frames * 3 * (juce::int64) sizeof (float) + 16;
    if (in.getNumBytesRemaining() != bytesNeeded)
        return fail (juce::String::fromUTF8 (u8"サイズ不一致（途中まで書かれた／壊れている）"));
    c.f0.resize ((size_t) frames); c.voicing.resize ((size_t) frames); c.rms.resize ((size_t) frames);
    for (auto& v : c.f0) v = in.readFloat();
    for (auto& v : c.voicing) v = in.readFloat();
    for (auto& v : c.rms) v = in.readFloat();
    ContentDigest stored;
    stored.a = (std::uint64_t) in.readInt64();
    stored.b = (std::uint64_t) in.readInt64();
    if (stored != c.digest())
        return fail (juce::String::fromUTF8 (u8"内容 digest 不一致"));
    if (auto fileDigest = digestFromFile (sidecar); fileDigest.has_value() && *fileDigest != stored)
        return fail (juce::String::fromUTF8 (u8"ファイル名の digest と内容が不一致"));
    juce::String v;
    if (! c.validate (&v))
        return fail (juce::String::fromUTF8 (u8"検証失敗: ") + v);
    return c;
}

std::optional<PitchCurve> PitchSidecar::readFor (const juce::File& wavFile, const ContentDigest& curveDigest,
                                                 const SourceIdentity& expectedSource, juce::String* why)
{
    const auto file = fileFor (wavFile, curveDigest);
    if (! file.existsAsFile())
    {
        if (why != nullptr)
            *why = juce::String::fromUTF8 (u8"サイドカーが無い: ") + file.getFileName();
        return std::nullopt;
    }
    auto curve = read (file, why);
    if (! curve.has_value())
        return std::nullopt;
    if (curve->source != expectedSource)
    {
        if (why != nullptr)
            *why = juce::String::fromUTF8 (u8"元 WAV と食い違う（差し替えられた）");
        return std::nullopt;
    }
    return curve;
}
