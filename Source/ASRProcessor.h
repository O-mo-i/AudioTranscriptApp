#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include "CharacterTimestamp.h"

//==============================================================================
/**
 * ASR 后台处理器。
 * 在独立线程中通过 juce::ChildProcess 调用 asr_worker.py，
 * 解析返回的 JSON，填充 CharacterTimestamp 数组。
 *
 * 支持两种启动方式：
 *   start(audioFile)       — 直接处理已存在的 WAV 文件
 *   startFromSource(...)   — 从 AudioFormatReader 导出 WAV 后再处理（后台线程完成导出）
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

    /**
     * 从 AudioFormatReader 导出 WAV 后启动 ASR 识别。
     * WAV 导出在后台线程完成，不阻塞主线程。
     * readerFactory 在后台线程中被调用用于创建 reader。
     */
    void startFromSource(std::function<std::unique_ptr<juce::AudioFormatReader>()> readerFactory,
                          const juce::File& outputFile);

    /** 下载模型到本地缓存（异步，后台线程运行）。
     *  完成后通过 onDownloadComplete 回调报告结果。
     */
    void startDownload(const juce::String& modelName);

    /** 联网校验模型完整性（异步，后台线程运行）。
     *  完成后通过 onDownloadComplete 回调报告结果。
     */
    void startVerify(const juce::String& modelName);

    /** 取消当前正在进行的操作 */
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

    /** WAV 导出进度回调（0.0 ~ 1.0，在主线程调用） */
    std::function<void(double progress)> onExportProgress;

    /** 下载完成回调（在主线程调用） */
    std::function<void(bool success, const juce::String& message)> onDownloadComplete;

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

    /** 设置本地模型目录路径。
     *  例如: ASRProcessor::setModelDirectory("C:/Models/Qwen3-ASR");
     *  如果不设置，从 HuggingFace 缓存目录加载。
     */
    static void setModelDirectory(const juce::String& path);

    /** 获取本地模型目录路径 */
    static juce::String getModelDirectory() noexcept { return modelDirectory; }

    /** 获取 ASR 调试日志文件路径（asr_debug.log，在宿主可执行文件同目录） */
    static juce::File getDebugLogFile();

private:
    /** 模型操作类型 */
    enum class ModelOp { kNone, kDownload, kVerify };

    void run() override;

    /** 后台执行 WAV 导出 */
    bool runWavExport();
    /** 后台执行 ASR 识别 */
    void runASR();
    /** 后台执行模型操作 */
    void runModelOp();
    void callError(const juce::String& msg);

    juce::File audioFile;
    juce::String downloadModelName;
    std::atomic<bool> isActive{ false };
    ModelOp currentModelOp{ ModelOp::kNone };

    /** startFromSource 参数 */
    std::function<std::unique_ptr<juce::AudioFormatReader>()> readerFactory;
    juce::File exportFile;

    static juce::String pythonPath;
    static juce::String modelName;
    static juce::String deviceName;
    static juce::String scriptsDirectory;
    static juce::String modelDirectory;

    static constexpr int wavExportBlockSize = 65536;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ASRProcessor)
};
