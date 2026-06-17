#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ARADocumentController.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

//==============================================================================
//  在构造中获取 ARA 文档控制器的引用（用于数据同步）
static TranscriptDocumentController* getTranscriptDocumentController(juce::AudioProcessorEditorARAExtension& editor)
{
    if (auto* ev = editor.getARAEditorView())
    {
        auto* rawDC = ev->getDocumentController();
        return juce::ARADocumentControllerSpecialisation::
            getSpecialisedDocumentController<TranscriptDocumentController>(rawDC);
    }
    return nullptr;
}

//==============================================================================
TranscriptPluginEditor::TranscriptPluginEditor(TranscriptPluginProcessor& p)
    : juce::AudioProcessorEditor(&p),
      juce::AudioProcessorEditorARAExtension(&p),
      processor(p),
      asrProgressBar(asrProgressValue)
{
    //── 文档控制器引用（用于数据同步 & 存档恢复） ──
    auto* documentController = getTranscriptDocumentController(*this);

    //── 数据管理器（以物理 ARAAudioSource* 为键） ────
    dataManager = &processor.getDataManager();
    dataManager->onActiveSourceChanged = [this](juce::ARAAudioSource* src)
    {
        onActiveSourceChanged(src);
    };

    //── ARA 选择变化监听 ────────────────────
    if (auto* editorView = getARAEditorView())
    {
        editorView->addListener(this);
        onNewSelection(editorView->getViewSelection());
    }

    // 强制刷新 UI：编辑器可能关闭后重开
    if (auto* src = dataManager->getActiveSource())
        onActiveSourceChanged(src);

    //── 存档恢复后刷新 UI ─────────────────────
    if (documentController)
    {
        documentController->onDataRestored = [this]()
        {
            if (auto* editorView = getARAEditorView())
            {
                auto sel = editorView->getViewSelection();
                auto regions = sel.getPlaybackRegions<juce::ARAPlaybackRegion>();
                if (!regions.empty())
                {
                    auto* audioMod = regions.front()->getAudioModification<juce::ARAAudioModification>();
                    if (auto* src = audioMod->getAudioSource())
                    {
                        dataManager->setActiveSource(src);
                        return;  // setActiveSource 已触发 onActiveSourceChanged
                    }
                }
            }
            // 没有选区：至少刷新一下当前状态
            onActiveSourceChanged(dataManager->getActiveSource());
        };
    }

    //── 进度条（8px 细条） ────────────────────
    addAndMakeVisible(waveform);
    waveform.onTimeSelected = [this](double t) { syncAudioToText(t); };

    //── 转录文本 ────────────────────────────
    addAndMakeVisible(transcriptEditor);
    transcriptEditor.setTimestamps(nullptr);
    transcriptEditor.onCaretMoved = [this](int i) { syncTextToAudio(i); };
    transcriptEditor.onSpacePressed = [this]() -> bool
    {
        auto& phs = processor.getPlayHeadState();
        if (auto* editorView = getARAEditorView())
        {
            auto* dc = editorView->getDocumentController();
            if (auto* playbackCtrl = dc->getHostPlaybackController())
            {
                bool isPlaying = phs.isPlaying.load(std::memory_order_relaxed);
                if (isPlaying)
                    playbackCtrl->requestStopPlayback();
                else
                    playbackCtrl->requestStartPlayback();
                return true;
            }
        }
        return false;
    };

    //── ASR 控件 ────────────────────────────
    modelSelector.addItem("Qwen/Qwen3-ASR-0.6B", 1);
    modelSelector.addItem("openai/whisper-small", 2);
    modelSelector.addItem("openai/whisper-medium", 3);
    modelSelector.addItem("openai/whisper-large-v3", 4);
    modelSelector.setSelectedId(1);
    modelSelector.setTooltip(juce::String::fromUTF8("\xe9\x80\x89\xe6\x8b\xa9 ASR \xe8\xaf\x86\xe5\x88\xab\xe6\xa8\xa1\xe5\x9e\x8b"));
    addAndMakeVisible(modelSelector);

    asrButton.setButtonText("ASR");
    asrButton.onClick = [this] { startASR(); };
    asrButton.setEnabled(false);
    addAndMakeVisible(asrButton);

    cleanupButton.setButtonText(juce::String::fromUTF8("\xe6\xb8\x85\xe7\x90\x86"));
    cleanupButton.onClick = [this] { cleanupAll(); };
    addAndMakeVisible(cleanupButton);

    downloadButton.setButtonText(juce::String::fromUTF8(
        "\xe4\xb8\x8b\xe8\xbd\xbd\xe6\xa8\xa1\xe5\x9e\x8b"));
    downloadButton.setTooltip(juce::String::fromUTF8(
        "\xe9\xa6\x96\xe6\xac\xa1\xe4\xbd\xbf\xe7\x94\xa8\xe5\x89\x8d\xe5\x9c\xa8"
        "\xe6\x9c\x89\xe7\xbd\x91\xe7\x8e\xaf\xe5\xa2\x83\xe4\xb8\x8b\xe7\x82\xb9"
        "\xe5\x87\xbb\xe6\xad\xa4\xe6\x8c\x89\xe9\x92\xae\xef\xbc\x8c\xe4\xb8\x8b"
        "\xe8\xbd\xbd\xe6\x89\x80\xe9\x9c\x80\xe7\x9a\x84\xe6\xa8\xa1\xe5\x9e\x8b"
        "\xe5\x88\xb0\xe6\x9c\xac\xe5\x9c\xb0\xe7\xbc\x93\xe5\xad\x98"));
    downloadButton.onClick = [this] { downloadModel(); };
    addAndMakeVisible(downloadButton);

    verifyButton.setButtonText(juce::String::fromUTF8(
        "\xe8\x81\x94\xe7\xbd\x91\xe6\xa0\xa1\xe9\xaa\x8c"));
    verifyButton.setTooltip(juce::String::fromUTF8(
        "\xe8\x81\x94\xe7\xbd\x91\xe6\xa3\x80\xe6\x9f\xa5\xe6\xa8\xa1\xe5\x9e\x8b"
        "\xe6\x96\x87\xe4\xbb\xb6\xe5\xae\x8c\xe6\x95\xb4\xe6\x80\xa7\xef\xbc\x8c"
        "\xe4\xb8\x8d\xe8\x87\xaa\xe5\x8a\xa8\xe4\xb8\x8b\xe8\xbd\xbd"));
    verifyButton.onClick = [this] { verifyModel(); };
    addAndMakeVisible(verifyButton);

    asrStatusLabel.setText(juce::String::fromUTF8("\xe5\x9c\xa8 Studio One \xe4\xb8\xad\xe9\x80\x89\xe4\xb8\xad\xe9\x9f\xb3\xe9\xa2\x91\xe7\x89\x87\xe6\xae\xb5\xe5\x90\x8e\xe5\x8f\xaf\xe8\xbf\x9b\xe8\xa1\x8c ASR \xe8\xaf\x86\xe5\x88\xab"),
                           juce::dontSendNotification);
    asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    asrStatusLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(asrStatusLabel);

    asrProgressBar.setTextToDisplay({});
    asrProgressBar.setVisible(false);
    addAndMakeVisible(asrProgressBar);

    //── ASR 回调 ────────────────────────────
    auto cleanupTemp = [this]()
    {
        if (tempAudioFile.exists())
        {
            tempAudioFile.deleteFile();
            tempAudioFile = {};
        }
    };

    asrProcessor.onResult = [this, cleanupTemp, documentController](const std::vector<CharacterTimestamp>& timestamps,
                                                  const juce::String& fullText)
    {
        cleanupTemp();
        asrProgressBar.setVisible(false);

        auto key = dataManager->getActiveKey();
        if (key.isEmpty()) return;

        // 同时存入 Processor 层和文档控制器的 dataManager
        dataManager->setASRResult(key, timestamps, fullText);
        if (documentController)
            documentController->getDataManager().setASRResult(key, timestamps, fullText);
        if (auto* activeSrc = dataManager->getActiveSource())
            onActiveSourceChanged(activeSrc);

        asrStatusLabel.setText(juce::String::fromUTF8("ASR \xe8\xaf\x86\xe5\x88\xab\xe5\xae\x8c\xe6\x88\x90"),
                               juce::dontSendNotification);
        asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::greenyellow);
        asrButton.setEnabled(true);
        transcriptEditor.setCaretPosition(0);
    };

    asrProcessor.onError = [this, cleanupTemp](const juce::String& errorMsg)
    {
        cleanupTemp();
        asrProgressBar.setVisible(false);
        asrStatusLabel.setText(juce::String::fromUTF8("ASR \xe9\x94\x99\xe8\xaf\xaf: ") + errorMsg,
                               juce::dontSendNotification);
        asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::orangered);
        asrButton.setEnabled(true);
    };

    asrProcessor.onProgress = [this](double pct)
    {
        asrProgressValue = pct;
        asrProgressBar.repaint();
    };

    startTimerHz(30);
    selectionPollCounter = 0;

    setResizable(true, false);
    setResizeLimits(600, 300, 3000, 2000);
    setSize(1200, 750);
}

