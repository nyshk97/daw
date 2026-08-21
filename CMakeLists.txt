cmake_minimum_required(VERSION 3.22)
# VERSION がアプリバージョンの唯一の真実の源（Info.plist の CFBundleVersion と
# アプリ内表示の両方に流れる。Sparkle は CFBundleVersion で新旧を比較する）。
# リリース時は scripts/release.sh がこの値とタグの整合を検査する
project(daw VERSION 0.13.0 LANGUAGES C CXX)

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

# ---- signalsmith-stretch（オーディオクリップの移調・タイムストレッチ。MIT）----
# 公開タグが 1.1.0 までで main のヘッダは 1.3.2 のため commit SHA で pin する。
# 上流 CMake は 3.24 要求（LaLa は 3.22）なので add_subdirectory せず、ヘッダの
# include ディレクトリだけを使う（FFT で Signalsmith Linear に依存するため 2 リポジトリを取得）。
# SHA を上げるときは両者の整合（stretch が要求する linear のバージョン）を上流 CMakeLists で再確認すること
FetchContent_Declare(
    signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG 57b93f4e9206a089a45387eaa39bdc9f310d3308 # v1.3.2 相当（2026-08 時点の main）
)
FetchContent_Declare(
    signalsmith-linear
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
    GIT_TAG 5668673560146a9cfe38c25315071e3fd68c8317 # 0.3.1（stretch 上流 CMake の指定タグ）
)
# **取得のみ**（MakeAvailable は使わない）: MakeAvailable は上流を add_subdirectory するため、
# 上流の cmake_minimum_required(3.24) が LaLa の 3.22 宣言で configure を落とす。
# FetchContent_Populate の単引数形は新しい CMake では非推奨（CMP0169）だが、
# 3.22 互換で「ソースだけ取り出す」公式パターンはこれなので OLD を明示して使う
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
foreach(_dep IN ITEMS signalsmith-stretch signalsmith-linear)
    FetchContent_GetProperties(${_dep})
    if(NOT ${_dep}_POPULATED)
        FetchContent_Populate(${_dep})
    endif()
endforeach()
set(DAW_STRETCH_INCLUDE_DIRS
    "${signalsmith-stretch_SOURCE_DIR}/include"
    "${signalsmith-linear_SOURCE_DIR}/include"
)

# ---- WORLD（ボーカルのピッチ補正の再合成エンジン。BSD-3）----
# Phase 0 の lab（docs/labs/pitch-correction.md）で採用。上流の CMake は使わず、必要な .cpp だけを
# 直接コンパイルする（signalsmith と同じ「取得のみ」の型）。SHA を上げるときは lab の聴き比べを再実施する
FetchContent_Declare(
    world-vocoder
    GIT_REPOSITORY https://github.com/mmorise/World.git
    GIT_TAG d625e7608ca23a870018f01e7c562ac683d9847f # 2026-08 時点の master
)
FetchContent_GetProperties(world-vocoder)
if(NOT world-vocoder_POPULATED)
    FetchContent_Populate(world-vocoder)
