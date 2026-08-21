#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "Project.h"
#include "RenderRecipe.h"
#include "RenderedDomain.h"

// オーディオクリップの移調・タイムストレッチの「要求値 ⇄ 実効値（activeDomain）」を
// つなぐモデル層のヘルパー。メッセージスレッド専用。
// 設計の真実の源: docs/plans/2026-08-18-1028-audio-transpose-stretch.md
//
// 規則の要点:
// - activeDomain の有効契約は「原音・SR が一致し、ドメインがクリップ範囲を包含」。
//   要求値との一致は**求めない**（一致まで求めると、値を変えた瞬間に無加工へ落ちて
//   「完了までは古い音」の仕様が壊れる）
// - reconcile は関数1本。構造が変わり得る全経路（分割・複製・ペースト・削除・移動・
//   undo/redo・読込・ガチャの仮配置/キャンセル）から呼ぶ
// - 「永続状態＝鳴っている音」を常に保つ: レンダー失敗時は要求値を実効値へ巻き戻す
namespace ClipDomains
{
// 無加工の RenderedDomain（audio は原音そのもの・ratio = 1.0）。レンダリング不要で即座に作れる。
// audio が nullptr のときは nullptr を返す
std::shared_ptr<const RenderedDomain> makeNeutralDomain (
    const std::shared_ptr<juce::AudioBuffer<float>>& audio,
    juce::int64 domainOffset, juce::int64 domainLength, double sampleRate);

// activeDomain の有効契約（上記コメント参照）
bool domainValidFor (const Clip& clip, double sampleRate);

// 全クリップを走査し、契約の崩れた activeDomain を「クリップ自身の範囲の無加工」へリセットする
// （要求ドメインも自範囲へ）。要求値・要求ドメインとの不一致では落とさない。
// 戻り値: 1つでも変更したか（再描画・snapshot 再pushの要否）
bool reconcile (Project& project, double sampleRate);

// レンダリング要求（RenderCache へ渡す）。request 自身が原音の shared_ptr を保持する —
// 完成前に最後の参照元クリップが消えても、ワーカーが読んでいる最中の原音が解放されない
struct Request
{
    RenderFingerprint fingerprint;
    std::shared_ptr<juce::AudioBuffer<float>> sourceAudio;
    // ピッチ補正付きの要求だけが持つ不変の入力（ワーカーはこれだけを読む）。補正なし = nullptr
    std::shared_ptr<const RenderRecipe> recipe;
};

// 要求集合を作る（待機キューの真実の源。「全クリップの要求指紋の集合」を毎回作り直す）。
// - 要求が無加工のクリップ: その場で中立ドメインへ差し替える（レンダリング不要）
// - lookup がキャッシュ済みの結果を返したクリップ: その場で装着する（undo/redo 後の引き直し）
// - それ以外: Request として返す（指紋で dedup 済み）
// attachedAny = その場で装着（音・長さの変化）があったか（呼び出し側が snapshot を再pushする）
std::vector<Request> collectRequests (
    Project& project, double sampleRate,
    const std::function<std::shared_ptr<const RenderedDomain> (const RenderFingerprint&)>& lookup,
    bool& attachedAny);

// 完了結果の装着。**現在の要求指紋が一致するクリップだけ**へ、新しいドメインと実効状態を
// 同時に切り替える（音・長さ・波形が一斉に変わる）。戻り値: 1つ以上装着したか
bool attachRenderResult (Project& project, double sampleRate,
                         const std::shared_ptr<const RenderedDomain>& domain);

// レンダー失敗の巻き戻し。**現在の要求指紋が失敗指紋と一致するクリップだけ**（古い実行中の
// 失敗で新しい要求を潰さない）の要求値・要求ドメインを実効ドメインへ戻す。実効が無ければ
// 0 / 1.0 / 自範囲の無加工へ。戻り値: 1つ以上巻き戻したか（= モデル変更なので dirty 化が要る）
bool rollbackFailedRequest (Project& project, double sampleRate, const RenderFingerprint& failed);

// 要求値の受付（UIの吹き出しから呼ぶ。判断ロジックを ui/ に書かないための置き場所）。
// 安全限界へクランプして受理し、値が実際に変わったらレンダードメインをクリップ自身の範囲へ
// リセットする。view 長が 0 になる ratio は受理しない。戻り値: 値が変わったか
bool applyStretchRequest (Clip& clip, int semitones, double ratio);

// 小節数入力 → stretchRatio の換算（吹き出しの「小節数で指定」）。
// 小節数はUIの入力手段であって保存値ではない
double ratioForBars (double bars, double barLengthSamples, juce::int64 sourceLengthSamples);
} // namespace ClipDomains
