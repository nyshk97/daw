#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

// SynthInstance が保持するサンプラー本体。shared/ から audio/ を include するのは
// 「スナップショットが運ぶオーディオスレッド側オブジェクト」だからで、AUの
// AudioPluginInstance と同じ立ち位置（unique_ptr のため完全型が必要）
#include "../audio/SamplerEngine.h"
#include "../audio/TrackComp.h"
#include "../audio/TrackEq.h"
#include "CompParams.h"
#include "EqParams.h"

// send用固定バスの本数（Reverb A / Reverb B / Delay）。本数・並びは固定で、
// UI・保存形式・エンジンすべてこの順序を前提にする（バスの作成・削除UIは作らない）
inline constexpr int numSendBuses = 3;

// トラックの音量・パン・send・ミュート・ソロ。Trackモデル（メッセージスレッド）と
// PlaybackSnapshot（オーディオスレッドが参照）が shared_ptr で共有する。
// 値の変更はスナップショット再構築なしに atomic 経由で即反映される。
// バス・Masterのパラメータにもこの型を流用する（pan/sends/solo等の不要フィールドは未使用のまま。
// 注意: バス・Masterの gain 既定はユニティ1.0で、ここでの既定0.8とは異なる。
// Project 側が全生成経路で明示的に1.0を入れる）
struct TrackParams
{
    std::atomic<float> gain { 0.8f };
    std::atomic<float> pan { 0.0f };                            // -1（左）..+1（右）
    std::atomic<float> sends[numSendBuses] { { 0.0f }, { 0.0f }, { 0.0f } }; // 各バスへのsend量 0..1（post-fader）
    std::atomic<bool> mute { false };
    std::atomic<bool> solo { false };

    // 固定ストリップFXのON/OFF。EQは既定ON（バンドが中立なら音を変えない）。
    // Compは既定OFF（v16でDSP結線。コンプに「保証された中立設定」が無いため、
    // ピルが真のバイパスを担う）
    std::atomic<bool> eqEnabled { true };
    std::atomic<bool> compEnabled { false };

    // トラックEQの4バンド（v15。並び・不変条件は shared/EqParams.h）。書き込みは必ず
    // Eq::normalized() を通した値を Eq::store() すること。バス・Masterでは未使用のまま
    Eq::BandParams eqBands[Eq::numBands];

    // トラックCompのパラメータ（v16。shared/CompParams.h）。書き込みは必ず
    // Comp::normalized() を通した値を Comp::store() すること。バス・Masterでは未使用のまま
    Comp::Params comp;

    // ---- 以下はオーディオスレッド専用（peakL/peakR・SynthInstance::activeNotes と同じ流儀）----
    // EQ/CompのDSP実行状態（biquad履歴・平滑・GRエンベロープ）。スナップショット再構築を
    // 跨いで持続させるためここに住む。
    // **リアルタイム再生専用**: バウンスは BounceRenderer::TrackRender の独立インスタンスを使う
    TrackEq rtEq;
    TrackComp rtComp;

    TrackParams() { Eq::applyDefaults (eqBands); }

    // トラック出力のL/Rピークレベル（UIメーター用）。オーディオスレッドはCASでmax更新し、
    // UI（30Hz Timer）は exchange(0) で読み取りリセットする。1UIフレームに複数ブロックが
    // 走っても最大値が渡る（storeだと最後のブロックしか見えず瞬発音を取りこぼす）
    std::atomic<float> peakL { 0.0f };
    std::atomic<float> peakR { 0.0f };

    // Compの表示用レベル（peakL/peakRと同じCAS max→UI exchange(0)の流儀。
    // 消費はMainComponentのメータータイマーに一元化し、各表示へ配布する）。
    // compGrDb は正の減衰量dB（描画時だけ下向きにする）、compDetectorPeak は
    // 検波入力ピークの**線形振幅**（dBを入れると負値になり exchange(0) のmax更新と両立しない）
    std::atomic<float> compGrDb { 0.0f };
    std::atomic<float> compDetectorPeak { 0.0f };

    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<bool>::is_always_lock_free);
};

