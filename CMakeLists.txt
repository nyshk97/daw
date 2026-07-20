cmake_minimum_required(VERSION 3.22)
project(daw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    # 8.0.9 に pin: GOTCHAS.md の createWriterFor(AudioFormatWriterOptions) はこのバージョンで導入されたAPI。
    # バージョンを上げるときは GOTCHAS.md のコード例との整合を再確認すること
    GIT_TAG 8.0.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(daw
    PRODUCT_NAME "daw"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "録音のためにマイク入力を使用します"
)

target_sources(daw PRIVATE
    Source/Main.cpp
    Source/audio/PlaybackEngine.cpp
    Source/audio/Recorder.cpp
    Source/shared/Project.cpp
    Source/ui/MainComponent.cpp
    Source/ui/ProjectChooserComponent.cpp
    Source/ui/TimelineView.cpp
    Source/ui/TrackHeadersView.cpp
)

target_include_directories(daw PRIVATE Source)

target_compile_definitions(daw PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

target_link_libraries(daw
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
