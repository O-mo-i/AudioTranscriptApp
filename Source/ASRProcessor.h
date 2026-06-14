#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include "CharacterTimestamp.h"

//==============================================================================
/**
 * ASR 后台处理器。
 * 在独立线程中通过 juce::ChildProcess 调用 asr_worker.py，
 * 解析返回的 JSON，填充 CharacterTimestamp 数组。
 *
 * 用法:
 *   ASRProcessor proc;
 *   proc.onResult = [](auto& timestamps, auto& text) { ... };
 *   proc.start(audioFile);
 */
class ASRProcessor : private juce::Thread
{
public:
    ASRProcessor();
    ~ASRProcessor() override;

    /** 启动 ASR 识别（异步，后台线程运行） */
    void start(const juce::File& audioFile);

    /** 取消当前正在进行的识别 */
    void cancel();

    /** 是否正在运行 */
    bool isRunning() const { return isActive; }

    //==============================================================================
    /** 识别成功回调（在主线程调用） */
    std::function<void(const std::vector<CharacterTimestamp>& timestamps,
                        const juce::String& fullText)> onResult;

    /** 识别错误回调（在主线程调用） */
    std::function<void(const juce::String& errorMsg)> onError;

    /** 进度回调（0.0 ~ 1.0，在主线程调用） */
    std::function<void(double progress)> onProgress;

    //==============================================================================
    /** 设置 Python 解释器路径 (默认自动查找) */
    static void setPythonPath(const juce::String& path);

    /** 设置 ASR 模型名称 */
    static void setModelName(const juce::String& model);

    /** 设置推理设备 */
    static void setDevice(const juce::String& device);

private:
    void run() override;
    void callError(const juce::String& msg);

    juce::File audioFile;
    std::atomic<bool> isActive{ false };

    static juce::String pythonPath;
    static juce::String modelName;
    static juce::String deviceName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ASRProcessor)
};
