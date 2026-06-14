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
    std::function<bool()> onSpacePressed;

private:
    void timerCallback() override;
    bool keyPressed(const juce::KeyPress& key) override;

    const std::vector<CharacterTimestamp>* timestamps = nullptr;
    int lastKnownCaretPos = -1;
    bool programmaticChange = false;

    /** 上次高亮过的 globalTextIndex（用于判断是否跨行） */
    int lastHighlightedIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TranscriptEditor)
};
