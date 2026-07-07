#include "VirtualTranscriptComponent.h"

//==============================================================================
VirtualTranscriptComponent::VirtualTranscriptComponent()
    : canvas(*this), viewport(*this)
{
    viewport.setViewedComponent(&canvas, false);
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);

    setWantsKeyboardFocus(true);
}

VirtualTranscriptComponent::~VirtualTranscriptComponent() = default;

//==============================================================================
void VirtualTranscriptComponent::setTimestamps(const std::vector<CharacterTimestamp>* timestampsPtr)
{
    timestamps = timestampsPtr;
    lastHighlightedIndex = -1;
}

void VirtualTranscriptComponent::setParagraphTimestamps(const std::vector<ParagraphTimestamp>& pts)
{
    paragraphTimestamps = pts;
    // 只更新已有显示行的 paragraphTsIndex，不需要重建所有行
    for (auto& dl : displayLines)
    {
        dl.paragraphTsIndex = -1;
        for (int i = 0; i < (int)paragraphTimestamps.size(); ++i)
        {
            if (paragraphTimestamps[i].firstCharIndex == dl.startCharIndex)
            {
                dl.paragraphTsIndex = i;
                break;
            }
        }
    }
    canvas.repaint();
}

void VirtualTranscriptComponent::setTimeOffset(double offset)
{
    if (std::abs(offset - timeOffset) > 0.001)
    {
        timeOffset = offset;
        canvas.repaint();
    }
}

void VirtualTranscriptComponent::setCaretPosition(int idx)
{
    clickedCharIndex = idx;
    caretVisible = true;
    canvas.repaint();
}

void VirtualTranscriptComponent::setText(const juce::String& text, juce::NotificationType)
{
    fullText = text;
    rebuildDisplayLines();
    clickedCharIndex = 0;
    lastHighlightedIndex = -1;
    highlightTime = -1.0;
    selectionStart = selectionEnd = -1;
}

void VirtualTranscriptComponent::clear()
{
    fullText.clear();
    displayLines.clear();
    timestamps = nullptr;
    paragraphTimestamps.clear();
    clickedCharIndex = 0;
    lastHighlightedIndex = -1;
    highlightTime = -1.0;
    selectionStart = selectionEnd = -1;
    canvas.setSize(0, 0);
    canvas.repaint();
}

//==============================================================================
//  播放高亮
//
//  策略：找到 startTime <= timeInSeconds 的最后一个字（即"当前正在播放的位置"），
//  高亮会持续到下一个字开始，不会在字间间隙消失。
//==============================================================================
void VirtualTranscriptComponent::highlightByTime(double timeInSeconds)
{
    highlightTime = timeInSeconds;
    if (timestamps == nullptr) return;

    int bestIdx = -1;
    double bestStart = -1.0;

    for (const auto& ts : *timestamps)
    {
        if (ts.startTime <= timeInSeconds && ts.startTime > bestStart)
        {
            bestStart = ts.startTime;
            bestIdx = ts.globalTextIndex;
        }
    }

    if (bestIdx >= 0 && bestIdx != lastHighlightedIndex)
    {
        lastHighlightedIndex = bestIdx;
        canvas.repaint();
    }
}

