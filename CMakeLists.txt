cmake_minimum_required(VERSION 3.16)

option(USE_VCPKG "Use bundled vcpkg toolchain" ON)

# Must happen before project()
if(USE_VCPKG AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(acheron LANGUAGES CXX)

include(qt.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTORCC ON)

option(BUILD_TESTS "Build test targets" ON)
option(ENABLE_VOICE "Build with voice/audio support" ON)
option(ENABLE_RNNOISE "Build with RNNoise noise suppression (requires ENABLE_VOICE)" ON)
option(ENABLE_RNNOISE_RTCD "Use RNNoise x86 runtime SIMD dispatch (SSE4.1/AVX2)" ON)
if(BUILD_TESTS)
    enable_testing()
endif()

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
if(ENABLE_VOICE)
    if(USE_VCPKG)
        find_package(unofficial-sodium CONFIG REQUIRED)
        find_package(Opus CONFIG REQUIRED)

        set(SODIUM_TARGET unofficial-sodium::sodium)
        set(OPUS_TARGET Opus::opus)
    else()
        find_package(PkgConfig REQUIRED)

        pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
        pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

        set(SODIUM_TARGET PkgConfig::SODIUM)
        set(OPUS_TARGET PkgConfig::OPUS)
    endif()
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Network Sql Test LinguistTools)
find_package(Qt${QT_VERSION_MAJOR}
    COMPONENTS
        Core
        Gui
        Widgets
        Network
        Sql
        Concurrent
        Test
        LinguistTools
)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS ZlibPrivate QUIET)
qt_standard_project_setup(I18N_TRANSLATED_LANGUAGES en)

# If Qt::ZlibPrivate is not available, create a QtZlib compatibility include
# directory so that #include <QtZlib/zlib.h> resolves using system zlib.
if(NOT TARGET Qt::ZlibPrivate)
    find_package(ZLIB REQUIRED)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/qtzlib-compat/QtZlib")
    file(WRITE "${CMAKE_BINARY_DIR}/qtzlib-compat/QtZlib/zlib.h" "#include <zlib.h>\n")
    file(WRITE "${CMAKE_BINARY_DIR}/qtzlib-compat/QtZlib/zconf.h" "#include <zconf.h>\n")
    add_library(QtZlibCompat INTERFACE)
    target_include_directories(QtZlibCompat INTERFACE
        "${CMAKE_BINARY_DIR}/qtzlib-compat"
        ${ZLIB_INCLUDE_DIRS}
    )
    target_link_libraries(QtZlibCompat INTERFACE ZLIB::ZLIB)
endif()

find_package(Qt${QT_VERSION_MAJOR}Keychain QUIET)
if(Qt${QT_VERSION_MAJOR}Keychain_FOUND)
    message(STATUS "Using system-installed QtKeychain")
else()
    message(STATUS "System QtKeychain not found, using vendored submodule")
    if(QT_VERSION_MAJOR EQUAL 6)
        set(BUILD_WITH_QT6 ON CACHE BOOL "" FORCE)
    else()
        set(BUILD_WITH_QT6 OFF CACHE BOOL "" FORCE)
    endif()
    set(BUILD_TEST_APPLICATION OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_TRANSLATIONS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(vendor/qtkeychain EXCLUDE_FROM_ALL)
endif()

if(ENABLE_VOICE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    find_package(MLSPP QUIET)
    if(NOT TARGET MLSPP::mlspp)
        message(STATUS "mlspp not found, using subproject")
        set(DISABLE_GREASE ON CACHE BOOL "" FORCE)
        set(TESTING OFF CACHE BOOL "" FORCE)
        set(MLS_CXX_NAMESPACE "mlspp" CACHE STRING "" FORCE)
        set(CMAKE_EXPORT_PACKAGE_REGISTRY OFF CACHE BOOL "" FORCE)
        add_subdirectory(vendor/mlspp EXCLUDE_FROM_ALL)
        if(NOT TARGET MLSPP::mlspp)
            add_library(MLSPP::mlspp ALIAS mlspp)
        endif()
        if(NOT TARGET MLSPP::hpke)
            add_library(MLSPP::hpke ALIAS hpke)
        endif()
        set(MLSPP_FOUND TRUE CACHE BOOL "" FORCE)
        # Write a shim config so libdave's find_dependency(MLSPP) finds this
        # instead of the broken build-tree config that add_subdirectory generates
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mlspp-shim/MLSPPConfig.cmake"
            "set(MLSPP_FOUND TRUE)\n")
        set(MLSPP_DIR "${CMAKE_CURRENT_BINARY_DIR}/mlspp-shim" CACHE PATH "" FORCE)
    else()
        message(STATUS "Found system mlspp")
    endif()

    # Prevent find_package from finding mlspp's broken build-tree config via registry
    set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)
    # libdave (Discord Audio/Video E2E Encryption)
    set(TESTING OFF CACHE BOOL "" FORCE)
    set(PERSISTENT_KEYS OFF CACHE BOOL "" FORCE)
    # Prevent libdave's own vcpkg manifest from being processed; our
    # project-level vcpkg.json already provides all required packages.
    set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "" FORCE)
    add_subdirectory(vendor/libdave/cpp EXCLUDE_FROM_ALL)
