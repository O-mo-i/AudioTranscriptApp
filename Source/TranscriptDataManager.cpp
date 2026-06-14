#include "TranscriptDataManager.h"

juce::String TranscriptDataManager::makeSourceKey(const juce::ARAAudioSource* source)
{
    if (source == nullptr) return {};

    // 三层指纹：持久ID | 采样率 | 总采样数
    // 持久ID 在 Session 内通常稳定，但如果 Studio One 跨轨重设了它，
    // sampleRate|sampleCount 后缀仍然可以匹配（见 getDataForKey 的回退逻辑）
    return juce::String(source->getPersistentID())
         + "|" + juce::String((juce::int64)source->getSampleRate())
         + "|" + juce::String(source->getSampleCount());
}

AudioSourceData& TranscriptDataManager::getOrCreateData(const juce::String& key)
{
    return dataMap[key];
}

void TranscriptDataManager::setASRResult(const juce::String& key,
                                          const std::vector<CharacterTimestamp>& ts,
                                          const juce::String& text)
{
    auto& data = getOrCreateData(key);
    data.timestamps = ts;
    data.fullText = text;
    data.asrComplete = true;

    juce::MessageManager::callAsync([this, key]()
    {
        if (onASRCompleted)
            onASRCompleted(key);
    });
}

bool TranscriptDataManager::hasASRResult(const juce::String& key) const
{
    auto it = dataMap.find(key);
    return it != dataMap.end() && it->second.asrComplete;
}

//==============================================================================
//  查找：两级匹配
//    1) 精确匹配 full key（持久ID|采样率|总采样数）
//    2) 回退：提取最后两段（采样率|总采样数）遍历匹配
//       匹配后自动将旧条目重键到新 key 下（后续走快速路径）
//==============================================================================
const AudioSourceData* TranscriptDataManager::getDataForKey(const juce::String& key) const
{
    auto it = dataMap.find(key);
    if (it != dataMap.end())
        return &it->second;

    // 回退匹配（const 版不能修改 map，只返回匹配结果）
    auto suffix = key.fromFirstOccurrenceOf("|", false, false);
    for (const auto& [storedKey, data] : dataMap)
    {
        auto storedSuffix = storedKey.fromFirstOccurrenceOf("|", false, false);
        if (suffix.isNotEmpty() && suffix == storedSuffix)
            return &data;
    }
    return nullptr;
}

AudioSourceData* TranscriptDataManager::getDataForKey(const juce::String& key)
{
    auto it = dataMap.find(key);
    if (it != dataMap.end())
        return &it->second;

    // 回退匹配 + 自动重键
    auto suffix = key.fromFirstOccurrenceOf("|", false, false);
    for (auto& [storedKey, data] : dataMap)
    {
        auto storedSuffix = storedKey.fromFirstOccurrenceOf("|", false, false);
        if (suffix.isNotEmpty() && suffix == storedSuffix)
        {
            // 将旧条目重设键为当前 key，后续跨轨不再回退
            auto node = dataMap.extract(storedKey);
            node.key() = key;
            return &dataMap.insert(std::move(node)).position->second;
        }
    }
    return nullptr;
}

//==============================================================================
void TranscriptDataManager::setActiveSource(juce::ARAAudioSource* source)
{
    activeSource = source;
    activeKey = makeSourceKey(source);
    if (onActiveSourceChanged)
        onActiveSourceChanged(source);
}

AudioSourceData* TranscriptDataManager::getActiveData()
{
    if (activeKey.isEmpty()) return nullptr;
    return getDataForKey(activeKey);
}
