#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../shared/PlaybackSnapshot.h"
#include "BusDelay.h"
#include "BusReverb.h"
#include "MasterLimiter.h"

struct Track; // shared/Project.h（buildItemRenderの実装側でinclude）

// オフラインバウンス（書き出し）。オーディオデバイス・リアルタイムスレッドとは無関係で、
// 専用のバックグラウンドスレッド（juce::Thread）がレンダリングとファイルIOの全てを行う。
//
// スレッド境界の前提:
// - start() / cancel() / cancelAndWait() / status() / progress() / takeResult() はメッセージスレッド専用
// - Request の中身は start() 以降ワーカーが専有する。SynthInstance はリアルタイム再生用と
//   共有してはならない（processBlock が並走する）。必ず SynthBank::createIndependent() で
//   作った専用インスタンスを渡すこと
// - mute/solo/gain は開始時に確定した値を Request に焼き込む（共有atomicの TrackParams は参照しない）
// - 完了通知は push 型でなく pull 型: 呼び出し側（MainComponentのTimer）が status() をポーリングする
//
// 書き出しは2パス（メモリに全曲を蓄積しない）:
//   パス1: ブロックごとにミックスして一時WAV（32bit float）へストリーム書き出し＋ピーク計測
//   パス2: ピーク>1.0なら全体をスケールしつつ 24bit WAV の一時ファイルへ変換
//   成功時のみ POSIX rename() で出力先を原子的に置換する（キャンセル・失敗時は既存ファイル不変）
class BounceRenderer : private juce::Thread
{
public:
    static constexpr int renderBlockSize = 1024; // 専用synthはこのブロックサイズで prepareToPlay しておくこと

    struct TrackRender
    {
        std::vector<ClipPlayback> clips;      // オーディオトラックのみ
        std::vector<MidiNotePlayback> notes;  // MIDIトラックのみ。startPpq昇順（buildSnapshotが保証）
        std::shared_ptr<SynthInstance> synth; // MIDIトラックのみ。バウンス専用インスタンス
        float gain = 1.0f;                    // 開始時に固定済み（非可聴トラックはRequestに入れない）
        float pan = 0.0f;                     // -1..+1（モノクリップは等パワー補正型・ステレオクリップとシンセはバランス型。RTと同じ法則）
        float sends[numSendBuses] { 0.0f, 0.0f, 0.0f }; // post-fader send量

        // トラックEQ・Comp・Sat（開始時にプレーン値で固定。共有atomicの TrackParams は
        // 参照しない）。eq / compDsp / satDsp はバウンス専用の独立DSPインスタンス —
        // RT再生用の TrackParams::rtEq 等と履歴を共有すると両スレッドが同時更新してしまうため、
        // renderPass 開始時に snapTo で初期化する
        bool eqEnabled = true;
        Eq::Values eqBands = Eq::defaultValues();
        TrackEq eq;
        bool compEnabled = false;
        Comp::Values comp;
        TrackComp compDsp;
        bool satEnabled = true;
        Sat::Values sat;
        TrackSaturator satDsp;
        bool lofiEnabled = true;
        Lofi::Values lofi;
        TrackLofi lofiDsp;

        // FX設定（EQ/Comp/Sat/Lo-fi の enabled＋プレーン値）を TrackParams からまとめて固定する。
        // 個別コピーの手書きは新FX追加時の漏れ事故になる（実際にテスト側で Sat/Lo-fi が
        // 落ちていた）ため、本番（buildItemRender / MainComponent::startBounce）も
        // テストも必ずこれを通す
        void loadFxFrom (const TrackParams& params);
    };

    struct Request
    {
        std::vector<TrackRender> tracks;
        double sampleRate = 0.0;
        double bpm = 120.0;
        juce::int64 startSample = 0; // レンダリング範囲。通常は 0〜曲末、サイクルON時はその範囲
        juce::int64 endSample = 0;
        bool wantTail = false;       // 余韻テールを付けるか（判定は resolveWantTail /
                                     // trackWantsTail に一元化。サイクル・曲末フェード・
                                     // ⌘Eの厳密長書き出しでは呼び出し側が false に落とす）
        juce::File targetFile;

        // 固定バス・Master（開始時に固定済み。RTのprocessと同じくバスFX→Masterゲインの順）
        float busGain[numSendBuses] { 1.0f, 1.0f, 1.0f };
        bool busMute[numSendBuses] { false, false, false };
        float masterGain = 1.0f;

        // バスFX（v19。開始時にプレーン値で固定。常在なので必ず通る）
        Reverb::Values busReverb[2] { Reverb::defaultsForBus (0), Reverb::defaultsForBus (1) };
        Delay::Values busDelay;

