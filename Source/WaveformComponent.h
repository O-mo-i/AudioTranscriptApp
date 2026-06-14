#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

/**
 * 细长播放进度条（取代大波形）。
 *
 * 只在顶部占用 8px 高度，用深色背景 + 亮色已播放段 + 红色三角指针
 * 显示当前播放进度。点击可在对应位置跳转。
 */
class WaveformComponent : public juce::Component
{
public:
    WaveformComponent();
    ~WaveformComponent() override;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;

    /** 设置音频源（仅用于获取总时长） */
    void setAudioSource(juce::ARAAudioSource* source);

    /** 外部驱动播放头位置（来自 PlayHeadState 轮询） */
    void setPlayheadPosition(double timeInSeconds);

    double getTotalLength() const noexcept { return totalLength; }

    std::function<void(double timeInSeconds)> onTimeSelected;

private:
    double playheadPosition = 0.0;
    double totalLength = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformComponent)
};
