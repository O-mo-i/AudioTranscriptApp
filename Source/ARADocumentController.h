#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "TranscriptDataManager.h"

//==============================================================================
/**
 * ARA 音频修改对象子类。
 * 每个 ARAAudioSource 在宿主中修改（如裁剪、变速）后产生一个 AudioModification。
 * 我们使用默认实现即可，无需额外状态。
 */
class TranscriptAudioModification : public juce::ARAAudioModification
{
public:
    using juce::ARAAudioModification::ARAAudioModification;
};

//==============================================================================
/**
 * 最小化的 ARA 播放渲染器。
 * 我们不做音频处理（transcription 是分析型插件），
 * 因此所有 processBlock 都直接返回 true（直通/无操作）。
 */
class TranscriptPlaybackRenderer : public juce::ARAPlaybackRenderer
{
public:
    using juce::ARAPlaybackRenderer::ARAPlaybackRenderer;

    void prepareToPlay(double, int, int,
                       juce::AudioProcessor::ProcessingPrecision,
                       AlwaysNonRealtime) override {}

    void releaseResources() override {}

    bool processBlock(juce::AudioBuffer<float>&,
                      juce::AudioProcessor::Realtime,
                      const juce::AudioPlayHead::PositionInfo&) noexcept override
    {
        return true; // 不修改音频，直接返回成功
    }
};

//==============================================================================
/**
 * ARA 文档控制器特化 —— 插件的 ARA "大脑"。
 *
 * 职责：
 *   1. 管理多轨数据（通过 TranscriptDataManager）
 *   2. 创建 PlaybackRenderer（宿主播放时调用）
 *   3. 持久化 ASR 结果（doStore / doRestore）
 *   4. 通过 ARA 模型回调跟踪文档变化
 */
class TranscriptDocumentController : public juce::ARADocumentControllerSpecialisation
{
public:
    TranscriptDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                                  const ARA::ARADocumentControllerHostInstance* instance)
        : juce::ARADocumentControllerSpecialisation(entry, instance)
    {
        globalDataManager = &dataManager;
    }

    /** 工程级别全局数据管理器（所有实例共享） */
    static TranscriptDataManager* getGlobalDataManager() noexcept { return globalDataManager; }
    TranscriptDataManager& getDataManager() noexcept { return dataManager; }

    /** 当从存档恢复完成后触发 */
    std::function<void()> onDataRestored;

protected:
    //── ARA 模型对象工厂 ────────────────────
    juce::ARAAudioModification* doCreateAudioModification(
        juce::ARAAudioSource* audioSource,
        ARA::ARAAudioModificationHostRef hostRef,
        const juce::ARAAudioModification* optionalModificationToClone) noexcept override;

    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override;

    //── 持久化 ──────────────────────────────
    bool doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                     const juce::ARARestoreObjectsFilter* filter) noexcept override;
    bool doStoreObjectsToStream(juce::ARAOutputStream& output,
                                 const juce::ARAStoreObjectsFilter* filter) noexcept override;

private:
    static TranscriptDataManager* globalDataManager;
    TranscriptDataManager dataManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TranscriptDocumentController)
};
