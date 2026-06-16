#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include "CharacterTimestamp.h"

class TranscriptEditor : public juce::TextEditor,
                         private juce::Timer
{
public:
    /** 段落时间戳：显示在段落左侧，不参与文本内容 */
    struct ParagraphTimestamp
    {
        int firstCharIndex;  // 段落首字在 TextEditor 中的 index
        double timeSeconds;  // 起始时间（秒）
    };

    TranscriptEditor();
    ~TranscriptEditor() override;

    void setTimestamps(const std::vector<CharacterTimestamp>* timestampsPtr);
    void setParagraphTimestamps(const std::vector<ParagraphTimestamp>& pts);
    void setTimeOffset(double offset);
    void highlightByTime(double timeInSeconds);
    int getCaretCharIndex() const;

    std::function<void(int charIndex)> onCaretMoved;
    std::function<bool()> onSpacePressed;

private:
    void timerCallback() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void paint(juce::Graphics& g) override;

    const std::vector<CharacterTimestamp>* timestamps = nullptr;
    std::vector<ParagraphTimestamp> paragraphTimestamps;
    double timeOffset{ 0.0 };
    int lastKnownCaretPos = -1;
    bool programmaticChange = false;

    /** 上次高亮过的 globalTextIndex（用于判断是否跨行） */
    int lastHighlightedIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TranscriptEditor)
};
