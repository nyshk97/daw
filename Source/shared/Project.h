#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>

#include "PitchCorrection.h"
#include "PlaybackSnapshot.h"
#include "ProjectKey.h"
#include "Ppq.h"
#include "RenderedDomain.h"

// メッセージスレッドが所有するデータモデル。オーディオスレッドへは
// buildSnapshot() で作った PlaybackSnapshot を SnapshotExchange 経由で渡す。

// ループ回数の上限（手編集JSONの極端値で展開量が暴走しないための安全弁。実用上は十分な回数）
inline constexpr int maxLoopCount = 999;

// クリップはソースWAVへの非破壊参照（offsetSamples から lengthSamples 分）。
// 分割・複製で複数クリップが同じ audio（と fileName）を共有する。
// 不変条件: 0 <= offsetSamples / offsetSamples + lengthSamples <= バッファ全長 / lengthSamples >= 1。
// 読込時のクランプと splitClip がこれを保つ
struct Clip
{
    static constexpr int samplesPerPeak = 512; // 描画用ピークキャッシュの集約単位

    juce::String fileName;      // プロジェクトフォルダ相対（例: clip-001.wav）
    juce::String name;          // 表示名。取り込みクリップのみ元ファイル名が入る（録音クリップは空=無ラベル）
    // 採用ループ由来のクリップの出自（ライブラリ相対パス。v14）。**表示専用**で、
    // アンカー（Project::loopAnchor）のライフサイクルには関与しない — 分割・複製・ペーストで
    // 構造体コピーにより継承されるが、それで挙動が変わることはない（設計は loop-track plan）
    juce::String loopSource;
    juce::int64 startSample = 0;
    juce::int64 offsetSamples = 0;  // ソースWAV内の読み出し開始位置
    juce::int64 lengthSamples = 0;  // 再生長（サンプル）
    bool muted = false;         // リージョン単位のミュート（再生スナップショットから除外）
    // 本体の後ろに何回繰り返すか（0 = ループなし）。元を編集すると繰り返し全部に反映される
    // ＝実体は1つ。長さは本体長の整数倍のみ（中途半端な終端を作らない）
    int loopCount = 0;
    // リージョン単位のゲイン（線形倍率。値域は GainScale = ±12dB 相当の約0.251..3.981）。
    // 素材の一部だけを均すトリムで、Track::sampleGain と同じスケール・同じ流儀。
    // 波形の描画振幅にも掛ける（「見た目＝出る音」を保つため）
    float gain = 1.0f;
    // フェードイン/アウト長（サンプル単位・絶対時間。BPM変更の影響を受けない）。
    // 掛かるのは「ループの連なり全体」の先頭と末尾だけで、繰り返しの間はシームレス
    // （各反復に掛けると繰り返すたびに音量が抜けてポンプするため）。カーブはリニア固定。
    // 不変条件: 0 <= fadeInSamples, 0 <= fadeOutSamples,
    //           fadeInSamples + fadeOutSamples <= sourceTotalLengthSamples()（clampFades が強制する。原音座標）
    juce::int64 fadeInSamples = 0;
    juce::int64 fadeOutSamples = 0;
    std::shared_ptr<juce::AudioBuffer<float>> audio; // 1ch=モノ（録音）/ 2ch=ステレオ（取り込み）。メモリ常駐

    // ---- 移調・タイムストレッチ（v20・非破壊）----
    // transposeSemitones / stretchRatio / renderDomain* は**要求値**（まだ音になっていない
    // かもしれない値）。描画・ヒットテスト・再生・終端計算はすべて activeDomain（実効）と、
    // そこから導いた view だけを見る。要求値から見かけ長を計算する経路は作らないこと
    // （レンダー完了前に使うと再生長と実バッファ長が食い違う）。
    // 設計の真実の源: docs/plans/2026-08-18-1028-audio-transpose-stretch.md
    int transposeSemitones = 0; // ±ClipStretchLimits::maxSemitones
    double stretchRatio = 1.0;  // 原音長 → 見かけ長の倍率（0.911 = 短くなる）
    // 要求レンダードメイン（原音座標・永続化）。分割の子は親のドメインを継承し、値を変更したら
    // クリップ自身の範囲へリセットする。length == 0 は「未設定 = 自身の範囲」（requestedDomain* が解決）
    juce::int64 renderDomainOffset = 0;
    juce::int64 renderDomainLength = 0;
    // 実効状態（鳴っている・見えている音）。永続化せず、undo state にも積まない
    // （UndoStack が積む際にクリアする）。有効契約は ClipDomains::domainValidFor —
    // 原音・SR が一致し、ドメインがクリップ範囲を包含していれば、要求値と不一致でも有効
    // （「完了までは古い音」を守るため）
    std::shared_ptr<const RenderedDomain> activeDomain;

