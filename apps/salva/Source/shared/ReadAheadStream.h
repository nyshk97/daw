#pragma once

#include <atomic>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

// ディスクストリーミング再生用の自前read-aheadソース（planの技術判断そのもの）。
//
// なぜ自前か: `AudioFormatReaderSource` 単体はオーディオスレッド上のファイルI/O、
// JUCEの `BufferingAudioSource` はオーディオコールバックが callbackLock を取り、
// バックグラウンド側が同じロックを保持したままディスクを読む＝優先度逆転（GOTCHAS.md）。
//
// 構造: 固定長のブロックリング（SPSC・シングルライター/シングルリーダー）。
//   - ライター（バックグラウンドスレッド）が fillOnce() でディスクから埋める
//   - リーダー（オーディオスレッド）は readAudio() でロックなし・アロケーションなしに読む
//
// 時間整合の契約:
//   - 各ブロックは絶対サンプル位置＋seek generation を持ち、リーダーは現在の要求位置・
//     現generationと一致するブロックだけを再生する
//   - 枯渇時は無音＋atomicカウンタ。遅れて届いた過去のブロックは破棄する
//     （順に再生するとそのストリームだけ以後ずっと時間がずれるため。欠けは捨てて位置を保つ）
//   - シークは generation を進めて旧データを無効化する（リングの消去はしない。
//     旧世代ブロックはリーダー/discardStale が読み捨てて空きが戻る）
//   - ループ端は予測可能なのでギャップを作らない: ライターが終端到達前に選択範囲先頭を
//     先読みし、リング上で隙間なく折り返す
//
// スレッド割り当て（このクラス自身はスレッドを持たない。テストは全関数を直接呼ぶ）:
//   - prepare / setLoop / requestSeek: メッセージスレッド
//   - fillOnce: バックグラウンドスレッド（AudioFormatReader は呼び出し側が所有）
//   - readAudio / discardStale: オーディオスレッド
class ReadAheadStream
{
public:
    static constexpr int blockSamples = 4096;
    static constexpr int numBlocks = 64; // 4096×64 ≒ 6秒 @44.1kHz。メモリは2ch floatで約2MB

    ReadAheadStream();

    // 新しいソースに切り替える（サンプル数を設定し、seekで全旧データを無効化する）。
    // リングの消去はgenerationに任せるため、オーディオコールバック並走中でも安全
    void prepare (juce::int64 sourceLengthSamples);

    juce::int64 sourceLength() const { return sourceLengthSamples.load(); }

    // ループ範囲（元音源サンプル基準・endは排他的）。再生中に変えたら requestSeek で
    // 再同期するのは呼び出し側の契約（ライターの先読みが旧範囲のままになるため）
    void setLoop (juce::int64 start, juce::int64 end, bool enabled);

    // 再生位置の変更要求。generationを進め、旧データを無効化する
    void requestSeek (juce::int64 position);

    // --- ライター側（バックグラウンドスレッド） ---
    // リーダーの進行に合わせて次のブロックを1つ書く。書いたサンプル数を返す
    // （0 = リング満杯 or 終端で書くものがない。呼び出し側は少し待って再試行する）。
    //
    // maxGeneration = このreaderで応じてよいseek世代の上限。現在の要求世代がこれを
    // 超えていたら**採用せず0を返す**（ソース差し替えの競合対策: 「旧readerのデータを
    // 新世代として公開し、切替前のファイルが最大1ブロック鳴る」を防ぐ。呼び出し側は
    // 「pendingの新readerが無い」ことを確認した時点の世代を渡す契約）
    int fillOnce (juce::AudioFormatReader& reader, juce::uint32 maxGeneration);
    int fillOnce (juce::AudioFormatReader& reader) { return fillOnce (reader, latestGeneration()); }

    // 現在の要求世代（fillOnceのmaxGeneration用スナップショット）
    juce::uint32 latestGeneration() const { return seekGeneration.load (std::memory_order_acquire); }

    // --- リーダー側（オーディオスレッド） ---
    // outL/outR へ numSamples 書く。欠け・停止相当は無音（先に全消去してから重ねる）。
    // シーク適用・ループ折り返し・旧世代/過去ブロックの破棄を含む
    void readAudio (float* outL, float* outR, int numSamples);

