cmake_minimum_required(VERSION 3.15)
project(AudioTranscriptEditor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(JUCE)

# ARA SDK 路径（Celemony ARA_SDK v2.3.0）
juce_set_ara_sdk_path("${CMAKE_CURRENT_SOURCE_DIR}/../ARA_SDK")

juce_add_plugin(AudioTranscriptEditor
    PRODUCT_NAME             "Audio Transcript Editor"
    VERSION                  1.0.0
    COMPANY_NAME             "YourCompany"
    PLUGIN_MANUFACTURER_CODE "ATrn"
    PLUGIN_CODE              "ATEd"
    FORMATS                  VST3
    IS_ARA_EFFECT            TRUE
    ARA_ANALYSIS_TYPES       kARAContentTypeNotes
    PLUGIN_CATEGORY          kPlugCategEffect
)

target_sources(AudioTranscriptEditor
    PRIVATE
        Source/PluginProcessor.h
        Source/PluginProcessor.cpp
        Source/PluginEditor.h
        Source/PluginEditor.cpp
        Source/WaveformComponent.h
        Source/WaveformComponent.cpp
        Source/TranscriptEditor.h
        Source/TranscriptEditor.cpp
        Source/CharacterTimestamp.h
        Source/ASRProcessor.h
        Source/ASRProcessor.cpp
        Source/TranscriptDataManager.h
        Source/TranscriptDataManager.cpp
        Source/ARADocumentController.h
        Source/ARADocumentController.cpp
)

target_link_libraries(AudioTranscriptEditor
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_gui_basics
        juce::juce_events
)

juce_generate_juce_header(AudioTranscriptEditor)

# 只编译 VST3（无 VST2），无参数 ID 冲突问题
add_compile_definitions(JUCE_IGNORE_VST3_MISMATCHED_PARAMETER_ID_WARNING=1)