    // ---- ボーカルのピッチ補正（v21・非破壊）----
    // 設計の真実の源: docs/plans/2026-08-20-2244-vocal-pitch-correction.md
    // 安定 ID（v21。MidiRegion::id と同じ採番 Project::allocateId）。独立ウィンドウのエディタは index でなく
    // この id でクリップを毎回引き直す。分割の右側・複製・ペーストは 0 にして ensureUniqueIds で採番する
    juce::uint64 id = 0;
    // 編集状態（永続化）。nullopt = 補正なし。分割では丸ごとコピーし表示だけ範囲で絞る（親子で digest が一致し
    // 分割直後は再レンダーしない）。子を編集した瞬間に detachToDomain で自範囲へ写す
    std::optional<PitchCorrection> pitchCorrection;
    // 解析カーブ（永続化しない。読込時に pitchCorrection->curveDigest のサイドカーから読む。分割・複製で共有）
    std::shared_ptr<const PitchCurve> pitchCurve;
    // ピッチエディタの**未確定プレビュー**（永続化しない・undo state にも積まない・指紋判定にも出てこない）。
    // activeDomain を触らずに「鳴っている音・見えている波形」だけを差し替えるための置き場。
    // 再生 snapshot・描画・単独試聴は必ず effectiveDomain() を通す（activeDomain の直読みは未確定中に
    // 補正前の音が鳴る）。renderPending / collectRequests / reconcile からは見えない（巻き戻されない）。
    // 長さは activeDomain と常に同じ（プレビューは音程と目標だけが違い、時間写像は同じ）
    std::shared_ptr<const RenderedDomain> previewDomain;
    const RenderedDomain* effectiveDomain() const
    {
        return previewDomain != nullptr ? previewDomain.get() : activeDomain.get();
    }
    // previewDomain の寿命規則（1 文）: 「プレビューが活動中」か「待っている本レンダーと同じ内容」なら残す。それ以外は外す。
    // activeDomain が変わりうる全経路（本レンダー装着・インライン装着・巻き戻し・reconcile・ジェスチャー終了）の後で呼ぶ。
    // 戻り値: 外したか（snapshot の再 push が要る）
    // 「この指紋の内容が、いま待っている本レンダーそのものか」（プレビュー・in-flight 要求の寿命判定の共通述語）
    bool awaitsRender (const RenderFingerprint& fingerprint, double sampleRate) const
    {
        return renderPending (sampleRate) && fingerprint == requestedFingerprint (sampleRate);
    }
    bool dropPreviewIfCurrent (double sampleRate, bool previewActive)
    {
        if (previewDomain == nullptr || previewActive)
            return false;
        // 活動中でないなら、残してよいのは「待っている本レンダーと同じ内容のプレビュー」だけ
        //（取り消した recipe のレンダー待ちに別内容のプレビューが便乗して残らない。取消・閉じるでも同じ＝旧音へ戻す理由が無い）
        if (awaitsRender (previewDomain->fingerprint(), sampleRate))
            return false;
        previewDomain = nullptr;
        return true;
    }
    // 分割で親のレンダードメインを共有したままか（補正のノート列も親の domain 座標で表現されている）
    bool sharesInheritedDomain() const
    {
        return requestedDomainOffset() != offsetSamples || requestedDomainLength() != lengthSamples;
    }
    // 補正をクリップ自身の範囲で表現したもの（共有中なら detach した複製、そうでなければそのまま）。
    // エディタの working はこれを起点にする（「working は常に自範囲」の不変条件）。実装は Project.cpp
    std::optional<PitchCorrection> pitchCorrectionInOwnDomain() const;
    // 複製・ペースト・分割の右側など「新しい id のクリップ」を作るときの複製。id と未確定プレビューを落とす
    // （音・波形・補正・ドメイン共有は構造体コピーで継承）。新しい複製経路はこれを通すこと
    Clip cloneForNewId() const
    {
        Clip c = *this;
        c.id = 0;
        c.previewDomain = nullptr;
        return c;
    }

    juce::int64 requestedDomainOffset() const
    {
        return renderDomainLength > 0 ? renderDomainOffset : offsetSamples;
    }
    juce::int64 requestedDomainLength() const
    {
        return renderDomainLength > 0 ? renderDomainLength : lengthSamples;
    }
    // レンダードメインをクリップ自身の範囲へ戻す。**親の domain を共有していた補正は先に自範囲へ写す**
    //（写さずに戻すと、以後 sharesInheritedDomain() が false になり誰も detach しない＝親座標のノートが
    // 自範囲で解釈されて誤った目標音・再読込で無効化。呼び出し側の規律でなくここで塞ぐ）。実装は Project.cpp
    void resetRenderDomainToSelf();
    // 現在の補正が中立（Strength 0・Δ 0）か。巻き戻し（ClipDomains::rollbackFailedRequest）で「鳴っている音に補正が
    // 入っていない」ときだけ、この中立補正（ノートの手直し）を保持する判定に使う。鳴っている音に補正が入っている枝では
    // **保持しない**（保持すると要求≠実効のまま renderPending が解けず同じ失敗を繰り返す。plan の方針変更ログ
    // 2026-08-21 /code-review 4 回目を参照）
    bool hasNeutralPitchCorrection() const
    {
        return pitchCorrection.has_value() && pitchCorrection->isAudiblyNeutral();
    }

    // 要求指紋（sampleRate は呼び出し側の実効SR）。RenderCache のキー・pending 判定に使う
    RenderFingerprint requestedFingerprint (double sampleRate) const
    {
        return { audio.get(), requestedDomainOffset(), requestedDomainLength(),
                 transposeSemitones, stretchRatio, sampleRate, requestedRecipeDigest() };
    }
    // 補正の要求 digest（補正なし・カーブ未解決なら null）。補正は「サイドカー（カーブ）が読めている」
    // ときだけ鳴らせる — カーブ無しでは目標カーブを計算できない（有声マスク・元ピッチが要る）
    // 聴感上中立な補正（Strength 0・タイミング未編集）は recipe を出さない＝移調・伸縮だけなら従来の signalsmith 経路、
    // 無加工なら中立ドメイン（WORLD の素通しで音色を変えない。プレビューも同じ規則＝聴いた音と確定した音が一致する）
    ContentDigest requestedRecipeDigest() const
    {
        return pitchCorrection.has_value() && pitchCurve != nullptr && ! pitchCorrection->isAudiblyNeutral()
                 ? pitchCorrection->digest() : ContentDigest{};
    }