    // 停止中もコールバックごとに呼ぶ: 旧世代ブロックを読み捨ててリングの空きを戻し、
    // 保留中のシークを適用する（これが無いとシーク後の再生開始が毎回枯渇から始まる）
    void discardStale();

    // readerPositionから連続で minSamples 読めるか（再生開始プライミングの判定用）。
    // ループは折り返し先を連続として数える。非ループのソース終端まで書けている場合だけ
    // それ以上待てないので true。先に discardStale() で採用・読み捨てを済ませてから呼ぶ。
    // オーディオスレッド専用
    bool hasPendingData (int minSamples) const;

    // 現在の要求位置のまま世代だけ進める（遷移がコールバックと重なったブロックの後始末用。
    // オーディオスレッドから呼べる）。全ストリームに撃つと、採用タイミング差でずれた
    // readerPositionが同じ要求位置へ収束し、ライターもそこから書き直す。
    // ※ seekPositionを書き直さないのは、並行する新しいrequestSeekの位置を上書きしないため
    void reissueSeek() { seekGeneration.fetch_add (1, std::memory_order_release); }

    // 枯渇カウンタの2段公開（オーディオスレッド専用）: readAudioはブロック中の枯渇を
    // 一時値に貯めるだけで、公開カウンタ(starvedSamples)には触れない。ブロックが確定したら
    // commit、遷移競合で破棄したら discard を呼ぶ。UIが破棄ブロックの一時値を観測することは
    // なく、公開カウンタは単調増加のまま
    void commitStarved()
    {
        if (pendingStarved > 0)
        {
            starvedCount.fetch_add (pendingStarved, std::memory_order_relaxed);
            pendingStarved = 0;
        }
    }
    void discardStarved() { pendingStarved = 0; }

    // --- 観測（UIスレッドから読む） ---
    juce::int64 playheadPosition() const { return playheadSample.load(); }
    // UI表示用: 未適用のシークがあればその位置、なければリーダーの現在位置
    // （GOTCHAS「要求と結果の2つのatomic」のUI側ヘルパー）
    juce::int64 uiPosition() const;
    juce::uint64 starvedSamples() const { return starvedCount.load(); }
    bool reachedEnd() const { return endFlag.load(); }
    void clearEndFlag() { endFlag.store (false); }

private:
    struct Block
    {
        juce::int64 start = 0;
        juce::uint32 generation = 0;
        int numSamples = 0;
        std::vector<float> left, right;
    };

    // ループを考慮した位置の正規化（end ちょうどは先頭へ折り返す）
    juce::int64 normalize (juce::int64 pos, juce::int64 ls, juce::int64 le, bool loop) const
    {
        return (loop && pos == le) ? ls : pos;
    }

    void adoptSeekForReader();

    std::vector<Block> blocks;
    juce::AudioBuffer<float> writerTemp; // fillOnce専用（ライタースレッドのみ）

    // リングのインデックス（単調増加。slot = index % numBlocks）
    std::atomic<juce::uint32> writeIndex { 0 };
    std::atomic<juce::uint32> readIndex { 0 };

    // シーク要求（メッセージスレッドが書き、両スレッドが世代比較で適用する）
    std::atomic<juce::uint32> seekGeneration { 0 };
    std::atomic<juce::int64> seekPosition { 0 };

    std::atomic<juce::int64> sourceLengthSamples { 0 };
    std::atomic<juce::int64> loopStartSample { 0 };
    std::atomic<juce::int64> loopEndSample { 0 };
    std::atomic<bool> loopOn { false };

    // ライター専用状態
    juce::uint32 writerGeneration = 0;
    juce::int64 writerPosition = 0;

    // リーダー専用状態
    juce::uint32 readerGeneration = 0;
    juce::int64 readerPosition = 0;

    // リーダーが公開する観測値
    std::atomic<juce::int64> playheadSample { 0 };
    std::atomic<juce::uint32> appliedGeneration { 0 };
    std::atomic<juce::uint64> starvedCount { 0 };
    juce::uint64 pendingStarved = 0; // ブロック中の未公開の枯渇量（オーディオスレッドのみ）
    std::atomic<bool> endFlag { false };

    static_assert (std::atomic<juce::int64>::is_always_lock_free);
    static_assert (std::atomic<juce::uint32>::is_always_lock_free);
};