TranscriptPluginEditor::~TranscriptPluginEditor()
{
    stopTimer();
    asrProcessor.cancel();

    if (currentRegion)
        currentRegion->removeListener(this);

    if (auto* editorView = getARAEditorView())
        editorView->removeListener(this);

    if (tempAudioFile.exists())
        tempAudioFile.deleteFile();
}

//==============================================================================
void TranscriptPluginEditor::refreshAfterStateRestore()
{
    if (auto* editorView = getARAEditorView())
    {
        auto sel = editorView->getViewSelection();
        auto regions = sel.getPlaybackRegions<juce::ARAPlaybackRegion>();
        if (!regions.empty())
        {
            auto* audioMod = regions.front()->getAudioModification<juce::ARAAudioModification>();
            if (auto* src = audioMod->getAudioSource())
            {
                dataManager->setActiveSource(src);
                return;
            }
        }
    }
    onActiveSourceChanged(dataManager->getActiveSource());
}

//==============================================================================
void TranscriptPluginEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xFF1a1a2e));

    if (getARAEditorView() == nullptr)
    {
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(18.0f));
        g.drawFittedText(juce::String::fromUTF8(
            "\xe6\xad\xa4\xe6\x8f\x92\xe4\xbb\xb6\xe4\xbb\x85\xe6\x94\xaf\xe6\x8c\x81"
            "\xe5\x9c\xa8 Studio One ARA \xe6\xa8\xa1\xe5\xbc\x8f\xe4\xb8\x8b\xe8\xbf"
            "\x90\xe8\xa1\x8c\xe3\x80\x82"),
                         getLocalBounds(), juce::Justification::centred, 1);
    }
}