    // 要求と実効の不一致（レンダリング待ち）。この間は長さ依存操作（分割・終端直後へ複製）を
    // 無効にする — startSample は landing 後に追従しないため、実効 1.0 / 要求 1.5 の処理中に
    // 切ると重なりが生まれる
    bool renderPending (double sampleRate) const
    {
        if (activeDomain == nullptr)
            return ! requestedFingerprint (sampleRate).isNeutral();
        return requestedFingerprint (sampleRate) != activeDomain->fingerprint();
    }

    // ---- 長さのメソッドは2本 ----
    // 原音側の不変条件（clampFades）・分割・保存はこちら（従来の totalLengthSamples の定義）
    juce::int64 sourceTotalLengthSamples() const { return lengthSamples * (1 + juce::jmax (0, loopCount)); }

    // ---- 実効（見かけ）座標のヘルパー。activeDomain が無い間は無加工として振る舞う ----
    // 実効の移調量（リージョンの +2 バッジ用）。要求値 transposeSemitones を描くと、レンダー
    // 完了前にバッジだけ新しい値へ変わり「完了まで見た目も音も古いまま・同時に切り替える」
    // 契約が破れる。要求値を見せてよいのは吹き出しとメニュー併記（＝これから適用される値の
    // 編集画面）だけ
    int effectiveTransposeSemitones() const
    {
        return activeDomain != nullptr ? activeDomain->semitones : 0;
    }
    // 補正が鳴っているか（バッジ用。プレビュー中も含む＝見えている音と一致させる）
    bool effectivePitchCorrected() const
    {
        const auto* d = effectiveDomain();
        return d != nullptr && ! d->recipeDigest.isNull();
    }

    juce::int64 viewStartRendered() const
    {
        const auto* d = effectiveDomain();
        return d != nullptr ? d->mapBoundary (offsetSamples) : (juce::int64) 0;
    }
    juce::int64 viewEndRendered() const
    {
        const auto* d = effectiveDomain();
        return d != nullptr ? d->mapBoundary (offsetSamples + lengthSamples) : lengthSamples;
    }
    // 本体1反復の見かけ長。**絶対境界の差**で求める（独立に round(length × ratio) しない —
    // 隣接 view の境界が一致せずバッファ終端を越えて読む）
    juce::int64 renderedLengthSamples() const { return viewEndRendered() - viewStartRendered(); }

    // ループを含む総再生長の見かけ（描画・ヒットテスト・再生・終端計算はこちらを見る）
    juce::int64 renderedTotalLengthSamples() const
    {
        return renderedLengthSamples() * (1 + juce::jmax (0, loopCount));
    }

    // フェード長の原音 → 実効変換（chain 座標変換）。フェードは「ループの連なり全体」の両端に
    // 掛かるため、単純な × ratio では反復ごとの丸めが積み上がって合わない。
    // 「完了した本体数」と「本体内の端数」に分解して写す（fromEnd = フェードアウト側。終端から測る）
    juce::int64 renderedFadeLength (juce::int64 fadeSamples, bool fromEnd) const;
    juce::int64 renderedFadeIn() const { return renderedFadeLength (fadeInSamples, false); }
    juce::int64 renderedFadeOut() const { return renderedFadeLength (fadeOutSamples, true); }

    // 実効 → 原音の逆変換（フェードドラッグ用）。/ ratio ではなく chain 変換の逆。
    // 戻り値を renderedFadeLength に通すと入力（到達可能な実効値）へ厳密に戻る
    juce::int64 sourceFadeFromRendered (juce::int64 renderedFade, bool fromEnd) const;

    // 実効座標での「相手を押しのけない」クランプ（clampedFadeIn/Out の実効版。ドラッグ側が使う）
    juce::int64 clampedRenderedFadeIn (juce::int64 samples) const
    {
        const auto limit = juce::jmax ((juce::int64) 0,
                                       renderedTotalLengthSamples() - renderedFadeOut());
        return juce::jlimit ((juce::int64) 0, limit, samples);
    }
    juce::int64 clampedRenderedFadeOut (juce::int64 samples) const
    {
        const auto limit = juce::jmax ((juce::int64) 0,
                                       renderedTotalLengthSamples() - renderedFadeIn());
        return juce::jlimit ((juce::int64) 0, limit, samples);
    }

    // フェードの不変条件をモデル層で強制する（MidiRegion::clampNote と同じ立ち位置）。
    // 規則は「fadeIn を先に頭打ち → 残りで fadeOut を頭打ち」。読込・splitClip・
    // ループ解除・ループのドラッグから通す。
    //
    // 比率維持にしないのは、フェード長がユーザーが絶対時間（ms）で決めた値であり、
    // 全長が変わるたびに勝手に伸縮すると意図が壊れるため。単純な頭打ちなら決定的で、
    // どの経路からでも同じ関数を通せる。
    //
    // ⚠️ フェード自体のドラッグはここを直接通さないこと。この関数は fadeIn 優先なので
    // fadeIn を伸ばすと fadeOut が押し縮められる（＝「相手を押しのけない」規則に反する）。
    // ドラッグ側で先に `total - 相手のフェード` へ制限してから通す（通した時点で no-op になる）
    void clampFades()
    {
        const auto total = sourceTotalLengthSamples();
        fadeInSamples = juce::jlimit ((juce::int64) 0, total, fadeInSamples);
        fadeOutSamples = juce::jlimit ((juce::int64) 0, total - fadeInSamples, fadeOutSamples);
    }