        // Master Limiter（v17。開始時にプレーン値で固定。常在なので必ず通る。
        // 出力の遅延はワーカーが先頭Lサンプル破棄＋末尾Lサンプル無音flushで吸収し、
        // 書き出しファイルの長さ・頭出しは従来と変わらない）
        Limiter::Values limiter;

        // 曲末フェードアウト（16分音符単位・無効は 0/0）。サンプル換算は RT と同じ SongFade を通す
        // （片方だけ別式にすると再生とバウンスの出力一致が崩れる）
        int fadeOutStartSixteenths = 0;
        int fadeOutEndSixteenths = 0;
        juce::int64 fadeStartSample() const;
        juce::int64 fadeEndSample() const;
        bool hasFadeOut() const { return fadeOutEndSixteenths > fadeOutStartSixteenths; }

        // 通常バウンス（サイクルOFF）に曲末フェードを反映する。フェード未設定なら何もしない。
        // 呼び出し側（MainComponent）の判断をここへ閉じてテスト可能にしている:
        // - 終端＝フェード終端。**素材終端との jmin にしない**（終端を素材より後ろへ置いたとき
        //   ＝MIDIのリリースや余韻をフェードさせたいときに切れてしまう）
        // - テールを落とす。テールループは無音判定の前に必ず1ブロック書くため、endSampleを
        //   合わせるだけでは末尾に無音が付いて固定終端にならない。フェード終端でゲインは
        //   厳密に0なので、鳴り残った余韻を捨てても聴こえ方は変わらない
        void applySongFadeToRange();

        // tracks からテールの既定値を決める（トラック・busGain/busMute を詰め終えたら呼ぶ。
        // サイクル・曲末フェードの上書きより前）。判定は trackWantsTail ＋ バスFXテール
        // （send > 0 のトラックがあり送り先バスが有効出力 — mute/soloの再判定は不要:
        // 呼び出し側が可聴トラックだけを tracks に焼き込む契約）。本番の組み立て
        //（MainComponent::startBounce）とテストの両方がこれを使う（判定の再実装禁止）
        void resolveWantTail();

        // バスFXテールが要るか（resolveWantTail の一部＝テールループの窓・上限の分岐にも使う）
        bool busFxTailActive() const;
        // このバスへ send > 0 のトラックがあり、かつバスが有効出力か（窓の計算が個別に見る）
        bool busSendActive (int busIndex) const;
    };

    // このトラックは範囲終端後の余韻テールを必要とするか（Request::wantTail の入口判定）:
    // - MIDIトラック（synthあり）= ノートのリリース余韻
    // - オーディオトラック = TrackFx::producesTail なFX（現在はEQ）のリングアウト。
    //   将来のstateful FXは producesTail に足せばテール経路と入口判定の両方へ同時に効く
    static bool trackWantsTail (const TrackRender& track);

    // 選択された1アイテム（クリップ or MIDIリージョン）だけをレンダリング対象にした
    // TrackRender とレンダリング範囲を組み立てる（⌘Eのリージョン書き出し用。純粋なモデル→Request変換で
    // テスト対象）。トラックのmute/solo・アイテム自身のmutedは見ない（明示選択が優先）。
    // クリップ範囲はモデル上の区間（クランプで再生長が縮んだ分は末尾無音）、MIDIはリージョン境界を
    // PPQ→サンプル換算した厳密長。synthは呼び出し側が生成して埋める。
    // index範囲外・クリップの参照WAVなしは false（ノート空のMIDIリージョンは notes 空で true）
    static bool buildItemRender (const Track& track, int itemIndex, double bpm, double sampleRate,
                                 TrackRender& out, juce::int64& rangeStart, juce::int64& rangeEnd);

    // 固定モード（One Shot）のサンプル音源が鳴り切る終端（サンプル位置。0 = 延長不要）。
    // 通常バウンス（全体）はレンダリング範囲をここまで延ばす: テールは最大5秒＋1ブロックでも
    // -60dBを下回ると打ち切るため、曲末の長いワンショットや途中に無音区間を含むサンプルが切れる。
    // サイクルバウンス・リージョン書き出しは「指定範囲を書き出す」のが仕様なので延長しない。
    // 追従モードはノート長で止まるので対象外（既存のテールで足りる）
    static juce::int64 oneShotEndSample (const Track& track, const std::vector<MidiNotePlayback>& notes,
                                        double bpm, double sampleRate);

    enum class Status { idle, running, success, cancelled, failed };

    struct Result
    {
        Status status = Status::idle;
        juce::String errorMessage;      // failed のとき
        juce::int64 writtenSamples = 0; // 最終WAVの長さ（テール込み）
        float peak = 0.0f;              // スケール前のミックスピーク
        bool scaled = false;            // ピーク>1.0でスケールダウンしたか

