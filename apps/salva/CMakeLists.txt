# Salva — レコード/ローカル音源のサンプリング素材化アプリ（apps/salva/CLAUDE.md参照）。
# ルートの CMakeLists.txt から add_subdirectory で読み込まれる（JUCE・codesign identity は
# 親スコープで解決済みのものを使う）。

# Salva専用バージョン（LaLaの project VERSION と独立。Sparkleの新旧比較も
# この値が CFBundleVersion に流れる。リリース時は scripts/release-salva.sh が
# タグ salva-vX.Y.Z との整合を検査する — Phase 7で整備）
set(SALVA_VERSION 0.4.0)

# dev/常用版の切り替え（LaLaと同じ流儀: 名前・bundle id・アイコンを分離し、
# Dock・⌘Tab・TCC 権限が独立する）
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(SALVA_PRODUCT_NAME "Salva")
    set(SALVA_BUNDLE_ID "local.d0ne1s.salva")
    set(SALVA_ICON "${CMAKE_CURRENT_SOURCE_DIR}/Assets/icon.png")
else()
    set(SALVA_PRODUCT_NAME "Salva-dev")
    set(SALVA_BUNDLE_ID "local.d0ne1s.salva.dev")
    set(SALVA_ICON "${CMAKE_CURRENT_SOURCE_DIR}/Assets/icon-dev.png")
endif()

# ---- Sparkle（アプリ内アップデート。LaLaと同じ流儀・配信は完全分離）----
# framework・鍵運用はLaLaと共有（.sparkle/ はルートのconfigureで取得済み、
# EdDSA鍵もkeychainアカウント "daw" を共用）。feed URLだけがsalva-releases固定。
# SUFeedURLはRelease のみ（dev版で誤って常用版の更新を引かない）
set(SALVA_SPARKLE_PUBLIC_KEY "qicnPwvUMYYTOBCfRGm4wmJHdGYbpv+MPsO9n6OZhVk=")
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_salva_sparkle_feed "<key>SUFeedURL</key><string>https://github.com/nyshk97/salva-releases/releases/latest/download/appcast.xml</string>")
else()
    set(_salva_sparkle_feed "")
endif()
set(SALVA_PLIST_TO_MERGE
    "<plist><dict>${_salva_sparkle_feed}<key>SUPublicEDKey</key><string>${SALVA_SPARKLE_PUBLIC_KEY}</string><key>SUEnableAutomaticChecks</key><false/></dict></plist>")

juce_add_gui_app(salva
    PRODUCT_NAME "${SALVA_PRODUCT_NAME}"
    VERSION "${SALVA_VERSION}"
    BUNDLE_ID "${SALVA_BUNDLE_ID}"
    # フルブリード画像（macOS 26以降はシステム側が角丸マスク・影を付与する）。
    # 再生成は apps/salva/Assets/make_icon.swift を参照（dev 版は --dev フラグ）
    ICON_BIG "${SALVA_ICON}"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "レコード録音のためにオーディオ入力を使用します"
    PLIST_TO_MERGE "${SALVA_PLIST_TO_MERGE}"
)

# separate.sh はバンドルのResourcesへ同梱（配布版はソースrepoの位置を知らないため。
# POST_BUILDのリソース先行コピー→codesignのループにも自動で乗る）
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/Resources/separate.sh
    PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

target_sources(salva PRIVATE
    Source/Main.cpp
    Source/audio/PlayerEngine.cpp
    Source/audio/RegionExport.cpp
    Source/audio/SalvaRecorder.cpp
    Source/audio/StemSeparator.cpp
    Source/shared/ReadAheadStream.cpp
    Source/shared/SalvaSettings.cpp
    Source/shared/StemCache.cpp
    Source/ui/RecordView.cpp
    Source/ui/SalvaMainComponent.cpp
    Source/ui/StemPanel.cpp
    Source/ui/WaveformView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Resources/separate.sh
    ${CMAKE_SOURCE_DIR}/Source/mac/SparkleBridge.mm
    ${CMAKE_SOURCE_DIR}/Source/shared/Log.cpp
    ${CMAKE_SOURCE_DIR}/Source/shared/SpawnedProcess.cpp
)

