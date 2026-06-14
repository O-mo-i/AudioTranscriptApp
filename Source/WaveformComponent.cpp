#include "WaveformComponent.h"

WaveformComponent::WaveformComponent(juce::AudioTransportSource& transport)
    : thumbnail(512, formatManager, thumbnailCache),
      transportSource(transport)
{
    thumbnail.addChangeListener(this);
    formatManager.registerBasicFormats();
    startTimerHz(30);
}

WaveformComponent::~WaveformComponent()
{
    thumbnail.removeChangeListener(this);
    stopTimer();
}

void WaveformComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &thumbnail)
    {
        // 缩略图异步加载完成后强制重绘，但不覆盖 totalLength
        // totalLength 由 loadAudioFile 从外部传入，确保与 MainComponent 一致
        repaint();
    }
}

void WaveformComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.fillAll(juce::Colour(0xFF1a1a2e));

    if (totalLength > 0.0 && thumbnail.getNumChannels() > 0)
    {
        g.setColour(juce::Colour(0xFF6a6abe));
        thumbnail.drawChannels(g, bounds.reduced(2).toNearestInt(), 0.0, totalLength, 1.0f);
    }
    else
    {
        g.setColour(juce::Colours::grey);
        g.setFont(juce::Font(16.0f));
        g.drawText(juce::String::fromUTF8("点击 文件 > 打开音频 来选择音频文件"),
                   bounds, juce::Justification::centred);
    }

    if (totalLength > 0.0)
    {
        const float playheadX = bounds.getX()
                                + (float)(playheadPosition / totalLength) * bounds.getWidth();
        g.setColour(juce::Colours::red);
        g.drawLine(playheadX, bounds.getY(), playheadX, bounds.getBottom(), 2.0f);
    }
}

void WaveformComponent::resized() {}

void WaveformComponent::mouseDown(const juce::MouseEvent& event)
{
    if (totalLength <= 0.0) return;

    const double clickTime = juce::jlimit(0.0, totalLength,
                                          (event.position.x / getWidth()) * totalLength);
    setPlayheadPosition(clickTime);
    transportSource.setPosition(clickTime);

    if (onTimeSelected)
        onTimeSelected(clickTime);
}

void WaveformComponent::setPlayheadPosition(double timeInSeconds)
{
    playheadPosition = juce::jlimit(0.0, totalLength, timeInSeconds);
    repaint();
}

void WaveformComponent::loadAudioFile(const juce::File& file, double lengthInSeconds)
{
    // 重置状态
    totalLength = 0.0;
    playheadPosition = 0.0;
    repaint();

    // 彻底清空旧缩略图和缓存
    thumbnail.clear();

    // 使用 setSource + FileInputSource 强制新建 reader 读取文件，
    // 比 setReader(reader, hash) 更彻底，避免缓存残留导致旧波形不刷新
    thumbnail.setSource(new juce::FileInputSource(file));

    // 使用外部传入的时长（与 MainComponent 同一来源：reader->lengthInSamples / sampleRate）
    totalLength = lengthInSeconds;

    // 立即触发重绘，缩略图异步完成后也会通过 changeListenerCallback 再次重绘
    repaint();
}

void WaveformComponent::timerCallback()
{
    if (transportSource.isPlaying())
    {
        playheadPosition = transportSource.getCurrentPosition();
        repaint();
    }
}
