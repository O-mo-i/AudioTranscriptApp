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
}

TranscriptEditor::~TranscriptEditor() = default;

void TranscriptEditor::setTimestamps(const std::vector<CharacterTimestamp>* timestampsPtr)
{
    timestamps = timestampsPtr;
    lastHighlightedIndex = -1;
}

void TranscriptEditor::setParagraphTimestamps(const std::vector<ParagraphTimestamp>& pts)
{
    paragraphTimestamps = pts;
    rebuildTimestampCache();
    repaint();
}

void TranscriptEditor::setTimeOffset(double offset)
{
    if (std::abs(offset - timeOffset) > 0.001)
    {
        timeOffset = offset;
        rebuildTimestampCache();
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

    // 再在左侧绘制段落时间戳（使用缓存，不调用 getCaretRectangleForCharIndex）
    if (timestampDisplayCache.empty())
        return;

    g.setFont(juce::Font("Microsoft YaHei", 14.0f, juce::Font::plain));
    g.setColour(juce::Colour(0xFF888888));

    for (const auto& cached : timestampDisplayCache)
    {
        if (cached.charBounds.isEmpty())
            continue;

        // 在段落首字左侧绘制时间戳，垂直居中
        g.drawFittedText(cached.timeStr,
            cached.charBounds.getX() - 62, cached.charBounds.getY(),
            56, cached.charBounds.getHeight(),
            juce::Justification::centredRight, 1);
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

//==============================================================================
//  鼠标事件：仅当用户真实点击导致光标变化时触发 onCaretMoved
//  避免 setText 等程序性操作引发光标归零导致的错误同步
//==============================================================================
void TranscriptEditor::mouseDown(const juce::MouseEvent& event)
{
    caretPosBeforeMouseDown = getCaretPosition();
    juce::TextEditor::mouseDown(event);
}

void TranscriptEditor::mouseUp(const juce::MouseEvent& event)
{
    juce::TextEditor::mouseUp(event);

    int newPos = getCaretPosition();
    if (newPos != caretPosBeforeMouseDown)
    {
        if (onCaretMoved)
            onCaretMoved(newPos);
    }
}

//==============================================================================
//  视口变化：重新缓存可见段落的时间戳屏幕位置
//==============================================================================
void TranscriptEditor::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    juce::TextEditor::mouseWheelMove(event, wheel);
    rebuildTimestampCache();
}

void TranscriptEditor::resized()
{
    juce::TextEditor::resized();
    rebuildTimestampCache();
}

//==============================================================================
//  重建时间戳显示缓存
//  一次性计算所有段落时间戳的屏幕坐标，避免 paint 中重复计算
//==============================================================================
void TranscriptEditor::rebuildTimestampCache()
{
    timestampDisplayCache.clear();
    timestampDisplayCache.reserve(paragraphTimestamps.size());

    for (const auto& pt : paragraphTimestamps)
    {
        auto charBounds = getCaretRectangleForCharIndex(pt.firstCharIndex);

        auto globalTime = pt.timeSeconds + timeOffset;
        auto timeStr = juce::String::formatted("%02d:%02d",
            (int)(globalTime / 60.0), ((int)globalTime) % 60);

        timestampDisplayCache.push_back({charBounds, timeStr});
    }
}
