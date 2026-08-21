#include "ClipDomains.h"

#include <map>

namespace
{
// activeDomain の実効指紋（Clip::renderPending と同じ定義）
RenderFingerprint activeFingerprint (const RenderedDomain& domain)
{
    return { domain.sourceAudio.get(), domain.domainOffset, domain.domainLength,
             domain.semitones, domain.ratio, domain.sampleRate, domain.recipeDigest };
}
} // namespace

std::shared_ptr<const RenderedDomain> ClipDomains::makeNeutralDomain (
    const std::shared_ptr<juce::AudioBuffer<float>>& audio,
    juce::int64 domainOffset, juce::int64 domainLength, double sampleRate)
{
    if (audio == nullptr)
        return nullptr;

    auto domain = std::make_shared<RenderedDomain>();
    domain->audio = audio;       // 原音そのもの
    domain->sourceAudio = audio; // 指紋のアドレス再利用を防ぐ強参照（無加工では audio と同一）
    domain->audioBaseOffset = domainOffset;
    domain->domainOffset = domainOffset;
    domain->domainLength = domainLength;
    domain->semitones = 0;
    domain->ratio = 1.0;
    domain->sampleRate = sampleRate;
    domain->timeMap = TimeMap::uniform (domainOffset, domainLength, 1.0);
    domain->peakCache = buildDomainPeakCache (*audio, domainOffset, domainLength,
                                              Clip::samplesPerPeak);
    return domain;
}

bool ClipDomains::domainValidFor (const Clip& clip, double sampleRate)
{
    const auto& d = clip.activeDomain;
    if (d == nullptr || clip.audio == nullptr)
        return false;
    // 原音・SR の一致
    if (d->sourceAudio.get() != clip.audio.get()
        || ! juce::exactlyEqual (d->sampleRate, sampleRate))
        return false;
    // ドメイン自体の健全性（負の開始・原音外を通さない。checked addition）
    const auto sourceLength = (juce::int64) clip.audio->getNumSamples();
    if (d->domainOffset < 0 || d->domainLength < 1 || d->domainLength > sourceLength
        || d->domainOffset > sourceLength - d->domainLength)
        return false;
    // ドメインがクリップ範囲を包含していること。要求値との一致は求めない
    return d->domainOffset <= clip.offsetSamples
        && d->domainOffset + d->domainLength >= clip.offsetSamples + clip.lengthSamples;
}

bool ClipDomains::reconcile (Project& project, double sampleRate)
{
    // 構造が変わった直後の入口なので、ここで未採番・重複のクリップ id も振り直す（分割の右側・複製・
    // ペーストは id=0 で入ってくる）。既存 id は変わらないのでエディタの参照は保たれる
    project.ensureUniqueIds();
    bool changed = false;
    for (auto& track : project.tracks)
    {
        for (auto& clip : track.clips)
        {
            if (clip.audio == nullptr)
                continue;
            if (domainValidFor (clip, sampleRate))
                continue;
            // 包含が崩れた（または未設定）: ドメインをクリップ自身の範囲へリセットして
            // 無加工へ差し替える。要求値（semitones/ratio）は保持し、必要なレンダリングは
            // collectRequests が積み直す
            clip.resetRenderDomainToSelf();
            clip.activeDomain = makeNeutralDomain (clip.audio, clip.offsetSamples,
                                                   clip.lengthSamples, sampleRate);
            changed = true;
        }
    }
    return changed;
}

std::vector<ClipDomains::Request> ClipDomains::collectRequests (
    Project& project, double sampleRate,
    const std::function<std::shared_ptr<const RenderedDomain> (const RenderFingerprint&)>& lookup,
    bool& attachedAny)
{
    attachedAny = false;
    std::map<RenderFingerprint, Request> wanted;

    for (auto& track : project.tracks)
    {
        for (auto& clip : track.clips)
        {
            if (clip.audio == nullptr || ! clip.renderPending (sampleRate))
                continue;

            const auto fingerprint = clip.requestedFingerprint (sampleRate);
            if (fingerprint.isNeutral())
            {
                // 無加工はレンダリング不要で即座に切り替えられる
                clip.activeDomain = makeNeutralDomain (clip.audio, clip.requestedDomainOffset(),
                                                       clip.requestedDomainLength(), sampleRate);
                attachedAny = true;
                continue;
            }
            if (lookup != nullptr)
            {
                if (auto cached = lookup (fingerprint))
                {
                    // キャッシュ済み（undo/redo 直後など）はその場で装着する
                    clip.activeDomain = cached;
                    attachedAny = true;
                    continue;
                }
            }
            // 同じ指紋のクリップ同士（複製）は1件の要求にまとまり、結果の1本を共有する
            if (wanted.count (fingerprint) > 0)
                continue;
            Request request { fingerprint, clip.audio, nullptr };
            if (fingerprint.hasRecipe())
            {
                // 補正付き: ワーカーへ渡す不変の入力をここで固める（以後の編集はこの要求に影響しない）
                auto recipe = std::make_shared<RenderRecipe>();
                recipe->sourceAudio = clip.audio;
                recipe->sampleRate = sampleRate;
                recipe->domainOffset = fingerprint.domainOffset;
                recipe->domainLength = fingerprint.domainLength;
                recipe->transposeSemitones = fingerprint.semitones;
                recipe->stretchRatio = fingerprint.ratio;
                recipe->curve = clip.pitchCurve;
                recipe->correction = *clip.pitchCorrection;
                request.recipe = std::move (recipe);
            }
            wanted.emplace (fingerprint, std::move (request));
        }
    }

    std::vector<Request> requests;
    requests.reserve (wanted.size());
    for (auto& [fingerprint, request] : wanted)
        requests.push_back (request);
    return requests;
}