endif()

# find or build rnnoise
include(rnnoise.cmake)

set(PROJECT_SOURCES
    resources.qrc
    src/main.cpp
    src/Core/Snowflake.cpp
    src/Core/ClientInstance.cpp
    src/Core/Session.cpp
    src/Core/Logging.cpp
    src/Core/MessageManager.cpp
    src/Core/ImageManager.cpp
    src/Core/UserManager.cpp
    src/Core/TypingTracker.cpp
    src/Core/PermissionManager.cpp
    src/Core/PermissionComputer.cpp
    src/Core/ReadStateManager.cpp
    src/Core/MemberListManager.cpp
    src/Core/RelationshipManager.cpp
    src/Core/MurmurHash3.cpp
    src/Core/EmojiSegmenter.cpp
    src/Core/Markdown/Parser.cpp
    src/Core/Theme/Tokens.cpp
    src/Core/Theme/Manager.cpp
    src/Core/Theme/Generator.cpp
    src/Core/Theme/Stylesheet.cpp
    src/Core/Theme/Fonts.cpp
    src/UI/MainWindow.cpp
    src/UI/BrowserCaptchaResolver.cpp
    src/UI/TypingIndicator.cpp
    src/UI/SlowModeIndicator.cpp
    src/UI/ConnectionBanner.cpp
    src/UI/ElidedLabel.cpp
    src/UI/ImageViewer.cpp
    src/UI/Accounts/AccountsWindow.cpp
    src/UI/Settings/SettingsWindow.cpp
    src/UI/Settings/GeneralPage.cpp
    src/UI/Settings/AppearancePage.cpp
    src/UI/Accounts/AccountsModel.cpp
    src/UI/ChannelList/ChannelDelegate.cpp
    src/UI/ChannelList/ChannelTreeModel.cpp
    src/UI/ChannelList/ChannelFilterProxyModel.cpp
    src/UI/ChannelList/ChannelTreeView.cpp
    src/UI/ChannelList/ServerRailModel.cpp
    src/UI/ChannelList/ServerRailDelegate.cpp
    src/UI/ChannelList/ServerRailView.cpp
    src/UI/TabBar/TabBar.cpp
    src/UI/Chat/ChatModel.cpp
    src/UI/Chat/ChatDelegate.cpp
    src/UI/Chat/ChatView.cpp
    src/UI/Chat/ChatLayout.cpp
    src/UI/Dialogs/TokenInputDialog.cpp
    src/UI/Dialogs/QRLoginDialog.cpp
    src/UI/Dialogs/BasePopup.cpp
    src/UI/Dialogs/ConfirmPopup.cpp
    src/UI/Dialogs/UserProfilePopup.cpp
    src/UI/MemberList/MemberListModel.cpp
    src/UI/MemberList/MemberListDelegate.cpp
    src/UI/MemberList/MemberListView.cpp
    src/UI/Input/MessageInput.cpp
    src/UI/Input/AttachmentPreviewPanel.cpp
    src/Storage/DatabaseManager.cpp
    src/Storage/BaseRepository.cpp
    src/Storage/Transaction.cpp
    src/Storage/AccountRepository.cpp
    src/Storage/GuildRepository.cpp
    src/Storage/ChannelRepository.cpp
    src/Storage/MessageRepository.cpp
    src/Storage/UserRepository.cpp
    src/Storage/MemberRepository.cpp
    src/Storage/RoleRepository.cpp
    src/Discord/Client.cpp
    src/Discord/Gateway.cpp
    src/Discord/RemoteAuthClient.cpp
    src/Discord/IngestThread.cpp
    src/Discord/HttpClient.cpp
    src/Discord/RequestWorker.cpp
    src/Discord/CurlUtils.cpp
    src/Discord/CdnUrls.cpp
    src/Discord/ClientIdentity.cpp
    src/Proto/ProtoReader.cpp
    src/Proto/UserSettings.cpp
    src/Core/TokenStore.cpp
    src/App.cpp
)

