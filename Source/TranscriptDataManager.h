#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>
#include <functional>
#include "CharacterTimestamp.h"

/**
 * 每个音频源（AudioSource）对应的数据块。
 */
struct AudioSourceData
{
    std::vector<CharacterTimestamp> timestamps;
    juce::String fullText;
    double totalLength = 0.0;
    bool asrComplete = false;
};

/**
 * 多轨转录数据管理器。
 *
 * 核心：std::map<juce::String, AudioSourceData>
 * 键是物理音频源的【稳定指纹字符串】，永远不依赖内存指针。
 *
 * 指纹生成：makeSourceKey()
 *   组合 sampleRate + sampleCount + channelArrangement 信息，
 *   ARA 规范保证同一音频文件在跨轨、复制、切碎后这些属性不变。
 *
 * setActiveSource() 仅用于跟踪当前 UI 选中了哪个音频源，
 * 实际数据的存取统一走物理指纹键。
 */
class TranscriptDataManager
{
public:
    /** 从 ARAAudioSource 生成稳定指纹键（纯字符串，不依赖指针） */
    static juce::String makeSourceKey(const juce::ARAAudioSource* source);

    AudioSourceData& getOrCreateData(const juce::String& key);
    AudioSourceData& getOrCreateDataForKey(const juce::ARAAudioSource* source)
    {
        return getOrCreateData(makeSourceKey(source));
    }

    void setASRResult(const juce::String& key,
                      const std::vector<CharacterTimestamp>& ts,
                      const juce::String& text);

    bool hasASRResult(const juce::String& key) const;

    const AudioSourceData* getDataForKey(const juce::String& key) const;
    AudioSourceData* getDataForKey(const juce::String& key);

    //── 激活源切换（仅用于 UI 状态跟踪） ──────────
    void setActiveSource(juce::ARAAudioSource* source);
    juce::ARAAudioSource* getActiveSource() const noexcept { return activeSource; }
    AudioSourceData* getActiveData();

    /** 当激活源切换时触发（UI 层监听此回调来刷新波形和文本） */
    std::function<void(juce::ARAAudioSource* source)> onActiveSourceChanged;

    /** 当某个音频源的 ASR 结果就绪时触发 */
    std::function<void(const juce::String& key)> onASRCompleted;

    //── 持久化辅助 ──────────────────────────
    void clear() { dataMap.clear(); activeSource = nullptr; }
    size_t size() const { return dataMap.size(); }

    using DataMap = std::map<juce::String, AudioSourceData>;
    DataMap::iterator begin() { return dataMap.begin(); }
    DataMap::iterator end()   { return dataMap.end(); }
    DataMap::const_iterator begin() const { return dataMap.begin(); }
    DataMap::const_iterator end()   const { return dataMap.end(); }

    /** 活动源的指纹键（用于异步回调后查找数据） */
    juce::String getActiveKey() const noexcept { return activeKey; }
    void setActiveKey(const juce::String& k) noexcept { activeKey = k; }

private:
    DataMap dataMap;
    juce::ARAAudioSource* activeSource = nullptr;
    juce::String activeKey;
};
