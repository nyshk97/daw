#pragma once

#include <optional>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../audio/BiquadFilter.h"
#include "MasterMeterStats.h"

// ラウドネス計測の計算部品（ITU-R BS.1770-5 / EBU R128 準拠）。
// plan: docs/plans/2026-08-16-1523-fx-batch2-meters-limiter.md
//
// - K-weighting: 「耳の感度」を近似する前処理フィルタ（高域シェルフ＋低域カット）。
//   これを通した二乗平均がラウドネス＝「大きく聴こえる度合い」の物理量になる
// - short-term: 直近3秒の窓。ミックス中に「いまどのくらい」を読む値
// - integrated: 再生した範囲全体の平均。無音や静かな区間をゲート（除外）してから平均する
//   （絶対ゲート-70 LUFS → 相対ゲート-10 LU の2段。curtain区間で平均が薄まるのを防ぐ）
//
// **K-weightingフィルタの適用者は1箇所だけ**: RT経路は MasterMeterSource（オーディオ
// スレッド）が適用し、ここは係数・エネルギー→LUFS変換・ゲーティング集計の部品を提供する。
// ファイル一括計測（measureFile。バウンス完了時用）だけは自前でフィルタも回す
namespace Loudness
{
inline constexpr double subBlockSeconds = 0.1;   // リングの刻み（100ms）
inline constexpr int subBlocksPerGatingBlock = 4; // 400msゲーティングブロック = 100ms×4（75%重複で1刻みずつ進む）
inline constexpr int shortTermSubBlocks = 30;     // short-term 3秒 = 100ms×30
inline constexpr int correlationSubBlocks = 3;    // 相関の表示窓 約300ms
inline constexpr double absoluteGateLufs = -70.0;
inline constexpr double relativeGateLu = -10.0;

// K-weighting 2段の係数（fs依存。libebur128と同じ解析式でBS.1770の48kHzテーブルを一般SRへ拡張）
void kWeightingCoefficients (double sampleRate, Biquad& shelf, Biquad& highpass);

// エネルギー（K-weighting後のch合算 mean square）→ LUFS
inline double energyToLufs (double meanSquare)
{
    return -0.691 + 10.0 * std::log10 (meanSquare > 1.0e-12 ? meanSquare : 1.0e-12);
}

// integrated（2段ゲーティング）。subPowers = 100msサブブロックごとのch合算 mean square
// （完全な公称長のサブブロックのみ渡すこと。末尾の不完全な400ms窓は規格どおり捨てる）。
// 有効なゲーティングブロックが1つも無ければ nullopt
std::optional<double> computeIntegratedLufs (const std::vector<float>& subPowers);

// リング → 表示値の集約（メッセージスレッド専用。読み手はMainComponentの常時Timerの一箇所だけ）。
// 世代切替（再生開始）で内部を仕切り直し、リングあふれを検知したら計測無効にする
class MasterMeterAggregator
{
public:
    // リングの新着を取り込み、表示値を更新する（30Hz Timerから呼ぶ。popで消費する）
    void consume (MasterMeterRing& ring);

    const MasterMeterFeed& feed() const { return current; }

    // プロジェクト切替等での全消去
    void reset();

private:
    void resetSession (juce::uint32 newGeneration);
    void rebuildFeed();

    juce::uint32 generation = 0;
    bool haveGeneration = false;
    bool overflowed = false; // 現行世代が汚染されているか（consumeで毎回導出）
    // あふれで汚染された世代（同一セッション内では無効のまま・別世代が来たら回復）
    juce::uint32 taintedGeneration = 0;
    bool haveTaint = false;

    // integrated用: 完全サブブロックのch合算 mean square（再生開始からの全履歴）
    std::vector<float> subPowers;

    // short-term / 相関用: 直近サブブロックの生統計（部分ブロック含む）
    struct Recent
    {
        double kwSumSq = 0.0; // L+R合算の二乗和
        double rawLL = 0.0, rawRR = 0.0, rawLR = 0.0;
        int numSamples = 0;
        bool complete = false; // 公称100msぶん揃った完全ブロックか（short-termの3秒窓判定用）
    };
    std::vector<Recent> recent; // 末尾が最新（shortTermSubBlocks+1 に切り詰め）

    float maxTruePeak = 0.0f;
    MasterMeterFeed current;
};

// バウンス完了時のファイル一括計測（BounceRendererワーカーから呼ぶ。メッセージスレッド禁止）。
// integrated LUFS と最大トゥルーピーク(dBTP)を返す。読めなければ false
bool measureFile (const juce::File& file, double& integratedLufsOut, double& truePeakDbOut);
} // namespace Loudness