    // ハンドルのドラッグで狙った長さ（samples）を、**相手のフェードを押しのけない**範囲へ収める。
    // 適用はしない（呼び出し側が代入する）ので、ドラッグ中の「値が実際に変わったか」判定にも使える。
    // ここが clampFades() と別に要るのは、clampFades が fadeIn 優先だから
    // （fadeIn を伸ばすと fadeOut が押し縮められ、ドラッグの規則に反する）
    juce::int64 clampedFadeIn (juce::int64 samples) const
    {
        const auto limit = juce::jmax ((juce::int64) 0, sourceTotalLengthSamples() - fadeOutSamples);
        return juce::jlimit ((juce::int64) 0, limit, samples);
    }

    juce::int64 clampedFadeOut (juce::int64 samples) const
    {
        const auto limit = juce::jmax ((juce::int64) 0, sourceTotalLengthSamples() - fadeInSamples);
        return juce::jlimit ((juce::int64) 0, limit, samples);
    }

};

// 区間の下端/上端（signed）。絶対値1本にしないのは、512サンプルが1周期に満たない低音
// （48kHzなら93.75Hz未満。808はここに入る）だと片側にしか振れない区間が生まれ、
// 絶対値を±で描くと実際にはない側へ波形が伸びてしまうため
struct PeakRange
{
    float lo = 0.0f;
    float hi = 0.0f;
};

// バッファ全長の描画用ピークキャッシュ（Clip::samplesPerPeak 単位・ステレオはL/Rを合成）。
// サンプル音源の波形表示用（クリップの波形は RenderedDomain::peakCache を部分参照して描く）
std::vector<PeakRange> buildFullPeakCache (const juce::AudioBuffer<float>& audio);

// クリップを splitSample（絶対サンプル位置）で左右に分ける。左右は同じソースWAVを共有参照する。
// 分割点が内側（開始 < 分割点 < 終端）にないときは false（境界ちょうどは分割しない）。
// フェードは外側だけを継承する（左: fadeIn / 右: fadeOut）。内側を0にしないと
// 分割点にフェードが移動してしまう。
// ループは解除して返す（左右どちらに繰り返しを引き継ぐか自明でないため）。解除はフェードの
// クランプより先に行うので、返ってきた左右は不変条件を満たしている。
// 伸縮クリップの分割は**再レンダーしない**: 分割点は「表示座標 → 原音境界へ逆算 →
// mapBoundary で canonical な表示境界」の順で1回だけ決め、左右が同じ RenderedDomain を
// 共有して view（offset/length）だけを分ける。canonical 境界が view の端と一致する
//（＝長さ0の view ができる）分割は false
bool splitClip (const Clip& clip, juce::int64 splitSample, Clip& left, Clip& right);

// MIDIノート。startPpq はリージョン相対。
// 不変条件: pitch 0..127 / velocity 1..127 / startPpq >= 0（リージョン内）/ lengthPpq >= 1。
// リージョン端を越えて伸びるノートは許容し、再生時にリージョン境界でノートオフ（マスク）する。
struct MidiNote
{
    juce::uint64 id = 0;
    int pitch = 60;
    juce::int64 startPpq = 0;
    juce::int64 lengthPpq = Ppq::ticksPerQuarter;
    int velocity = 100;
};

struct MidiRegion
{
    juce::uint64 id = 0;
    juce::int64 startPpq = 0;                  // 曲頭からの絶対位置（>= 0）
    juce::int64 lengthPpq = Ppq::ticksPerBar;  // >= 1
    bool muted = false;                        // リージョン単位のミュート（再生スナップショットから除外）
    // 本体の後ろに何回繰り返すか（0 = ループなし）。Clip::loopCount と同じ規則
    int loopCount = 0;
    std::vector<MidiNote> notes;

    // ループを含む総再生長（描画・ヒットテスト・終端計算はこちらを見る）
    juce::int64 totalLengthPpq() const { return lengthPpq * (1 + juce::jmax (0, loopCount)); }

    // 不変条件をモデル層で強制する。ノートの追加・移動・リサイズ後に必ず通すこと
    void clampNote (MidiNote& note) const
    {
        note.pitch = juce::jlimit (0, 127, note.pitch);
        note.velocity = juce::jlimit (1, 127, note.velocity);
        note.startPpq = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, lengthPpq - 1), note.startPpq);
        note.lengthPpq = juce::jmax ((juce::int64) 1, note.lengthPpq);
    }
};

// リージョンを splitPpq（絶対PPQ）で左右に分ける。分割点をまたぐノートは左にフル長のまま残し（Keep）、
// 分割点以降に始まるノートは相対シフトして右へ移す。右の id は 0 のまま返す（呼び出し側で採番する）。
// 分割点が内側にないときは false
bool splitMidiRegion (const MidiRegion& region, juce::int64 splitPpq, MidiRegion& left, MidiRegion& right);

// ---- 再生用の展開（ループ回数ぶん繰り返す）----
// 通常再生・⌘B（Project::buildSnapshot）と ⌘E（BounceRenderer::buildItemRender）の両方から呼ぶ。
// 展開規則を1箇所に集めるためのヘルパーなので、変えると両方の経路に効く。
//
// ミュートは判断しない。⌘Eは「明示選択が優先」でリージョンのミュートを無視する既存仕様なので、
// muted による除外は呼び出し側（buildSnapshot）の責任にしている。

// ノートを絶対PPQへフラット化して out へ足す。**各反復の末尾で境界マスクをかける**ので、
// 境界をまたぐロングノートは次の反復へ持ち越さない（最終ループ終端だけで切るのでは足りない）。
// fixedPitch >= 0 なら全ノートのピッチを置き換える（GMドラムの固定ピッチ規則。-1で置換なし）
void appendRegionNotes (const MidiRegion& region, int fixedPitch, std::vector<MidiNotePlayback>& out);