        // 出来上がったファイルの計測（完了表示用。ワーカーが書き出し直後に測って載せる）
        bool loudnessMeasured = false;
        double integratedLufs = 0.0;
        double truePeakDb = 0.0;
    };

    BounceRenderer() : juce::Thread ("Bounce Renderer") {}

    ~BounceRenderer() override
    {
        cancelAndWait();
    }

    // 実行中なら false。開始後、Request の所有はワーカーに移る
    bool start (Request&& requestToRun)
    {
        if (isThreadRunning())
            return false;
        request = std::move (requestToRun);
        result = {};
        progressValue.store (0.0f);
        currentStatus.store (Status::running);
        startThread();
        return true;
    }

    // 非同期キャンセル要求（ワーカーは次のブロック境界で中断し、一時ファイルを削除して終了する）
    void cancel() { signalThreadShouldExit(); }

    // キャンセル要求＋ワーカー終了まで待つ（閉じる/終了フロー用。レンダリングは数秒想定）
    void cancelAndWait()
    {
        signalThreadShouldExit();
        stopThread (-1);
    }

    Status status() const { return currentStatus.load(); }
    float progress() const { return progressValue.load(); }

    // 終了後（status != running）に結果を取り出して idle に戻す
    Result takeResult()
    {
        jassert (! isThreadRunning());
        auto taken = std::move (result);
        result = {};
        currentStatus.store (Status::idle);
        return taken;
    }

private:
    void run() override;

    Status renderAndWrite();
    // パス1/パス2。false = 中断（キャンセル、またはrenderFailed=trueのときIO失敗）
    bool renderPass (juce::AudioFormatWriter& writer);
    bool convertPass (juce::AudioFormatWriter& writer);

    // MIDIトラックの直線走査状態（ワーカー専用）。RTエンジンと違いシーク・プレビュー・
    // resoundが存在しないので、次ノートのインデックスと発音中リストだけで足りる
    struct SynthCursor
    {
        size_t nextNote = 0;
        std::vector<std::pair<juce::int64, int>> active; // (endPpq, pitch)
    };

    void scheduleBlockMidi (const TrackRender& track, SynthCursor& cursor,
                            juce::int64 pos, int numSamples, double tps);
    // track は EQ の実行状態（track.eq）を進めるため非const
    void renderSynthInto (juce::AudioBuffer<float>& mix, std::vector<juce::AudioBuffer<float>>& busMix,
                          TrackRender& track, int numSamples);

    // 素通しバス（busGain/busMute適用）をmixへ合流 → Masterゲイン → Limiter →
    // 曲末フェード（RTのprocessと同じ順序。フェードはLimiter後・遅延後の位置 absPos-L で評価）
    // absPos = このブロックの絶対サンプル位置
    void mixBusesAndMaster (juce::AudioBuffer<float>& mix,
                            std::vector<juce::AudioBuffer<float>>& busMix, int numSamples,
                            juce::int64 absPos);

    // Limiterの遅延吸収付き書き込み: 先頭 limiterHeadRemaining サンプルを捨ててから書く。
    // false = IO失敗（errorMessage・renderFailedは設定済み）
    bool writeMixDroppingHead (juce::AudioFormatWriter& writer,
                               const juce::AudioBuffer<float>& mix, int numSamples);

    Request request;
    Result result;
    std::atomic<Status> currentStatus { Status::idle };
    std::atomic<float> progressValue { 0.0f };

    juce::File tempFloatFile, tempFinalFile;
    juce::MidiBuffer midiScratch;
    juce::AudioBuffer<float> synthScratch;
    juce::AudioBuffer<float> eqTrackScratch; // EQ有効トラックのクリップ合算用（EQをトラックgainの前に掛けるため）
    juce::uint64 eqBlockSerial = 0;          // TrackEq の連続性判定用（renderPassで0に戻しブロックごとに+1）
    MasterLimiter masterLimiter;             // バウンス専用インスタンス（renderPass開始時にsnapTo）
    BusReverb busReverbs[2];                 // バスFXのバウンス専用インスタンス（同上）
    BusDelay busDelayDsp;
    int limiterHeadRemaining = 0;            // 未破棄のLimiter遅延分（renderPass開始時にLへ）
    std::unique_ptr<juce::AudioFormatReader> convertReader; // パス2で一時float WAVを読み戻す
    std::vector<SynthCursor> cursors;
    float runningPeak = 0.0f;
    juce::int64 samplesWritten = 0;
    bool renderFailed = false; // パスがfalseを返した理由の区別（true=IO失敗 / false=キャンセル）

    JUCE_DECLARE_NON_COPYABLE (BounceRenderer)
};
