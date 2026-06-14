cmake_minimum_required(VERSION 3.15)
project(AudioTranscriptEditor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# 静态链接 VC++ 运行时，脱离 Visual Studio 运行
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(JUCE)

juce_add_gui_app(AudioTranscriptEditor
    PRODUCT_NAME     "Audio Transcript Editor"
    VERSION          1.0.0
    COMPANY_NAME     "YourCompany"
    ICON_BIG         ""
    ICON_SMALL       ""
)

target_sources(AudioTranscriptEditor
    PRIVATE
        Source/Main.cpp
        Source/MainComponent.h
        Source/MainComponent.cpp
        Source/WaveformComponent.h
        Source/WaveformComponent.cpp
        Source/TranscriptEditor.h
        Source/TranscriptEditor.cpp
        Source/CharacterTimestamp.h
        Source/ASRProcessor.h
        Source/ASRProcessor.cpp
)

target_link_libraries(AudioTranscriptEditor
    PRIVATE
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_gui_basics
)

juce_generate_juce_header(AudioTranscriptEditor)