endif()
set(DAW_WORLD_INCLUDE_DIR "${world-vocoder_SOURCE_DIR}/src")
set(DAW_WORLD_SOURCES
    "${world-vocoder_SOURCE_DIR}/src/cheaptrick.cpp"
    "${world-vocoder_SOURCE_DIR}/src/common.cpp"
    "${world-vocoder_SOURCE_DIR}/src/d4c.cpp"
    "${world-vocoder_SOURCE_DIR}/src/fft.cpp"
    "${world-vocoder_SOURCE_DIR}/src/matlabfunctions.cpp"
    "${world-vocoder_SOURCE_DIR}/src/synthesis.cpp"
)
# 上流ソースの警告は LaLa の警告ゼロ基準から除外する（サードパーティ）
# 警告は除外し、Debug でも -O2 で組む（中をデバッグしない。-O0 だと 27s の合成に 3 秒かかり、エディタの体感が別物になる）
set_source_files_properties(${DAW_WORLD_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-O2")

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
    Source/mac/AboutPanel.mm
    Source/mac/TitleBarStyle.mm
    Source/mac/SparkleBridge.mm
    Source/audio/AudioImporter.cpp
    Source/audio/AudioFilePreview.cpp
    Source/audio/BounceRenderer.cpp
    Source/audio/BufferAudition.cpp
    Source/audio/ClipStretcher.cpp
    Source/audio/PitchAnalyzer.cpp
    Source/audio/PitchAnalysisWorker.cpp
    Source/audio/VocalResynth.cpp
    Source/audio/VocalNoteAudition.cpp
    Source/audio/RenderCache.cpp
    Source/audio/BusDelay.cpp
    Source/audio/BusReverb.cpp
    Source/audio/MasterLimiter.cpp
    Source/audio/PlaybackEngine.cpp
    Source/audio/Recorder.cpp
    Source/audio/ReferenceAnalyzer.cpp
    Source/audio/ReferenceReportGenerator.cpp
    Source/audio/SamplerEngine.cpp
    Source/audio/TrackComp.cpp
    Source/audio/TrackEq.cpp
    Source/audio/TrackLofi.cpp
    Source/audio/TrackSaturator.cpp
    Source/audio/UrlDownloader.cpp
    Source/shared/ClipDomains.cpp
    Source/shared/GachaSession.cpp
    Source/shared/Log.cpp
    Source/shared/LoudnessMeter.cpp
    Source/shared/MidiImport.cpp
    Source/shared/PitchCorrection.cpp
    Source/shared/PitchCurve.cpp
    Source/shared/PitchNotes.cpp
    Source/shared/TimeMap.cpp
    Source/shared/Project.cpp
    Source/shared/ReferenceAlign.cpp
    Source/shared/ReferenceExport.cpp
    Source/shared/SpawnedProcess.cpp
    Source/shared/SpectrumAnalyzer.cpp
    Source/shared/SynthBank.cpp
    Source/ui/CompEditorView.cpp
    Source/ui/DelayEditorView.cpp
    Source/ui/EqEditorView.cpp
    Source/ui/FxEditorView.cpp
    Source/ui/GachaPanelView.cpp
    Source/ui/InstrumentDetailView.cpp
    Source/ui/LimiterEditorView.cpp
    Source/ui/LofiEditorView.cpp
    Source/ui/ReverbEditorView.cpp
    Source/ui/SatEditorView.cpp
    Source/ui/AudioFileBrowserView.cpp
    Source/ui/MainComponent.cpp
    Source/ui/MainComponentPitch.cpp
    Source/ui/PitchEditorView.cpp
    Source/ui/MixerOverlay.cpp
    Source/ui/PianoRollView.cpp
    Source/ui/ProjectChooserComponent.cpp
    Source/ui/RightPanel.cpp
    Source/ui/TimelineView.cpp
    Source/ui/TrackHeadersView.cpp
)

target_include_directories(daw PRIVATE Source)
# SYSTEM: 上流ヘッダを警告ゼロ基準の対象から外す（juce_recommended_warning_flags と衝突させない）
target_include_directories(daw SYSTEM PRIVATE ${DAW_STRETCH_INCLUDE_DIRS} ${DAW_WORLD_INCLUDE_DIR})
target_sources(daw PRIVATE ${DAW_WORLD_SOURCES})

# ---- サードパーティのライセンス同梱（MIT の配布条件）----
# signalsmith-stretch / Signalsmith Linear のライセンス文を名前を付けてコピーし、
# アプリバンドルの Resources に入れる（リリース成果物 = .app 丸ごとなのでこれで同梱される）
set(DAW_LICENSE_DIR "${CMAKE_BINARY_DIR}/third-party-licenses")
file(MAKE_DIRECTORY "${DAW_LICENSE_DIR}")
file(COPY_FILE "${signalsmith-stretch_SOURCE_DIR}/LICENSE.txt"
    "${DAW_LICENSE_DIR}/LICENSE-signalsmith-stretch.txt" ONLY_IF_DIFFERENT)
file(COPY_FILE "${signalsmith-linear_SOURCE_DIR}/LICENSE.txt"
    "${DAW_LICENSE_DIR}/LICENSE-signalsmith-linear.txt" ONLY_IF_DIFFERENT)
file(COPY_FILE "${world-vocoder_SOURCE_DIR}/LICENSE.txt"
    "${DAW_LICENSE_DIR}/LICENSE-world.txt" ONLY_IF_DIFFERENT) # BSD-3（ピッチ補正の再合成）
target_sources(daw PRIVATE
    "${DAW_LICENSE_DIR}/LICENSE-signalsmith-stretch.txt"
    "${DAW_LICENSE_DIR}/LICENSE-signalsmith-linear.txt"
    "${DAW_LICENSE_DIR}/LICENSE-world.txt"
)
set_source_files_properties(
    "${DAW_LICENSE_DIR}/LICENSE-signalsmith-stretch.txt"
    "${DAW_LICENSE_DIR}/LICENSE-signalsmith-linear.txt"
    "${DAW_LICENSE_DIR}/LICENSE-world.txt"
    PROPERTIES MACOSX_PACKAGE_LOCATION Resources
)

target_compile_definitions(daw PRIVATE
    # アプリ表示名（Debug=LaLa-dev / Release=LaLa）とバージョン。Main.cpp が参照する
    DAW_APP_NAME="${DAW_PRODUCT_NAME}"
    DAW_APP_VERSION="${PROJECT_VERSION}"
    # JUCE_WEB_BROWSER はレポートウィンドウ（WKWebView）が使うため有効（既定値）。
    # daw_tests 側は UI を含まないので 0 のまま
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
    # メッセージループまで漏れた例外を unhandledException に届ける（ログしてからterminateする）
    JUCE_CATCH_UNHANDLED_EXCEPTIONS=1
)

target_link_libraries(daw
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_extra
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
    Source/audio/AudioImporter.cpp
    Source/audio/AudioFilePreview.cpp
    Source/audio/BounceRenderer.cpp
    Source/audio/BufferAudition.cpp
    Source/audio/ClipStretcher.cpp
    Source/audio/PitchAnalyzer.cpp
    Source/audio/PitchAnalysisWorker.cpp
    Source/audio/VocalResynth.cpp
    Source/audio/VocalNoteAudition.cpp
    Source/audio/RenderCache.cpp
    Source/audio/BusDelay.cpp
    Source/audio/BusReverb.cpp
    Source/audio/MasterLimiter.cpp
    Source/audio/PlaybackEngine.cpp
    Source/audio/Recorder.cpp
    Source/audio/ReferenceAnalyzer.cpp
    Source/audio/ReferenceReportGenerator.cpp
    Source/audio/SamplerEngine.cpp
    Source/audio/TrackComp.cpp
    Source/audio/TrackEq.cpp
    Source/audio/TrackLofi.cpp
    Source/audio/TrackSaturator.cpp
    Source/audio/UrlDownloader.cpp
    Source/shared/ClipDomains.cpp
    Source/shared/GachaSession.cpp
    Source/shared/Log.cpp
    Source/shared/LoudnessMeter.cpp
    Source/shared/MidiImport.cpp
    Source/shared/PitchCorrection.cpp
    Source/shared/PitchCurve.cpp
    Source/shared/PitchNotes.cpp
    Source/shared/TimeMap.cpp
    Source/shared/Project.cpp
    Source/shared/ReferenceAlign.cpp
    Source/shared/ReferenceExport.cpp
    Source/shared/SpawnedProcess.cpp
    Source/shared/SpectrumAnalyzer.cpp
    Source/shared/SynthBank.cpp
)

target_include_directories(daw_tests PRIVATE Source)
target_include_directories(daw_tests SYSTEM PRIVATE ${DAW_STRETCH_INCLUDE_DIRS} ${DAW_WORLD_INCLUDE_DIR})
target_sources(daw_tests PRIVATE ${DAW_WORLD_SOURCES})

target_compile_definitions(daw_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
)

target_link_libraries(daw_tests
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

add_test(NAME daw_tests COMMAND daw_tests)

# ---- Salva（レコード/ローカル音源のサンプリング素材化アプリ。apps/salva/CLAUDE.md参照）----
# JUCE・codesign identity の解決後に読み込む（サブディレクトリ側が両方を使う）
add_subdirectory(apps/salva)
