#include "ReadAheadStream.h"

ReadAheadStream::ReadAheadStream()
    : writerTemp (2, blockSamples)
{
    blocks.resize (numBlocks);
    for (auto& b : blocks)
    {
        b.left.resize ((size_t) blockSamples);
        b.right.resize ((size_t) blockSamples);
    }
}

void ReadAheadStream::prepare (juce::int64 newSourceLengthSamples)
{
    sourceLengthSamples.store (newSourceLengthSamples);
    endFlag.store (false);
    requestSeek (0);
}

void ReadAheadStream::setLoop (juce::int64 start, juce::int64 end, bool enabled)
{
    loopStartSample.store (start);
    loopEndSample.store (end);
    loopOn.store (enabled && end > start);
}

void ReadAheadStream::requestSeek (juce::int64 position)
{
    // 位置を先に置いてから世代を進める（release）。読む側は世代（acquire）→位置の順。
    // 連続シークで「新しい位置＋古い世代」を読む窓はあるが、次のコールバックで追いつく
    seekPosition.store (position, std::memory_order_relaxed);
    seekGeneration.fetch_add (1, std::memory_order_release);
}

int ReadAheadStream::fillOnce (juce::AudioFormatReader& reader, juce::uint32 maxGeneration)
{
    const auto gen = seekGeneration.load (std::memory_order_acquire);
    // このreaderに属する保証がない世代には応じない（wrap耐性のため差分の符号で比較）
    if ((juce::int32) (gen - maxGeneration) > 0)
        return 0;
    // シーク適用（世代が進んでいたら新しい位置から読み直す）
    if (gen != writerGeneration)
    {
        writerGeneration = gen;
        writerPosition = seekPosition.load (std::memory_order_relaxed);
    }

    // リング満杯なら何もしない（リーダーが消費・破棄すると空きが戻る）
    const auto wi = writeIndex.load (std::memory_order_relaxed);
    if (wi - readIndex.load (std::memory_order_acquire) >= (juce::uint32) numBlocks)
        return 0;

    const auto ls = loopStartSample.load();
    const auto le = loopEndSample.load();
    const bool loop = loopOn.load();
    const auto len = sourceLengthSamples.load();

    // ループ端は予測可能: 終端に達したら選択範囲先頭へ隙間なく折り返して先読みする
    auto pos = normalize (writerPosition, ls, le, loop);
    if (pos >= len)
        return 0; // 非ループの終端（またはループ外の末尾）。書くものがない

    // このブロックの終わり: ループ中はループ終端、それ以外はファイル終端で切る
    const auto limit = (loop && pos < le) ? juce::jmin (le, len) : len;
    const int n = (int) juce::jmin ((juce::int64) blockSamples, limit - pos);
    if (n <= 0)
        return 0;

    auto& block = blocks[wi % numBlocks];
    // ブロックのバッファを直接JUCEバッファとして参照させて読み込む（コピー1回・確保なし）
    float* channels[2] = { block.left.data(), block.right.data() };
    juce::AudioBuffer<float> dest (channels, 2, n);
    // モノラルソースは reader.read が両チャンネルへ複製する
    reader.read (&dest, 0, n, pos, true, true);

    block.start = pos;
    block.generation = writerGeneration;
    block.numSamples = n;
    writerPosition = pos + n; // ちょうど le に達したら次回の normalize で ls へ折り返す
    writeIndex.store (wi + 1, std::memory_order_release);
    return n;
}

void ReadAheadStream::adoptSeekForReader()
{
    const auto gen = seekGeneration.load (std::memory_order_acquire);
    if (gen != readerGeneration)
    {
        readerGeneration = gen;
        readerPosition = seekPosition.load (std::memory_order_relaxed);
        endFlag.store (false);
    }
}

void ReadAheadStream::readAudio (float* outL, float* outR, int numSamples)
{
    // 欠けは無音の契約: 先に全部消してから、あるぶんだけ重ねる
    juce::FloatVectorOperations::clear (outL, numSamples);
    juce::FloatVectorOperations::clear (outR, numSamples);

    adoptSeekForReader();

    const auto ls = loopStartSample.load();
    const auto le = loopEndSample.load();
    const bool loop = loopOn.load();
    const auto len = sourceLengthSamples.load();

    int out = 0;
    while (out < numSamples)
    {
        const auto pos = normalize (readerPosition, ls, le, loop);
        readerPosition = pos;

        if (pos >= len)
        {
            // 非ループの終端。残りは無音のまま（枯渇には数えない）。UIのTimerが見て停止する
            endFlag.store (true);
            break;
        }

        const auto limit = (loop && pos < le) ? juce::jmin (le, len) : len;
        const int want = (int) juce::jmin ((juce::int64) (numSamples - out), limit - pos);

        const auto ri = readIndex.load (std::memory_order_relaxed);
        if (writeIndex.load (std::memory_order_acquire) == ri)
        {
            // 枯渇: 無音を出して位置は進める（追いついたら続きから鳴る＝時間を保つ）
            starvedCount.fetch_add ((juce::uint64) want, std::memory_order_relaxed);
            readerPosition = pos + want;
            out += want;
            continue;
        }

        auto& block = blocks[ri % numBlocks];
        if (block.generation != readerGeneration || block.start + block.numSamples <= pos)
        {
            // 旧世代 or 遅れて届いた過去のブロックは破棄（順に再生すると以後ずっとずれる）
            readIndex.store (ri + 1, std::memory_order_release);
            continue;
        }

        if (block.start > pos)
        {
            // 次のデータはまだ先: そこまで無音で埋める（ブロック欠落）
            const int gap = (int) juce::jmin ((juce::int64) want, block.start - pos);
            starvedCount.fetch_add ((juce::uint64) gap, std::memory_order_relaxed);
            readerPosition = pos + gap;
            out += gap;
            continue;
        }

        const int offset = (int) (pos - block.start);
        const int available = juce::jmin (want, block.numSamples - offset);
        juce::FloatVectorOperations::copy (outL + out, block.left.data() + offset, available);
        juce::FloatVectorOperations::copy (outR + out, block.right.data() + offset, available);
        readerPosition = pos + available;
        out += available;

        if (offset + available == block.numSamples)
            readIndex.store (ri + 1, std::memory_order_release);
    }

    playheadSample.store (readerPosition);
    appliedGeneration.store (readerGeneration, std::memory_order_release);
}

void ReadAheadStream::discardStale()
{
    adoptSeekForReader();

    // 旧世代ブロックを先頭から読み捨てる（停止中でもリングの空きを戻し、
    // 次の再生開始が枯渇から始まらないようにする）
    while (true)
    {
        const auto ri = readIndex.load (std::memory_order_relaxed);
        if (writeIndex.load (std::memory_order_acquire) == ri)
            break;
        if (blocks[ri % numBlocks].generation == readerGeneration)
            break;
        readIndex.store (ri + 1, std::memory_order_release);
    }

    playheadSample.store (readerPosition);
    appliedGeneration.store (readerGeneration, std::memory_order_release);
}

juce::int64 ReadAheadStream::uiPosition() const
{
    // 未適用のシークがあればその位置を返す（適用後の結果と、どちらを読んでも新しい側になる）
    if (seekGeneration.load (std::memory_order_acquire) != appliedGeneration.load (std::memory_order_acquire))
        return seekPosition.load (std::memory_order_relaxed);
    return playheadSample.load();
}