// クリップを ClipPlayback 列へ展開して out へ足す。開始位置だけが本体長ずつ進み、
// ソース参照範囲（offset/length）は全反復で共通。範囲外読みの最終防衛線もここで掛ける
void appendClipPlaybacks (const Clip& clip, std::vector<ClipPlayback>& out);

enum class TrackType { audio, midi };

// send用固定バスの表示名（固定文言は英語）。並びは TrackParams::sends / project.json の "buses" と対応
namespace SendBuses
{
    inline constexpr const char* names[numSendBuses] = { "Reverb A", "Reverb B", "Delay" };
    inline constexpr const char* shortNames[numSendBuses] = { "A", "B", "D" }; // sendノブの豆ラベル用
}

// セクションマーカー（ルーラー下のラベル帯）。区間方式: 終端は持たず、
// 次のマーカーの開始（最後は曲末）までが自分の区間。最初のマーカーより前は無ラベル。
// Project::markers は常に startBar 昇順・同一barなしを保つ（下のヘルパー経由で編集すること）
enum class SectionType { intro, verse, hook, bridge, outro, other };

struct SectionMarker
{
    // 曲頭からの拍数（0始まり・4/4固定で4拍=1小節）。上限なし。
    // 拍より細かくはしない（セクションは曲構造のラベルであり、2/4小節が挟まる曲の
    // 「半小節ずれ」に追従できれば十分。1/16等はただの誤操作リスクになる）
    int startBeats = 0;
    SectionType type = SectionType::other;

    int bar() const { return startBeats / 4 + 1; }  // 1始まりの小節番号（JSON・ログ表記用）
    int beat() const { return startBeats % 4; }     // 小節内の拍 0..3
};

namespace SectionMarkers
{
    inline constexpr SectionType allTypes[] = { SectionType::intro,  SectionType::verse,
                                                SectionType::hook,   SectionType::bridge,
                                                SectionType::outro,  SectionType::other };

    juce::String typeName (SectionType type);                       // "intro" 等（JSONと表示名の共通ベース）
    bool typeFromName (const juce::String& name, SectionType& out); // 未知名は false

    // 挿入（同一位置へは種別変更として働く）。昇順を保つ
    void set (std::vector<SectionMarker>& markers, int startBeats, SectionType type);
    void removeAt (std::vector<SectionMarker>& markers, int index);

    // index のマーカーを newStartBeats へ動かすときの移動先（隣のマーカーの手前・>=0 にクランプ。適用はしない）
    int clampStartBeats (const std::vector<SectionMarker>& markers, int index, int newStartBeats);

    // 自動採番済み表示名。同種別が2個以上あるときだけ出現順に verse1, verse2... と番号を付ける
    juce::String displayName (const std::vector<SectionMarker>& markers, int index);
}

// 採用ループ（ハーモニーのアンカー。v14）。上モノのループが進行とキーを決め、ベースガチャは
// これに従う（docs/design/reference-beat.md「音色の調達」）。
// - **Project 所有でクリップとは独立**: 由来クリップを全部消しても残り、消えるのは Loops タブ
//   での差し替えか明示解除のみ。同時に持てるのは1本だけ
// - roots は採用時に looproots.py が1回検出した値の永続化（ベースガチャは保存済みの
//   この値を読む。契約の真実の源は tools/library/looproots.py の docstring）
struct LoopAnchor
{
    juce::String libraryPath;      // ライブラリ相対パス（出自の表示・差し替え判定用）
    double bpm = 0.0;              // ループの表記BPM（逆コピーの源）
    ProjectKey key;                // ループのキー（逆コピーの源）
    int loopBars = 0;              // ループ小節数（4/4）
    int slotsPerBar = 0;           // 1小節あたりのハーモニースロット数（1/2/4/8/16）
    std::vector<int> roots;        // ルート列 0..11。長さ = loopBars × slotsPerBar
    std::vector<float> confidence; // スロット別の検出信頼度 0..1（roots と同長）
    bool degraded = false;         // 低信頼でトニック連打に退化した契約か

    // 契約（looproots.py）と同じ規則。読込時の検証と、書き込み側の事故防止の両方で使う
    bool isValid() const
    {
        const bool slotsOk = slotsPerBar == 1 || slotsPerBar == 2 || slotsPerBar == 4
                          || slotsPerBar == 8 || slotsPerBar == 16;
        // BPM は bass.py の実効 BPM と同じ 30..300（NaN/inf は範囲比較が false になり弾かれる）
        if (! slotsOk || loopBars < 1 || ! (bpm >= 30.0 && bpm <= 300.0))
            return false;
        // アンカー自身のキーも契約どおりに（root 12 等を save が書き込むと、次回 load で
        // アンカーごと消える「時限破損」になる。mode は enum 外の値のキャスト混入に対する防御）
        if (key.root < 0 || key.root > 11
            || (key.mode != KeyMode::major && key.mode != KeyMode::minor))
            return false;
        // ライブラリ**相対**パスの契約（index.json と同じ）。空・絶対パス・.. 上り は不正。
        // ".." は**パス要素として**のみ拒否する（"foo..bar.wav" のような正常なファイル名は通す）
        if (libraryPath.isEmpty() || libraryPath.startsWithChar ('/'))
            return false;
        for (const auto& segment : juce::StringArray::fromTokens (libraryPath, "/", ""))
            if (segment == "..")
                return false;
        // 長さ比較は size_t で行う（int の loopBars × slotsPerBar は巨大値で符号付きオーバーフロー）
        if (roots.size() != (size_t) loopBars * (size_t) slotsPerBar
            || confidence.size() != roots.size())
            return false;
        for (int r : roots)
            if (r < 0 || r > 11)
                return false;
        for (float c : confidence)
            if (! (c >= 0.0f && c <= 1.0f))
                return false;
        return true;
    }
};