void TranscriptPluginEditor::resized()
{
    auto bounds = getLocalBounds().reduced(8);

    auto controlArea = bounds.removeFromBottom(44);

    auto topStrip = bounds.removeFromTop(28);
    auto statusArea  = topStrip.removeFromTop(20);
    auto waveArea    = topStrip;

    asrStatusLabel.setBounds(statusArea);
    waveform.setBounds(waveArea);

    transcriptEditor.setBounds(bounds);

    asrProgressBar.setBounds(getWidth() - 350, 5, 340, 14);

    const int btnW = 60;
    const int comboW = 190;
    const int gap = 4;

    modelSelector.setBounds(controlArea.removeFromLeft(comboW).reduced(gap));
    downloadButton.setBounds(controlArea.removeFromLeft(btnW + 20).reduced(gap));
    verifyButton.setBounds(controlArea.removeFromLeft(btnW + 10).reduced(gap));
    asrButton.setBounds(controlArea.removeFromLeft(btnW).reduced(gap));
    cleanupButton.setBounds(controlArea.removeFromLeft(btnW).reduced(gap));
}

//==============================================================================
//  定时器：轮询播放头位置并驱动进度条 + 文本高亮
//==============================================================================
void TranscriptPluginEditor::timerCallback()
{
    auto& phs = processor.getPlayHeadState();
    bool isPlaying = phs.isPlaying.load(std::memory_order_relaxed);
    double absPos = phs.timeInSeconds.load(std::memory_order_relaxed);

    // 实时换算为音频块相对时间
    double sourceRelTime = juce::jmax(0.0, absoluteToSourceTime(absPos));

    waveform.setPlayheadPosition(sourceRelTime);

    //── 实时更新段落时间戳偏移量（适配音频块拖动、剪切） ──
    {
        double offset = (currentRegion != nullptr) ? (getRegionPlaybackStart() - getViewStart()) : 0.0;
        transcriptEditor.setTimeOffset(offset);
    }

    //── 定时轮询当前 ARA 选区（兜底跨轨断链） ──
    if (++selectionPollCounter >= 15)  // 每隔 ~500ms 检查一次
    {
        selectionPollCounter = 0;
        pollSelectionChanged();
    }

    if (!isPlaying || currentTimestamps == nullptr)
        return;

    for (const auto& ts : *currentTimestamps)
    {
        if (sourceRelTime >= ts.startTime && sourceRelTime < ts.endTime)
        {
            if (ts.globalTextIndex != lastHighlightedCharIndex)
            {
                lastHighlightedCharIndex = ts.globalTextIndex;
                transcriptEditor.highlightByTime(sourceRelTime);
            }
            return;
        }
    }
}

