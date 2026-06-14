#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ARADocumentController.h"

//==============================================================================
TranscriptPluginProcessor::TranscriptPluginProcessor()
    : juce::AudioProcessor(getBusesProperties())
{
}

TranscriptDataManager& TranscriptPluginProcessor::getDataManager() noexcept
{
    if (auto* gdm = TranscriptDocumentController::getGlobalDataManager())
        return *gdm;
    return fallbackDataManager;
}

TranscriptPluginProcessor::~TranscriptPluginProcessor() = default;

//==============================================================================
void TranscriptPluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    playHeadState.update(juce::nullopt);
    prepareToPlayForARA(sampleRate, samplesPerBlock,
                        getMainBusNumOutputChannels(), getProcessingPrecision());
}

void TranscriptPluginProcessor::releaseResources()
{
    playHeadState.update(juce::nullopt);
    releaseResourcesForARA();
}

bool TranscriptPluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

void TranscriptPluginProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                              juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;

    auto* audioPlayHead = getPlayHead();
    playHeadState.update(audioPlayHead != nullptr
                         ? audioPlayHead->getPosition()
                         : juce::nullopt);

    // 让 ARA 扩展处理（如果没有 ARA 绑定，processBlockForARA 返回 false）
    if (!processBlockForARA(buffer, isRealtime(), audioPlayHead))
        processBlockBypassed(buffer, midiMessages);
}

//==============================================================================
double TranscriptPluginProcessor::getTailLengthSeconds() const
{
    double tail;
    if (getTailLengthSecondsForARA(tail))
        return tail;
    return 0.0;
}

//==============================================================================
bool TranscriptPluginProcessor::hasEditor() const  { return true; }

juce::AudioProcessorEditor* TranscriptPluginProcessor::createEditor()
{
    return new TranscriptPluginEditor(*this);
}

//==============================================================================
void TranscriptPluginProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // ARA 插件通过 doStoreObjectsToStream 持久化
    juce::ignoreUnused(destData);
}

void TranscriptPluginProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    // ARA 插件通过 doRestoreObjectsFromStream 恢复
    juce::ignoreUnused(data, sizeInBytes);
}

//==============================================================================
juce::AudioProcessor::BusesProperties TranscriptPluginProcessor::getBusesProperties()
{
    return BusesProperties().withInput("Input",  juce::AudioChannelSet::stereo(), true)
                            .withOutput("Output", juce::AudioChannelSet::stereo(), true);
}

//==============================================================================
//  JUCE 插件入口
//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TranscriptPluginProcessor();
}

//==============================================================================
//  ARA 工厂 — 当宿主启用 ARA 时，JUCE 通过此函数创建文档控制器
//==============================================================================
#if JucePlugin_Enable_ARA
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<TranscriptDocumentController>();
}
#endif