namespace LoopAnchors
{
    // bass.py --roots に渡す契約 JSON（真実の源は tools/library/looproots.py の docstring）。
    // アンカーの永続化と同じ値から生成する — 採用時に1回検出した進行を、生成のたびに
    // 再検出せずここから配る
    juce::String rootsContractJson (const LoopAnchor& anchor);

    // looproots.py が出力した契約 JSON から anchor を組み立てる（**生の型まで strict に検証** —
    // 変換後の isValid だけだと roots: [9.5] のような型違反が 9 に化けて通ってしまう）。
    // bpm は契約に無いので呼び出し側が候補から設定し、libraryPath も候補のライブラリ相対パスで
    // 上書きしてから isValid を通すこと（契約の source は表示用のファイル名にすぎない）
    bool anchorFromContractJson (const juce::String& json, LoopAnchor& out);
}

// MIDIトラックの音源種別。gm = macOS内蔵GM音源（DLSMusicDevice）/ sample = 外部ワンショット
enum class InstrumentKind { gm, sample };

struct Track
{
    juce::uint64 id = 0; // プロジェクト内で一意。永続化される。0 = 未採番（読込時に採番）
    TrackType type = TrackType::audio;
    juce::String name;
    std::shared_ptr<TrackParams> params = std::make_shared<TrackParams>();

    // type == audio のとき
    std::vector<Clip> clips;

    // type == midi のとき
    InstrumentKind instrument = InstrumentKind::gm;
    // GM用（instrument == sample の間も保持し続ける。楽器プルダウンでGMへ戻したとき元の楽器が復元される）
    int gmProgram = 0;   // GMプログラム番号 0..127
    bool drums = false;  // true = ch10で発音（gmProgramは無視）
    int drumPitch = -1;  // >=0: 固定ピッチ打楽器（Kick等）。再生時ノートのピッチをこの値に置き換える（GM専用の規則）
    std::vector<MidiRegion> midiRegions;

    // サンプル音源用（instrument == sample のとき有効。GMへ戻しても保持する）。
    // undo対象にするため TrackParams（atomic共有）でなく Track のフィールドとして持ち、
    // オーディオスレッドへは SynthBank::sync() が SamplerEngine の atomic へミラーする（真実の源はここ）
    juce::String sampleFile;            // プロジェクトフォルダ相対（instr-NNN.wav）
    juce::String sampleName;            // 表示名（元ファイル名・拡張子なし）
    bool samplePitchFollow = false;     // false=固定（One Shot）/ true=音程追従（ノート長ゲート）
    // true = 新しい打点で前の音を切る（Logicの Quick Sampler「Polyphony: 1」相当・実機TR-808と同じ）。
    // 長い音（808等）を連打すると重なって濁る・出力がクリップするため、その回避用。既定OFF＝重ねる
    bool sampleMono = false;
    int sampleRootNote = 60;            // 追従時の基準ノート（既定 C3=60）
    float sampleGain = 1.0f;            // サンプル音量（線形倍率。値域は GainScale = ±12dB 相当の約0.251..3.981）
    juce::int64 sampleStartOffset = 0;  // 頭の無音カット位置（バッファ内サンプル）
    double sampleSourceRate = 0.0;      // ファイル自体のSR（再生比率 sourceRate/deviceRate の計算に必要）
    std::shared_ptr<juce::AudioBuffer<float>> sampleAudio; // メモリ常駐（Clip::audio と同じ寿命規則）
    std::vector<PeakRange> samplePeakCache;                // 波形描画用（Clip::samplesPerPeak 単位・全長）

    bool hasSample() const { return sampleAudio != nullptr && sampleSourceRate > 0.0; }
    bool usesSampler() const { return type == TrackType::midi && instrument == InstrumentKind::sample; }
};