//==============================================================================
//  定时轮询选区 —— 跨轨移动后 ARAEditorView::Listener 可能断链，
//  本方法通过 timer 主动查询当前选区，确保数据不丢失。
//==============================================================================
void TranscriptPluginEditor::pollSelectionChanged()
{
    if (auto* editorView = getARAEditorView())
    {
        auto selection = editorView->getViewSelection();
        auto regions = selection.getPlaybackRegions<juce::ARAPlaybackRegion>();
        if (regions.empty())
            return;

        auto* region = regions.front();
        auto* audioMod = region->getAudioModification<juce::ARAAudioModification>();
        auto* audioSource = audioMod->getAudioSource();

        // 与当前 activeSource 对比，不一样则触发切换
        if (audioSource != nullptr && audioSource != dataManager->getActiveSource())
            onNewSelection(selection);
    }
}

//==============================================================================
//  计算当前切片在物理音频源中的视口时间区间
//
//  直接使用 ARA 官方时轴映射：
//    getStartInAudioModificationTime() — 切片在物理音频修改中的绝对起点
//    getDurationInAudioModificationTime() — 切片在物理音频修改中的持续时长
//  这两个值由 Studio One 底层直接设置，不受播放位置、排序影响。
//==============================================================================
juce::Range<double> TranscriptPluginEditor::getCurrentViewRange() const
{
    if (currentRegion == nullptr)
        return {0.0, std::numeric_limits<double>::max()};

    auto viewStart = currentRegion->getStartInAudioModificationTime();
    auto viewEnd   = currentRegion->getEndInAudioModificationTime();

    return {viewStart, viewEnd};
}
void TranscriptPluginEditor::onNewSelection(const juce::ARAViewSelection& selection)
{
    auto regions = selection.getPlaybackRegions<juce::ARAPlaybackRegion>();
    if (regions.empty())
        return;

    // 解除旧区域的监听
    if (currentRegion)
        currentRegion->removeListener(this);

    currentRegion = regions.front();
    auto* audioMod = currentRegion->getAudioModification<juce::ARAAudioModification>();
    auto* audioSource = audioMod->getAudioSource();

    // 监听新区域的属性变化（用户拖动时更新时轴偏移）
    currentRegion->addListener(this);

    if (audioSource != nullptr)
        dataManager->setActiveSource(audioSource);
}

