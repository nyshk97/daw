#pragma once

#include <map>

#include "Project.h"

// メッセージスレッド専用。トラックID → MIDIトラックの音源インスタンスの対応を管理する
// （GM音源＝DLSMusicDevice / サンプル音源＝SamplerEngine）。
//
// インスタンスの生成・破棄はここ（メッセージスレッド）でのみ行い、オーディオスレッドへは
// PlaybackSnapshot の shared_ptr<SynthInstance> として渡す。楽器・サンプルファイル・
// サンプルレート変更はインスタンスの差し替えで実現する（旧インスタンスは参照する全スナップショットの
// 解放後に deleteRetired() 経由で破棄されるため、レンダリング中の破棄は起きない）。
// 一方でサンプルの音量・頭カット・音程モード・ルート音は「作り直さず atomic の更新だけ」で
// 反映する（作り直すと発音中の音が切れる）。
class SynthBank
{
public:
    // プロジェクトの現状（MIDIトラックの有無・楽器・サンプル・サンプルレート）に合わせて
    // インスタンスを生成・差し替え・破棄し、サンプル設定の atomic を更新する。変更があれば true を返す
    // （呼び出し側はスナップショットを再pushする）。sampleRate <= 0 の間は何もしない
    bool sync (const Project& project, double sampleRate, int deviceBlockSize);

    std::shared_ptr<SynthInstance> get (juce::uint64 trackId) const;

    // バウンス用: 共有インスタンスとは完全に独立した新規インスタンスを生成して返す
    // （sync()の管理対象外・呼び出し側が所有）。音源種別はトラックのモデルから決める。
    // GM音源はオフラインレンダリング前提で setNonRealtime(true) を設定する。
    // 失敗時は nullptr（理由は takeCreateErrors() に入る。サンプル欠損時も nullptr）
    std::shared_ptr<SynthInstance> createIndependent (const Track& track,
                                                      double sampleRate, int blockSize);

    // 生成失敗のユーザー向けメッセージを取り出す（取り出したら空になる）。
    // 失敗はキャッシュされ再試行されないため、1回の失敗につき1件だけ入る。
    // 呼び出し側（MainComponentのTimer）がダイアログ表示に使う
    juce::StringArray takeCreateErrors();

private:
    struct Entry
    {
        std::shared_ptr<SynthInstance> synth; // 生成失敗時はnullptrのまま保持（毎フレーム再試行しない）
        InstrumentKind instrument = InstrumentKind::gm;
        int gmProgram = 0;
        bool drums = false;
        juce::String sampleFile;   // サンプル差し替えの検出用
        bool pitchFollow = false;  // 前回の音程モード（変化したら requestStopAll() を立てる）
    };

    std::map<juce::uint64, Entry> entries;
    juce::StringArray pendingCreateErrors;

    std::shared_ptr<SynthInstance> createSynth (int gmProgram, bool drums,
                                                double sampleRate, int blockSize);
    // サンプル音源。sampleAudio が無い（ファイル欠損）ときは nullptr（トラックは無音）
    std::shared_ptr<SynthInstance> createSampler (const Track& track, double sampleRate, int blockSize);
    static void applySampleParams (SynthInstance& synth, const Track& track); // atomicのミラー更新
};
