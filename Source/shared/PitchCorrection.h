#pragma once

#include <optional>
#include <vector>
#include <juce_core/juce_core.h>

#include "PitchCurve.h"
#include "PitchNotes.h"
#include "ProjectKey.h"
#include "TimeMap.h"

// ボーカルのピッチ補正 — クリップが持つ編集状態と、そこから導く「目標カーブ」「時間写像」。
// 設計の真実の源: docs/plans/2026-08-20-2244-vocal-pitch-correction.md（Phase 2）
//
// 語彙:
// - 原音サンプル座標: ソース WAV 内の絶対位置（TimeNode::sourceSample はこちら。カーブのフレーム＝ホップ単位
//   だと分割境界を表せないのでサンプル）
// - 初期配置: 現在の stretchRatio による一様写像 round((src − domainOffset) × ratio)。保存しない
// - timingDeltaSamples: 初期配置からの render 座標のずれ。保存値は書き換えず、timeMap 構築時に決定的に
//   区間別下限へ射影する（保存済み Δ が新しい ratio の下限を破っても同じ入力なら同じ timeMap）
// - タイミングのずれは**ノードが持ち、ノートはノードを参照する**（隙間 0 の e_A == s_B は同じノード index
//   を指す＝共有ノードの Δ が1つしか無く、再読込で曖昧にならない）
// - domain 端点は timeNodes に含めない（Δ は常に 0 で固定）。`BoundaryRef` の型で区別する

struct TimeNode
{
    juce::int64 sourceSample = 0;       // 開区間 (domainOffset, domainOffset + domainLength) に限る
    juce::int64 timingDeltaSamples = 0; // 初期配置からのずれ（render 座標）

    bool operator== (const TimeNode& o) const
    {
        return sourceSample == o.sourceSample && timingDeltaSamples == o.timingDeltaSamples;
    }
};

struct BoundaryRef
{
    enum class Kind { domainStart, domainEnd, node };
    Kind kind = Kind::node;
    int index = 0; // kind == node のときだけ有効

    static BoundaryRef domainStart() { return { Kind::domainStart, 0 }; }
    static BoundaryRef domainEnd() { return { Kind::domainEnd, 0 }; }
    static BoundaryRef node (int i) { return { Kind::node, i }; }
    bool isNode() const { return kind == Kind::node; }
    bool operator== (const BoundaryRef& o) const { return kind == o.kind && (kind != Kind::node || index == o.index); }
    bool operator!= (const BoundaryRef& o) const { return ! (*this == o); }
};

struct PitchNote
{
    BoundaryRef start, end;
    int targetMidi = 60;  // 半音単位（セントは持たない）
    bool bypass = false;  // しゃくり・話し声区間を素に戻す

    bool operator== (const PitchNote& o) const
    {
        return start == o.start && end == o.end && targetMidi == o.targetMidi && bypass == o.bypass;
    }
};

// スナップに使うスケールの選び方。スナップは「固定」方式: スケールは targetMidi を決める瞬間にだけ使い、
// 保存するのは確定した targetMidi。後からプロジェクトキーを変えても音は変わらない
enum class PitchScaleMode { projectKey, chromatic, custom };

struct PitchCorrection
{
    ContentDigest curveDigest;                 // 必須。どの世代のサイドカーを使うか
    PitchScaleMode scaleMode = PitchScaleMode::projectKey;
    ProjectKey customKey;                      // scaleMode == custom のとき
    float strength = 1.0f;                     // 0..1。ノート中心を目標へ寄せる割合
    float speedMs = 120.0f;                    // ノート内の動きを目標へ引き寄せる時定数。0 = ケロケロ
    std::vector<TimeNode> timeNodes;           // sourceSample 昇順・重複なし
    std::vector<PitchNote> notes;              // 解決後の座標で昇順・非重複

    static constexpr float keroSpeedMs = 0.0f;
    static constexpr float defaultSpeedMs = 120.0f;

    juce::var toJson() const;
    // 形式外・値域外は nullopt（呼び出し側は「サイドカー欠損」と同じ扱い＝補正無効化）。
    // 構造の検証（index・順序・重なり）は validate() で domain を渡して行う
    static std::optional<PitchCorrection> fromJson (const juce::var& v);

    ContentDigest digest() const; // 内容ハッシュ（レンダー指紋に入れる。音に関係しない scaleMode/customKey は含めない）
    // 鳴りが原音と同じか（強さ 0 かつタイミングのずれ無し）。開いた直後の「何も動かさない」状態の判定に使う
    bool isAudiblyNeutral() const;

    // BoundaryRef → 原音サンプル座標
    juce::int64 resolve (const BoundaryRef& ref, juce::int64 domainOffset, juce::int64 domainLength) const;
    // 読込検証: timeNodes は昇順・重複なし・開区間内、Δ は有限、各 Note は resolve(start) < resolve(end)、
    // index は範囲内、ノート同士は解決後の座標で重ならない（昇順）
    bool validate (juce::int64 domainOffset, juce::int64 domainLength, juce::String* why = nullptr) const;

