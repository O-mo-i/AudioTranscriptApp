#include "ASRProcessor.h"
#include <juce_events/juce_events.h>

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif

static void logToFile(const juce::String& msg)
{
    DBG(msg);
    ASRProcessor::getDebugLogFile().appendText(msg + "\n", false, false);
}

juce::File ASRProcessor::getDebugLogFile()
{
    return juce::File::getSpecialLocation(
        juce::File::currentExecutableFile).getParentDirectory()
        .getChildFile("asr_debug.log");
}

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
juce::String ASRProcessor::modelDirectory;

ASRProcessor::ASRProcessor()
    : juce::Thread("ASR Worker") {}

ASRProcessor::~ASRProcessor() { cancel(); }

void ASRProcessor::start(const juce::File& file)
{
    cancel();
    currentModelOp = ModelOp::kNone;
    readerFactory = nullptr;
    audioFile = file;
    isActive = true;
    startThread();
}

void ASRProcessor::startFromSource(
    std::function<std::unique_ptr<juce::AudioFormatReader>()> factory,
    const juce::File& outputFile)
{
    cancel();
    currentModelOp = ModelOp::kNone;
    readerFactory = std::move(factory);
    exportFile = outputFile;
    isActive = true;
    startThread();
}

void ASRProcessor::startDownload(const juce::String& modelName)
{
    cancel();
    currentModelOp = ModelOp::kDownload;
    downloadModelName = modelName;
    isActive = true;
    startThread();
}

