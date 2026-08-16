#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "BusDelay.h"
#include "BusReverb.h"
#include "MasterLimiter.h"
#include "MasterMeterSource.h"
#include "Recorder.h"
#include "../shared/TransportState.h"
#include "../shared/PlaybackSnapshot.h"
#include "../shared/Ppq.h"
#include "../shared/PreviewFifo.h"

class AudioFilePreview;
class AnalyzerTap;

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
    void setFilePreview (AudioFilePreview* previewToUse) { filePreview = previewToUse; }
    // アナライザタップ（EQエディタのスペクトラム表示）。オーディオ開始前にセットすること
    void setAnalyzerTap (AnalyzerTap* tapToUse) { analyzerTap = tapToUse; }
    // Masterメーターのリング（LUFS/相関/TPの十分統計量）。オーディオ開始前にセットすること
    void setMasterMeterRing (MasterMeterRing* ringToUse) { masterMeter.setRing (ringToUse); }

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

    // 1セグメント分の出力（クリップ・MIDI・バス/Master・クリック）。オーディオスレッド専用。
    // サイクル（ループ範囲）で1コールバックが複数セグメントに分割されるため、process() から
    // セグメントごとに呼ばれる。outOffset = デバイスバッファ内の書き込み開始位置
    // （スクラッチバッファは常にオフセット0から segLen 分を使い、最終出力だけずらす）。
    // fadeStartSample/fadeEndSample = 曲末フェード（サンプル換算済み・無効は 0/0）。呼び出し側が
    // 境界でセグメントを切るので、1セグメントは「フェード前／区間内／終端以後」のどれかに収まる
    // timelineJumped: シーク・再生開始・サイクルラップ等の時間不連続（EQのIIR履歴リセットに使う。
    // snapshotChanged は含めない — リージョンゲイン調整等の差し替えは時間が連続している）
    // limiterReset: Master Limiterのディレイ/GR包絡リセット（再生開始・明示シークのみ。
    // **サイクルラップを含めない** — timelineJumpedと違い、ラップでリセットすると
    // 毎ループ先頭にlookahead分の無音が入る。ラップは連続ストリームとして扱う）
    // meterPlayEdge: Masterメーターの世代切替（再生開始エッジのみ。シーク・ラップは含めない＝
    // integratedは「再生開始から」の平均）。meterStopEdge: 停止エッジ（部分統計の確定＋TP flush）
    void processSegment (juce::AudioBuffer<float>& buffer, int outOffset, int segLen, juce::int64 segPos,
                         bool playing, bool armed, juce::int64 punchIn,
                         PlaybackSnapshot* snapshot, bool anySolo, bool canProcess, float masterGain,
                         juce::int64 fadeStartSample, juce::int64 fadeEndSample,
                         double sr, double bpm, double beatLen,
                         bool silenceTransport, bool silenceAll, bool resound, bool timelineJumped,
                         bool limiterReset, bool meterPlayEdge, bool meterStopEdge);

    TransportState& transport;
    SnapshotExchange& snapshots;
    PreviewFifo& previewFifo;
    Recorder recorder;
    AudioFilePreview* filePreview = nullptr;
    AnalyzerTap* analyzerTap = nullptr; // 非所有（MainComponentが所有）。テスト等ではnullptrのまま

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

    // Master Limiter（RT用の実行状態。Master 1本＝エンジン全体で1個なのでここに住む。
    // バウンスは BounceRenderer が独立インスタンスを持つ）。停止中もプレビューがMaster経路を
    // 通るため常時処理する。パラメータはスナップショットの masterParams->limiter を毎セグメント読む
    MasterLimiter masterLimiter;

    // バスFX（RT用の実行状態。各バス1個＝エンジン全体で3個。バウンスは独立インスタンス）。
    // バスMute/Gain 0 中も処理は止めない（状態凍結→解除時に古いエコーが復活するのを防ぐ。
    // 出力の加算だけを止める）。リセットはLimiterと同じ limiterReset（再生開始・明示シークのみ。
    // サイクルラップでは呼ばない＝エコー・残響はループを跨いで続く）
    BusReverb busReverbs[2];
    BusDelay busDelay;

    // Masterメーターの計測DSP（Limiter・フェード後の最終出力を測る。再生中は常時稼働）
    MasterMeterSource masterMeter;

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

    // サイクル境界でラップした直後を「内部シーク」として次セグメント（コールバック跨ぎ含む）へ
    // 伝えるフラグ。立っているセグメントは消音＋跨ぎノート再発音＋クリック拍リセットを行う
    bool cycleWrapPending = false;

    // 再生中のノート編集（MIDI構成の差し替え）検出用。差し替え時は消音→跨ぎノート再発音で
    // 「削除されたノートのオフが新スナップショットに存在せず鳴りっぱなし」になるのを防ぐ。
    // ポインタでなく PlaybackSnapshot::midiGeneration を見るので、オーディオ側だけの差し替え
    // （リージョンゲインのドラッグ）では発音を乱さない。0 = まだ何も受け取っていない
    juce::uint64 lastSeenMidiGeneration = 0;

    // EQ用のセグメント連番（processSegmentごとに+1）。各トラックの TrackEq がこの連続性で
    // 「処理されない期間があったか」（＝再進入・履歴リセットが要るか）を判定する
    juce::uint64 eqSerial = 0;

    juce::int64 lastBeatIndex = 0;
    double clickPhase = 0.0;
    double clickFreq = 880.0;
    float clickAmp = 0.0f;
    int clickSamplesLeft = 0;
    int clickTotalSamples = 1;
};
