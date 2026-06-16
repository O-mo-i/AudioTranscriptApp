#include "ASRProcessor.h"
#include <juce_events/juce_events.h>

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif

// 文件日志辅助 — 同时写 DBG 和文件
static void logToFile(const juce::String& msg)
{
    DBG(msg);
    auto logFile = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile).getParentDirectory()
        .getChildFile("asr_debug.log");
    logFile.appendText(msg + "\n", false, false);
}

// 获取 .vst3 插件 DLL 自身的路径（非宿主路径）
// 用 VirtualQuery 获取本函数所在内存区域对应的模块基址，再转成文件路径
#if JUCE_WINDOWS
static juce::File getPluginDllPath()
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(&getPluginDllPath, &mbi, sizeof(mbi)) == 0)
        return {};

    auto hMod = static_cast<HMODULE>(mbi.AllocationBase);
    WCHAR path[MAX_PATH + 1]{};
    if (GetModuleFileNameW(hMod, path, MAX_PATH) == 0)
        return {};

    auto result = juce::File(path);
    logToFile("[ASR] plugin DLL self path: " + result.getFullPathName());
    return result;
}
#endif

juce::String ASRProcessor::pythonPath;
juce::String ASRProcessor::modelName       = "openai/whisper-small";
juce::String ASRProcessor::deviceName      = "cuda";
juce::String ASRProcessor::scriptsDirectory;
bool ASRProcessor::offlineMode  = true;    // 默认离线模式
juce::String ASRProcessor::modelDirectory;

ASRProcessor::ASRProcessor()
    : juce::Thread("ASR Worker") {}

ASRProcessor::~ASRProcessor() { cancel(); }

void ASRProcessor::start(const juce::File& file)
{
    cancel();
    audioFile = file;
    isActive = true;
    startThread();
}

void ASRProcessor::cancel()
{
    if (isThreadRunning())
    {
        stopThread(5000);
        isActive = false;
    }
}

//==============================================================================
void ASRProcessor::callError(const juce::String& msg)
{
    isActive = false;
    logToFile("[ASR] ERROR: " + msg);

    if (onError)
    {
        juce::MessageManager::callAsync([this, msg]()
        {
            onError(msg);
        });
    }
}

