#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "Recorder.h"
#include "../shared/TransportState.h"
#include "../shared/PlaybackSnapshot.h"
#include "../shared/Ppq.h"
#include "../shared/PreviewFifo.h"

// サンプル位置ベースの自前ミックスエンジン。process() はオーディオスレッドで走る。
// クリップ/トラック構成は SnapshotExchange 経由で受け取り、単一値は TransportState の atomic を読む。
// UIへの直接参照は持たない。
class PlaybackEngine
{
public:
    PlaybackEngine (TransportState& transportState, SnapshotExchange& snapshotExchange,
                    PreviewFifo& previewFifoToUse);

    // ---- AudioAppComponent から転送される（processのみオーディオスレッド）----
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate);
    void process (const juce::AudioSourceChannelInfo& bufferToFill);
    void releaseResources();

    // ---- メッセージスレッド専用 ----
    void play();
    void stop();

    // カウントイン付き録音。punchInSample 以降の入力だけがファイルに書かれる。
    // 再生ヘッドは punchInSample - countInSamples に移動して再生が始まる（既存クリップも鳴る）
    bool startRecording (const juce::File& file, juce::int64 punchInSample,
                         juce::int64 countInSamples, double deviceSampleRate);
    void stopRecording(); // 残データのflushまで待つ（メッセージスレッドなのでブロックしてよい）
    bool isRecording() const;

private:
    // ノートオンの1ブロック・1トラックあたり上限。超過分は「新規ノートオンを対応オフごと捨てる」
    // （送信済みノートオンのオフは soundingCount 経由で必ず送るので鳴りっぱなしは起きない）
    static constexpr int maxNoteOnsPerBlock = 1024;

    // AUに渡すスクラッチバッファの最大チャンネル数（DLSは2バス計4ch。余裕を持って確保）
    static constexpr int maxSynthChannels = 8;

    // MIDIトラックのAUレンダリングとイベント生成。オーディオスレッド専用。
    // 出力は最終バッファでなく mixScratch / busScratch へ書く（プレビュー発音も含めて
    // 全MIDI出力が pan→sendバス→Master を通る）。
    // silenceTransport: 送信済みの再生ノートを止めてから始める（トランスポートエッジ＋スナップショット差し替え時）
    // silenceAll: プレビュー発音も含めて全消音＋CC123（トランスポートエッジのみ。
    //             スナップショット差し替え時は「ノート作成→プレビュー→push」の流れでプレビューを殺さないよう区別する）
    // resound: 再生位置を跨いでいるノートを offset 0 で再発音（シーク・再生開始・差し替え時）
    void renderMidiTracks (PlaybackSnapshot& snapshot, int numSamples, juce::int64 pos,
                           bool playing, bool silenceTransport, bool silenceAll, bool resound,
                           double sr, double bpm, bool anySolo);

    TransportState& transport;
    SnapshotExchange& snapshots;
    PreviewFifo& previewFifo;
    Recorder recorder;

    double currentSampleRate = 0.0;

    // MIDIレンダリング用の事前確保バッファ（prepareToPlayで確保。コールバック内では確保しない）
    juce::AudioBuffer<float> synthScratch;
    juce::MidiBuffer midiScratch;

    // オーディオトラックのクリップ合算用モノスクラッチ（メーターは加算後ピークを測る必要があるため。
    // クリップはモノソースで、panはミックス分配時に掛けるのでモノ1本で正確）。全トラックで再利用する
    juce::AudioBuffer<float> trackScratch;

    // ステレオミックスの組み立て場所。全トラックのpost-fader/pan信号がここに集まり、
    // sendバス（素通し）→ Masterゲイン → デバイスバッファ（ch0/1のみ）の順で流れる。
    // デバイスバッファへ直接書かないのは、チャンネル数の差異（1ch/3ch以上）の吸収と
    // Master処理を1箇所に集めるため
    juce::AudioBuffer<float> mixScratch;                 // 2ch
    juce::AudioBuffer<float> busScratch[numSendBuses];   // 各2ch。sendの蓄積先（毎ブロックclear）

    // ---- 以下はオーディオスレッド専用の状態 ----

    // プレビュー発音（停止中のみ）。オンはFIFOコマンド、オフは固定発音長のサンプルカウントで自動送出
    static constexpr int maxPreviewNotes = 64;
    static constexpr int maxPreviewCommandsPerBlock = 64;
    struct PreviewNote
    {
        juce::uint64 trackId = 0;
        int pitch = 0;
        juce::int64 samplesLeft = 0;
    };
    PreviewNote previewNotes[maxPreviewNotes];
    int numPreviewNotes = 0;
    PreviewFifo::Command previewCommands[maxPreviewCommandsPerBlock];
    int numPreviewCommands = 0;

    bool prevPlaying = false;

    // 再生中のノート編集（スナップショット差し替え）検出用。差し替え時は消音→跨ぎノート再発音で
    // 「削除されたノートのオフが新スナップショットに存在せず鳴りっぱなし」になるのを防ぐ。
    // 比較にしか使わない（参照はしない。旧スナップショットは解放済みの可能性がある）
    const PlaybackSnapshot* lastSeenSnapshot = nullptr;
    juce::int64 lastBeatIndex = 0;
    double clickPhase = 0.0;
    double clickFreq = 880.0;
    float clickAmp = 0.0f;
    int clickSamplesLeft = 0;
    int clickTotalSamples = 1;
};
