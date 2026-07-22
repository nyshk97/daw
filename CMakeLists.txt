cmake_minimum_required(VERSION 3.22)
project(daw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# dev/常用版の切り替え（後述）は CMAKE_BUILD_TYPE 前提のため、それが空になる
# マルチ構成ジェネレータ（Xcode 等）は明示的に拒否する
# （Xcode で生成すると Release まで dev 版の名前・bundle id・TCC 権限になってしまう）
get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_is_multi_config)
    message(FATAL_ERROR
        "マルチ構成ジェネレータ（${CMAKE_GENERATOR}）は非対応です。"
        "単一構成ジェネレータで -DCMAKE_BUILD_TYPE=Debug|Release を指定してください")
endif()

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

# Debug は dev 版として別アプリ扱いにする（名前・bundle id・アイコンを分離し、
# Dock・⌘Tab・TCC 権限が常用版（Release）と独立する）。
# bundle id が異なるため dev 版の初回起動時のみマイク許可の再付与が必要。
# プロジェクト置き場（~/Music/daw/）とログ（~/Library/Logs/daw/）は両版で共有
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(DAW_PRODUCT_NAME "daw")
    set(DAW_BUNDLE_ID "com.yourcompany.daw")
    set(DAW_ICON "Assets/icon.png")
else()
    set(DAW_PRODUCT_NAME "daw-dev")
    set(DAW_BUNDLE_ID "com.yourcompany.daw.dev")
    set(DAW_ICON "Assets/icon-dev.png")
endif()

juce_add_gui_app(daw
    PRODUCT_NAME "${DAW_PRODUCT_NAME}"
    BUNDLE_ID "${DAW_BUNDLE_ID}"
    # フルブリード画像（macOS 26以降はシステム側が角丸マスク・影を付与する）。
    # 再生成は Assets/make_icon.swift を参照（dev 版は --dev フラグ）
    ICON_BIG "${DAW_ICON}"
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
    # アプリ表示名（Debug=daw-dev / Release=daw）。Main.cpp が参照する
    DAW_APP_NAME="${DAW_PRODUCT_NAME}"
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

# 署名は POST_BUILD で行う（`--build --target daw` の単体ビルドでも必ず署名されるように、
# daw 自身のビルドに含める。別ターゲット方式だと --target daw で署名がスキップされ
# ad-hoc のまま残り、TCC のマイク許可が失われる）。
# ただし Makefile ジェネレータはバンドルリソース（JUCE 生成の Icon.icns や
# RecentFilesMenuTemplate.nib 等、MACOSX_PACKAGE_LOCATION 付きソース）を
# POST_BUILD より後にコピーするため、素朴に署名すると初回・cleanビルドで
# シールが壊れる（"a sealed resource is missing or invalid"）。
# そこで同じリソース一式を署名の前に自前でコピーして封印対象に入れる。
# 後続の CMake によるコピーは同一内容の上書きなのでシールは壊れない
get_target_property(_daw_sources daw SOURCES)
set(_daw_resource_copy_cmds "")
set(_daw_found_icns FALSE)
foreach(_src IN LISTS _daw_sources)
    get_source_file_property(_loc "${_src}" MACOSX_PACKAGE_LOCATION)
    if(_loc)
        get_filename_component(_name "${_src}" NAME)
        list(APPEND _daw_resource_copy_cmds
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_src}" "$<TARGET_BUNDLE_DIR:daw>/Contents/${_loc}/${_name}")
        if(_src MATCHES "\\.icns$")
            set(_daw_found_icns TRUE)
        endif()
    endif()
endforeach()
if(NOT _daw_found_icns)
    message(FATAL_ERROR "JUCE が生成する .icns がターゲットソースに見つかりません（JUCE 更新で内部構造が変わった可能性。署名順序の前提を再確認すること）")
endif()

add_custom_command(TARGET daw POST_BUILD
    ${_daw_resource_copy_cmds}
    COMMAND codesign --force
        --sign "${DAW_CODESIGN_IDENTITY}"
        "$<TARGET_BUNDLE_DIR:daw>"
    COMMENT "Signing ${DAW_PRODUCT_NAME}.app with stable identity for persistent TCC permissions"
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
