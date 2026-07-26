#pragma once

#include <atomic>
#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>

// 外部ワンショットサンプル1個を鳴らす軽量サンプラー。MIDIトラックの音源として
// DLSMusicDevice（AUプラグイン）の代わりに使う（SynthInstance が両者を排他で持つ）。
//
// スレッド境界:
// - processBlock() はオーディオスレッド専用。ヒープ確保・ロック・ログを一切しない
//   （ボイス配列は固定長で事前確保、サンプルバッファは shared_ptr で共有所有）
// - set系（gain/startOffset/pitchFollow/rootNote）と requestStopAll() はメッセージスレッドから
//   呼ぶ。すべて atomic 経由で、発音中のボイスには影響しない（次の発音から反映）
// - サンプルバッファ・ソースSR・デバイスSRは生成時に確定する。差し替え・SR変更は
//   SynthBank がインスタンスごと作り直す（旧インスタンスはスナップショット退役後に破棄）
//
// 発音・消音の規則（MIDIにはノート個体のIDが無いため明文化する）:
//   noteOn  : 空きボイスへ割り当て（満杯なら最古を奪う）。ゲイン = gain × velocity/127
//             再生レート = sourceRate/deviceRate（追従モードは 2^((pitch-rootNote)/12) 倍）
//   noteOff : 固定モード（One Shot）= 無視（最後まで鳴らす）
//             追従モード = 同ピッチの未リリースボイスのうち最古を5msフェードでリリース
//   CC123（All Notes Off）: 全ボイスを5msフェードで停止
//   mono ON : noteOn のたびに既存の全ボイスを切ってから鳴らす
//             （Logicの Quick Sampler「Polyphony: 1」相当＝リトリガーあり。実機TR-808と同じ挙動で、
//              808ロールのように長い音を連打しても重ならない。OFF（既定）は重ねて鳴らす）
//
// Mono時に同時に鳴るボイス:
//   通常は「現在の1本＋リリース中の尾（5ms）」の2本まで。
//   - 同じ位置に複数の noteOn が来たとき（同時刻の和音等）は、まだ一度も出力していないボイスを
//     フェードせず即停止するので尾は増えない（出力していないボイスのフェードは振幅を足すだけで
//     音楽的な中身がない）
//   - リリース長（5ms）より短い間隔で連打された場合だけ尾が複数残る（ボイス上限16で有界）。
//     1/64音符@200BPMでも約19ms間隔なので、実用上は起きない
//
// noteOff とMonoの関係: MIDIのnoteOffにはノート個体のIDがないため、Monoで先に切ったノートの
// noteOff が後から届くと「今鳴っている新しいボイス」を止めてしまう（追従＋Monoで重なるノートを
// 書いたときに起きる）。pendingNoteOffs でピッチごとに「飲むべきオフの数」を持って回避する。
// 万一オフが届かず数が残っても、停止・シーク・サイクル折返し（CC123）と停止要求でクリアされる。
//
// ボイスは「発音時のモード」を自分で保持する。pitchFollow は非同期に変わるため、
// noteOff の扱いをトラックの現在モードで判断するとブロック境界で不整合が出る。
class SamplerEngine
{
public:
    static constexpr int maxVoices = 16;           // 固定長（連打を重ねて鳴らすため多めに取る）
    static constexpr double releaseSeconds = 0.005; // リリースのフェード長（急に切るとクリックが出る）

    // audio は生成後に書き換えないこと（オーディオスレッドが読み続ける）。
    // sourceSampleRate はファイル自体のSR、deviceSampleRate は出力先のSR
    SamplerEngine (std::shared_ptr<const juce::AudioBuffer<float>> audio,
                   double sourceSampleRate, double deviceSampleRate);

    // ---- メッセージスレッド（発音中のボイスには影響せず、次の発音から反映）----
    void setGain (float newGain) { gain.store (newGain); }
    void setStartOffset (juce::int64 offset) { startOffset.store (offset); }
    void setRootNote (int note) { rootNote.store (juce::jlimit (0, 127, note)); }
    void setPitchFollow (bool follow) { pitchFollow.store (follow); }
    void setMono (bool monoMode) { mono.store (monoMode); }

    // 次のブロックの先頭で全ボイスを5msフェード停止する。
    // 音程モード（固定⇄追従）が切り替わった瞬間に SynthBank::sync() が立てる。
    // 注意: サンプル自体の差し替えはこの経路を通らない（SamplerEngineごと交換され、旧インスタンスは
    // 退役スナップショット側へ移ってもうレンダリングされないため、旧ボイスのフェードは原理的に
    // 出力できない）。フェードの有無がモード変更と非対称なのはこの構造による
    void requestStopAll() { stopAllRequested.store (true); }

    // ---- オーディオスレッド専用 ----
    // plugin->processBlock と同じ形。buffer の ch0/1 へ加算する（bufferはclear済み前提でなくてよい）
    void processBlock (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi);

    // テスト・診断用（オーディオスレッドと同時に呼ばないこと）
    int numActiveVoices() const;

private:
    struct Voice
    {
        bool active = false;
        int pitch = 0;
        double position = 0.0;   // ソースバッファ内の小数読み出し位置
        double rate = 1.0;       // 1出力サンプルあたりに進むソースサンプル数
        float voiceGain = 1.0f;  // gain × velocity/127（発音時に焼き込む）
        bool followMode = false; // 発音時のモード（noteOffの扱いをこの値で決める）
        bool releasing = false;
        bool sounded = false;    // 一度でも出力したか（未出力ならフェードせず即停止できる）
        int releaseLeft = 0;     // 残りフェードサンプル数
        juce::uint64 order = 0;  // 発音順（最古のボイスを奪う・リリースするときの比較キー）
    };

    void handleNoteOn (int pitch, int velocity);
    void handleNoteOff (int pitch);
    // swallowFollowOffs: 切った追従ボイスの分だけ pendingNoteOffs を増やす（Mono由来の停止で使う）。
    // CC123・停止要求のときは false（全部止めるので、あとから来るオフを飲む必要がない）
    void releaseAll (bool swallowFollowOffs);
    void renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);

    std::shared_ptr<const juce::AudioBuffer<float>> sample;
    const float* sourceL = nullptr; // sample が生きている間だけ有効（生成時に解決）
    const float* sourceR = nullptr;
    juce::int64 sourceLength = 0;
    double baseRate = 1.0; // sourceRate / deviceRate
    int releaseSamples = 1;

    std::atomic<float> gain { 1.0f };
    std::atomic<juce::int64> startOffset { 0 };
    std::atomic<int> rootNote { 60 };
    std::atomic<bool> pitchFollow { false };
    std::atomic<bool> mono { false };
    std::atomic<bool> stopAllRequested { false };

    // GOTCHAS.md: lock-freeでないatomicはミューテックスへフォールバックしてRT違反になる
    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<juce::int64>::is_always_lock_free);
    static_assert (std::atomic<int>::is_always_lock_free);
    static_assert (std::atomic<bool>::is_always_lock_free);

    Voice voices[maxVoices];
    juce::uint64 nextOrder = 1;

    // ---- オーディオスレッド専用 ----
    // ピッチごとの「飲むべき noteOff の数」。Monoが先に切ったノートのオフを1件ずつ無効化する
    juce::uint8 pendingNoteOffs[128] = {};

    JUCE_DECLARE_NON_COPYABLE (SamplerEngine)
};
