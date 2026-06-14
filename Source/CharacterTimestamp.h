#pragma once
#include <juce_core/juce_core.h>

struct CharacterTimestamp
{
    juce::String character;
    double startTime = 0.0;
    double endTime = 0.0;
    int globalTextIndex = 0;
    bool isParagraphStart = false;
};
