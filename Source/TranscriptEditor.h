#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include "CharacterTimestamp.h"

class TranscriptEditor : public juce::TextEditor
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
    bool keyPressed(const juce::KeyPress& key) override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void resized() override;

    /** 重建段落时间戳显示缓存（内容变更或视口滚动后调用） */
    void rebuildTimestampCache();

    const std::vector<CharacterTimestamp>* timestamps = nullptr;
    std::vector<ParagraphTimestamp> paragraphTimestamps;
    double timeOffset{ 0.0 };

    /** 上次高亮过的 globalTextIndex（用于判断是否跨行） */
    int lastHighlightedIndex = -1;

    /** 鼠标按下前的光标位置，用于判断点击是否真的改变了光标 */
    int caretPosBeforeMouseDown = -1;

    /** 段落时间戳显示缓存：避免 paint 中频繁调用 getCaretRectangleForCharIndex */
    struct TimestampDisplayCache
    {
        juce::Rectangle<int> charBounds;
        juce::String timeStr;
    };
    std::vector<TimestampDisplayCache> timestampDisplayCache;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TranscriptEditor)
};
