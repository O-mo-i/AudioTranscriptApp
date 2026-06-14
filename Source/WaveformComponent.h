#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>

class WaveformComponent : public juce::Component,
                          private juce::ChangeListener,
                          private juce::Timer
{
public:
    WaveformComponent(juce::AudioTransportSource& transport);
    ~WaveformComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;

    void setPlayheadPosition(double timeInSeconds);
    double getPlayheadPosition() const noexcept { return playheadPosition; }

    // 接收外部传入的时长，与 MainComponent 保持一致
    void loadAudioFile(const juce::File& file, double lengthInSeconds);
    double getTotalLength() const noexcept { return totalLength; }

    std::function<void(double timeInSeconds)> onTimeSelected;

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void timerCallback() override;

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache{ 5 };
    juce::AudioThumbnail thumbnail;
    juce::AudioTransportSource& transportSource;

    double playheadPosition = 0.0;
    double totalLength = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformComponent)
};