//==============================================================================
//  ARA 播放区域属性即将变化 —— 用户拖动音频块时触发
//==============================================================================
void TranscriptPluginEditor::willUpdatePlaybackRegionProperties(
    juce::ARAPlaybackRegion* region,
    juce::ARAPlaybackRegion::PropertiesPtr newProperties)
{
    juce::ignoreUnused(region, newProperties);
    // currentRegion 的 TimeRange 在属性更新后会自动反映新位置，
    // 我们只需确保各成员方法在每次调用时实时读取 getRegionPlaybackStart()
    // 不需要手动缓存任何值。
}

//==============================================================================
//  ARA 播放区域即将销毁 —— 剪切/删除音频块时安全清除引用，防止死锁与崩溃
//==============================================================================
void TranscriptPluginEditor::willDestroyPlaybackRegion(juce::ARAPlaybackRegion* region)
{
    if (region == currentRegion)
    {
        currentRegion->removeListener(this);
        currentRegion = nullptr;

        // 安全清空相关 UI 状态
        transcriptEditor.clear();
        transcriptEditor.setTimestamps(nullptr);
        transcriptEditor.setParagraphTimestamps({});
        currentTimestamps = nullptr;
        filteredTimestamps.clear();
        filteredFullText.clear();
        waveform.setAudioSource(nullptr);
        lastHighlightedCharIndex = -1;
    }
}

//==============================================================================
//  激活源切换 —— 刷新进度条和文本
//==============================================================================
void TranscriptPluginEditor::onActiveSourceChanged(juce::ARAAudioSource* source)
{
    lastHighlightedCharIndex = -1;

    if (source == nullptr)
    {
        waveform.setAudioSource(nullptr);
        transcriptEditor.clear();
        transcriptEditor.setTimestamps(nullptr);
        transcriptEditor.setParagraphTimestamps({});
        currentTimestamps = nullptr;
        asrButton.setEnabled(false);
        asrStatusLabel.setText(juce::String::fromUTF8(
            "\xe8\xaf\xb7\xe9\x80\x89\xe4\xb8\xad\xe4\xb8\x80\xe4\xb8\xaa\xe9\x9f\xb3"
            "\xe9\xa2\x91\xe7\x89\x87\xe6\xae\xb5"),
                               juce::dontSendNotification);
        asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
        return;
    }

    waveform.setAudioSource(source);

    auto key = TranscriptDataManager::makeSourceKey(source);
    auto* data = dataManager->getDataForKey(key);
    if (data != nullptr && data->asrComplete)
    {
        // 按当前视口过滤：直接用时间戳判断，逐字重织新文本
        auto viewRange = getCurrentViewRange();
        DBG("====== [VIEW RANGE CHECK] ======");
        DBG("viewStart: " + juce::String(viewRange.getStart()));
        DBG("viewEnd:   " + juce::String(viewRange.getEnd()));
        DBG("Total source timestamps: " + juce::String((int)data->timestamps.size()));
        DBG("================================");
        filteredTimestamps.clear();
        filteredFullText.clear();

        // 预估内存并预分配，避免高频字符串内存扩容
        {
            size_t estimatedBytes = 0;
            for (const auto& ts : data->timestamps)
            {
                if (ts.startTime >= viewRange.getStart() && ts.startTime < viewRange.getEnd())
                    estimatedBytes += ts.character.length() + 1; // +1 为可能的换行符
            }
            filteredFullText.preallocateBytes(estimatedBytes);
        }

        std::vector<TranscriptEditor::ParagraphTimestamp> paragraphTimestamps;
        bool isFirstParagraph = true;

        for (size_t i = 0; i < data->timestamps.size(); ++i)
        {
            const auto& ts = data->timestamps[i];

            // 只通过时间戳判断是否在当前切片视口内
            if (ts.startTime >= viewRange.getStart() && ts.startTime < viewRange.getEnd())
            {
                // 段落首字：补换行（时间戳不加入文本内容）
                if (ts.isParagraphStart && !isFirstParagraph)
                    filteredFullText += "\n";

                auto adjusted = ts;
                adjusted.globalTextIndex = filteredFullText.length();
                filteredFullText += ts.character;
                filteredTimestamps.push_back(adjusted);

                // 记录段落时间戳（单独存储，不参与文本内容）
                // 视口首字（可能是剪切产生的断点）总是显示时间戳
                if (paragraphTimestamps.empty())
                    paragraphTimestamps.push_back({adjusted.globalTextIndex, ts.startTime});
                else if (ts.isParagraphStart)
                    paragraphTimestamps.push_back({adjusted.globalTextIndex, ts.startTime});

                isFirstParagraph = false;
            }
        }

        transcriptEditor.setText(filteredFullText, juce::dontSendNotification);
        transcriptEditor.setTimestamps(&filteredTimestamps);
        transcriptEditor.setParagraphTimestamps(paragraphTimestamps);
        transcriptEditor.setTimeOffset(getRegionPlaybackStart() - getViewStart());
        currentTimestamps = &filteredTimestamps;
        asrButton.setEnabled(true);
        asrStatusLabel.setText(juce::String::fromUTF8("ASR \xe5\xb7\xb2\xe5\xae\x8c\xe6\x88\x90"),
                               juce::dontSendNotification);
        asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::greenyellow);
    }
    else
    {
        transcriptEditor.clear();
        transcriptEditor.setTimestamps(nullptr);
        currentTimestamps = nullptr;
        asrButton.setEnabled(true);
        asrStatusLabel.setText(juce::String::fromUTF8(
            "\xe7\x82\xb9\xe5\x87\xbb ASR \xe5\xbc\x80\xe5\xa7\x8b\xe8\xaf\x86\xe5\x88\xab"),
                               juce::dontSendNotification);
        asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    }
}

