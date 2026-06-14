#include "ASRProcessor.h"
#include <juce_events/juce_events.h>

// 文件日志辅助 — 同时写 DBG 和文件
static void logToFile(const juce::String& msg)
{
    DBG(msg);
    auto logFile = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile).getParentDirectory()
        .getChildFile("asr_debug.log");
    logFile.appendText(msg + "\n", false, false);
}

juce::String ASRProcessor::pythonPath;
juce::String ASRProcessor::modelName  = "openai/whisper-small";
juce::String ASRProcessor::deviceName = "cuda";

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
//  工具函数（必须在 run() 之前定义）
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

    // 定位 asr_worker.py — 从 .exe 所在目录向上追溯
    juce::File projectDir = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile).getParentDirectory();

    int depth = 0;
    while (projectDir.exists() && !projectDir.getChildFile("scripts").isDirectory()
           && depth < 5)
    {
        projectDir = projectDir.getParentDirectory();
        depth++;
    }

    logToFile("[ASR] exe dir: "
        + juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName());
    logToFile("[ASR] project dir candidate (depth=" + juce::String(depth) + "): "
        + projectDir.getFullPathName());

    juce::File pythonScript = projectDir.getChildFile("scripts")
                                  .getChildFile("asr_worker.py");
    logToFile("[ASR] pythonScript path: " + pythonScript.getFullPathName()
        + "  exists=" + (pythonScript.existsAsFile() ? "yes" : "no"));

    if (!pythonScript.existsAsFile())
    {
        callError("Cannot find asr_worker.py");
        return;
    }

    // 构建命令行 — 优先使用项目虚拟环境中的 Python
    auto quote = [](const juce::String& s) { return "\"" + s + "\""; };

    juce::String py;
    if (pythonPath.isNotEmpty())
    {
        py = quote(pythonPath);
    }
    else
    {
        juce::File pythonExe = projectDir.getChildFile("scripts")
                                   .getChildFile(".venv")
                                   .getChildFile("Scripts")
                                   .getChildFile("python.exe");
        if (pythonExe.existsAsFile())
        {
            py = quote(pythonExe.getFullPathName());
            logToFile("[ASR] using venv python: " + pythonExe.getFullPathName());
        }
        else
        {
            py = "python";  // 回退到系统 PATH
            logToFile("[ASR] venv python not found, fallback to system python");
        }
    }

    juce::String cmd;
    cmd << py
        << " " << quote(pythonScript.getFullPathName())
        << " --audio " << quote(audioFile.getFullPathName())
        << " --device " << deviceName
        << " --model " << modelName;

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

            // 从缓冲区逐行提取
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

            // 保留未完成行
            auto remaining = size - start;
            if (remaining > 0 && start > 0)
                memmove(lineBuf.getData(), data + start, remaining);
            lineBuf.setSize(remaining);
        }
        else
        {
            wait(50);  // 无数据时短暂休眠
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

    // Track whether we've already emitted the very first paragraph
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
        isFirstParagraph = false; // reset after first word (which always has the flag)

        CharacterTimestamp ts;
        ts.character       = item["word"].toString();
        ts.startTime       = (double)item["start"];
        ts.endTime         = (double)item["end"];
        ts.globalTextIndex = fullText.length();
        fullText += ts.character;
        timestamps.push_back(ts);
    }

    isActive = false;

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
void ASRProcessor::setPythonPath(const juce::String& path) { pythonPath = path; }
void ASRProcessor::setModelName(const juce::String& m)     { modelName  = m; }
void ASRProcessor::setDevice(const juce::String& d)        { deviceName = d; }
