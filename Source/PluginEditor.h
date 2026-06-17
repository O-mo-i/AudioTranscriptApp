#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "TranscriptEditor.h"
#include "ASRProcessor.h"
#include "CharacterTimestamp.h"

class TranscriptPluginProcessor;
class TranscriptDataManager;

/**
 * ARA 插件编辑器。
 *
 * 多轨支持：当用户在 Studio One 中选中不同的 Clip 时，
 * ARAEditorView::Listener::onNewSelection 被触发，
 * 编辑器自动切换进度条和转录文本。
 *
 * 生命周期：所有数据托管在 Processor 层的 TranscriptDataManager 中，
 * 以底层的 ARAAudioSource*（物理音频源）为键，跨轨不丢失。
 *
 * 时轴同步：实时监听当前 ARAPlaybackRegion 的属性变化，
 * 用户拖动音频块后，点击文字驱动跳转时使用最新的时间线位置。
 */
class TranscriptPluginEditor : public juce::AudioProcessorEditor,
                                private juce::AudioProcessorEditorARAExtension,
                                private juce::Timer,
                                private juce::ARAEditorView::Listener,
                                private juce::ARAPlaybackRegion::Listener
{
public:
    explicit TranscriptPluginEditor(TranscriptPluginProcessor& p);
    ~TranscriptPluginEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** setStateInformation 异步恢复后刷新 UI */
    void refreshAfterStateRestore();

    juce::AudioProcessorEditorARAExtension* getARAClientExtensions() override { return this; }

private:
    //── 内部工具 ────────────────────────────
    void timerCallback() override;
    void onActiveSourceChanged(juce::ARAAudioSource* source);
    /** 重织整轨文本：遍历当前 Track 的所有 Regions，按宿主时间轴排序后拼接显示 */
    void refreshTrackText();
    /** 定时轮询当前 ARA 选区，检测跨轨后的选择变更 */
    void pollSelectionChanged();
    void syncTextToAudio(int charIndex);
    double findTimeByCharIndex(int charIndex) const;
    void startASR();
    void downloadModel();
    void verifyModel();

    /** 计算当前切片在物理音频源中的视口区间 */
    juce::Range<double> getCurrentViewRange() const;

    /** 从 currentRegion 实时获取宿主时间线偏移 */
    double getRegionPlaybackStart() const noexcept
    {
        return currentRegion != nullptr ? currentRegion->getTimeRange().getStart() : 0.0;
    }

    /** 切片在音频源中的物理起点偏移（用于时间换算） */
    double getViewStart() const noexcept
    {
        return currentRegion != nullptr ? currentRegion->getStartInAudioModificationTime() : 0.0;
    }

    /** 音频源相对时间（阿字级时间戳）→ 宿主绝对时间线 */
    double sourceTimeToAbsolute(double sourceTime) const noexcept
    {
        return getRegionPlaybackStart() + (sourceTime - getViewStart());
    }

    /** 宿主绝对时间线 → 音频源相对时间（阿字级时间戳） */
    double absoluteToSourceTime(double absTime) const noexcept
    {
        return absTime - getRegionPlaybackStart() + getViewStart();
    }

    //── ARA 选择变化 ────────────────────────
    void onNewSelection(const juce::ARAViewSelection& selection) override;

    //── ARA 播放区域属性变化（用户拖动音频块时触发） ──
    void willUpdatePlaybackRegionProperties(
        juce::ARAPlaybackRegion* region,
        juce::ARAPlaybackRegion::PropertiesPtr newProperties) override;

    //── ARA 播放区域销毁（剪切/删除音频块时防止死锁与生命周期崩溃） ──
    void willDestroyPlaybackRegion(juce::ARAPlaybackRegion* region) override;

    //── ARA 播放区域属性已更新（音频块拖动/属性变化后重织文本） ──
    void didUpdatePlaybackRegionProperties(juce::ARAPlaybackRegion* region) override;

    //── 成员 ────────────────────────────────
    TranscriptPluginProcessor& processor;
    TranscriptDataManager* dataManager = nullptr;

    TranscriptEditor transcriptEditor;

    void cleanupAll();

    // ASR 控件
    juce::ComboBox modelSelector;
    juce::TextButton asrButton;
    juce::TextButton cleanupButton;
    juce::TextButton downloadButton;
    juce::TextButton verifyButton;
    juce::Label asrStatusLabel;
    juce::ProgressBar asrProgressBar;
    double asrProgressValue{ 0.0 };

    // ASR 引擎
    ASRProcessor asrProcessor;

    // 当前显示的时间戳数据
    const std::vector<CharacterTimestamp>* currentTimestamps = nullptr;

    // ASR 产生的临时音频文件
    juce::File tempAudioFile;

    //── 切片视口过滤后的文本与时间戳 ──────────
    std::vector<CharacterTimestamp> filteredTimestamps;
    juce::String filteredFullText;

    /** 上次实际传给 TextEditor 的文本，用于跳过重复的 setText */
    juce::String lastSetText;
    /** 缓存当前轨道的拼接结果，跨轨切回时避免全文重织 */
    struct TrackCache {
        juce::ARARegionSequence* sequence = nullptr;
        juce::String fullText;
        std::vector<CharacterTimestamp> timestamps;
        std::vector<TranscriptEditor::ParagraphTimestamp> paragraphTimestamps;
    } trackCache;

    //── 播放高亮防抖 ────────────────────────
    int lastHighlightedCharIndex = -1;

    //── 当前 ARA 播放区域（实时监听属性变化） ──
    juce::ARAPlaybackRegion* currentRegion = nullptr;

    //── 当前 ARA Region Sequence（对应宿主 Track，用于整轨拼接） ──
    juce::ARARegionSequence* currentRegionSequence = nullptr;

    //── 定时轮询选区计数器（30Hz 定时器，每 15 帧 = ~500ms 检查一次） ──
    int selectionPollCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TranscriptPluginEditor)
};
