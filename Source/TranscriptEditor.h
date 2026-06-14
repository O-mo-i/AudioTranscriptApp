#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include "CharacterTimestamp.h"

class TranscriptEditor : public juce::TextEditor,
                         private juce::Timer
{
public:
    TranscriptEditor();
    ~TranscriptEditor() override;

    void setTimestamps(const std::vector<CharacterTimestamp>* timestampsPtr);
    void highlightByTime(double timeInSeconds);
    int getCaretCharIndex() const;

    std::function<void(int charIndex)> onCaretMoved;

private:
    void timerCallback() override;

    const std::vector<CharacterTimestamp>* timestamps = nullptr;
    int lastKnownCaretPos = -1;
    bool programmaticChange = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TranscriptEditor)
};
