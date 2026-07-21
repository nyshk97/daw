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
    Source/shared/Log.cpp
    Source/shared/Project.cpp
    Source/shared/SynthBank.cpp
    Source/ui/MainComponent.cpp
    Source/ui/PianoRollView.cpp
    Source/ui/ProjectChooserComponent.cpp
    Source/ui/TimelineView.cpp
    Source/ui/TrackHeadersView.cpp
)

target_include_directories(daw PRIVATE Source)

target_compile_definitions(daw PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
    # メッセージループまで漏れた例外を unhandledException に届ける（ログしてからterminateする）
    JUCE_CATCH_UNHANDLED_EXCEPTIONS=1
)

target_link_libraries(daw
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ---- テスト（CTest から実行するコンソールアプリ。GUI不要のモデル・音源検証）----
enable_testing()

juce_add_console_app(daw_tests PRODUCT_NAME "daw_tests")

target_sources(daw_tests PRIVATE
    Tests/TestsMain.cpp
    Source/audio/PlaybackEngine.cpp
    Source/audio/Recorder.cpp
    Source/shared/Log.cpp
    Source/shared/Project.cpp
    Source/shared/SynthBank.cpp
)

target_include_directories(daw_tests PRIVATE Source)

target_compile_definitions(daw_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
)

target_link_libraries(daw_tests
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

add_test(NAME daw_tests COMMAND daw_tests)