// エンジン用の再生スナップショット。メッセージスレッドが構築し、オーディオスレッドは読むだけ。
// shared_ptr の参照カウント操作（コピー・破棄）はメッセージスレッドでのみ起きる。
struct ClipPlayback
{
    std::shared_ptr<const juce::AudioBuffer<float>> audio; // 1ch=モノ / 2ch=ステレオ。メモリ常駐。生存保証のため所有を共有
    juce::int64 startSample = 0;
    juce::int64 offsetSamples = 0;  // ソースバッファ内の読み出し開始位置（buildSnapshotでバッファ範囲内を保証済み）
    juce::int64 lengthSamples = 0;  // 再生長
    float gain = 1.0f;              // リージョン単位のゲイン（線形倍率。Clip::gain のコピー。値域は GainScale）

    // フェードの境目（すべて絶対サンプル位置。ループ展開した全反復に同じ値が入る）。
    // 相対長で持たないのは、フェードが「連なり全体の両端」に掛かるため複数の ClipPlayback を
    // またぐことがあるから（例: 1小節×4ループに2小節のフェードアウト）。
    // 空区間（end <= start）＝そのフェードなし。エンベロープの評価と区間分割は shared/ClipFade.h
    juce::int64 fadeInStart = 0,  fadeInEnd = 0;
    juce::int64 fadeOutStart = 0, fadeOutEnd = 0;
};

// MIDIトラックの音源1インスタンス＋オーディオスレッド専用の発音状態。
// 中身は GM音源（DLSMusicDevice = plugin）か サンプル音源（sampler）のどちらか一方（排他）。
// SynthBank（メッセージスレッド）が生成・所有し、スナップショットが shared_ptr で共有する。
// 破棄は「参照する全スナップショットの解放後」＝ deleteRetired() 経由でメッセージスレッドのみで起きるため、
// オーディオスレッドがレンダリング中のAUが消えることはない（ClipPlayback::audio と同じ寿命保証）。
struct SynthInstance
{
    std::unique_ptr<juce::AudioPluginInstance> plugin;  // GM音源（sampler と排他）
    std::unique_ptr<SamplerEngine> sampler;             // サンプル音源（plugin と排他）
    int midiChannel = 1;            // ドラムキットは10
    double preparedSampleRate = 0.0; // このレートで prepareToPlay 済み。デバイスと不一致ならレンダリングをスキップ
    int preparedBlockSize = 0;
    int totalOutputChannels = 2;    // processBlock に渡すべきチャンネル数（DLSは2バス計4ch）。ミックスはch0/1のみ

    // サンプル音源の固定モード（One Shot）か。PlaybackEngine が resound の可否判定に読む
    // （One Shot は noteOff で消えないので、再発音すると二重に鳴る）。更新は SynthBank::sync()
    std::atomic<bool> oneShot { false };

    bool hasRenderer() const { return plugin != nullptr || sampler != nullptr; }

    // ---- 以下はオーディオスレッド専用（メッセージスレッドは触らない）----
    // 「実際にノートオンを送った論理ノート」の追跡。ノートオフはこの配列に載っているものだけ送る。
    // 上限超過で捨てたノートの終端が来ても誤ってオフを送らない（同ピッチの別ノートを止めない）ためと、
    // 消音時のオフ送出数を maxActiveNotes に有界化するための仕組み。
    // endPpq はフラット化済みノートの絶対終了PPQ（識別キー）。プレビュー発音は endPpq = -1
    static constexpr int maxActiveNotes = 256;
    struct ActiveNote
    {
        juce::int64 endPpq = 0;
        int pitch = 0;
    };
    ActiveNote activeNotes[maxActiveNotes];
    int numActiveNotes = 0;

    bool addActive (juce::int64 endPpq, int pitch)
    {
        if (numActiveNotes >= maxActiveNotes)
            return false; // 満杯ならノートオンごと諦める（呼び出し側はオンを送らない）
        activeNotes[numActiveNotes++] = { endPpq, pitch };
        return true;
    }

    int findActive (juce::int64 endPpq, int pitch) const
    {
        for (int i = 0; i < numActiveNotes; ++i)
            if (activeNotes[i].pitch == pitch && activeNotes[i].endPpq == endPpq)
                return i;
        return -1;
    }

    void removeActive (int index)
    {
        activeNotes[index] = activeNotes[--numActiveNotes]; // swap-with-last（順序は不要）
    }
};

