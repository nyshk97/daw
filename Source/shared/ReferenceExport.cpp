#include "ReferenceExport.h"

#include "ReferenceAlign.h"

namespace ReferenceExport
{
juce::String sanitizeName (const juce::String& raw)
{
    // パス区切りと Finder/シェルで事故りやすい文字を落とし、前後の空白とドットを刈る
    // （先頭ドットは隠しフォルダになる）。制御文字も除去
    juce::String cleaned;
    for (auto c : raw)
    {
        if (c < 0x20 || juce::String ("/\\:*?\"<>|").containsChar (c))
            continue;
        cleaned += c;
    }
    cleaned = cleaned.trim();
    while (cleaned.startsWithChar ('.'))
        cleaned = cleaned.substring (1).trim();
    // フォルダ名としての常識的な長さに切る（表示とパスの取り回し。日本語でも80文字あれば十分）
    if (cleaned.length() > 80)
        cleaned = cleaned.substring (0, 80).trim();
    return cleaned.isEmpty() ? "reference" : cleaned;
}

juce::File allocateFolder (const juce::File& projectDir, const juce::String& rawName)
{
    const auto base = sanitizeName (rawName);
    const auto references = projectDir.getChildFile ("references");
    auto candidate = references.getChildFile (base);
    for (int i = 2; candidate.exists() && i < 10000; ++i)
        candidate = references.getChildFile (base + "-" + juce::String (i));
    return candidate;
}

bool exportClipRange (const juce::File& projectDir, const Clip& clip,
                      const juce::File& folder, juce::String& error)
{
    const auto source = projectDir.getChildFile (clip.fileName);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));
    if (reader == nullptr)
    {
        error = juce::String::fromUTF8 (u8"ソースWAVを読めません: ") + source.getFullPathName();
        return false;
    }

    // モデルの不変条件（0 <= offset / offset+length <= 全長）は読込時に保証されているが、
    // 手編集JSON経由の値でも範囲外読みしないよう最終防衛線を掛ける
    const auto offset = juce::jlimit ((juce::int64) 0, reader->lengthInSamples, clip.offsetSamples);
    const auto length = juce::jlimit ((juce::int64) 0, reader->lengthInSamples - offset,
                                      clip.lengthSamples);
    if (length <= 0)
    {
        error = juce::String::fromUTF8 (u8"リージョンの範囲が空です");
        return false;
    }

    if (! folder.createDirectory())
    {
        error = juce::String::fromUTF8 (u8"フォルダを作成できません: ") + folder.getFullPathName();
        return false;
    }

    const auto target = folder.getChildFile ("track.wav");
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (target.createOutputStream());
    if (stream == nullptr)
    {
        error = juce::String::fromUTF8 (u8"track.wav を作成できません");
        return false;
    }
    using Opts = juce::AudioFormatWriterOptions;
    auto writer = wav.createWriterFor (stream,
        Opts{}.withSampleRate (reader->sampleRate)
              .withNumChannels ((int) reader->numChannels)
              .withBitsPerSample ((int) juce::jmax (16u, reader->bitsPerSample)));
    if (writer == nullptr)
    {
        error = juce::String::fromUTF8 (u8"track.wav のライターを作成できません");
        return false;
    }

    if (! writer->writeFromAudioReader (*reader, offset, length))
    {
        writer.reset();
        error = juce::String::fromUTF8 (u8"track.wav の書き込みに失敗しました");
        return false;
    }

    // 元クリップの同定メモ（後からの「原曲を頭出し」用）。分析パイプラインはこのファイルを無視する。
    // 書けなくても分析自体は成立するので失敗にはしない
    ReferenceAlign::writeSourceDescriptor (folder,
                                           { clip.fileName, clip.offsetSamples, clip.lengthSamples });
    return true;
}
}
