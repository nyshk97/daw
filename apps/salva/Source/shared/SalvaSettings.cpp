#include "SalvaSettings.h"

juce::File SalvaSettings::defaultFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support/salva/settings.json");
}

SalvaSettings SalvaSettings::load (const juce::File& file)
{
    SalvaSettings s;
    const auto parsed = juce::JSON::parse (file.loadFileAsString());
    if (auto* obj = parsed.getDynamicObject())
    {
        s.outputDeviceName = obj->getProperty ("outputDevice").toString();
        s.inputDeviceName = obj->getProperty ("inputDevice").toString();
        if (obj->hasProperty ("inputChannelPairStart"))
            s.inputChannelPairStart = (int) obj->getProperty ("inputChannelPairStart");
        s.recordDirectory = obj->getProperty ("recordDirectory").toString();
        s.exportDirectory = obj->getProperty ("exportDirectory").toString();
        s.venvPathOverride = obj->getProperty ("venvPathOverride").toString();
        if (auto* arr = obj->getProperty ("recentFiles").getArray())
            for (const auto& v : *arr)
                if (s.recentFiles.size() < maxRecentFiles && v.toString().isNotEmpty())
                    s.recentFiles.add (v.toString());
    }
    return s;
}

void SalvaSettings::save (const juce::File& file) const
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("outputDevice", outputDeviceName);
    obj->setProperty ("inputDevice", inputDeviceName);
    obj->setProperty ("inputChannelPairStart", inputChannelPairStart);
    obj->setProperty ("recordDirectory", recordDirectory);
    obj->setProperty ("exportDirectory", exportDirectory);
    obj->setProperty ("venvPathOverride", venvPathOverride);
    juce::Array<juce::var> arr;
    for (const auto& p : recentFiles)
        arr.add (p);
    obj->setProperty ("recentFiles", arr);

    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (juce::var (obj)), false, false, "\n");
}

void SalvaSettings::addRecentFile (const juce::String& path)
{
    recentFiles.removeString (path);
    recentFiles.insert (0, path);
    while (recentFiles.size() > maxRecentFiles)
        recentFiles.remove (recentFiles.size() - 1);
}
