#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <vector>
#include "WaveformComponent.h"
#include "TranscriptEditor.h"
#include "CharacterTimestamp.h"
#include "ASRProcessor.h"

class TestToneSource : public juce::AudioSource
{
public:
    void prepareToPlay(int, double sampleRate) override { currentSampleRate = sampleRate; phase = 0.0; }
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        if (!enabled) { bufferToFill.clearActiveBufferRegion(); return; }
        auto* l = bufferToFill.buffer->getWritePointer(0, bufferToFill.startSample);
        auto* r = bufferToFill.buffer->getWritePointer(1, bufferToFill.startSample);
        for (int i = 0; i < bufferToFill.numSamples; ++i)
        {
            float s = (float)(std::sin(phase) * 0.3);
            l[i] = s; r[i] = s;
            phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / currentSampleRate;
        }
    }
    void setEnabled(bool e) { enabled = e; }
    bool isEnabled() const { return enabled; }
private:
    double currentSampleRate = 44100.0;
    double phase = 0.0;
    bool enabled = false;
};

class MainComponent : public juce::Component
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void openAudioFile();
    void aboutDialog();
    void onAudioFileLoaded(const juce::File& file);

    bool hasAudioFile() const     { return totalLength > 0.0; }
    double getTotalLength() const { return totalLength; }

private:
    void syncTextToAudio(int charIndex);
    void syncAudioToText(double timeInSeconds);
    double findTimeByCharIndex(int charIndex) const;
    int findCharByTime(double timeInSeconds) const;
    void loadDemoData();
    void updatePlayButtonText();
    void startASR();

    juce::AudioDeviceManager audioDeviceManager;
    juce::AudioSourcePlayer audioSourcePlayer;
    TestToneSource testTone;
    juce::AudioTransportSource transportSource;
    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource> currentAudioSource;
    std::unique_ptr<juce::FileChooser> fileChooser;
    double totalLength = 0.0;
    juce::File currentAudioFile;

    WaveformComponent waveform;
    TranscriptEditor transcriptEditor;
    juce::TextButton openButton;
    juce::TextButton playButton;
    juce::TextButton stopButton;
    juce::TextButton asrButton;
    juce::ComboBox modelSelector;
    juce::Label asrStatusLabel;

    // ASR 引擎
    ASRProcessor asrProcessor;

    // ASR 进度条
    juce::ProgressBar asrProgressBar;
    double asrProgressValue{ 0.0 };

    std::vector<CharacterTimestamp> timestamps;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