//==============================================================================
//  文本点击 → 通知宿主跳转播放位置
//  使用 currentRegion->getTimeRange() 实时获取最新时间线位置
//==============================================================================
void TranscriptPluginEditor::syncTextToAudio(int charIndex)
{
    double wordTime = findTimeByCharIndex(charIndex);
    if (wordTime < 0.0) return;

    waveform.setPlayheadPosition(wordTime);

    double absoluteHostTime = sourceTimeToAbsolute(wordTime);

    if (auto* editorView = getARAEditorView())
    {
        auto* dc = editorView->getDocumentController();
        if (auto* playbackCtrl = dc->getHostPlaybackController())
            playbackCtrl->requestSetPlaybackPosition(absoluteHostTime);
    }
}

//==============================================================================
//  进度条点击 → 高亮文本 + 驱动跳转
//==============================================================================
void TranscriptPluginEditor::syncAudioToText(double timeInSeconds)
{
    waveform.setPlayheadPosition(timeInSeconds);

    lastHighlightedCharIndex = -1;
    transcriptEditor.highlightByTime(timeInSeconds);

    // 实时换算为宿主绝对时间线
    double absoluteHostTime = sourceTimeToAbsolute(timeInSeconds);
    if (auto* editorView = getARAEditorView())
    {
        auto* dc = editorView->getDocumentController();
        if (auto* playbackCtrl = dc->getHostPlaybackController())
            playbackCtrl->requestSetPlaybackPosition(absoluteHostTime);
    }
}