// 再生用にフラット化したノート（リージョン相対→絶対PPQ変換・リージョン境界マスク適用済み）。startPpq昇順
struct MidiNotePlayback
{
    juce::int64 startPpq = 0;
    juce::int64 endPpq = 0; // マスク適用後の終了位置（絶対PPQ）
    int pitch = 60;
    int velocity = 100;
};

struct TrackPlayback
{
    juce::uint64 trackId = 0;                   // プレビューコマンドの宛先解決に使う
    std::shared_ptr<TrackParams> params;
    std::vector<ClipPlayback> clips;            // オーディオトラックのみ
    bool hasStereoClip = false;                 // true=エンジンはステレオ経路（モノのみトラックは現行経路を素通し）
    std::shared_ptr<SynthInstance> synth;       // MIDIトラックのみ（未生成ならnullptr）
    std::vector<MidiNotePlayback> notes;        // MIDIトラックのみ
};

struct PlaybackSnapshot
{
    std::vector<TrackPlayback> tracks;

    // send用固定バス（gain=リターン量・mute・peakL/peakRを使用）とMaster（gain・peakL/peakRを使用）。
    // Project が所有する実体を shared_ptr で共有する（トラックの params と同じ寿命規則）
    std::shared_ptr<TrackParams> busParams[numSendBuses];
    std::shared_ptr<TrackParams> masterParams;

    // 曲末フェードアウト（16分音符単位・無効は 0/0）。**サンプルに換算して入れないこと**:
    // BPM変更（MainComponent::applyBpmText）もSR変更もスナップショットを再pushしないため、
    // 焼き込むと表示だけが新しい小節位置へ動いて音が古い位置に取り残される。
    // 換算はエンジン側が毎ブロック SongFade::sixteenthsToSamples で行う
    int fadeOutStartSixteenths = 0;
    int fadeOutEndSixteenths = 0;

    // MIDIの構成世代。エンジンはこの値が変わったときだけ「全ノートオフ＋跨ぎノート再発音」を行う
    // （再生中の編集でノートオフが失われて鳴りっぱなしになるのを防ぐ安全機構）。
    // オーディオ側だけが変わった差し替え（リージョンゲインのドラッグ等）では前回と同じ値を入れることで、
    // 鳴っているMIDIを乱さずに音を更新できる。埋めるのは MainComponent::pushSnapshot 系のみ
    juce::uint64 midiGeneration = 0;
};

// GOTCHAS.md パターン3: UIイベント→構築→atomicポインタ差し替え。解放は必ずメッセージスレッド側。
//
//   メッセージスレッド: push() で新スナップショットを渡し、Timer で deleteRetired() を回す
//   オーディオスレッド: 毎ブロック acquire() で最新を取得（delete は絶対にしない）
//
// retired スロットが空いているときだけ current を差し替えるので、オーディオスレッドが
// 手放したスナップショットは必ず retired 経由でメッセージスレッドに戻って解放される。
class SnapshotExchange
{
public:
    SnapshotExchange() = default;

    ~SnapshotExchange()
    {
        // オーディオコールバック停止後（shutdownAudio 後）に呼ばれる前提
        delete pending.exchange (nullptr);
        delete retired.exchange (nullptr);
        delete current.exchange (nullptr);
    }

    // ---- メッセージスレッド専用 ----
    void push (std::unique_ptr<PlaybackSnapshot> snapshot)
    {
        // オーディオスレッドが未取得の pending は誰にも見られていないので即 delete してよい
        delete pending.exchange (snapshot.release());
    }

    void deleteRetired()
    {
        delete retired.exchange (nullptr);
    }

    // ---- オーディオスレッド専用 ----
    PlaybackSnapshot* acquire()
    {
        if (pending.load() != nullptr && retired.load() == nullptr)
        {
            if (auto* next = pending.exchange (nullptr))
            {
                retired.store (current.load());
                current.store (next);
            }
        }
        return current.load();
    }

private:
    std::atomic<PlaybackSnapshot*> pending { nullptr }; // メッセージスレッドが書く
    std::atomic<PlaybackSnapshot*> retired { nullptr }; // オーディオスレッドが書き、メッセージスレッドが解放
    std::atomic<PlaybackSnapshot*> current { nullptr }; // オーディオスレッドのみ書く

    JUCE_DECLARE_NON_COPYABLE (SnapshotExchange)
};
