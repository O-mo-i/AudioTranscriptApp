#include "MainComponent.h"

MainComponent::MainComponent()
    : waveform(transportSource), asrProgressBar(asrProgressValue)
{
    setSize(1000, 700);

    auto err = audioDeviceManager.initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty()) DBG("Audio init error: " + err);

    audioSourcePlayer.setSource(&testTone);
    audioDeviceManager.addAudioCallback(&audioSourcePlayer);
    testTone.setEnabled(false);

    formatManager.registerBasicFormats();

    // ── 打开音频按钮 ──
    openButton.setButtonText(juce::String::fromUTF8("打开音频"));
    openButton.onClick = [this] { openAudioFile(); };

    // ── 播放/暂停按钮 ──
    playButton.setButtonText(juce::String::fromUTF8("▶"));
    playButton.onClick = [this]
    {
        if (!hasAudioFile()) return;
        if (transportSource.isPlaying()) transportSource.stop(); else transportSource.start();
        updatePlayButtonText();
    };
    playButton.setEnabled(false);

    // ── 停止按钮 ──
    stopButton.setButtonText(juce::String::fromUTF8("⏹"));
    stopButton.onClick = [this]
    {
        if (!hasAudioFile()) return;
        transportSource.stop(); transportSource.setPosition(0.0);
        waveform.setPlayheadPosition(0.0);
        updatePlayButtonText();
    };
    stopButton.setEnabled(false);

    // ── ASR 模型选择 ──
    modelSelector.addItem("Qwen/Qwen3-ASR-0.6B", 1);
    modelSelector.addItem("openai/whisper-small", 2);
    modelSelector.addItem("openai/whisper-medium", 3);
    modelSelector.addItem("openai/whisper-large-v3", 4);
    modelSelector.setSelectedId(1);
    modelSelector.setTooltip(juce::String::fromUTF8("选择 ASR 识别模型"));

    // ── ASR 启动按钮 ──
    asrButton.setButtonText("ASR");
    asrButton.onClick = [this] { startASR(); };
    asrButton.setEnabled(false);

    // ── ASR 状态标签 ──
    asrStatusLabel.setText(juce::String::fromUTF8("加载音频后可进行 ASR 识别"),
                           juce::dontSendNotification);
    asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    asrStatusLabel.setJustificationType(juce::Justification::centred);

    // ASR 结果回调
    asrProcessor.onResult = [this](const std::vector<CharacterTimestamp>& asrTimestamps,
                                    const juce::String& fullText)
    {
        asrProgressBar.setVisible(false);
        timestamps = asrTimestamps;
        transcriptEditor.setText(fullText, juce::dontSendNotification);
        transcriptEditor.setTimestamps(&timestamps);

        asrStatusLabel.setText(juce::String::fromUTF8("ASR 识别完成"),
                               juce::dontSendNotification);
        asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::greenyellow);

        asrButton.setEnabled(true);
        transcriptEditor.setCaretPosition(0);
    };

    // ASR 错误回调
    asrProcessor.onError = [this](const juce::String& errorMsg)
    {
        asrProgressBar.setVisible(false);
        asrStatusLabel.setText(juce::String::fromUTF8("ASR 错误: ") + errorMsg,
                               juce::dontSendNotification);
        asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::orangered);
        asrButton.setEnabled(true);
    };

    // ASR 进度回调
    asrProcessor.onProgress = [this](double pct)
    {
        asrProgressValue = pct;
        asrProgressBar.repaint();
    };

    asrProgressBar.setTextToDisplay({});   // 细条进度条不需要文字
    asrProgressBar.setVisible(false);

    addAndMakeVisible(waveform);
    addAndMakeVisible(transcriptEditor);
    addAndMakeVisible(openButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(modelSelector);
    addAndMakeVisible(asrButton);
    addAndMakeVisible(asrProgressBar);
    addAndMakeVisible(asrStatusLabel);

    waveform.onTimeSelected = [this](double t) { syncAudioToText(t); };
    transcriptEditor.onCaretMoved = [this](int i) { syncTextToAudio(i); };
    transcriptEditor.setTimestamps(&timestamps);

    loadDemoData();
}

MainComponent::~MainComponent()
{
    audioDeviceManager.removeAudioCallback(&audioSourcePlayer);
    audioSourcePlayer.setSource(nullptr);
    transportSource.setSource(nullptr);
    currentAudioSource.reset();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xFF1a1a2e));
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(8);

    // 底部控制栏
    auto controlArea = bounds.removeFromBottom(44);
    waveform.setBounds(bounds.removeFromTop(bounds.getHeight() * 2 / 3));
    transcriptEditor.setBounds(bounds);

    const int btnW = 100;
    const int comboW = 220;
    const int gap = 4;

    openButton.setBounds(controlArea.removeFromLeft(btnW).reduced(gap));
    playButton.setBounds(controlArea.removeFromLeft(btnW).reduced(gap));
    stopButton.setBounds(controlArea.removeFromLeft(btnW).reduced(gap));

    controlArea.removeFromLeft(12);
    modelSelector.setBounds(controlArea.removeFromLeft(comboW).reduced(gap));
    asrButton.setBounds(controlArea.removeFromLeft(btnW).reduced(gap));

    // ASR 状态标签放在右上
    asrStatusLabel.setBounds(getWidth() - 360, 0, 350, 25);

    // ASR 进度条 — 右上角状态标签下方，细条样式（仅 ASR 进行中可见）
    asrProgressBar.setBounds(getWidth() - 350, 28, 340, 5);
}

