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

# ad-hoc 署名だとリビルドごとに cdhash が変わり、TCC のマイク許可が毎回リセットされる。
# 安定した証明書で再署名して許可を永続化する。証明書ハッシュはマシンごと・更新ごとに異なるため
# ハードコードせず、configure 時に keychain の Apple Development 証明書を自動解決する
# （複数PCでの開発と年次の証明書更新に追従する。証明書を入れ替えたら configure の再実行で反映され、
#  リビルド前後の `codesign -dr -` 一致で許可維持を確認できる）。
# 明示指定したいときは -DDAW_CODESIGN_IDENTITY=<ハッシュ> で上書きする
if(NOT DEFINED DAW_CODESIGN_IDENTITY)
    execute_process(
        COMMAND security find-identity -v -p codesigning
        OUTPUT_VARIABLE _codesign_identities
        ERROR_QUIET
    )
    string(REGEX MATCH "([0-9A-F]+) \"Apple Development" _codesign_match "${_codesign_identities}")
    if(CMAKE_MATCH_1)
        set(DAW_CODESIGN_IDENTITY "${CMAKE_MATCH_1}")
        message(STATUS "Codesign identity: Apple Development (${DAW_CODESIGN_IDENTITY})")
    else()
        # 証明書の無いマシンでもビルドは通す（ad-hoc。TCC のマイク許可はリビルドごとにリセットされる）
        set(DAW_CODESIGN_IDENTITY "-")
        message(WARNING "Apple Development 証明書が見つからないため ad-hoc 署名にフォールバックします")
    endif()
endif()

add_custom_command(TARGET daw POST_BUILD
    COMMAND codesign --force
        --sign "${DAW_CODESIGN_IDENTITY}"
        "$<TARGET_BUNDLE_DIR:daw>"
    COMMENT "Signing daw.app with stable identity for persistent TCC permissions"
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
