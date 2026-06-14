#include "ARADocumentController.h"
#include "TranscriptDataManager.h"

TranscriptDataManager* TranscriptDocumentController::globalDataManager = nullptr;

//==============================================================================
//  ARA 模型对象工厂
//==============================================================================
juce::ARAAudioModification* TranscriptDocumentController::doCreateAudioModification(
    juce::ARAAudioSource* audioSource,
    ARA::ARAAudioModificationHostRef hostRef,
    const juce::ARAAudioModification* optionalModificationToClone) noexcept
{
    return new TranscriptAudioModification(audioSource, hostRef,
               static_cast<const TranscriptAudioModification*>(optionalModificationToClone));
}

juce::ARAPlaybackRenderer* TranscriptDocumentController::doCreatePlaybackRenderer() noexcept
{
    return new TranscriptPlaybackRenderer(getDocumentController());
}

//==============================================================================
//  持久化 — 将 ASR 结果保存在 DAW 工程文件中
//
//  直接遍历 dataManager 全局账本，不依赖 ARA 过滤器。
//==============================================================================
bool TranscriptDocumentController::doRestoreObjectsFromStream(
    juce::ARAInputStream& input,
    const juce::ARARestoreObjectsFilter* filter) noexcept
{
    juce::ignoreUnused(filter);
    dataManager.clear();

    auto numSources = input.readInt64();
    for (juce::int64 i = 0; i < numSources; ++i)
    {
        if (i % 10 == 0)
        {
            float progress = (float)i / (float)juce::jmax((juce::int64)1, numSources);
            getDocumentController()->getHostArchivingController()
                ->notifyDocumentUnarchivingProgress(progress);
        }

        juce::String key = input.readString();
        auto& data = dataManager.getOrCreateData(key);
        data.fullText   = input.readString();
        data.totalLength = input.readDouble();
        data.asrComplete = input.readBool();

        int numWords = (int)input.readInt64();
        for (int w = 0; w < numWords; ++w)
        {
            CharacterTimestamp ts;
            ts.character       = input.readString();
            ts.startTime       = input.readDouble();
            ts.endTime         = input.readDouble();
            ts.globalTextIndex = (int)input.readInt64();
            data.timestamps.push_back(ts);
        }
    }

    // 恢复段落标记（与 Python postprocess_words 一致）
    // 规则：前一字结尾是 。？！ or 静音间隙 > 1.5s → 新段落
    for (auto& [k, d] : dataManager)
    {
        if (d.timestamps.size() > 0)
            d.timestamps[0].isParagraphStart = true;
        for (size_t i = 1; i < d.timestamps.size(); ++i)
        {
            auto& prev = d.timestamps[i - 1];
            auto& cur  = d.timestamps[i];
            // 前一字以句号/问号/感叹号结尾
            if (prev.character.isNotEmpty())
            {
                auto lastChar = prev.character[prev.character.length() - 1];
                if (lastChar == 0x3002 || lastChar == 0xFF01 || lastChar == 0xFF1F) // 。！？
                {
                    cur.isParagraphStart = true;
                    continue;
                }
            }
            // 静音间隙 > 1.5 秒
            if ((cur.startTime - prev.endTime) > 1.5)
                cur.isParagraphStart = true;
        }
    }

    getDocumentController()->getHostArchivingController()
        ->notifyDocumentUnarchivingProgress(1.0f);

    // 通知编辑器刷新 UI
    juce::MessageManager::callAsync([this]()
    {
        if (onDataRestored)
            onDataRestored();
    });

    return !input.failed();
}

bool TranscriptDocumentController::doStoreObjectsToStream(
    juce::ARAOutputStream& output,
    const juce::ARAStoreObjectsFilter* filter) noexcept
{
    juce::ignoreUnused(filter);

    // 直接保存整个 dataManager 的内容，不依赖过滤器
    juce::int64 count = (juce::int64)dataManager.size();
    if (!output.writeInt64(count))
        return false;

    juce::int64 idx = 0;
    for (auto& [key, data] : dataManager)
    {
        float progress = (float)idx++ / (float)juce::jmax((juce::int64)1, count);
        getDocumentController()->getHostArchivingController()
            ->notifyDocumentArchivingProgress(progress);

        if (!output.writeString(key))
            return false;
        if (!output.writeString(data.fullText))
            return false;
        if (!output.writeDouble(data.totalLength))
            return false;
        if (!output.writeBool(data.asrComplete))
            return false;
        if (!output.writeInt64((juce::int64)data.timestamps.size()))
            return false;

        for (auto& ts : data.timestamps)
        {
            if (!output.writeString(ts.character))     return false;
            if (!output.writeDouble(ts.startTime))      return false;
            if (!output.writeDouble(ts.endTime))        return false;
            if (!output.writeInt64(ts.globalTextIndex)) return false;
        }
    }

    getDocumentController()->getHostArchivingController()
        ->notifyDocumentUnarchivingProgress(1.0f);

    return true;
}