class Project
{
public:
    // v2: MIDIトラック・ID追加 / v3: クリップのoffsetSamples・lengthSamples /
    // v4: pan・sends・固定バス3本・Master / v5: サイクル（ループ範囲）/
    // v6: ステレオクリップ（ch数はJSONに持たずWAV自体から判定）・クリップ表示名（取り込み用）/
    // v7: プロジェクトメモ / v8: サンプル音源（instrument・sample*）/
    // v9: リージョン/クリップのループ（loopCount。欠損＝ループなし）/
    // v10: オーディオリージョンのゲイン（clips[].gain。線形倍率・欠損＝1.0）/
    // v11: オーディオリージョンのフェード（clips[].fadeInSamples / fadeOutSamples。欠損＝0）/
    // v12: 曲末フェードアウト（fadeOut.start / fadeOut.end。16分音符単位・欠損＝未設定）/
    // v13: キー（key.root 0..11 / key.mode major|minor。欠損＝未設定。旧LaLaがキー付き
    //      projectを開いて保存するとキーを黙って消すため、追加時に版を上げている）/
    // v14: 採用ループのアンカー（loopAnchor。欠損＝未採用）とクリップの出自（clips[].loopSource。
    //      欠損＝ループ由来でない）。旧LaLaが開いて保存するとアンカーが黙って消えるため版を上げる
    // v15: トラックEQの4バンド（fx.eq.bands 配列。欠損＝既定値。旧LaLaが開いて保存すると
    //      バンド設定が黙って消えるため版を上げる）
    // v16: トラックCompのパラメータ（fx.comp.threshold/ratio/attack/release/makeup/detectorHpf）。
    //      v15以前の fx.comp.enabled はDSPが無かった頃の値で無意味なため、読込時に一律OFFへ
    //      リセットする（既定もOFF: コンプに保証された中立設定が無く、ピルが真のバイパスを担う）
    // v17: Master Limiter（master.limiter.gain/ceiling/release。欠損＝既定値 Gain 0 /
    //      Ceiling -1.0 / Release 60ms。常在なので enabled は持たない）
    // v18: トラックSat（fx.sat.enabled/drive/mix。欠損＝既定値 enabled=ON・Drive0＝中立。
    //      同planで後続のLo-fi（fx.lofi）も v18 に足す — 欠損は既定値で読むため、
    //      Sat のみ保存された途中版のプロジェクトも壊れない）
    // v19: バスFX（buses[0/1].reverb = size/damp/width/predelay/lowcut・buses[2].delay =
    //      time/feedback/tone/pingpong。欠損＝バスindex別の既定値 Reverb::defaultsForBus /
    //      Delay::defaults。保存は常に明示書き出し＝「既定値なら省略」はしない）
    // v20: オーディオクリップの移調・タイムストレッチ（clips[].transposeSemitones /
    //      stretchRatio / renderDomainOffset / renderDomainLength。欠損＝無加工・クリップ
    //      自身の範囲）。値のみ保存し、加工済みバッファは読込時に再生成する（WAV永続化しない）。
    //      旧LaLaが開いて保存すると値が黙って消えるため版を上げる
    // v21: ボーカルのピッチ補正（clips[].id = 安定ID・clips[].pitchCorrection = 編集状態。欠損＝補正なし。
    //      解析カーブはサイドカー clip-NNN.<curveDigest>.pitch に世代不変で保存し、project.json には
    //      curveDigest だけを持つ。サイドカーが無い／壊れている／WAVと食い違う場合は補正を無効化して
    //      警告＋dirty 化（「永続状態＝鳴っている音」）。id=0・重複は読込時に再採番）
    static constexpr int currentVersion = 21;

    juce::File directory;
    double bpm = 120.0;
    std::optional<ProjectKey> key; // 未設定 = nullopt（キーを決めていない曲を壊さない）
    std::optional<LoopAnchor> loopAnchor; // 採用ループ（v14）。未採用 = nullopt
    double sampleRate = 0.0; // 0 = 未確定（最初の録音時にデバイスレートで確定）
    juce::String memo;       // プロジェクトごとの自由記述メモ（v7。旧形式は空文字）
    std::vector<Track> tracks;
    std::vector<SectionMarker> markers; // 常にstartBar昇順・同一barなし（SectionMarkersヘルパーで編集する）

    // サイクル（ループ）範囲。16分音符単位・0始まり（タイムラインの最小グリッド1/16と一致。
    // BPM変更後も音楽的位置を維持する）。範囲が有効なのは start < end のときだけ。
    // 音量・ミュートと同じくundo対象外（Logicもサイクル操作はundoしない）
    int cycleStartSixteenths = 0;
    int cycleEndSixteenths = 0;
    bool cycleEnabled = false;
    bool hasCycleRange() const { return cycleStartSixteenths < cycleEndSixteenths; }

    // 曲末フェードアウト（マスターフェード）。プロジェクトに1本だけ。単位・流儀はサイクルと同じ
    // （16分音符・0始まり・有効なのは start < end のときだけ）。
    // サイクルと違い**undo対象**にする（範囲の繰り返しと違って鳴る音そのものが変わるため）。
    // 終端以降は無音（フェード後にゲインが戻ることはない＝「曲そのものの終端」）
    int fadeOutStartSixteenths = 0;
    int fadeOutEndSixteenths = 0;
    bool hasFadeOut() const { return fadeOutStartSixteenths < fadeOutEndSixteenths; }

    // 最後のアイテムの「見た目上の終端」を16分音符単位で返す（0 = 鳴るアイテムが1つもない）。
    // 曲末フェードの終端を自動で置くための値で、バウンス範囲の算出（MainComponent::startBounce）
    // とは規則が違うので共用しない:
    // - リージョンミュートは除外する（「この部分は使わない」という恒久的な意思表示）
    // - **トラックのミュート・ソロは考慮しない**（一時的なモニター状態であって曲の長さではない。
    //   ミュートして作業中に⌃Fを押したら終端が変わる、という驚きの方が大きい）
    // - One Shot のサンプル全長は含めない（バウンスが含めるのは「鳴り切るまで書き出す」ため）
    // - 端数は**切り上げる**（最近傍だと終端が手前へ吸着して最後の音を切る）
    int lastItemEndSixteenths (double sampleRate) const;

    // 「startSixteenths から曲末まで」でフェードを引くときの終端を解決する。
    // 0 = no-op（何もしない）。判断をUIから切り離してテスト可能にするためモデル側に置く。
    // 規則:
    // - 鳴るアイテムが1つも無ければ 0。**既存フェードが残っていても 0**（全アイテムを消した後に
    //   ⌃F で開始点だけ動かせてしまうのを防ぐ）
    // - 既にフェードがあるなら終端は動かさない（2本目は作らず開始点だけ移す）
    // - 開始点が終端以後なら 0（判定は呼び出し側でグリッドへ丸めた後の値で行うこと）
    int resolveSongFadeEnd (int startSixteenths, double sampleRate) const;

