#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "TranscriptDataManager.h"

/**
 * 播放头状态 —— processBlock 每次被宿主调用时更新。
 * PluginEditor 中的 Timer 定期读取此状态来驱动文本光标高亮。
 */
struct PlayHeadState
{
    std::atomic<bool> isPlaying{ false };
    std::atomic<double> timeInSeconds{ 0.0 };

    void update(const juce::Optional<juce::AudioPlayHead::PositionInfo>& info)
    {
        if (info.hasValue())
        {
            isPlaying.store(info->getIsPlaying(), std::memory_order_relaxed);
            timeInSeconds.store(info->getTimeInSeconds().orFallback(0.0), std::memory_order_relaxed);
        }
        else
        {
            isPlaying.store(false, std::memory_order_relaxed);
        }
    }
};

//==============================================================================
/**
 * VST3 + ARA 插件核心处理器。
 *
 * 数据账本统一托管在 TranscriptDocumentController（工程级别的全局唯一实例），
 * 通过 getDataManager() 访问的始终是同一个 TranscriptDataManager，
 * 跨轨/跨实例全部指向同一份内存。
 */
class TranscriptPluginProcessor : public juce::AudioProcessor,
                                   public juce::AudioProcessorARAExtension
{
public:
    TranscriptPluginProcessor();
    ~TranscriptPluginProcessor() override;

    //── AudioProcessor ──────────────────────
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override { return "Audio Transcript Editor"; }

    int getNumPrograms() override      { return 1; }
    int getCurrentProgram() override   { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    bool acceptsMidi() const override     { return false; }
    bool producesMidi() const override    { return false; }

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    double getTailLengthSeconds() const override;

    //── 公开数据访问 ─────────────────────────
    /** 返回工程级别全局唯一的 TranscriptDataManager */
    TranscriptDataManager& getDataManager() noexcept;

    PlayHeadState& getPlayHeadState() noexcept { return playHeadState; }

private:
    static juce::AudioProcessor::BusesProperties getBusesProperties();

    PlayHeadState playHeadState;

    // DocumentController 就绪前的应急兜底
    TranscriptDataManager fallbackDataManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TranscriptPluginProcessor)
};