//==============================================================================
void ASRProcessor::run()
{
    isActive = true;

    auto quote = [](const juce::String& s) { return "\"" + s + "\""; };

    // ── 从 .vst3 插件 DLL 自身路径查找 scripts ─────────
    // 目录结构：
    //   VST3安装目录/
    //     scripts/
    //       asr_worker.py
    //       .venv/Scripts/python.exe
    //     Audio Transcript Editor.vst3/
    //       Contents/x86_64-win/Audio Transcript Editor.vst3  ← DLL 自身
    //
    // getPluginDllPath() 返回 DLL 路径，即 .vst3 文件本身。
    // 向上走两级（x86_64-win/ → Contents/ → .vst3/）得到 .vst3 目录，
    // 再取父目录即 VST3 安装目录，scripts/ 同级放置。
    // ──────────────────────────────────────────────────────────
    juce::File vst3Dir;

   #if JUCE_WINDOWS
    auto dllFile = getPluginDllPath();
    if (dllFile.exists())
    {
        // DLL 在 x86_64-win/ 中 → 父目录 → 父目录 = .vst3 目录 → 父目录 = 安装目录
        auto dllParent = dllFile.getParentDirectory();   // x86_64-win/
        auto contentsDir = dllParent.getParentDirectory();   // Contents/
        auto bundleDir   = contentsDir.getParentDirectory(); // .vst3/

        if (bundleDir.getFileExtension().toLowerCase() == ".vst3")
            vst3Dir = bundleDir.getParentDirectory();
    }
   #endif

    // 开发回退：从构建目录的产物路径定位
    if (vst3Dir == juce::File{})
    {
        auto exeDir = juce::File::getSpecialLocation(
            juce::File::currentExecutableFile).getParentDirectory();
        vst3Dir = exeDir.getParentDirectory().getParentDirectory();
    }

    logToFile("[ASR] vst3 base dir: " + vst3Dir.getFullPathName());

    juce::File scriptsDir = vst3Dir.getChildFile("scripts");
    juce::File pythonExeFile = scriptsDir.getChildFile(".venv")
                                        .getChildFile("Scripts")
                                        .getChildFile("python.exe");
    juce::File pythonScriptFile = scriptsDir.getChildFile("asr_worker.py");

    if (!pythonExeFile.existsAsFile())
    {
        callError("Python not found at: " + pythonExeFile.getFullPathName()
                  + "\nPlease copy the 'scripts' folder next to the VST3 plugin.");
        return;
    }

    if (!pythonScriptFile.existsAsFile())
    {
        callError("Script not found at: " + pythonScriptFile.getFullPathName()
                  + "\nPlease copy the 'scripts' folder next to the VST3 plugin.");
        return;
    }

    logToFile("[ASR] python: " + pythonExeFile.getFullPathName());
    logToFile("[ASR] script: " + pythonScriptFile.getFullPathName());
    logToFile("[ASR] audio : " + audioFile.getFullPathName());

    // 构建命令行
    juce::String cmd;
    cmd << quote(pythonExeFile.getFullPathName())
        << " " << quote(pythonScriptFile.getFullPathName())
        << " --audio " << quote(audioFile.getFullPathName())
        << " --device " << deviceName
        << " --model " << quote(modelName);

    // 离线模式
    if (offlineMode)
        cmd << " --offline";

    // 本地模型目录
    if (modelDirectory.isNotEmpty())
        cmd << " --model-dir " << quote(modelDirectory);

    logToFile(juce::String("[ASR] offline: ") + (offlineMode ? "yes" : "no"));
    if (modelDirectory.isNotEmpty())
        logToFile("[ASR] model-dir: " + modelDirectory);

    logToFile("[ASR] cmd: " + cmd);

    // 启动子进程（同时捕获 stdout 和 stderr）
    juce::ChildProcess proc;
    if (!proc.start(cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        callError("Failed to start Python process. Make sure Python is installed and on PATH.");
        return;
    }

    // ── 按行流式读取子进程输出 ──────────────────────────
    juce::MemoryBlock lineBuf;
    juce::String jsonLine;
    char readBuf[4096];

    while (proc.isRunning() && !threadShouldExit())
    {
        if (auto n = proc.readProcessOutput(readBuf, sizeof(readBuf)))
        {
            lineBuf.append(readBuf, n);

            auto* data = static_cast<const char*>(lineBuf.getData());
            auto size = lineBuf.getSize();
            int start = 0;

            for (int i = 0; i < size; ++i)
            {
                if (data[i] == '\n')
                {
                    if (i > start)
                    {
                        juce::String line = juce::String::fromUTF8(data + start, i - start);
                        line = line.trim();

                        if (line.startsWith("PROGRESS:"))
                        {
                            int pct = line.fromFirstOccurrenceOf("PROGRESS:", false, true)
                                        .trim().getIntValue();
                            if (onProgress)
                            {
                                juce::MessageManager::callAsync([this, pct]()
                                {
                                    onProgress(juce::jlimit(0.0, 1.0, pct / 100.0));
                                });
                            }
                        }
                        else if (line.startsWith("{") && line.endsWith("}"))
                        {
                            jsonLine = line;
                        }
                    }
                    start = i + 1;
                }
            }

            auto remaining = size - start;
            if (remaining > 0 && start > 0)
                memmove(lineBuf.getData(), data + start, remaining);
            lineBuf.setSize(remaining);
        }
        else
        {
            wait(50);
        }
    }

    // 进程退出后，读取残余数据
    char tail[4096];
    while (auto n = proc.readProcessOutput(tail, sizeof(tail)))
        lineBuf.append(tail, n);

    // 处理最后一行（可能没有换行结尾）
    if (lineBuf.getSize() > 0)
    {
        auto* data = static_cast<const char*>(lineBuf.getData());
        juce::String lastLine = juce::String::fromUTF8(data, (int)lineBuf.getSize());
        lastLine = lastLine.trim();

        if (lastLine.startsWith("{") && lastLine.endsWith("}"))
            jsonLine = lastLine;
    }

    // ── 解析 JSON ───────────────────────────────────────
    logToFile("[ASR] process exit code: " + juce::String(proc.getExitCode()));
    logToFile("[ASR] jsonLine length: " + juce::String(jsonLine.length()) + " chars");

    if (jsonLine.isEmpty())
    {
        logToFile("[ASR] no JSON line found in process output");
        callError("ASR process produced no JSON output");
        return;
    }

    auto json = juce::JSON::parse(jsonLine);
    if (json == juce::var() || !json.hasProperty("success"))
    {
        logToFile("[ASR] raw jsonLine:\n" + jsonLine);
        callError("ASR returned invalid JSON");
        return;
    }
    if (!(bool)json["success"])
    {
        auto errMsg = json["error"].toString();
        logToFile("[ASR] python error: " + errMsg);
        callError(errMsg);
        return;
    }

    // 解析 words -> CharacterTimestamp
    auto wordsVar = json["words"];
    if (!wordsVar.isArray())
    {
        callError("ASR 'words' is not an array");
        return;
    }

    std::vector<CharacterTimestamp> timestamps;
    juce::String fullText;
    auto* arr = wordsVar.getArray();

    bool isFirstParagraph = true;

    for (int i = 0; i < arr->size(); ++i)
    {
        auto& item = (*arr)[i];
        bool isParagraphStart = item["is_paragraph_start"];

        if (isParagraphStart)
        {
            const double t = (double)item["start"];
            if (!isFirstParagraph)
                fullText += "\n";
            fullText += juce::String::formatted("%02d:%02d ",
                (int)(t / 60.0), ((int)t) % 60);
        }
        isFirstParagraph = false;

        CharacterTimestamp ts;
        ts.character          = item["word"].toString();
        ts.startTime          = (double)item["start"];
        ts.endTime            = (double)item["end"];
        ts.globalTextIndex    = fullText.length();
        ts.isParagraphStart   = isParagraphStart;
        fullText += ts.character;
        timestamps.push_back(ts);
    }

    isActive = false;

    // === 诊断：校验 C++ 实际收到的字符数 ===
    logToFile("====== [ASR DATA CHECK] ======");
    logToFile("Total characters parsed in C++: " + juce::String((int)timestamps.size()));
    if (!timestamps.empty())
    {
        logToFile("First char startTime: " + juce::String(timestamps.front().startTime));
        logToFile("Last char startTime:  " + juce::String(timestamps.back().startTime));
        logToFile("First char text: \"" + timestamps.front().character + "\"");
        logToFile("Last char text:  \"" + timestamps.back().character + "\"");
    }
    logToFile("jsonLine length:   " + juce::String((int)jsonLine.length()) + " chars");
    logToFile("================================");

    // 回调到主线程
    if (onResult && !threadShouldExit())
    {
        juce::MessageManager::callAsync([this, timestamps, fullText]()
        {
            onResult(timestamps, fullText);
        });
    }
}

//==============================================================================
void ASRProcessor::setPythonPath(const juce::String& path)      { pythonPath = path; }
void ASRProcessor::setModelName(const juce::String& m)         { modelName  = m; }
void ASRProcessor::setDevice(const juce::String& d)            { deviceName = d; }
void ASRProcessor::setScriptsDirectory(const juce::String& p)  { scriptsDirectory = p; }
void ASRProcessor::setOfflineMode(bool offline)                { offlineMode = offline; }
void ASRProcessor::setModelDirectory(const juce::String& p)    { modelDirectory = p; }
