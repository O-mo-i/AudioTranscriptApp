#include "TranscriptEditor.h"

TranscriptEditor::TranscriptEditor()
{
    setMultiLine(true);
    setReadOnly(false);
    setScrollbarsShown(true);
    setFont(juce::Font("Microsoft YaHei", 18.0f, juce::Font::plain));

    setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF2a2a3e));
    setColour(juce::TextEditor::textColourId, juce::Colours::white);
    setColour(juce::TextEditor::highlightColourId, juce::Colour(0x804a4a8e));
    setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xFF4a4a8e));

    setIndents(10, 10);

    startTimer(100); // Poll caret position every 100ms
}

TranscriptEditor::~TranscriptEditor()
{
    stopTimer();
}

void TranscriptEditor::setTimestamps(const std::vector<CharacterTimestamp>* timestampsPtr)
{
    timestamps = timestampsPtr;
}

void TranscriptEditor::highlightByTime(double timeInSeconds)
{
    if (timestamps == nullptr)
        return;

    for (const auto& ts : *timestamps)
    {
        if (timeInSeconds >= ts.startTime && timeInSeconds < ts.endTime)
        {
            programmaticChange = true;

            setCaretPosition(ts.globalTextIndex);
            setHighlightedRegion(juce::Range<int>(ts.globalTextIndex,
                                                   ts.globalTextIndex + ts.character.length()));

            scrollEditorToPositionCaret(0, 0);

            lastKnownCaretPos = getCaretCharIndex();
            programmaticChange = false;
            return;
        }
    }
}

int TranscriptEditor::getCaretCharIndex() const
{
    return getCaretPosition();
}

void TranscriptEditor::timerCallback()
{
    if (programmaticChange)
        return;

    int currentPos = getCaretCharIndex();
    if (currentPos != lastKnownCaretPos)
    {
        lastKnownCaretPos = currentPos;
        if (onCaretMoved)
            onCaretMoved(currentPos);
    }
}