void ASRProcessor::startVerify(const juce::String& modelName)
{
    cancel();
    currentModelOp = ModelOp::kVerify;
    downloadModelName = modelName;
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

void ASRProcessor::run()
{
    isActive = true;

    if (currentModelOp != ModelOp::kNone)
    {
        runModelOp();
        return;
    }

    if (readerFactory && exportFile.existsAsFile())
    {
        if (!runWavExport())
            return;
        audioFile = exportFile;
    }

    runASR();
}

//==============================================================================
bool ASRProcessor::runWavExport()
{
    logToFile("[ASR] Starting WAV export to: " + exportFile.getFullPathName());

    auto reader = readerFactory();
    if (reader == nullptr)
    {
        callError("Failed to create audio reader for WAV export");
        return false;
    }

    auto sampleRate = reader->sampleRate;
    auto totalSamples = reader->lengthInSamples;
    auto numChannels = reader->numChannels;

    if (sampleRate <= 0.0 || totalSamples <= 0)
    {
        callError("Invalid audio source (sample rate or length is zero)");
        return false;
    }

    logToFile("[ASR] Export: " + juce::String(totalSamples) + " samples, "
              + juce::String(numChannels) + "ch, "
              + juce::String(sampleRate) + "Hz -> "
              + juce::String(totalSamples / (int64_t)sampleRate) + "s audio");

    auto outStream = std::make_unique<juce::FileOutputStream>(exportFile);
    if (!outStream->openedOk())
    {
        callError("Cannot create temp WAV file");
        return false;
    }

    juce::WavAudioFormat wavFormat;
    auto writer = wavFormat.createWriterFor(outStream.release(),
        sampleRate, (unsigned int)numChannels, 16, {}, 0);
    if (writer == nullptr)
    {
        callError("Cannot create WAV writer");
        return false;
    }

    juce::AudioBuffer<float> tempBuf((int)numChannels, wavExportBlockSize);
    int64_t samplesWritten = 0;

    while (samplesWritten < totalSamples && !threadShouldExit())
    {
        int toRead = (int)juce::jmin((int64_t)wavExportBlockSize,
                                      totalSamples - samplesWritten);
        reader->read(&tempBuf, 0, toRead, samplesWritten, true, true);
        writer->writeFromAudioSampleBuffer(tempBuf, 0, toRead);
        samplesWritten += toRead;

        if (onExportProgress && totalSamples > 0)
        {
            double pct = (double)samplesWritten / (double)totalSamples;
            juce::MessageManager::callAsync([this, pct]()
            {
                onExportProgress(pct * 0.1);
            });
        }
    }

    writer->flush();
    delete writer;

    if (threadShouldExit())
        return false;

    logToFile("[ASR] WAV export complete: "
              + juce::String(exportFile.getSize() / 1024 / 1024) + " MB");
    return true;
}

//==============================================================================
void ASRProcessor::runASR()
{
    auto quote = [](const juce::String& s) { return "\"" + s + "\""; };

    juce::File vst3Dir;

   #if JUCE_WINDOWS
    auto dllFile = getPluginDllPath();
    if (dllFile.exists())
    {
        auto dllParent   = dllFile.getParentDirectory();
        auto contentsDir = dllParent.getParentDirectory();
        auto bundleDir   = contentsDir.getParentDirectory();
        if (bundleDir.getFileExtension().toLowerCase() == ".vst3")
            vst3Dir = bundleDir.getParentDirectory();
    }
   #endif

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

    juce::String cmd;
    cmd << quote(pythonExeFile.getFullPathName())
        << " " << quote(pythonScriptFile.getFullPathName())
        << " --audio " << quote(audioFile.getFullPathName())
        << " --device " << deviceName
        << " --model " << quote(modelName);

    cmd << " --offline";

    if (modelDirectory.isNotEmpty())
        cmd << " --model-dir " << quote(modelDirectory);

    if (modelDirectory.isNotEmpty())
        logToFile("[ASR] model-dir: " + modelDirectory);

    logToFile("[ASR] cmd: " + cmd);

    juce::ChildProcess proc;
    if (!proc.start(cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        callError("Failed to start Python process.");
        return;
    }

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
                                    onProgress(juce::jlimit(0.0, 1.0, 0.1 + 0.9 * (pct / 100.0)));
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

    char tail[4096];
    while (auto n = proc.readProcessOutput(tail, sizeof(tail)))
        lineBuf.append(tail, n);

    if (lineBuf.getSize() > 0)
    {
        auto* data = static_cast<const char*>(lineBuf.getData());
        juce::String lastLine = juce::String::fromUTF8(data, (int)lineBuf.getSize());
        lastLine = lastLine.trim();

        if (lastLine.startsWith("{") && lastLine.endsWith("}"))
            jsonLine = lastLine;
    }

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

    auto wordsVar = json["words"];
    if (!wordsVar.isArray())
    {
        callError("ASR 'words' is not an array");
        return;
    }

    std::vector<CharacterTimestamp> timestamps;
    juce::String fullText;
    auto* arr = wordsVar.getArray();

    timestamps.reserve((size_t)arr->size());
    fullText.preallocateBytes(fullText.length() + jsonLine.length() / 2);

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

    logToFile("====== [ASR DATA CHECK] ======");
    logToFile("Total characters parsed in C++: " + juce::String((int)timestamps.size()));
    if (!timestamps.empty())
    {
        logToFile("First char startTime: " + juce::String(timestamps.front().startTime));
        logToFile("Last char startTime:  " + juce::String(timestamps.back().startTime));
    }
    logToFile("================================");

    if (onResult && !threadShouldExit())
    {
        juce::MessageManager::callAsync([this, timestamps, fullText]()
        {
            onResult(timestamps, fullText);
        });
    }
}

//==============================================================================
void ASRProcessor::runModelOp()
{
    logToFile("[ASR] start model op (" + juce::String(
        currentModelOp == ModelOp::kDownload ? "download" : "verify")
        + ") for: " + downloadModelName);

    juce::File vst3Dir;

   #if JUCE_WINDOWS
    auto dllFile = getPluginDllPath();
    if (dllFile.exists())
    {
        auto dllParent   = dllFile.getParentDirectory();
        auto contentsDir = dllParent.getParentDirectory();
        auto bundleDir   = contentsDir.getParentDirectory();
        if (bundleDir.getFileExtension().toLowerCase() == ".vst3")
            vst3Dir = bundleDir.getParentDirectory();
    }
   #endif

    if (vst3Dir == juce::File{})
    {
        auto exeDir = juce::File::getSpecialLocation(
            juce::File::currentExecutableFile).getParentDirectory();
        vst3Dir = exeDir.getParentDirectory().getParentDirectory();
    }

    logToFile("[ASR-DL] vst3 base dir: " + vst3Dir.getFullPathName());

    juce::File scriptsDir = vst3Dir.getChildFile("scripts");
    juce::File pythonExeFile = scriptsDir.getChildFile(".venv")
                                        .getChildFile("Scripts")
                                        .getChildFile("python.exe");
    juce::File pythonScriptFile = scriptsDir.getChildFile("asr_worker.py");

    if (!pythonExeFile.existsAsFile() || !pythonScriptFile.existsAsFile())
    {
        callError("Python or script not found in " + scriptsDir.getFullPathName());
        return;
    }

    auto quote = [](const juce::String& s) { return "\"" + s + "\""; };

    juce::String opFlag = (currentModelOp == ModelOp::kDownload)
        ? "--download-only" : "--verify-only";

    juce::String cmd;
    cmd << quote(pythonExeFile.getFullPathName())
        << " " << quote(pythonScriptFile.getFullPathName())
        << " " << opFlag
        << " --device " << deviceName
        << " --model " << quote(downloadModelName);

    if (modelDirectory.isNotEmpty())
        cmd << " --model-dir " << quote(modelDirectory);

    logToFile("[ASR] cmd: " + cmd);

    juce::ChildProcess proc;
    if (!proc.start(cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        callError("Failed to start Python process for model download.");
        return;
    }

    juce::String jsonLine;
    char buf[4096];
    while (proc.isRunning() && !threadShouldExit())
    {
        if (auto n = proc.readProcessOutput(buf, sizeof(buf)))
        {
            juce::String chunk = juce::String::fromUTF8(buf, (int)n);
            for (auto& line : juce::StringArray::fromLines(chunk))
            {
                line = line.trim();
                if (line.startsWith("PROGRESS:"))
                {
                }
                else if (line.startsWith("{") && line.endsWith("}"))
                {
                    jsonLine = line;
                }
            }
        }
        else
        {
            wait(50);
        }
    }

    char tail[4096];
    while (auto n = proc.readProcessOutput(tail, sizeof(tail)))
    {
        juce::String chunk = juce::String::fromUTF8(tail, (int)n);
        for (auto& line : juce::StringArray::fromLines(chunk))
        {
            line = line.trim();
            if (line.startsWith("{") && line.endsWith("}"))
                jsonLine = line;
        }
    }

    logToFile("[ASR-DL] exit code: " + juce::String(proc.getExitCode()));
    logToFile("[ASR-DL] jsonLine: " + jsonLine);

    isActive = false;

    bool success = false;
    juce::String message;

    if (jsonLine.isNotEmpty())
    {
        auto json = juce::JSON::parse(jsonLine);
        success = json.hasProperty("success") && (bool)json["success"];
        message = success ? juce::String::fromUTF8("\xe6\xa8\xa1\xe5\x9e\x8b\xe4\xb8\x8b\xe8\xbd\xbd\xe5\xae\x8c\xe6\x88\x90")
                          : json["error"].toString();
        if (!success && message.isEmpty())
            message = juce::String::fromUTF8("\xe6\xa8\xa1\xe5\x9e\x8b\xe4\xb8\x8b\xe8\xbd\xbd\xe5\xa4\xb1\xe8\xb4\xa5");
    }
    else
    {
        message = juce::String::fromUTF8("\xe6\x97\xa0\xe6\xb3\x95\xe8\x8e\xb7\xe5\x8f\x96\xe4\xb8\x8b\xe8\xbd\xbd\xe7\xbb\x93\xe6\x9e\x9c");
    }

    if (onDownloadComplete)
    {
        juce::MessageManager::callAsync([this, success, message]()
        {
            onDownloadComplete(success, message);
        });
    }
}

void ASRProcessor::setPythonPath(const juce::String& path)      { pythonPath = path; }
void ASRProcessor::setModelName(const juce::String& m)         { modelName  = m; }
void ASRProcessor::setDevice(const juce::String& d)            { deviceName = d; }
void ASRProcessor::setScriptsDirectory(const juce::String& p)  { scriptsDirectory = p; }
void ASRProcessor::setModelDirectory(const juce::String& p)    { modelDirectory = p; }
