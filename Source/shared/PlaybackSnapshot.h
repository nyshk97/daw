#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

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

    // 固定ストリップFXのON/OFF（固定チェーンに「挿す」概念はなく常在。既定ON）。
    // DSP実装（スライス3〜4）でオーディオスレッドが読む前提の共有atomic。
    // それまではUI＋保存のみで音に影響しない
    std::atomic<bool> eqEnabled { true };
    std::atomic<bool> compEnabled { true };

    // トラック出力のピークレベル（UIメーター用）。オーディオスレッドはCASでmax更新し、
    // UI（30Hz Timer）は exchange(0) で読み取りリセットする。1UIフレームに複数ブロックが
    // 走っても最大値が渡る（storeだと最後のブロックしか見えず瞬発音を取りこぼす）
    std::atomic<float> peakLevel { 0.0f };

    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<bool>::is_always_lock_free);
};

// エンジン用の再生スナップショット。メッセージスレッドが構築し、オーディオスレッドは読むだけ。
// shared_ptr の参照カウント操作（コピー・破棄）はメッセージスレッドでのみ起きる。
struct ClipPlayback
{
    std::shared_ptr<const juce::AudioBuffer<float>> audio; // モノラル・メモリ常駐。生存保証のため所有を共有
    juce::int64 startSample = 0;
    juce::int64 offsetSamples = 0;  // ソースバッファ内の読み出し開始位置（buildSnapshotでバッファ範囲内を保証済み）
    juce::int64 lengthSamples = 0;  // 再生長
};

// GM音源（DLSMusicDevice）1インスタンス＋オーディオスレッド専用の発音状態。
// SynthBank（メッセージスレッド）が生成・所有し、スナップショットが shared_ptr で共有する。
// 破棄は「参照する全スナップショットの解放後」＝ deleteRetired() 経由でメッセージスレッドのみで起きるため、
// オーディオスレッドがレンダリング中のAUが消えることはない（ClipPlayback::audio と同じ寿命保証）。
struct SynthInstance
{
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    int midiChannel = 1;            // ドラムキットは10
    double preparedSampleRate = 0.0; // このレートで prepareToPlay 済み。デバイスと不一致ならレンダリングをスキップ
    int preparedBlockSize = 0;
    int totalOutputChannels = 2;    // processBlock に渡すべきチャンネル数（DLSは2バス計4ch）。ミックスはch0/1のみ

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
    std::shared_ptr<SynthInstance> synth;       // MIDIトラックのみ（未生成ならnullptr）
    std::vector<MidiNotePlayback> notes;        // MIDIトラックのみ
};

struct PlaybackSnapshot
{
    std::vector<TrackPlayback> tracks;

    // send用固定バス（gain=リターン量・mute・peakLevelを使用）とMaster（gain・peakLevelを使用）。
    // Project が所有する実体を shared_ptr で共有する（トラックの params と同じ寿命規則）
    std::shared_ptr<TrackParams> busParams[numSendBuses];
    std::shared_ptr<TrackParams> masterParams;
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