//==============================================================================
//  重建显示行（按宽度折行）
//==============================================================================
void VirtualTranscriptComponent::rebuildDisplayLines()
{
    displayLines.clear();
    if (fullText.isEmpty())
    {
        canvas.setSize(0, 0);
        return;
    }

    int availableWidth = getWidth() - textLeftMargin - 10;
    if (availableWidth < 50) availableWidth = 50;

    auto& paraTs = paragraphTimestamps;

    // 按换行符切分段落
    int lineStart = 0;
    int globalCharIdx = 0;
    int textLen = fullText.length();

    while (lineStart < textLen)
    {
        // 找下一个换行符
        int newlinePos = fullText.indexOf(lineStart, "\n");
        if (newlinePos < 0) newlinePos = textLen;

        juce::String paragraph = fullText.substring(lineStart, newlinePos);
        int paraFirstChar = globalCharIdx;

        // 查找此段落对应的时间戳索引
        int tsIdx = -1;
        for (int i = 0; i < (int)paraTs.size(); ++i)
        {
            if (paraTs[i].firstCharIndex == paraFirstChar)
            {
                tsIdx = i;
                break;
            }
        }

        if (font.getStringWidth(paragraph) <= availableWidth)
        {
            // 一行即可显示
            displayLines.push_back({ paragraph, paraFirstChar, tsIdx });
        }
        else
        {
            // 需要折行
            int pos = 0;
            bool firstSubLine = true;
            while (pos < paragraph.length())
            {
                // 贪心：从 pos 开始累积字符直到超过宽度
                float w = 0.0f;
                int end = pos;
                int lastBreak = -1;

                while (end < paragraph.length())
                {
                    juce::String ch = paragraph.substring(end, end + 1);
                    w += font.getStringWidth(ch);
                    if (w > (float)availableWidth && end > pos)
                        break;

                    if (ch == " " || ch == "　")
                        lastBreak = end;

                    ++end;
                }

                // 如果这一行有空格分隔点，且不是第一行，在空格后折行
                int breakAt = end;
                if (lastBreak >= pos && lastBreak < end - 1)
                    breakAt = lastBreak + 1;

                // 防止死循环：如果 breakAt == pos，至少前进一个字符
                if (breakAt <= pos)
                    breakAt = pos + 1;

                juce::String subLine = paragraph.substring(pos, breakAt);
                displayLines.push_back({ subLine, paraFirstChar + pos,
                                         firstSubLine ? tsIdx : -1 });
                firstSubLine = false;
                pos = breakAt;
            }
        }

        // 跳过换行符，更新全局字符索引
        globalCharIdx += paragraph.length();
        lineStart = newlinePos;
        if (lineStart < textLen && fullText[lineStart] == '\n')
        {
            ++lineStart;          // 跳过 \n
            ++globalCharIdx;      // 换行符也占 globalTextIndex
        }
    }

    // 更新画布尺寸（虚拟文档高度）
    int totalHeight = (int)displayLines.size() * lineHeight + lineHeight / 2;
    canvas.setSize(juce::jmax(getWidth(), 100), totalHeight);
    canvas.repaint();
}

