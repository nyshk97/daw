#pragma once

#include <vector>
#include <juce_core/juce_core.h>

namespace AudioBrowserNavigation
{
inline juce::File initialDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
        .getChildFile ("Downloads");
}

class History
{
public:
    void visit (const juce::File& directory)
    {
        if (index + 1 < (int) entries.size())
            entries.erase (entries.begin() + index + 1, entries.end());
        entries.push_back (directory);
        index = (int) entries.size() - 1;
    }

    bool canMove (int delta) const
    {
        const int next = index + delta;
        return next >= 0 && next < (int) entries.size();
    }

    juce::File move (int delta)
    {
        if (! canMove (delta))
            return {};
        index += delta;
        return entries[(size_t) index];
    }

private:
    std::vector<juce::File> entries;
    int index = -1;
};
}
