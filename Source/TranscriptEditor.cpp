#include "TranscriptEditor.h"

TranscriptEditor::TranscriptEditor()
{
    setMultiLine(true);
    setReadOnly(true);
    setCaretVisible(true);
    setScrollbarsShown(true);
    setFont(juce::Font("Microsoft YaHei", 18.0f, juce::Font::plain));

    setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF2a2a3e));
    setColour(juce::TextEditor::textColourId, juce::Colours::white);
    setColour(juce::TextEditor::highlightColourId, juce::Colour(0x804a4a8e));
    setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xFF4a4a8e));

    setIndents(70, 10);  // 左侧留出时间戳空间

    startTimer(100);
}

TranscriptEditor::~TranscriptEditor()
{
    stopTimer();
}

void TranscriptEditor::setTimestamps(const std::vector<CharacterTimestamp>* timestampsPtr)
{
    timestamps = timestampsPtr;
    lastHighlightedIndex = -1;
}

void TranscriptEditor::setParagraphTimestamps(const std::vector<ParagraphTimestamp>& pts)
{
    paragraphTimestamps = pts;
    repaint();
}

void TranscriptEditor::setTimeOffset(double offset)
{
    if (std::abs(offset - timeOffset) > 0.001)
    {
        timeOffset = offset;
        repaint();
    }
}

//==============================================================================
//  播放高亮
//
//  核心策略：
//    - 用 setHighlightedRegion 做字级高亮（不依赖光标驱动）
//    - 利用 setScrollToShowCursor 控制 JUCE 内部自动滚动：
//        * 同一行内移动 → 禁止自动滚动，视图绝对静止
//        * 跨行时 → 允许自动滚动，自然跟随
//    - 不调用任何外部 scroll* 方法
//==============================================================================
void TranscriptEditor::highlightByTime(double timeInSeconds)
{
    if (timestamps == nullptr)
        return;

    for (const auto& ts : *timestamps)
    {
        if (timeInSeconds >= ts.startTime && timeInSeconds < ts.endTime)
        {
            const int startIdx = ts.globalTextIndex;
            const int endIdx   = ts.globalTextIndex + (int)ts.character.length();

            // ── 判断是否跨行：在最后索引到新索引之间找 \n ──
            bool crossedLine = (lastHighlightedIndex < 0);
            if (!crossedLine)
            {
                const auto& text = getText();
                int lo = juce::jmin(lastHighlightedIndex, startIdx);
                int hi = juce::jmax(lastHighlightedIndex, startIdx);
                for (int i = lo; i < hi; ++i)
                {
                    if (text[i] == '\n') { crossedLine = true; break; }
                }
            }

            programmaticChange = true;

            if (crossedLine)
            {
                // 跨行/跨段 → 允许自动滚动
                setScrollToShowCursor(true);
                setHighlightedRegion(juce::Range<int>(startIdx, endIdx));
            }
            else
            {
                // 同一行 → 临时关闭自动滚动，更新高亮后恢复
                setScrollToShowCursor(false);
                setHighlightedRegion(juce::Range<int>(startIdx, endIdx));
                setScrollToShowCursor(true);
            }

            lastHighlightedIndex = startIdx;
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

void TranscriptEditor::paint(juce::Graphics& g)
{
    // 先让 TextEditor 绘制文本
    juce::TextEditor::paint(g);

    // 再在左侧绘制段落时间戳
    if (paragraphTimestamps.empty())
        return;

    g.setFont(juce::Font("Microsoft YaHei", 14.0f, juce::Font::plain));
    g.setColour(juce::Colour(0xFF888888));

    for (const auto& pt : paragraphTimestamps)
    {
        auto charBounds = getCaretRectangleForCharIndex(pt.firstCharIndex);
        if (charBounds.isEmpty())
            continue;

        auto globalTime = pt.timeSeconds + timeOffset;
        auto timeStr = juce::String::formatted("%02d:%02d",
            (int)(globalTime / 60.0), ((int)globalTime) % 60);

        // 在段落首字左侧绘制时间戳，垂直居中
        g.drawFittedText(timeStr,
            charBounds.getX() - 62, charBounds.getY(),
            56, charBounds.getHeight(),
            juce::Justification::centredRight, 1);
    }
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

bool TranscriptEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        if (onSpacePressed && onSpacePressed())
            return true;
        return true;
    }

    return juce::TextEditor::keyPressed(key);
}