    bool operator== (const PitchCorrection& o) const
    {
        return curveDigest == o.curveDigest && scaleMode == o.scaleMode && customKey == o.customKey
            && juce::exactlyEqual (strength, o.strength) && juce::exactlyEqual (speedMs, o.speedMs)
            && timeNodes == o.timeNodes && notes == o.notes;
    }
};

namespace PitchCorrections
{
// 各区間の出力長の下限。ピッチ周期数個分（これ未満は PSOLA でも位相ボコーダーでも WORLD でも破綻する）。
// 規則は「区間の下限 = min(10ms, 初期配置での出力長)」（ratio 0.25 で 5ms になった隙間も初期配置は実現可能）
inline constexpr double minSegmentMs = 10.0;

// timeMap の構築（純関数）。始点 (domainOffset, 0)・timeNodes・終点 (domainOffset+length, round(length×ratio))。
// 候補位置 = 初期位置 + Δ を、左から「直前ノードの確定位置 + 区間別下限」へ、右から終点固定の制約へ射影
TimeMap buildTimeMap (const PitchCorrection& pc, juce::int64 domainOffset, juce::int64 domainLength,
                      double stretchRatio, double sampleRate);

// 目標カーブ: カーブのフレーム [firstFrame, firstFrame + shift.size()) ごとの移動量[半音]
// （有声マスク × 補正 + transpose。無声フレーム・ノート外・バイパスは transpose のみ）
struct TargetCurve
{
    int firstFrame = 0;
    std::vector<float> shiftSemitones;
};
TargetCurve targetCurve (const PitchCorrection& pc, const PitchCurve& curve,
                         juce::int64 domainOffset, juce::int64 domainLength, int transposeSemitones);

// スケール音へのスナップ（nullopt = クロマチック）。同距離なら上を取る
int snapToScale (double midi, const std::optional<ProjectKey>& scale);
// 補正の設定とプロジェクトキーから、実際にスナップへ使うスケールを解く（chromatic → nullopt）
std::optional<ProjectKey> effectiveScale (const PitchCorrection& pc, const std::optional<ProjectKey>& projectKey);

// ノートの原音範囲 [start, end) に含まれる有声フレームの中央値 MIDI（無ければ nullopt）
std::optional<double> noteMedianMidi (const PitchCurve& curve, juce::int64 startSample, juce::int64 endSample);

// 自動スナップ: 検出ノート（WAV 先頭基準の絶対フレーム）→ 現在の domain と交差させて両端クランプ・
// 空は除外・隣接境界は共有ノード・domain 端に一致する境界は domainStart/End。Δ は 0
PitchCorrection autoSnap (const PitchCurve& curve, const std::vector<DetectedPitchNote>& detected,
                          juce::int64 domainOffset, juce::int64 domainLength,
                          const std::optional<ProjectKey>& scale, PitchScaleMode mode,
                          const ProjectKey& customKey);

// 「キーに合わせ直す」: 各ノートの targetMidi を中央値から再スナップ（curveDigest・構造は維持）
void resnap (PitchCorrection& pc, const PitchCurve& curve, juce::int64 domainOffset, juce::int64 domainLength,
             const std::optional<ProjectKey>& scale);

// ---- 編集操作（UI から呼ぶだけ。規則はここに置きテストで固定）----

// 横ドラッグ: ノート i の開始/終了ノードを同量 Δ（render 座標）動かす。両隣の区間が吸収。
// 端点（domainStart/End）を参照するノートは動かせない。区間別下限を下回る Δ はクランプ。
// 戻り値: 実際に適用した Δ（0 = 動かなかった）
juce::int64 moveNote (PitchCorrection& pc, int noteIndex, juce::int64 deltaRender,
                      juce::int64 domainOffset, juce::int64 domainLength, double stretchRatio, double sampleRate);

// 分割: ノート i を原音サンプル sourceSample で2つに。新しい境界ノードを現在の timeMap 上へ線形補間で挿入
// （分割前後で timeMap が一致＝音が変わらない）。左右は targetMidi・bypass を継承。戻り値: 成功したか
bool splitNote (PitchCorrection& pc, int noteIndex, juce::int64 sourceSample,
                juce::int64 domainOffset, juce::int64 domainLength, double stretchRatio, double sampleRate);

// 結合: 隣接する2ノート（notes[i] と notes[i+1]）を1つに。左の開始ノードと右の終了ノードの間の中間ノードを
// すべて削除（隙間0なら共有ノード1個、隙間ありなら2個。隙間は結合ノートに取り込まれる）。
// targetMidi は長い方（原音長）、bypass は両方 true のときだけ true。戻り値: 成功したか
bool mergeNotes (PitchCorrection& pc, int leftIndex, juce::int64 domainOffset, juce::int64 domainLength);

// 分割子の detach: 親 domain で表現された状態を子の自範囲へ写す（範囲外のノートを捨て、境界をまたぐノートを
// 切り、範囲外のノードを捨てる）。親子の digest がここで分かれる
void detachToDomain (PitchCorrection& pc, juce::int64 oldOffset, juce::int64 oldLength,
                     juce::int64 newOffset, juce::int64 newLength);
} // namespace PitchCorrections
