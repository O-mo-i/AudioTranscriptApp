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

    /** 设置 scripts 目录路径（包含 asr_worker.py 的文件夹）。
     *  安装 VST3 后需要指向 scripts/ 所在的实际路径。
     *  例如: ASRProcessor::setScriptsDirectory("C:/MyApp/scripts");
     */
    static void setScriptsDirectory(const juce::String& path);

    //── 离线模式 ──────────────────────────────
    /** 设置离线模式。离线模式下禁止所有网络请求，仅从本地缓存加载模型。
     *  默认为 true（离线模式）。如果模型不在本地缓存中会给出清晰提示。
     */
    static void setOfflineMode(bool offline);

    /** 获取当前是否处于离线模式 */
    static bool isOfflineMode() noexcept { return offlineMode; }

    /** 设置本地模型目录路径（离线模式下从此路径加载模型）。
     *  例如: ASRProcessor::setModelDirectory("C:/Models/Qwen3-ASR");
     *  如果不设置，离线模式会从 HuggingFace 缓存目录加载。
     */
    static void setModelDirectory(const juce::String& path);

    /** 获取本地模型目录路径 */
    static juce::String getModelDirectory() noexcept { return modelDirectory; }

private:
    void run() override;
    void callError(const juce::String& msg);

    juce::File audioFile;
    std::atomic<bool> isActive{ false };

    static juce::String pythonPath;
    static juce::String modelName;
    static juce::String deviceName;
    static juce::String scriptsDirectory;
    static bool offlineMode;
    static juce::String modelDirectory;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ASRProcessor)
};