void MainComponent::updatePlayButtonText()
{
    playButton.setButtonText(transportSource.isPlaying()
        ? juce::String::fromUTF8("⏸")
        : juce::String::fromUTF8("▶"));
}

//==============================================================================
void MainComponent::openAudioFile()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        juce::String::fromUTF8("选择音频文件"),
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.wav;*.mp3;*.flac;*.aiff;*.ogg;*.m4a");

    fileChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser)
        {
            auto results = chooser.getResults();
            if (results.isEmpty()) return;
            onAudioFileLoaded(results.getReference(0));
        });
}

void MainComponent::onAudioFileLoaded(const juce::File& file)
{
    auto* reader = formatManager.createReaderFor(file);
    if (reader == nullptr) return;

    const double newLength = (reader->lengthInSamples > 0 && reader->sampleRate > 0)
                             ? reader->lengthInSamples / reader->sampleRate
                             : 0.0;

    // 设置音频文件引用以便后续调用 startASR
    currentAudioFile = file;
    waveform.loadAudioFile(file, newLength);

    if (!hasAudioFile())
    {
        audioSourcePlayer.setSource(nullptr);
        audioSourcePlayer.setSource(&transportSource);
    }

    transportSource.stop();
    transportSource.setSource(nullptr);
    currentAudioSource.reset();

    const double fileSampleRate = reader->sampleRate;
    const int numChannels = reader->numChannels;

    currentAudioSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    transportSource.setSource(currentAudioSource.get(), 0, nullptr,
                               fileSampleRate, numChannels);

    if (auto* dev = audioDeviceManager.getCurrentAudioDevice())
    {
        transportSource.prepareToPlay(
            dev->getCurrentBufferSizeSamples(),
            dev->getCurrentSampleRate());
    }
    transportSource.setGain(1.0f);
    transportSource.setPosition(0.0);

    totalLength = newLength;
    updatePlayButtonText();

    playButton.setEnabled(true);
    stopButton.setEnabled(true);
    asrButton.setEnabled(true);

    asrStatusLabel.setText(juce::String::fromUTF8("音频已加载，点击\"开始识别\"进行 ASR"),
                           juce::dontSendNotification);
    asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
}

void MainComponent::startASR()
{
    if (!hasAudioFile()) return;

    // 从下拉框获取模型名称
    static const std::pair<int, juce::String> models[] = {
        {1, "Qwen/Qwen3-ASR-0.6B"},
        {2, "openai/whisper-small"},
        {3, "openai/whisper-medium"},
        {4, "openai/whisper-large-v3"},
    };
    juce::String chosen = "Qwen/Qwen3-ASR-0.6B";
    for (auto& m : models)
    {
        if (m.first == modelSelector.getSelectedId()) { chosen = m.second; break; }
    }

    asrProcessor.setModelName(chosen);
    asrButton.setEnabled(false);
    asrProgressValue = 0.0;
    asrProgressBar.setVisible(true);
    asrProgressBar.repaint();
    asrStatusLabel.setText(juce::String::fromUTF8("ASR 识别中 (") + chosen + ") ...",
                           juce::dontSendNotification);
    asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);
    asrProcessor.start(currentAudioFile);
}

void MainComponent::aboutDialog()
{
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::InfoIcon,
        juce::String::fromUTF8("关于"),
        juce::String::fromUTF8("音频转录编辑器 v1.0.0\n\n"
            "基于 JUCE 框架开发的音频波形与转录文本\n双向同步编辑软件。"));
}

//==============================================================================
void MainComponent::syncTextToAudio(int charIndex)
{
    double t = findTimeByCharIndex(charIndex);
    if (t >= 0.0) { transportSource.setPosition(t); waveform.setPlayheadPosition(t); }
}

void MainComponent::syncAudioToText(double t)
{
    transportSource.setPosition(t);
    waveform.setPlayheadPosition(t);
    transcriptEditor.highlightByTime(t);
}

double MainComponent::findTimeByCharIndex(int i) const
{
    for (auto& ts : timestamps)
    {
        int end = ts.globalTextIndex + ts.character.length();
        if (i >= ts.globalTextIndex && i < end) return ts.startTime;
    }
    return -1.0;
}

int MainComponent::findCharByTime(double t) const
{
    for (auto& ts : timestamps)
        if (t >= ts.startTime && t < ts.endTime) return ts.globalTextIndex;
    return -1;
}

void MainComponent::loadDemoData()
{
    timestamps.clear();
    transcriptEditor.clear();

    // Simulate two paragraphs with MM:SS labels (matches post-ASR display format)
    struct { const char* text; double startSec; } paragraphs[] = {
        {"你好，欢迎使用音频转录编辑器。", 0.0},
        {"加载音频后将自动进行 ASR 语音识别。", 5.0},
    };

    juce::String full;
    bool isFirst = true;
    for (auto& para : paragraphs)
    {
        auto text = juce::String::fromUTF8(para.text);
        int mins = (int)(para.startSec / 60.0);
        int secs = ((int)para.startSec) % 60;

        if (!isFirst)
            full += "\n";
        full += juce::String::formatted("%02d:%02d ", mins, secs);

        double perChar = 5.0 / juce::jmax(1, text.length());
        for (int i = 0; i < text.length(); ++i)
        {
            CharacterTimestamp ts;
            ts.character       = text.substring(i, i + 1);
            ts.startTime       = para.startSec + i * perChar;
            ts.endTime         = para.startSec + (i + 1) * perChar;
            ts.globalTextIndex = full.length();
            full += ts.character;
            timestamps.push_back(ts);
        }
        isFirst = false;
    }

    transcriptEditor.setText(full, juce::dontSendNotification);
}