//==============================================================================
//  Canvas 绘制——只绘制视口内的行
//==============================================================================
void VirtualTranscriptComponent::Canvas::paint(juce::Graphics& g)
{
    auto& vt = owner;
    if (vt.displayLines.empty())
    {
        g.fillAll(juce::Colour(0xFF2a2a3e));
        return;
    }

    auto clipBounds = g.getClipBounds();
    int firstVis = juce::jmax(0, clipBounds.getY() / vt.lineHeight);
    int lastVis  = juce::jmin((int)vt.displayLines.size() - 1,
                               clipBounds.getBottom() / vt.lineHeight);

    // 背景
    g.fillAll(juce::Colour(0xFF2a2a3e));

    // 计算当前高亮区间的字符范围（逻辑与 highlightByTime 保持一致）
    int hlStart = -1, hlEnd = -1;
    if (vt.highlightTime >= 0.0 && vt.timestamps)
    {
        double bestStart = -1.0;
        for (const auto& ts : *vt.timestamps)
        {
            if (vt.highlightTime >= ts.startTime && ts.startTime > bestStart)
            {
                bestStart = ts.startTime;
                hlStart = ts.globalTextIndex;
                hlEnd   = ts.globalTextIndex + (int)ts.character.length();
            }
        }
    }

    // 时间戳字体
    juce::Font tsFont("Microsoft YaHei", 14.0f, juce::Font::plain);
    juce::Font textFont("Microsoft YaHei", 18.0f, juce::Font::plain);

    for (int i = firstVis; i <= lastVis; ++i)
    {
        const auto& line = vt.displayLines[i];
        int y = i * vt.lineHeight;
        int x = vt.textLeftMargin;

        //── 左侧段落时间戳 ──────────────────
        if (line.paragraphTsIndex >= 0)
        {
            const auto& pt = vt.paragraphTimestamps[line.paragraphTsIndex];
            double globalTime = pt.timeSeconds + vt.timeOffset;
            auto timeStr = juce::String::formatted("%02d:%02d",
                (int)(globalTime / 60.0), ((int)globalTime) % 60);

            g.setFont(tsFont);
            g.setColour(juce::Colour(0xFF888888));
            g.drawText(timeStr, 0, y, vt.textLeftMargin - 8, vt.lineHeight,
                       juce::Justification::centredRight, false);
        }

        //── 高亮背景 ──────────────────────
        g.setFont(textFont);
        if (hlStart >= 0 && hlEnd > hlStart)
        {
            int lineStart = line.startCharIndex;
            int lineEnd   = lineStart + line.text.length();

            if (hlStart < lineEnd && hlEnd > lineStart)
            {
                int localStart = juce::jmax(0, hlStart - lineStart);
                int localEnd   = juce::jmin((int)line.text.length(), hlEnd - lineStart);

                float hx = x + textFont.getStringWidth(line.text.substring(0, localStart));
                float hw = textFont.getStringWidth(line.text.substring(localStart, localEnd));

                g.setColour(juce::Colour(0x50FFFFFF));
                g.fillRect(hx, (float)y, hw, (float)vt.lineHeight);
            }
        }

        //── 文字选中背景（用户拖选）─────────
        if (vt.selectionStart >= 0 && vt.selectionEnd >= 0
            && vt.selectionStart != vt.selectionEnd)
        {
            int selLo = juce::jmin(vt.selectionStart, vt.selectionEnd);
            int selHi = juce::jmax(vt.selectionStart, vt.selectionEnd);
            int lineStart = line.startCharIndex;
            int lineEnd   = lineStart + line.text.length();

            if (selLo < lineEnd && selHi > lineStart)
            {
                int localStart = juce::jmax(0, selLo - lineStart);
                int localEnd   = juce::jmin((int)line.text.length(), selHi - lineStart);

                float sx = x + textFont.getStringWidth(line.text.substring(0, localStart));
                float sw = textFont.getStringWidth(line.text.substring(localStart, localEnd));

                g.setColour(juce::Colour(0x804a4a8e));
                g.fillRect(sx, (float)y, sw, (float)vt.lineHeight);
            }
        }

        //── 文本 ──────────────────────────
        g.setColour(juce::Colours::white);
        g.drawText(line.text, x, y, clipBounds.getWidth() - x, vt.lineHeight,
                   juce::Justification::centredLeft, true);

        //── 光标（当前点击位置）────────────
        if (vt.caretVisible && vt.hasKeyboardFocus(true)
            && vt.clickedCharIndex >= line.startCharIndex
            && vt.clickedCharIndex < line.startCharIndex + line.text.length())
        {
            int localIdx = vt.clickedCharIndex - line.startCharIndex;
            float cx = x + textFont.getStringWidth(line.text.substring(0, localIdx));
            g.setColour(juce::Colours::white);
            g.fillRect(cx, (float)y + 2.0f, 1.5f, (float)(vt.lineHeight - 4));
        }
    }
}

//==============================================================================
//  鼠标坐标 → 字符索引（坐标相对于 Canvas）
//==============================================================================
int VirtualTranscriptComponent::hitTestCharIndex(int canvasX, int canvasY) const
{
    int lineIdx = canvasY / lineHeight;
    if (lineIdx < 0 || lineIdx >= (int)displayLines.size())
        return (lineIdx < 0) ? 0 : fullText.length();

    const auto& line = displayLines[lineIdx];
    float clickX = (float)(canvasX - textLeftMargin);
    float accumulated = 0.0f;

    for (int i = 0; i < line.text.length(); ++i)
    {
        float charWidth = font.getStringWidth(line.text.substring(i, i + 1));
        if (clickX <= accumulated + charWidth / 2)
            return line.startCharIndex + i;
        accumulated += charWidth;
    }
    return line.startCharIndex + line.text.length();
}

