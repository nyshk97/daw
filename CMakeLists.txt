cmake_minimum_required(VERSION 3.22)
# VERSION がアプリバージョンの唯一の真実の源（Info.plist の CFBundleVersion と
# アプリ内表示の両方に流れる。Sparkle は CFBundleVersion で新旧を比較する）。
# リリース時は scripts/release.sh がこの値とタグの整合を検査する
project(daw VERSION 0.4.0 LANGUAGES C CXX)

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
    set(DAW_PRODUCT_NAME "LaLa")
    set(DAW_BUNDLE_ID "local.d0ne1s.daw")
    set(DAW_ICON "Assets/icon.png")
else()
    set(DAW_PRODUCT_NAME "LaLa-dev")
    set(DAW_BUNDLE_ID "local.d0ne1s.daw.dev")
    set(DAW_ICON "Assets/icon-dev.png")
endif()

# ---- Sparkle（アプリ内アップデート）----
# prebuilt Sparkle.framework を .sparkle/（gitignore 対象）に取得して埋め込む。
# バージョン pin と取得処理は scripts/fetch-sparkle.sh に集約
set(DAW_SPARKLE_DIR "${CMAKE_SOURCE_DIR}/.sparkle")
# 毎 configure で実行する（スクリプト側の .version スタンプ照合で最新なら即 no-op・ネットワーク不要）。
# 「ディレクトリ存在時はスキップ」にすると fetch-sparkle.sh の SPARKLE_VERSION を上げても
# 既存環境で永久に反映されない
execute_process(
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/fetch-sparkle.sh"
    RESULT_VARIABLE _sparkle_fetch_result
)
if(NOT _sparkle_fetch_result EQUAL 0)
    message(FATAL_ERROR "Sparkle の取得に失敗しました。scripts/fetch-sparkle.sh を手動実行してください")
endif()

# Sparkle の Info.plist キー。更新チェックは手動メニューのみ（自動チェック無効）。
# SUFeedURL は Release のみ（dev 版で誤って常用版の更新を引かないように。Debug は
# 手動チェック時に「フィード未設定」エラーを返すだけ）。
# SUEnableInstallerLauncherService は設定しない（Sandbox アプリ用の XPC。daw は非 Sandbox）。
# 公開鍵のペアの秘密鍵は Keychain（アカウント名 "daw"）。
# バックアップ: ~/Library/CloudStorage/Dropbox/secrets/sparkle-ed25519-daw-private.key
set(DAW_SPARKLE_PUBLIC_KEY "qicnPwvUMYYTOBCfRGm4wmJHdGYbpv+MPsO9n6OZhVk=")
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_daw_sparkle_feed "<key>SUFeedURL</key><string>https://github.com/nyshk97/daw-releases/releases/latest/download/appcast.xml</string>")
else()
    set(_daw_sparkle_feed "")
endif()
set(DAW_PLIST_TO_MERGE
    "<plist><dict>${_daw_sparkle_feed}<key>SUPublicEDKey</key><string>${DAW_SPARKLE_PUBLIC_KEY}</string><key>SUEnableAutomaticChecks</key><false/></dict></plist>")

juce_add_gui_app(daw
    PRODUCT_NAME "${DAW_PRODUCT_NAME}"
    BUNDLE_ID "${DAW_BUNDLE_ID}"
    # フルブリード画像（macOS 26以降はシステム側が角丸マスク・影を付与する）。
    # 再生成は Assets/make_icon.swift を参照（dev 版は --dev フラグ）
    ICON_BIG "${DAW_ICON}"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "録音のためにマイク入力を使用します"
    PLIST_TO_MERGE "${DAW_PLIST_TO_MERGE}"
)

target_sources(daw PRIVATE
    Source/Main.cpp
    Source/mac/SparkleBridge.mm
    Source/audio/BounceRenderer.cpp
    Source/audio/PlaybackEngine.cpp
    Source/audio/Recorder.cpp
    Source/shared/Log.cpp
    Source/shared/Project.cpp
    Source/shared/SynthBank.cpp
    Source/ui/FxEditorView.cpp
    Source/ui/MainComponent.cpp
    Source/ui/MixerOverlay.cpp
    Source/ui/PianoRollView.cpp
    Source/ui/ProjectChooserComponent.cpp
    Source/ui/TimelineView.cpp
    Source/ui/TrackHeadersView.cpp
)

target_include_directories(daw PRIVATE Source)

target_compile_definitions(daw PRIVATE
    # アプリ表示名（Debug=LaLa-dev / Release=LaLa）とバージョン。Main.cpp が参照する
    DAW_APP_NAME="${DAW_PRODUCT_NAME}"
    DAW_APP_VERSION="${PROJECT_VERSION}"
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

# Sparkle.framework のリンク（SparkleBridge.mm の #import と実行時ロードの両方に必要）。
# rpath は埋め込み先の Contents/Frameworks を指す
target_compile_options(daw PRIVATE "-F${DAW_SPARKLE_DIR}")
target_link_options(daw PRIVATE
    "-F${DAW_SPARKLE_DIR}"
    "-framework" "Sparkle"
    "-Wl,-rpath,@executable_path/../Frameworks"
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
    # Sparkle.framework も署名より前に埋め込む（署名後にコピーするとシールが壊れる）。
    # ditto で symlink（Versions/Current 等）を保持する。cmake -E copy_directory は
    # symlink を flatten して framework 自体の署名を壊すため使わない。
    # ditto は既存ディレクトリへ「上書きマージ」するため、Sparkle 更新でファイルが
    # 削除・改名された場合に旧ファイルが残って framework の署名シールが壊れる → 先に消す
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_BUNDLE_DIR:daw>/Contents/Frameworks/Sparkle.framework"
    COMMAND ditto "${DAW_SPARKLE_DIR}/Sparkle.framework"
        "$<TARGET_BUNDLE_DIR:daw>/Contents/Frameworks/Sparkle.framework"
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
    Source/audio/BounceRenderer.cpp
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
