#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include "CharacterTimestamp.h"

/**
 * 虚拟化转录文本显示组件。
 *
 * 替换原来的 TranscriptEditor（基于 juce::TextEditor），
 * 采用虚拟化渲染方案：只绘制视口内可见的行，
 * 文本总长度不影响滚动和绘制性能。
 */
class VirtualTranscriptComponent : public juce::Component
{
public:
    /** 段落时间戳：显示在段落左侧，不参与文本内容 */
    struct ParagraphTimestamp
    {
        int firstCharIndex;
        double timeSeconds;
    };

    VirtualTranscriptComponent();
    ~VirtualTranscriptComponent() override;

    void setTimestamps(const std::vector<CharacterTimestamp>* timestampsPtr);
    void setParagraphTimestamps(const std::vector<ParagraphTimestamp>& pts);
    void setTimeOffset(double offset);
    void highlightByTime(double timeInSeconds);
    int getCaretCharIndex() const { return clickedCharIndex; }
    void setCaretPosition(int idx);
    void setText(const juce::String& text, juce::NotificationType = juce::dontSendNotification);
    void clear();

    std::function<void(int charIndex)> onCaretMoved;
    std::function<bool()> onSpacePressed;
    std::function<void()> onSearchRequested;

    //── 搜索 API ────────────────────────────
    void searchText(const juce::String& keyword);
    void goToNextMatch();
    void goToPrevMatch();
    void clearSearch();
    int getSearchMatchCount() const { return (int)searchMatches.size(); }
    bool hasSearchResults() const { return !searchMatches.empty(); }
    int getCurrentSearchMatchIndex() const { return currentSearchMatch; }

private:
    //── 内部画布：只绘制视口内可见的行 ──────────
    class Canvas : public juce::Component
    {
    public:
        Canvas(VirtualTranscriptComponent& owner) : owner(owner)
        {
            setMouseCursor(juce::MouseCursor::IBeamCursor);
        }
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& event) override { owner.onCanvasMouseDown(event); }
        void mouseUp(const juce::MouseEvent& event) override   { owner.onCanvasMouseUp(event); }
        void mouseDrag(const juce::MouseEvent& event) override { owner.onCanvasMouseDrag(event); }
    private:
        VirtualTranscriptComponent& owner;
    };
    friend class TranscriptViewport;

    /** 自定义 Viewport：滚动时通知 Canvas 重绘 */
    class TranscriptViewport : public juce::Viewport
    {
    public:
        explicit TranscriptViewport(VirtualTranscriptComponent& owner) : owner(owner) {}
        void visibleAreaChanged(const juce::Rectangle<int>&) override
        {
            owner.canvas.repaint();
        }
    private:
        VirtualTranscriptComponent& owner;
    };

    bool keyPressed(const juce::KeyPress& key) override;
    void resized() override;
    void focusGained(FocusChangeType cause) override;
    void focusLost(FocusChangeType cause) override;

    /** Canvas 转发上来的鼠标事件（坐标相对于 Canvas） */
    void onCanvasMouseDown(const juce::MouseEvent& event);
    void onCanvasMouseDrag(const juce::MouseEvent& event);
    void onCanvasMouseUp(const juce::MouseEvent& event);

    /** 将鼠标坐标（相对于 Canvas）转换为字符索引 */
    int hitTestCharIndex(int canvasX, int canvasY) const;

    /** 滚动视口到当前搜索匹配位置 */
    void scrollToCurrentMatch();

    /** 根据当前宽度和全文重建显示行 */
    void rebuildDisplayLines();

    //── 显示行 ──────────────────────────────
    struct DisplayLine
    {
        juce::String text;
        int startCharIndex = 0;
        int paragraphTsIndex = -1;  // 对应 paragraphTimestamps 索引，-1 表示无
    };
    std::vector<DisplayLine> displayLines;

    //── 数据源（指针由外部管理） ──────────────
    const std::vector<CharacterTimestamp>* timestamps = nullptr;
    std::vector<ParagraphTimestamp> paragraphTimestamps;
    juce::String fullText;
    double timeOffset{ 0.0 };

    //── 高亮状态 ────────────────────────────
    double highlightTime{ -1.0 };
    int lastHighlightedIndex = -1;

    //── 点击/光标 ───────────────────────────
    int clickedCharIndex = 0;
    int charIndexBeforeMouseDown = -1;
    bool caretVisible = false;

    //── 文字选择 ────────────────────────────
    int selectionStart = -1;
    int selectionEnd = -1;

    //── 搜索 ──────────────────────────────
    struct SearchMatch
    {
        int startIndex = 0;
        int endIndex = 0;
    };
    std::vector<SearchMatch> searchMatches;
    int currentSearchMatch = -1;
    juce::String searchKeyword;

    //── 子组件 ──────────────────────────────
    TranscriptViewport viewport;
    Canvas canvas;

    //── 布局常量 ────────────────────────────
    juce::Font font{ "Microsoft YaHei", 18.0f, juce::Font::plain };
    static constexpr int lineHeight = 26;
    static constexpr int textLeftMargin = 70;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VirtualTranscriptComponent)
};