//==============================================================================
//  鼠标事件（由 Canvas 转发，坐标相对于 Canvas）
//==============================================================================
void VirtualTranscriptComponent::onCanvasMouseDown(const juce::MouseEvent& event)
{
    charIndexBeforeMouseDown = clickedCharIndex;
    grabKeyboardFocus();

    int idx = hitTestCharIndex(event.x, event.y);
    clickedCharIndex = idx;
    selectionStart = selectionEnd = idx;
    caretVisible = true;
    canvas.repaint();
}

void VirtualTranscriptComponent::onCanvasMouseDrag(const juce::MouseEvent& event)
{
    if (selectionStart < 0)  // 没有起始锚点则不操作
        return;

    int idx = hitTestCharIndex(event.x, event.y);
    clickedCharIndex = idx;
    selectionEnd = idx;
    caretVisible = true;
    canvas.repaint();
}

void VirtualTranscriptComponent::onCanvasMouseUp(const juce::MouseEvent&)
{
    if (clickedCharIndex != charIndexBeforeMouseDown)
    {
        if (onCaretMoved)
            onCaretMoved(clickedCharIndex);
    }
}

//==============================================================================
//  键盘事件
//==============================================================================
bool VirtualTranscriptComponent::keyPressed(const juce::KeyPress& key)
{
    // Ctrl+C / Cmd+C 复制选中文字（剥离时间戳）
    if ((key.getModifiers().isCtrlDown() || key.getModifiers().isCommandDown())
        && key.getKeyCode() == 'C')
    {
        if (selectionStart >= 0 && selectionEnd >= 0 && selectionStart != selectionEnd)
        {
            int lo = juce::jmin(selectionStart, selectionEnd);
            int hi = juce::jmax(selectionStart, selectionEnd);
            juce::String selected = fullText.substring(lo, hi);

            // 剥离每行开头的时间戳 "MM:SS "
            juce::String clean;
            juce::StringArray lines = juce::StringArray::fromLines(selected);
            for (int l = 0; l < lines.size(); ++l)
            {
                auto line = lines[l].trim();
                // 去掉行首的 "MM:SS " 模式（6 个字符：2位分+冒号+2位秒+空格）
                if (line.length() > 6
                    && line[2] == ':'
                    && line[5] == ' ')
                    line = line.substring(6);

                if (l > 0) clean += "\n";
                clean += line;
            }

            juce::SystemClipboard::copyTextToClipboard(clean);
        }
        return true;
    }

    if (key == juce::KeyPress::spaceKey)
    {
        if (onSpacePressed && onSpacePressed())
            return true;
        return true;
    }
    return false;
}

//==============================================================================
//  焦点
//==============================================================================
void VirtualTranscriptComponent::focusGained(FocusChangeType)
{
    caretVisible = true;
    canvas.repaint();
}

void VirtualTranscriptComponent::focusLost(FocusChangeType)
{
    caretVisible = false;
    canvas.repaint();
}

//==============================================================================
//  Resized
//==============================================================================
void VirtualTranscriptComponent::resized()
{
    viewport.setBounds(getLocalBounds());
    rebuildDisplayLines();

    // 恢复滚动位置
    if (clickedCharIndex > 0 && !displayLines.empty())
    {
        for (size_t i = 0; i < displayLines.size(); ++i)
        {
            if (displayLines[i].startCharIndex + (int)displayLines[i].text.length() > clickedCharIndex)
            {
                int targetY = (int)i * lineHeight;
                auto curPos = viewport.getViewPosition();
                if (targetY < curPos.y || targetY > curPos.y + getHeight())
                    viewport.setViewPosition(curPos.x, targetY);
                break;
            }
        }
    }
}