bool ClipDomains::attachRenderResult (Project& project, double sampleRate,
                                      const std::shared_ptr<const RenderedDomain>& domain)
{
    if (domain == nullptr)
        return false;
    const auto resultFingerprint = activeFingerprint (*domain);

    bool attached = false;
    for (auto& track : project.tracks)
        for (auto& clip : track.clips)
            if (clip.audio != nullptr
                && clip.requestedFingerprint (sampleRate) == resultFingerprint)
            {
                // 新しいドメインへ同時に切り替える（音・長さ・波形が一斉に変わる）。
                // 要求値はすでにこの指紋の値なので触らない
                clip.activeDomain = domain;
                attached = true;
            }
    return attached;
}

bool ClipDomains::rollbackFailedRequest (Project& project, double sampleRate,
                                         const RenderFingerprint& failed)
{
    bool rolledBack = false;
    for (auto& track : project.tracks)
    {
        for (auto& clip : track.clips)
        {
            // 巻き戻すのは「現在の要求指紋が失敗した指紋とまだ一致するクリップ」だけ。
            // +2 実行中に +3 へ変えて古い +2 が失敗した場合、+3 を巻き戻してはいけない
            if (clip.audio == nullptr || clip.requestedFingerprint (sampleRate) != failed)
                continue;

            if (domainValidFor (clip, sampleRate))
            {
                // 直前に成功していた値（実効）へ戻す — 要求だけが進んだ状態を残さない
                const auto& d = *clip.activeDomain;
                clip.transposeSemitones = d.semitones;
                clip.stretchRatio = d.ratio;
                clip.renderDomainOffset = d.domainOffset;
                clip.renderDomainLength = d.domainLength;
                // 補正も「鳴っている音」の状態へ。鳴っている音に補正が入っている（d.correction あり）なら**必ずそれに合わせる**
                // — 中立の手直しを残すと要求 fingerprint（recipe 無し）と実効（recipeDigest あり）が一致せず renderPending が
                // 解けないまま同じ失敗レンダーを繰り返す。手直しが消えるのは残念だがここは「永続状態＝鳴っている音」が優先
                //（失敗自体が maxRenderBytes 超過等の例外経路）。d.correction は d の domain 座標で記録されているので、
                // domain を d に戻すこの枝では座標も整合する
                if (d.correction != nullptr)
                    clip.pitchCorrection = *d.correction;
                else if (! clip.hasNeutralPitchCorrection())
                    clip.pitchCorrection.reset();
                // （d.correction == nullptr で中立補正を保持する場合: そのドメインは補正なしで鳴っており recipe も出ないので
                //   fingerprint は一致する。ノード座標は絶対サンプルで、domain が広がる向きの戻しは検証を破らない）
            }
            else
            {
                // 前例が無ければ無加工へ（原音を「その指紋の生成結果」として装着はしない —
                // 値も 0 / 1.0 に戻すので「永続状態＝鳴っている音」は保たれる）
                clip.transposeSemitones = 0;
                clip.stretchRatio = 1.0;
                if (! clip.hasNeutralPitchCorrection())
                    clip.pitchCorrection.reset();
                clip.resetRenderDomainToSelf();
                clip.activeDomain = makeNeutralDomain (clip.audio, clip.offsetSamples,
                                                       clip.lengthSamples, sampleRate);
            }
            rolledBack = true;
        }
    }
    return rolledBack;
}

bool ClipDomains::applyStretchRequest (Clip& clip, int semitones, double ratio)
{
    // 要求受付層: 安全限界へクランプして受理する（弾いた値を要求値として残さない）
    semitones = ClipStretchLimits::clampSemitones (semitones);
    ratio = ClipStretchLimits::clampRatio (ratio);
    if ((juce::int64) std::llround ((double) clip.lengthSamples * ratio) < 1)
        return false; // view 長 0 になる要求は受理しない
    if (clip.transposeSemitones == semitones && juce::exactlyEqual (clip.stretchRatio, ratio))
        return false;

    clip.transposeSemitones = semitones;
    clip.stretchRatio = ratio;
    // 値を変えたらドメインをクリップ自身の範囲へリセットする（1小節のチョップのために
    // 8小節を再レンダーしない。音が変わる瞬間なので境界の連続性が切れても違和感にならない）。
    // 共有中の補正は resetRenderDomainToSelf が自範囲へ写してから戻す
    clip.resetRenderDomainToSelf();
    return true;
}

double ClipDomains::ratioForBars (double bars, double barLengthSamples,
                                  juce::int64 sourceLengthSamples)
{
    if (! (bars > 0.0) || ! (barLengthSamples > 0.0) || sourceLengthSamples < 1)
        return 1.0;
    return ClipStretchLimits::clampRatio (bars * barLengthSamples / (double) sourceLengthSamples);
}