target_include_directories(salva PRIVATE
    Source
    # LaLaと共有する汎用部品（Source/shared/ のみ。LaLa固有の ui/・audio/ には
    # 依存しない — apps/salva/CLAUDE.md の境界）
    ${CMAKE_SOURCE_DIR}/Source
)

target_compile_definitions(salva PRIVATE
    SALVA_APP_NAME="${SALVA_PRODUCT_NAME}"
    SALVA_APP_VERSION="${SALVA_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_CATCH_UNHANDLED_EXCEPTIONS=1
)

target_link_libraries(salva
    PRIVATE
        juce::juce_audio_utils
        juce::juce_cryptography   # StemCacheのidentityハッシュ（MD5）
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Sparkle.framework のリンク（SparkleBridge.mm の #import と実行時ロードの両方に必要）。
# DAW_SPARKLE_DIR はルートのconfigureで解決済み
target_compile_options(salva PRIVATE "-F${DAW_SPARKLE_DIR}")
target_link_options(salva PRIVATE
    "-F${DAW_SPARKLE_DIR}"
    "-framework" "Sparkle"
    "-Wl,-rpath,@executable_path/../Frameworks"
)

# 安定署名（LaLaと同じ理由: ad-hoc だとリビルドごとに cdhash が変わり TCC の
# マイク許可が毎回リセットされる）。identity は親スコープの DAW_CODESIGN_IDENTITY。
# Makefile ジェネレータはバンドルリソースを POST_BUILD より後にコピーするため、
# 署名前に同じリソース一式を自前コピーして封印対象に入れる（ルートと同じ手口）
get_target_property(_salva_sources salva SOURCES)
set(_salva_resource_copy_cmds "")
set(_salva_found_icns FALSE)
foreach(_src IN LISTS _salva_sources)
    get_source_file_property(_loc "${_src}" MACOSX_PACKAGE_LOCATION)
    if(_loc)
        get_filename_component(_name "${_src}" NAME)
        list(APPEND _salva_resource_copy_cmds
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_src}" "$<TARGET_BUNDLE_DIR:salva>/Contents/${_loc}/${_name}")
        if(_src MATCHES "\\.icns$")
            set(_salva_found_icns TRUE)
        endif()
    endif()
endforeach()
if(NOT _salva_found_icns)
    message(FATAL_ERROR "JUCE が生成する .icns が salva のターゲットソースに見つかりません（JUCE 更新で内部構造が変わった可能性。署名順序の前提を再確認すること）")
endif()

add_custom_command(TARGET salva POST_BUILD
    ${_salva_resource_copy_cmds}
    # Sparkle.framework も署名より前に埋め込む（ルートのdawターゲットと同じ手口。
    # dittoでsymlinkを保持・更新時の残骸を防ぐため先に消す）
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_BUNDLE_DIR:salva>/Contents/Frameworks/Sparkle.framework"
    COMMAND ditto "${DAW_SPARKLE_DIR}/Sparkle.framework"
        "$<TARGET_BUNDLE_DIR:salva>/Contents/Frameworks/Sparkle.framework"
    COMMAND codesign --force
        --sign "${DAW_CODESIGN_IDENTITY}"
        "$<TARGET_BUNDLE_DIR:salva>"
    COMMENT "Signing ${SALVA_PRODUCT_NAME}.app with stable identity for persistent TCC permissions"
)

# ---- テスト（CTest から実行するコンソールアプリ。UIの.cppを含めない — LaLaと同じ方針）----
juce_add_console_app(salva_tests PRODUCT_NAME "salva_tests")

target_sources(salva_tests PRIVATE
    Tests/TestsMain.cpp
    Source/audio/RegionExport.cpp
    Source/audio/SalvaRecorder.cpp
    Source/shared/ReadAheadStream.cpp
    Source/shared/SalvaSettings.cpp
    Source/shared/StemCache.cpp
)

target_include_directories(salva_tests PRIVATE
    Source
    ${CMAKE_SOURCE_DIR}/Source
)

target_compile_definitions(salva_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    # producer(separate.sh)とconsumer(StemCache.h)のcontractVersion一致テスト用
    SALVA_SEPARATE_SH_PATH="${CMAKE_CURRENT_SOURCE_DIR}/Resources/separate.sh"
)

target_link_libraries(salva_tests
    PRIVATE
        juce::juce_audio_utils
        juce::juce_cryptography
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

add_test(NAME salva_tests COMMAND salva_tests)