//==============================================================================
double TranscriptPluginEditor::findTimeByCharIndex(int charIndex) const
{
    if (currentTimestamps == nullptr) return -1.0;

    for (auto& ts : *currentTimestamps)
    {
        int end = ts.globalTextIndex + ts.character.length();
        if (charIndex >= ts.globalTextIndex && charIndex < end)
            return ts.startTime;
    }
    return -1.0;
}

//==============================================================================
//  下载模型到本地缓存
//==============================================================================
void TranscriptPluginEditor::downloadModel()
{
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

    downloadButton.setEnabled(false);
    asrButton.setEnabled(false);
    asrStatusLabel.setText(juce::String::fromUTF8(
        "\xe6\xad\xa3\xe5\x9c\xa8\xe4\xb8\x8b\xe8\xbd\xbd\xe6\xa8\xa1\xe5\x9e\x8b: ")
        + chosen + " ...", juce::dontSendNotification);
    asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);

    asrProcessor.onDownloadComplete = [this](bool success, const juce::String& msg)
    {
        downloadButton.setEnabled(true);
        asrButton.setEnabled(true);
        asrStatusLabel.setText(msg, juce::dontSendNotification);
        asrStatusLabel.setColour(juce::Label::textColourId,
            success ? juce::Colours::greenyellow : juce::Colours::orangered);
    };

    asrProcessor.startDownload(chosen);
}

//==============================================================================
//  联网校验模型完整性
//==============================================================================
void TranscriptPluginEditor::verifyModel()
{
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

    verifyButton.setEnabled(false);
    asrStatusLabel.setText(juce::String::fromUTF8(
        "\xe6\xad\xa3\xe5\x9c\xa8\xe8\x81\x94\xe7\xbd\x91\xe6\xa0\xa1\xe9\xaa\x8c"
        "\xe6\xa8\xa1\xe5\x9e\x8b: ") + chosen + " ...",
        juce::dontSendNotification);
    asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);

    asrProcessor.onDownloadComplete = [this](bool success, const juce::String& msg)
    {
        verifyButton.setEnabled(true);
        asrStatusLabel.setText(msg, juce::dontSendNotification);
        asrStatusLabel.setColour(juce::Label::textColourId,
            success ? juce::Colours::greenyellow : juce::Colours::orangered);
    };

    asrProcessor.startVerify(chosen);
}

//==============================================================================
//  清理所有残留文件和保存的数据
//==============================================================================
void TranscriptPluginEditor::cleanupAll()
{
    // 1. 删除临时音频文件
    if (tempAudioFile.exists())
    {
        tempAudioFile.deleteFile();
        tempAudioFile = {};
    }

    // 2. 删除调试日志（asr_debug.log，在宿主可执行文件同目录）
    auto logFile = ASRProcessor::getDebugLogFile();
    if (logFile.exists())
        logFile.deleteFile();

    // 3. 删除 ASR 调试转储（asr_debug_dump.json，在用户主目录）
    auto dumpFile = juce::File::getSpecialLocation(
        juce::File::userHomeDirectory).getChildFile("asr_debug_dump.json");
    if (dumpFile.exists())
        dumpFile.deleteFile();

    // 4. 清除所有转录数据（内存 + 下次 DAW 保存时 ARA 持久化也会清空）
    dataManager->clear();

    // 5. 清除 UI
    transcriptEditor.clear();
    transcriptEditor.setTimestamps(nullptr);
    transcriptEditor.setParagraphTimestamps({});
    currentTimestamps = nullptr;
    waveform.setAudioSource(nullptr);
    lastHighlightedCharIndex = -1;
    filteredTimestamps.clear();
    filteredFullText.clear();

    // 6. 刷新 UI 到当前选区（显示"无数据"状态）
    if (currentRegion)
    {
        auto* audioMod = currentRegion->getAudioModification<juce::ARAAudioModification>();
        if (auto* src = audioMod->getAudioSource())
        {
            dataManager->setActiveSource(src);
            // setActiveSource -> onActiveSourceChanged 会刷新 UI
        }
    }
    else
    {
        onActiveSourceChanged(nullptr);
    }

    asrStatusLabel.setText(juce::String::fromUTF8(
        "\xe5\xb7\xb2\xe6\xb8\x85\xe9\x99\xa4\xe6\x89\x80\xe6\x9c\x89\xe7\xbc\x93"
        "\xe5\xad\x98\xe5\x92\x8c\xe4\xbf\x9d\xe5\xad\x98\xe7\x9a\x84\xe6\x95\xb0\xe6\x8d\xae"),
        juce::dontSendNotification);
    asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
}

