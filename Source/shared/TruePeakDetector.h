#pragma once

#include <array>
#include <cmath>

// トゥルーピーク検出（4倍オーバーサンプリング。BS.1770-5 Annex 2 と同じ考え方）。
// plan: docs/plans/2026-08-16-1523-fx-batch2-meters-limiter.md
//
// サンプル点の間でDA変換後に実際に出る山（inter-sample peak）を、4倍レートへの
// 帯域制限補間（windowed-sinc・4相ポリフェーズ・12タップ/相）で推定する。
// 係数は初回に1度だけ計算する（規格の固定小数テーブルの丸め違いは±0.01dB未満で、
// 検証はEBUテスト信号との照合で行う）。
//
// RT安全: 固定サイズ・確保なし。オーディオスレッドから毎サンプル呼んでよい
class TruePeakDetector
{
public:
    static constexpr int phases = 4;        // 4倍オーバーサンプリング
    static constexpr int tapsPerPhase = 24; // 補間カーネル長（96タップ相当。12だとクリック等の
                                            // 広帯域過渡でカーネル打ち切りがISPを最大0.9dB
                                            // 取りこぼした実測がありffmpegと照合が割れる）

    // コンストラクタで係数テーブルの静的初期化を済ませる（＝メッセージスレッドで走る。
    // オーディオコールバック内で初回の sin/cos 計算と初期化ガードを踏ませない）
    TruePeakDetector() : table (coefficients()) {}

    void reset() noexcept
    {
        for (auto& channel : history)
            channel.fill (0.0f);
        writePos.fill (0);
    }

    // 1サンプル進めて、このサンプル周辺の4補間点＋サンプル自身の最大絶対値を返す
    float processSample (int ch, float x) noexcept
    {
        auto& hist = history[(size_t) ch];
        hist[(size_t) writeIndexFor (ch)] = x;
        advance (ch);

        float peak = std::fabs (x); // サンプル点そのものも候補（補間はリップルで僅かに下回りうる）
        for (int p = 0; p < phases; ++p)
        {
            float acc = 0.0f;
            for (int j = 0; j < tapsPerPhase; ++j)
                acc += table[(size_t) p][(size_t) j] * hist[(size_t) tapIndex (ch, j)];
            peak = acc < 0.0f ? (peak < -acc ? -acc : peak) : (peak < acc ? acc : peak);
        }
        return peak;
    }

    // 停止エッジのflush用: 無音を流して遅延カーネル内の残りピークを出し切る
    float flush (int ch) noexcept
    {
        float peak = 0.0f;
        for (int i = 0; i < tapsPerPhase; ++i)
        {
            const float v = processSample (ch, 0.0f);
            peak = peak < v ? v : peak;
        }
        return peak;
    }

private:
    // 各chが独立した書き込み位置を持つと2chで履歴が割れるので、ch別に履歴・位置を持つ
    int writeIndexFor (int ch) const noexcept { return writePos[(size_t) ch]; }
    void advance (int ch) noexcept { writePos[(size_t) ch] = (writePos[(size_t) ch] + 1) % tapsPerPhase; }
    int tapIndex (int ch, int j) const noexcept
    {
        // j=0 が最古（カーネルの端）になるよう、書き込み位置から巻き戻す
        return (writePos[(size_t) ch] + j) % tapsPerPhase;
    }

    // 4倍補間のwindowed-sinc（Blackman-Harris窓・48点・カットオフ=元レートのナイキスト）。
    // phase p の係数[j] は h[4j + p]（hは通過帯ゲイン1へ正規化済み＝各相の係数和が1）。
    // **中心は整数（total/2）にすること**: 慣例の (total-1)/2 = 半整数中心だと補間点が
    // 1/8サンプルずれた格子（0.125, 0.375, ...）になり、fs/4のサンプル間ピークを
    // 常に0.125サンプル外して約-0.2dB過小評価する（実測で確認済み）
    static const std::array<std::array<float, tapsPerPhase>, phases>& coefficients()
    {
        static const auto table = []
        {
            std::array<std::array<float, tapsPerPhase>, phases> result {};
            constexpr int total = phases * tapsPerPhase;
            constexpr int center = total / 2;
            std::array<double, total> h {};
            for (int i = 0; i < total; ++i)
            {
                const double t = ((double) i - center) / (double) phases;
                const double sinc = t == 0.0 ? 1.0 : std::sin (juce_pi * t) / (juce_pi * t);
                // 中心対称のBlackman-Harris（cos(0)=1で中心が最大。係数和=1.0）
                const double x = juce_pi * ((double) i - center) / (total * 0.5);
                const double w = 0.35875 + 0.48829 * std::cos (x)
                                 + 0.14128 * std::cos (2.0 * x) + 0.01168 * std::cos (3.0 * x);
                h[(size_t) i] = sinc * w;
            }
            // 各相の係数和を1へ正規化（DCで完全にユニティ＝定常信号のTPがサンプルピークと一致）
            for (int p = 0; p < phases; ++p)
            {
                double sum = 0.0;
                for (int j = 0; j < tapsPerPhase; ++j)
                    sum += h[(size_t) (j * phases + p)];
                for (int j = 0; j < tapsPerPhase; ++j)
                    result[(size_t) p][(size_t) j] = (float) (h[(size_t) (j * phases + p)] / sum);
            }
            return result;
        }();
        return table;
    }

    static constexpr double juce_pi = 3.141592653589793238;

    const std::array<std::array<float, tapsPerPhase>, phases>& table; // 構築時に初期化済みの静的テーブル
    std::array<std::array<float, tapsPerPhase>, 2> history {};
    std::array<int, 2> writePos {};
};