    // send用固定バス3本（gain=リターン量・mute・reverb/delayのFXパラメータ使用）とMaster（gain使用）。
    // gainの既定はトラック（0.8）と違いユニティ1.0、バスFXの既定はバスindex別
    // （busDefaultParams()が保証。v3以前の読込・新規作成でもこの初期値のまま）
    std::shared_ptr<TrackParams> busParams[numSendBuses] { busDefaultParams (0), busDefaultParams (1),
                                                          busDefaultParams (2) };
    std::shared_ptr<TrackParams> masterParams { unityParams() };

    juce::String name() const { return directory.getFileName(); }

    // トラック・リージョン・ノートのIDを採番する（メッセージスレッド専用）
    juce::uint64 allocateId() { return nextId++; }
    void ensureUniqueIds(); // 読込後・構造変更後（ClipDomains::reconcile）に未採番(0)・重複IDを振り直す
    // 読込時にモデルを書き換えた（補正の無効化など）＝開いた直後から保存が必要。MainComponent が dirty に写す
    bool modifiedOnLoad = false;

    // project.json 書き出し＋未参照 clip-*.wav のGC。失敗時は error にメッセージ。
    // keepReferencedWavs: undo履歴が参照するファイル名（GCから保護する。Phase 3で使用）
    // keepReferencedWavs: undo/redo 履歴・クリップボードが参照する WAV（redo 復元に備えて消さない）
    // keepSidecars: 同じく保持するピッチ解析サイドカーのファイル名（undo 履歴・クリップボード・開いている
    //   エディタのプレビューが乗っている世代）。project 自身が参照する世代は常に残す。
    //   それ以外の `clip-*.<digest>.pitch` と、WAV ごと消えたものを削除する
    bool save (juce::String& error, const juce::StringArray& keepReferencedWavs = {},
               const juce::StringArray& keepSidecars = {});

    static std::unique_ptr<Project> load (const juce::File& dir,
                                          juce::StringArray& warnings,
                                          juce::String& error);
    static std::unique_ptr<Project> createNew (const juce::File& dir, juce::String& error);

    juce::File nextClipFile() const;       // clip-NNN.wav の空き連番
    juce::File nextInstrumentFile() const; // instr-NNN.wav の空き連番（サンプル音源）
    // スナップショットに載せる PlaybackSnapshot::midiGeneration の進め方。
    // エンジンはこの世代が変わったときだけ「全ノートオフ＋跨ぎノート再発音」を行う。
    // - midiStructure（既定・安全側）: リアルタイムエンジンへ渡す。MIDI構成が変わったかもしれない → 進める
    // - audioValuesOnly: リアルタイムエンジンへ渡すが、ノート・リージョン・トラック構成・音源を
    //   一切変えていないと呼び出し側が保証できるとき（リージョンゲインの調整）→ 据え置く。
    //   誤って据え置くと「削除したノートのオフが失われて鳴りっぱなし」が起きる
    // - offlineRender: エンジンへ渡さない構築（⌘Bのバウンス用）→ 世代に触らない。
    //   ここで進めてしまうと、次の audioValuesOnly のpushが「世代が変わった」と誤認され、
    //   鳴っているMIDIが消音＋再発音される（バウンス後にゲインを動かすと起きる）
    enum class SnapshotChange { midiStructure, audioValuesOnly, offlineRender };

    std::unique_ptr<PlaybackSnapshot> buildSnapshot (SnapshotChange change = SnapshotChange::midiStructure);

    static juce::File projectsRoot(); // ~/Music/daw

    // 1chはモノ、2ch以上は先頭2chをL/Rとして読む（3ch以上の余剰chは捨てる）。
    // sourceSampleRate != nullptr ならファイル自体のSRも返す（サンプル音源は元SRのまま保存し、
    // 再生比率の計算に使うため必須。クリップ読込はプロジェクトSRへ変換済みなので不要）
    static std::shared_ptr<juce::AudioBuffer<float>> loadWav (const juce::File& file,
                                                             double* sourceSampleRate = nullptr);

    // loadWav ＋ targetRate へのリサンプル（WindowedSinc・レイテンシ補償込み。AudioImporter と
    // 同じ変換品質）。ループ採用のように「ライブラリの wav をプロジェクト SR のバッファとして
    // メモリに欲しい」用途向け — クリップの契約（audio は常にプロジェクト SR）を守るための入口。
    // SR が一致していれば変換せずそのまま返す。失敗は nullptr
    static std::shared_ptr<juce::AudioBuffer<float>> loadWavResampled (const juce::File& file,
                                                                       double targetRate);

private:
    juce::uint64 nextId = 1; // 永続化される採番カウンタ
    // MIDI構成の世代。単調増加させるだけで永続化しない（エンジン側の初期値0を「未受信」に残すため
    // 最初の buildSnapshot で1になる）
    juce::uint64 midiGeneration = 0;

    static std::shared_ptr<TrackParams> unityParams()
    {
        auto params = std::make_shared<TrackParams>();
        params->gain.store (1.0f); // TrackParamsの既定0.8fを引き継がない（バス/Masterはユニティ）
        return params;
    }

    // バス用のparams生成（v19）。ユニティに加えてバスFXの既定値をバスindex別テーブル
    // （Reverb::defaultsForBus）から入れる。新規作成＝この初期値のまま・旧版読込（キー欠損）＝
    // load側が同じテーブルで埋め直す、の両経路がここへ収束する
    static std::shared_ptr<TrackParams> busDefaultParams (int busIndex)
    {
        auto params = unityParams();
        Reverb::store (params->reverb, Reverb::defaultsForBus (busIndex));
        Delay::store (params->delay, Delay::defaults);
        return params;
    }


};