//==============================================================================
//  启动 ASR 识别
//==============================================================================
void TranscriptPluginEditor::startASR()
{
    auto* activeSrc = dataManager->getActiveSource();
    if (activeSrc == nullptr)
    {
        asrStatusLabel.setText(juce::String::fromUTF8(
            "\xe6\xb2\xa1\xe6\x9c\x89\xe9\x80\x89\xe4\xb8\xad\xe7\x9a\x84\xe9\x9f\xb3"
            "\xe9\xa2\x91\xe7\x89\x87\xe6\xae\xb5"),
                               juce::dontSendNotification);
        return;
    }

    juce::File tempFile = juce::File::createTempFile(".wav");
    {
        std::unique_ptr<juce::ARAAudioSourceReader> reader(
            new juce::ARAAudioSourceReader(activeSrc));

        if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        {
            asrStatusLabel.setText(juce::String::fromUTF8(
                "\xe6\x97\xa0\xe6\xb3\x95\xe8\xaf\xbb\xe5\x8f\x96\xe9\x9f\xb3\xe9\xa2"
                "\x91\xe6\x95\xb0\xe6\x8d\xae"),
                                   juce::dontSendNotification);
            asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::orangered);
            return;
        }

        auto outStream = std::make_unique<juce::FileOutputStream>(tempFile);
        if (!outStream->openedOk())
        {
            asrStatusLabel.setText(juce::String::fromUTF8(
                "\xe6\x97\xa0\xe6\xb3\x95\xe5\x88\x9b\xe5\xbb\xba\xe4\xb8\xb4\xe6\x97"
                "\xb6\xe9\x9f\xb3\xe9\xa2\x91\xe6\x96\x87\xe4\xbb\xb6"),
                                   juce::dontSendNotification);
            return;
        }

        juce::WavAudioFormat wavFormat;
        auto opts = juce::AudioFormatWriterOptions{}
            .withSampleRate(reader->sampleRate)
            .withNumChannels((int)reader->numChannels)
            .withBitsPerSample(16);

        std::unique_ptr<juce::OutputStream> streamPtr = std::move(outStream);
        auto writer = wavFormat.createWriterFor(streamPtr, opts);
        if (writer == nullptr)
        {
            asrStatusLabel.setText(juce::String::fromUTF8(
                "\xe6\x97\xa0\xe6\xb3\x95\xe5\x86\x99\xe5\x85\xa5 WAV"),
                                   juce::dontSendNotification);
            return;
        }

        const int blockSize = 65536;
        juce::AudioBuffer<float> tempBuf((int)reader->numChannels, blockSize);
        juce::int64 samplesWritten = 0;
        while (samplesWritten < reader->lengthInSamples)
        {
            int toRead = (int)juce::jmin((juce::int64)blockSize,
                                          reader->lengthInSamples - samplesWritten);
            reader->read(&tempBuf, 0, toRead, samplesWritten, true, true);
            writer->writeFromAudioSampleBuffer(tempBuf, 0, toRead);
            samplesWritten += toRead;
        }
        writer->flush();
    }

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
    asrStatusLabel.setText(juce::String::fromUTF8("ASR \xe8\xaf\x86\xe5\x88\xab\xe4\xb8\xad (")
                           + chosen + ") ...",
                           juce::dontSendNotification);
    asrStatusLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);

    asrProcessor.start(tempFile);

    tempAudioFile = tempFile;
}
