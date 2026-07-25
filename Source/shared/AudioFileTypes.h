#pragma once

#include <juce_core/juce_core.h>

namespace AudioFileTypes
{
inline bool isSupported (const juce::File& file)
{
    return file.hasFileExtension ("wav;aif;aiff;flac;mp3;m4a");
}

inline bool isSupported (const juce::String& path)
{
    return isSupported (juce::File (path));
}
}