if(ENABLE_VOICE)
    list(APPEND PROJECT_SOURCES
        src/UI/VoiceStatusBar.cpp
        src/UI/VoiceWindow.cpp
        src/Discord/AV/VoiceGateway.cpp
        src/Discord/AV/UdpTransport.cpp
        src/Discord/AV/VoiceClient.cpp
        src/Discord/AV/RtpPacket.cpp
        src/Discord/AV/VoiceEncryption.cpp
        src/Discord/AV/DaveSession.cpp
        src/Core/AV/VoiceManager.cpp
        src/Core/AV/AudioPipeline.cpp
        src/Core/AV/IAudioBackend.cpp
        src/Core/AV/MiniaudioAudioBackend.cpp
        src/Core/AV/OpusEncoder.cpp
        src/Core/AV/OpusDecoder.cpp
        src/Core/AV/JitterBuffer.cpp
        src/Core/AV/AudioMixer.cpp
        src/Core/AV/NoiseSuppressor.cpp
    )
endif()

find_package(unofficial-nayuki-qr-code-generator CONFIG QUIET)
if(TARGET unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator)
    message(STATUS "Using installed nayuki-qr-code-generator")
else()
    message(STATUS "nayuki-qr-code-generator not found, using vendored version")
    list(APPEND PROJECT_SOURCES vendor/qrcodegen/cpp/qrcodegen.cpp)
endif()

qt_add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE vendor/emoji-segmenter)
if(TARGET unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator)
    target_link_libraries(${PROJECT_NAME} PRIVATE unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator)
else()
    target_include_directories(${PROJECT_NAME} PRIVATE vendor/qrcodegen/cpp)
endif()

if(BUILD_TESTS)
    qt_add_executable(TestMarkdown tests/tst_Markdown.cpp src/Core/Markdown/Parser.cpp src/Core/EmojiSegmenter.cpp)
    target_link_libraries(TestMarkdown PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestMarkdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/vendor/emoji-segmenter)
    add_test(NAME MarkdownTests COMMAND TestMarkdown)
endif()

qt_add_translations(${PROJECT_NAME})

if (MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL>)
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:/ZI> $<$<NOT:$<CONFIG:Debug>>:/Zi>)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)

    if(BUILD_TESTS)
        target_link_options(TestMarkdown PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL>)
        target_compile_options(TestMarkdown PRIVATE $<$<CONFIG:Debug>:/ZI> $<$<NOT:$<CONFIG:Debug>>:/Zi>)
        target_compile_options(TestMarkdown PRIVATE /utf-8)
    endif()
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(${PROJECT_NAME}
    PROPERTIES
        WIN32_EXECUTABLE TRUE
)

target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
if(NOT ENABLE_VOICE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ACHERON_NO_VOICE)
endif()

target_link_libraries(${PROJECT_NAME}
    PUBLIC
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Network
        Qt::Sql
        Qt::Concurrent
        CURL::libcurl
    PRIVATE
        qt${QT_VERSION_MAJOR}keychain
        OpenSSL::Crypto
)
if(ENABLE_VOICE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${SODIUM_TARGET}
        ${OPUS_TARGET}
        libdave
    )

    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniaudio")
    if(ENABLE_RNNOISE)
        target_link_libraries(${PROJECT_NAME} PRIVATE rnnoise)
        target_compile_definitions(${PROJECT_NAME} PRIVATE ACHERON_HAVE_RNNOISE)
    endif()
endif()
if(TARGET Qt::ZlibPrivate)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt::ZlibPrivate)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE QtZlibCompat)
endif()
target_precompile_headers(${PROJECT_NAME} PUBLIC src/pch.h)

include(CheckSymbolExists)
set(CMAKE_REQUIRED_LIBRARIES CURL::libcurl)
check_symbol_exists(curl_easy_impersonate "curl/curl.h" HAS_CURL_IMPERSONATE)
unset(CMAKE_REQUIRED_LIBRARIES)

if(HAS_CURL_IMPERSONATE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE IS_CURL_IMPERSONATE)
else()
    message(WARNING "curl_easy_impersonate not found, libcurl-impersonate is highly recommended")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/certs/cacert.pem")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/certs"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/certs/cacert.pem"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/certs/cacert.pem"
        COMMENT "Copying cacert.pem to build directory"
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/TwemojiCOLRv0.ttf")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/TwemojiCOLRv0.ttf"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/fonts/TwemojiCOLRv0.ttf"
        COMMENT "Copying TwemojiCOLRv0.ttf to build directory"
    )
endif()

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(UNIX AND NOT APPLE)
    install(FILES resources/desktop/io.github.ouwou.acheron.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
endif()
