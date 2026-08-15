#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

// 区間書き出し: 聴こえている構成（M/S込みミックス）を1ファイルへ。
// ステレオ24bit・元音源のsample rate。ピーク検査が要るため2パス
// （1パス目で区間をミックスしながらピーク測定→ゲイン確定→2パス目で24bit書き込み）。
// 一時ファイルへ書いて成功時にatomic rename（途中失敗で壊れた成果物を残さない）
namespace RegionExport
{
struct Source
{
    juce::File file;
    float gain = 1.0f; // 現状は鳴っているステム=1（M/S結果）。将来の減衰にも使える
};

struct Result
{
    bool ok = false;
    float appliedGain = 1.0f; // 1.0未満ならクリップ回避で全体を下げた（トースト通知の材料）
    juce::String error;
};

// 同期実行のコア（salva_testsの対象）。呼び出し側がバックグラウンドスレッドで包む。
// shouldCancel はチャンクごとに確認され、trueで一時ファイルを消して中断する
// （20分×複数ステムはアプリ終了時のstopThread猶予を超え得るため、強制終了でなく
// 正常なunwindで抜ける）
Result renderMix (const std::vector<Source>& sources, juce::int64 startSample, juce::int64 endSample,
                  double sampleRate, const juce::File& outFile,
                  const std::function<bool()>& shouldCancel = {});
} // namespace RegionExport

// バックグラウンドworker（UIを止めない）。UIのTimerがstatus()をポーリングする
class ExportWorker : private juce::Thread
{
public:
    ExportWorker() : juce::Thread ("salva export") {}
    ~ExportWorker() override { stopThread (10000); }

    bool startExport (std::vector<RegionExport::Source> sources, juce::int64 startSample,
                      juce::int64 endSample, double sampleRate, const juce::File& outFile);

    bool isBusy() const { return busy.load(); }
    // 完了を1回だけ消費。trueのとき outResult / outFile が有効
    bool consumeResult (RegionExport::Result& outResult, juce::File& outFile);

private:
    void run() override;

    std::vector<RegionExport::Source> sources;
    juce::int64 startSample = 0, endSample = 0;
    double sampleRate = 0.0;
    juce::File outFile;
    RegionExport::Result result;
    std::atomic<bool> busy { false };
    std::atomic<bool> done { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportWorker)
};
