#pragma once

#include <map>

#include "Project.h"

// メッセージスレッド専用。トラックID → GM音源（DLSMusicDevice）インスタンスの対応を管理する。
//
// インスタンスの生成・破棄はここ（メッセージスレッド）でのみ行い、オーディオスレッドへは
// PlaybackSnapshot の shared_ptr<SynthInstance> として渡す。楽器・サンプルレート変更は
// インスタンスの差し替えで実現する（旧インスタンスは参照する全スナップショットの解放後に
// deleteRetired() 経由で破棄されるため、レンダリング中の破棄は起きない）。
class SynthBank
{
public:
    // プロジェクトの現状（MIDIトラックの有無・楽器・サンプルレート）に合わせて
    // インスタンスを生成・差し替え・破棄する。変更があれば true を返す
    // （呼び出し側はスナップショットを再pushする）。sampleRate <= 0 の間は何もしない
    bool sync (const Project& project, double sampleRate, int deviceBlockSize);

    std::shared_ptr<SynthInstance> get (juce::uint64 trackId) const;

    // バウンス用: 共有インスタンスとは完全に独立した新規インスタンスを生成して返す
    // （sync()の管理対象外・呼び出し側が所有）。オフラインレンダリング前提で
    // setNonRealtime(true) を設定する。失敗時は nullptr（理由は takeCreateErrors() に入る）
    std::shared_ptr<SynthInstance> createIndependent (int gmProgram, bool drums,
                                                      double sampleRate, int blockSize);

    // 生成失敗のユーザー向けメッセージを取り出す（取り出したら空になる）。
    // 失敗はキャッシュされ再試行されないため、1回の失敗につき1件だけ入る。
    // 呼び出し側（MainComponentのTimer）がダイアログ表示に使う
    juce::StringArray takeCreateErrors();

private:
    struct Entry
    {
        std::shared_ptr<SynthInstance> synth; // 生成失敗時はnullptrのまま保持（毎フレーム再試行しない）
        int gmProgram = 0;
        bool drums = false;
    };

    std::map<juce::uint64, Entry> entries;
    juce::StringArray pendingCreateErrors;

    std::shared_ptr<SynthInstance> createSynth (int gmProgram, bool drums,
                                                double sampleRate, int blockSize);
};
