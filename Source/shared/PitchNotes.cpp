#include "PitchNotes.h"

#include <algorithm>
#include <cmath>

namespace
{
double medianOf (std::vector<double>& v)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// midi[start, end) の中央値
double medianRange (const std::vector<double>& midi, int start, int end)
{
    std::vector<double> v (midi.begin() + start, midi.begin() + end);
    return medianOf (v);
}
} // namespace

std::vector<DetectedPitchNote> PitchNotes::detect (const PitchCurve& curve)
{
    std::vector<DetectedPitchNote> notes;
    const int n = curve.numFrames();
    if (n <= 0 || curve.hopSamples <= 0)
        return notes;
    const double hopMs = curve.hopSamples / curve.sampleRate * 1000.0;
    const int smoothN = juce::jmax (1, (int) std::llround (smoothMs / hopMs));
    const int holdN = juce::jmax (1, (int) std::llround (holdMs / hopMs));
    const int minN = juce::jmax (1, (int) std::llround (minNoteMs / hopMs));

    std::vector<double> midi ((size_t) n, 0.0);
    for (int k = 0; k < n; ++k)
        midi[(size_t) k] = curve.isVoiced (k) ? curve.midiAt (k) : 0.0;

    auto close = [&] (int start, int end)
    {
        notes.push_back ({ start, end, (float) medianRange (midi, start, end) });
    };

    int k = 0;
    while (k < n)
    {
        if (! curve.isVoiced (k)) { ++k; continue; }
        int e = k;
        while (e < n && curve.isVoiced (e))
            ++e;
        // 有声区間 [k, e) をジャンプで切る
        int start = k;
        int awaySince = -1;
        for (int j = k; j < e; ++j)
        {
            const double cur = medianRange (midi, juce::jmax (start, j - smoothN + 1), j + 1);
            const double ref = (j - start >= smoothN) ? medianRange (midi, start, j + 1) : cur;
            if (std::abs (cur - ref) >= jumpSemitones)
            {
                if (awaySince < 0)
                    awaySince = j;
                if (j - awaySince + 1 >= holdN)
                {
                    // 平滑化の遅れぶん境界を巻き戻す: 生の値が「新しい音」に近い限り手前へ
                    const double newRef = medianRange (midi, awaySince, j + 1);
                    int cut = awaySince;
                    while (cut > start + 1
                           && std::abs (midi[(size_t) cut - 1] - newRef) < std::abs (midi[(size_t) cut - 1] - ref))
                        --cut;
                    close (start, cut);
                    start = cut;
                    awaySince = -1;
                }
            }
            else
                awaySince = -1;
        }
        close (start, e);
        k = e;
    }

    // 短いノートの吸収（同じ有声区間内の直前へ。先頭なら直後へ）
    std::vector<DetectedPitchNote> merged;
    std::vector<bool> shortFlag;
    for (const auto& nt : notes)
    {
        DetectedPitchNote cur = nt;
        if (cur.lengthFrames() < minN)
        {
            if (! merged.empty() && merged.back().endFrame == cur.startFrame)
            {
                auto& prev = merged.back();
                prev.endFrame = cur.endFrame;
                prev.medianMidi = (float) medianRange (midi, prev.startFrame, prev.endFrame);
                if (prev.lengthFrames() >= minN)
                    shortFlag.back() = false; // 短い2つが合わさって十分な長さになれば残す
                continue;
            }
            // 直後へ吸収するため保留
            merged.push_back (cur);
            shortFlag.push_back (true);
            continue;
        }
        if (! merged.empty() && shortFlag.back() && merged.back().endFrame == cur.startFrame)
        {
            const auto prev = merged.back();
            merged.pop_back();
            shortFlag.pop_back();
            cur.startFrame = prev.startFrame;
            cur.medianMidi = (float) medianRange (midi, cur.startFrame, cur.endFrame);
        }
        merged.push_back (cur);
        shortFlag.push_back (false);
    }
    std::vector<DetectedPitchNote> out;
    for (size_t i = 0; i < merged.size(); ++i)
        if (! shortFlag[i])
            out.push_back (merged[i]);
    return out;
}

PitchNotes::KeyEstimate PitchNotes::estimateKey (const std::vector<DetectedPitchNote>& notes)
{
    // Krumhansl–Kessler の調プロファイル（長調・短調。C を根音とした 12 音高クラスの重み）
    static const double majorProfile[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double minorProfile[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    KeyEstimate best;
    double hist[12] = {};
    double total = 0.0;
    for (const auto& n : notes)
    {
        const int pc = ((int) std::floor (n.medianMidi + 0.5f) % 12 + 12) % 12;
        hist[pc] += n.lengthFrames();
        total += n.lengthFrames();
    }
    if (total <= 0.0)
        return best;

    auto correlation = [&] (const double* profile, int root)
    {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) { mx += hist[i]; my += profile[i]; }
        mx /= 12; my /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i)
        {
            const double x = hist[i] - mx;
            const double y = profile[((i - root) % 12 + 12) % 12] - my; // プロファイルを root だけ回す
            sxy += x * y; sxx += x * x; syy += y * y;
        }
        return (sxx > 0 && syy > 0) ? sxy / std::sqrt (sxx * syy) : 0.0;
    };

    best.correlation = -2.0;
    for (int root = 0; root < 12; ++root)
    {
        for (int minor = 0; minor < 2; ++minor)
        {
            const double r = correlation (minor ? minorProfile : majorProfile, root);
            if (r > best.correlation)
            {
                best.correlation = r;
                best.key.root = root;
                best.key.mode = minor ? KeyMode::minor : KeyMode::major;
                best.valid = true;
            }
        }
    }
    return best;
}
