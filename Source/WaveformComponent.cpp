#include "WaveformComponent.h"

WaveformComponent::WaveformComponent() {}
WaveformComponent::~WaveformComponent() {}

void WaveformComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    // 深色背景条
    g.fillAll(juce::Colour(0xFF2a2a3e));

    auto bar = bounds.reduced(0, 1);
    g.setColour(juce::Colour(0xFF4a4a6e));
    g.fillRoundedRectangle(bar, 2.0f);

    if (totalLength > 0.0)
    {
        double ratio = juce::jlimit(0.0, 1.0, playheadPosition / totalLength);
        auto played = bar.withWidth(bar.getWidth() * (float)ratio);

        // 已播放段
        g.setColour(juce::Colour(0xFF7a7ad8));
        g.fillRoundedRectangle(played, 2.0f);

        // 红色三角指针
        float h = bar.getHeight();
        float mx = juce::jmin(played.getRight(), bar.getRight());
        g.setColour(juce::Colours::red);
        juce::Path tri;
        tri.addTriangle(mx, bar.getY(),
                        mx, bar.getBottom(),
                        mx + h * 0.8f, bar.getCentreY());
        g.fillPath(tri);
    }
}

void WaveformComponent::mouseDown(const juce::MouseEvent& event)
{
    if (totalLength <= 0.0) return;

    const double clickTime = juce::jlimit(0.0, totalLength,
                                          (event.position.x / getWidth()) * totalLength);
    setPlayheadPosition(clickTime);

    if (onTimeSelected)
        onTimeSelected(clickTime);
}

void WaveformComponent::setAudioSource(juce::ARAAudioSource* source)
{
    playheadPosition = 0.0;
    if (source != nullptr)
    {
        auto sampleRate = source->getSampleRate();
        totalLength = (sampleRate > 0.0)
                      ? (double)source->getSampleCount() / sampleRate
                      : 0.0;
    }
    else
    {
        totalLength = 0.0;
    }
    repaint();
}

void WaveformComponent::setPlayheadPosition(double timeInSeconds)
{
    playheadPosition = juce::jlimit(0.0, totalLength, timeInSeconds);
    repaint();
}
