cmake_minimum_required(VERSION 3.16)

option(USE_VCPKG "Use bundled vcpkg toolchain" ON)
option(ENABLE_LOTTIE "Build with rlottie for Lottie sticker playback" ON)
option(FFMPEG_FROM_VCPKG "Build ffmpeg from source through vcpkg and link it statically" OFF)

# Must happen before project()
if(USE_VCPKG AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

if(ENABLE_LOTTIE)
    list(APPEND VCPKG_MANIFEST_FEATURES "lottie")
endif()
if(FFMPEG_FROM_VCPKG)
    list(APPEND VCPKG_MANIFEST_FEATURES "ffmpeg")
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
option(ENABLE_FFMPEG "Build with ffmpeg/libav video playback support" ON)
option(ENABLE_VIDEO "Build with cameras and screen shares in calls, watched and sent (requires ENABLE_VOICE and ENABLE_FFMPEG)" ON)
option(ALLOW_PLAIN_CURL "Permit building against plain libcurl without curl-impersonate" OFF)
if(BUILD_TESTS)
    enable_testing()
endif()

find_package(CURL REQUIRED)
if(NOT TARGET CURL::libcurl AND TARGET CURL::libcurl-impersonate)
    add_library(CURL::libcurl INTERFACE IMPORTED)
    set_target_properties(CURL::libcurl PROPERTIES
        INTERFACE_LINK_LIBRARIES CURL::libcurl-impersonate
        INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIRS}")
endif()
# FindCURL adds what pkg-config says of the system's own libcurl to whichever library was named.
# beside curl-impersonate that is a second libcurl in the program, and requests go to either
if(CURL_LIBRARY MATCHES "impersonate")
    get_target_property(_curl_links CURL::libcurl INTERFACE_LINK_LIBRARIES)
    if(_curl_links)
        list(FILTER _curl_links EXCLUDE REGEX "(^|/)(lib)?curl(\\.[a-z0-9.]+)?$")
        set_property(TARGET CURL::libcurl PROPERTY INTERFACE_LINK_LIBRARIES "${_curl_links}")
    endif()
endif()
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

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Svg Network Sql Test LinguistTools)
find_package(Qt${QT_VERSION_MAJOR}
    COMPONENTS
        Core
        Gui
        Widgets
        Svg
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

include(ffmpeg.cmake)
# Core/Media/HardwareDevices.hpp looks for libva with dlopen, which older C libraries keep apart
if(TARGET ffmpeg AND UNIX AND CMAKE_DL_LIBS)
    target_link_libraries(ffmpeg INTERFACE ${CMAKE_DL_LIBS})
endif()
include(linuxcapture.cmake)

include(lottie.cmake)

# cameras and screen shares in calls. they need the RTC connection and ffmpeg, which are
# there for other things as well, and bring in what nothing else uses: Qt's OpenGL module,
# which the video surface draws through where it can (Qt 5 has it in Gui and Widgets), and
# ffmpeg's avfilter and avdevice, which ffmpeg.cmake takes where it finds them
set(ACHERON_HAVE_VIDEO OFF)
set(QT_OPENGL_LIBRARIES "")
if(ENABLE_VIDEO AND ENABLE_VOICE AND TARGET ffmpeg)
    set(ACHERON_HAVE_VIDEO ON)
    if(QT_VERSION_MAJOR GREATER_EQUAL 6)
        find_package(Qt6 QUIET COMPONENTS OpenGL)
        if(NOT TARGET Qt6::OpenGL)
            message(FATAL_ERROR
                "ENABLE_VIDEO is on but Qt's OpenGL module was not found. It comes with Qt's "
                "base; on Linux it also needs the OpenGL development headers. Configure with "
                "-DENABLE_VIDEO=OFF to build without cameras and screen shares.")
        endif()
        set(QT_OPENGL_LIBRARIES Qt6::OpenGL)
    endif()
    message(STATUS "Cameras and screen shares: on")
elseif(ENABLE_VIDEO)
    message(STATUS "Cameras and screen shares: off, as they need ENABLE_VOICE and ENABLE_FFMPEG")
else()
    message(STATUS "Cameras and screen shares: off")
endif()

set(PROJECT_SOURCES
    resources.qrc
    src/main.cpp
    src/Core/Snowflake.cpp
    src/Core/OrderedJson.cpp
    src/Core/ProxyConfig.cpp
    src/Core/ClientInstance.cpp
    src/Core/Session.cpp
    src/Core/Logging.cpp
    src/Core/MessageManager.cpp
    src/Core/MessageReactors.cpp
    src/Core/MessageSegments.cpp
    src/Core/ForumManager.cpp
    src/Core/Calls/CallStore.cpp
    src/Core/Streams/StreamManager.cpp
    src/Core/Streams/StreamPreviews.cpp
    src/Core/ImageManager.cpp
    src/Core/AnimatedImageCache.cpp
    src/Core/ApngDecoder.cpp
    src/Core/LottieDecoder.cpp
    src/Core/UserManager.cpp
    src/Core/TypingTracker.cpp
    src/Core/PermissionManager.cpp
    src/Core/PermissionComputer.cpp
    src/Core/RoleHierarchy.cpp
    src/Core/ReadStateManager.cpp
    src/Core/MemberListManager.cpp
    src/Core/RelationshipManager.cpp
    src/Core/Presence/PresenceManager.cpp
    src/Core/Presence/ActivityFormat.cpp
    src/Core/MurmurHash3.cpp
    src/Core/EmojiSegmenter.cpp
    src/Core/Emoji/EmojiManager.cpp
    src/Core/Gifs/FavoriteGifRules.cpp
    src/Core/Gifs/FavoriteGifs.cpp
    src/Core/Emoji/FrecencyTracker.cpp
    src/Core/Emoji/UnicodeEmojiIndex.cpp
    src/Core/Markdown/Parser.cpp
    src/Core/Theme/Tokens.cpp
    src/Core/Theme/Manager.cpp
    src/Core/Theme/Icons.cpp
    src/Core/Theme/Generator.cpp
    src/Core/Theme/Stylesheet.cpp
    src/Core/Theme/Fonts.cpp
    src/Core/Media/Player.cpp
    src/Core/Media/MediaDecoder.cpp
    src/Core/Media/RgbScaler.cpp
    src/Core/Media/InputConfinement.cpp
    src/Core/Media/AudioOutput.cpp
    src/Core/Media/PlayerPool.cpp
    src/Core/Media/ClipDecoder.cpp
    src/Core/Media/ClipPlayer.cpp
    src/Core/Media/CurlAvio.cpp
    src/UI/MainWindow.cpp
    src/UI/BrowserCaptchaResolver.cpp
    src/UI/TypingIndicator.cpp
    src/UI/SlowModeIndicator.cpp
    src/UI/ConnectionBanner.cpp
    src/UI/ChannelTopicLine.cpp
    src/UI/ElidedLabel.cpp
    src/UI/StatusIndicator.cpp
    src/UI/TrayIcon.cpp
    src/UI/ProxyLineEdit.cpp
    src/UI/ImageViewer.cpp
    src/UI/StreamPreviewPopup.cpp
    src/UI/VideoFullscreenWindow.cpp
    src/UI/Accounts/AccountsWindow.cpp
    src/UI/Settings/SettingsWindow.cpp
    src/UI/Settings/GeneralPage.cpp
    src/UI/Settings/AppearancePage.cpp
    src/UI/GuildSettings/AuditLogFormatter.cpp
    src/UI/GuildSettings/GuildAuditLogPage.cpp
    src/UI/GuildSettings/GuildBansPage.cpp
    src/UI/GuildSettings/GuildBoostPerksPage.cpp
    src/UI/GuildSettings/GuildEmojiPage.cpp
    src/UI/GuildSettings/GuildEngagementPage.cpp
    src/UI/GuildSettings/GuildInvitesPage.cpp
    src/UI/GuildSettings/GuildMembersPage.cpp
    src/UI/GuildSettings/GuildProfilePage.cpp
    src/UI/GuildSettings/GuildRolesPage.cpp
    src/UI/GuildSettings/GuildSettingsPage.cpp
    src/UI/GuildSettings/GuildSettingsSection.cpp
    src/UI/GuildSettings/GuildSettingsWindow.cpp
    src/UI/GuildSettings/GuildStickersPage.cpp
    src/UI/GuildSettings/ImageUpload.cpp
    src/UI/GuildSettings/MemberModeration.cpp
    src/UI/GuildSettings/UnsavedChangesBar.cpp
    src/UI/Accounts/AccountsModel.cpp
    src/UI/ChannelList/ChannelDelegate.cpp
    src/UI/ChannelList/ChannelTreeModel.cpp
    src/UI/ChannelList/ChannelFilterProxyModel.cpp
    src/UI/ChannelList/ChannelTreeView.cpp
    src/UI/ChannelList/ServerRailModel.cpp
    src/UI/ChannelList/ServerRailDelegate.cpp
    src/UI/ChannelList/ServerRailView.cpp
    src/UI/ChannelList/MentionJumpIndicator.cpp
    src/UI/TabBar/TabBar.cpp
    src/UI/Chat/ChatModel.cpp
    src/UI/Chat/ChatDelegate.cpp
    src/UI/Chat/ChatView.cpp
    src/UI/Chat/ChatLayout.cpp
    src/UI/Chat/InlineVideoController.cpp
    src/UI/Chat/GifPlayback.cpp
    src/UI/Chat/GifPainting.cpp
    src/UI/Chat/FrameAnimator.cpp
    src/UI/Chat/EmojiTextObject.cpp
    src/UI/Chat/MessageTextDocument.cpp
    src/UI/Chat/MessageTextBrowser.cpp
    src/UI/Chat/MediaTarget.cpp
    src/UI/Chat/VideoControls.cpp
    src/UI/Chat/MessageActionBar.cpp
    src/UI/Chat/ReactionTooltip.cpp
    src/UI/Forum/ForumPostModel.cpp
    src/UI/Forum/ForumPostDelegate.cpp
    src/UI/Forum/ForumBrowser.cpp
    src/UI/Forum/NewPostDialog.cpp
    src/UI/ThreadBrowser/ThreadBrowserPopup.cpp
    src/UI/Dialogs/TokenInputDialog.cpp
    src/UI/Dialogs/ProxyInputDialog.cpp
    src/UI/Dialogs/QRLoginDialog.cpp
    src/UI/Dialogs/DirectLoginDialog.cpp
    src/UI/Dialogs/BasePopup.cpp
    src/UI/Dialogs/ChannelTopicPopup.cpp
    src/UI/Dialogs/ConfirmPopup.cpp
    src/UI/Dialogs/ReactionsPopup.cpp
    src/UI/Dialogs/UserProfilePopup.cpp
    src/UI/Dialogs/ActivityCard.cpp
    src/UI/MemberList/MemberListModel.cpp
    src/UI/MemberList/MemberListDelegate.cpp
    src/UI/MemberList/MemberListView.cpp
    src/UI/Input/MessageInput.cpp
    src/UI/Input/EmojiAutocompletePopup.cpp
    src/UI/Input/TextEdgeNavigation.cpp
    src/UI/Emoji/EmojiButton.cpp
    src/UI/Emoji/EmojiGlyphs.cpp
    src/UI/Emoji/EmojiGridView.cpp
    src/UI/Emoji/EmojiPainting.cpp
    src/UI/Emoji/EmojiPicker.cpp
    src/UI/Emoji/ExpressionPicker.cpp
    src/UI/Gifs/GifMasonryLayout.cpp
    src/UI/Gifs/FavoriteGifGrid.cpp
    src/UI/Gifs/FavoriteGifPicker.cpp
    src/UI/Emoji/EmojiSectionRail.cpp
    src/UI/Emoji/PaintedImages.cpp
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
    src/Discord/LoginClient.cpp
    src/Discord/IngestThread.cpp
    src/Discord/HttpClient.cpp
    src/Discord/ApiError.cpp
    src/Discord/RequestWorker.cpp
    src/Discord/CurlUtils.cpp
    src/Discord/CdnUrls.cpp
    src/Discord/StreamKey.cpp
    src/Discord/ClientIdentity.cpp
    src/Discord/ContextProperties.cpp
    src/Proto/ProtoReader.cpp
    src/Proto/ProtoWriter.cpp
    src/Proto/UserSettings.cpp
    src/Proto/FrecencySettings.cpp
    src/Core/TokenStore.cpp
    src/App.cpp
)

if(ENABLE_VOICE)
    list(APPEND PROJECT_SOURCES
        src/UI/VoiceStatusBar.cpp
        src/UI/VoiceWindow.cpp
        src/Core/Audio/VoiceManager.cpp
        src/Core/Audio/AudioPipeline.cpp
        src/Core/Audio/IAudioBackend.cpp
        src/Core/Audio/MiniaudioAudioBackend.cpp
        src/Core/Audio/OpusDecoder.cpp
        src/Core/Audio/JitterBuffer.cpp
        src/Core/Audio/AudioMixer.cpp
        src/Core/Audio/NoiseSuppressor.cpp
    )
endif()

if(ACHERON_HAVE_VIDEO)
    list(APPEND PROJECT_SOURCES
        src/Core/Streams/StreamSession.cpp
        src/Core/Streams/StreamWatcher.cpp
        src/Core/Streams/StreamBroadcaster.cpp
        src/Core/Streams/StreamAudio.cpp
        src/Core/Video/CameraSender.cpp
        src/Core/Video/CallVideo.cpp
        src/UI/Call/CallView.cpp
        src/UI/Call/CallControlsBar.cpp
        src/UI/Call/CallStats.cpp
        src/UI/Call/CallTile.cpp
        src/UI/Call/CallTileModel.cpp
        src/UI/Settings/VideoPage.cpp
        src/UI/ScreenSharePicker.cpp
    )
endif()

# the RTC connection and, where the build has it, everything video needs below the program:
# codecs, capture, the encode pipelines and the surface video is drawn on, with the few parts of
# the program they stand on. the program and the tests take all of these from here
if(ENABLE_VOICE)
    set(MEDIA_ENGINE_SOURCES
        src/Core/Logging.cpp
        src/Core/Snowflake.cpp
        src/Core/OrderedJson.cpp
        src/Core/ProxyConfig.cpp
        src/Discord/CurlUtils.cpp
        src/Discord/ClientIdentity.cpp
        src/Discord/Voice/VoiceGateway.cpp
        src/Discord/Voice/UdpTransport.cpp
        src/Discord/Voice/Socks5UdpAssociation.cpp
        src/Discord/Voice/VoiceClient.cpp
        src/Discord/Voice/RtpPacket.cpp
        src/Discord/Voice/VoiceEncryption.cpp
        src/Discord/Voice/DaveSession.cpp
        src/Discord/Voice/Rtcp.cpp
        src/Discord/Voice/SinkWants.cpp
        src/Discord/Voice/VideoReceivePath.cpp
        src/Discord/Voice/VideoSendPath.cpp
        src/Discord/Voice/VideoDepacketizer.cpp
        src/Discord/Voice/VideoReceiver.cpp
        src/Discord/Voice/VideoPacketizer.cpp
        src/Discord/Voice/VideoSendStream.cpp
        src/Discord/Voice/SendSideEstimator.cpp
        src/Discord/Voice/TransportFeedback.cpp
        src/Core/Audio/VoiceConnection.cpp
        src/Core/Audio/OpusEncoder.cpp
        src/Core/Video/TileVisibility.cpp
    )
    if(ACHERON_HAVE_VIDEO)
        list(APPEND MEDIA_ENGINE_SOURCES
            src/Core/Media/RgbScaler.cpp
            src/Core/Media/CodecSupport.cpp
            src/Core/Media/DecodeDemand.cpp
            src/Core/Media/VideoDecoder.cpp
            src/Core/Media/VideoEncoder.cpp
            src/Core/Media/H264Sps.cpp
            src/Core/Audio/LoopbackCapture.cpp
            src/Core/Audio/LoopbackCaptureWindows.cpp
            src/Core/Audio/LoopbackCapturePipeWire.cpp
            src/Core/Streams/StreamConnection.cpp
            src/Core/Video/ScreenPipeline.cpp
            src/Core/Video/EncodePipeline.cpp
            src/Core/Video/ScreenCapture.cpp
            src/Core/Video/CardScaler.cpp
            src/Core/Video/CameraCapture.cpp
            src/Core/Video/CameraPipeline.cpp
            src/Core/Video/SendQuality.cpp
            src/Core/Video/SendRateController.cpp
            src/UI/Video/VideoSurface.cpp
            src/UI/Video/VideoFrameStore.cpp
            src/UI/Video/GlVideoLayer.cpp
        )
        if(WIN32)
            list(APPEND MEDIA_ENGINE_SOURCES
                src/UI/Video/D3D11GlBridge.cpp
                src/UI/Video/CardWorker.cpp
                src/Core/Video/ScreenCaptureWindows.cpp
                src/Core/Video/Windows/CardPictures.cpp
                src/Core/Video/Windows/GraphicsCaptureSession.cpp
                src/Core/Video/Windows/Monitors.cpp
                src/Core/Video/Windows/WindowDrawer.cpp
                src/Core/Video/Windows/WindowQueries.cpp
            )
        endif()
        if(UNIX AND NOT APPLE)
            list(APPEND MEDIA_ENGINE_SOURCES
                src/UI/Video/DmabufGlBridge.cpp
                src/Core/Video/ScreenCaptureLinux.cpp
                src/Core/Video/X11Capture.cpp
                src/Core/Video/PortalCapture.cpp
            )
        endif()
    endif()

    list(REMOVE_ITEM PROJECT_SOURCES ${MEDIA_ENGINE_SOURCES})
    add_library(acheron_media STATIC ${MEDIA_ENGINE_SOURCES})
    target_include_directories(acheron_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(acheron_media PRIVATE NOMINMAX)
    target_link_libraries(acheron_media PUBLIC Qt::Core Qt::Gui Qt::Widgets Qt::Network CURL::libcurl ${SODIUM_TARGET} ${OPUS_TARGET} libdave MLSPP::mlspp)
    if(ACHERON_HAVE_VIDEO)
        target_include_directories(acheron_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniaudio")
        target_compile_definitions(acheron_media PUBLIC ACHERON_HAVE_FFMPEG)
        target_link_libraries(acheron_media PUBLIC ffmpeg linuxcapture ${QT_OPENGL_LIBRARIES})
        if(WIN32)
            target_link_libraries(acheron_media PUBLIC ole32)
        endif()
    endif()
endif()

if(ENABLE_VOICE OR TARGET ffmpeg)
    list(APPEND PROJECT_SOURCES
        src/Core/Audio/MiniaudioImpl.cpp
        src/Core/Audio/AudioBackends.cpp
        src/UI/Settings/AudioPage.cpp
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
    qt_add_executable(TestMarkdown tests/tst_Markdown.cpp src/Core/Markdown/Parser.cpp
        src/Core/EmojiSegmenter.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestMarkdown PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestMarkdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/vendor/emoji-segmenter)
    add_test(NAME MarkdownTests COMMAND TestMarkdown)

    qt_add_executable(TestFrecency tests/tst_Frecency.cpp src/Core/Emoji/FrecencyTracker.cpp)
    target_link_libraries(TestFrecency PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestFrecency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME FrecencyTests COMMAND TestFrecency)

    qt_add_executable(TestUnicodeEmoji tests/tst_UnicodeEmoji.cpp
        src/Core/Emoji/UnicodeEmojiIndex.cpp src/Core/Logging.cpp resources.qrc)
    target_link_libraries(TestUnicodeEmoji PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestUnicodeEmoji PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME UnicodeEmojiTests COMMAND TestUnicodeEmoji)

    qt_add_executable(TestOrderedJson tests/tst_OrderedJson.cpp src/Core/OrderedJson.cpp
        src/Core/Snowflake.cpp src/Discord/ContextProperties.cpp)
    target_link_libraries(TestOrderedJson PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestOrderedJson PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME OrderedJsonTests COMMAND TestOrderedJson)

    qt_add_executable(TestProto tests/tst_Proto.cpp src/Proto/ProtoReader.cpp
        src/Proto/ProtoWriter.cpp src/Proto/FrecencySettings.cpp src/Core/Logging.cpp)
    target_link_libraries(TestProto PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestProto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ProtoTests COMMAND TestProto)

    qt_add_executable(TestProxy tests/tst_Proxy.cpp src/Core/ProxyConfig.cpp
        src/Core/Logging.cpp src/Core/ImageManager.cpp src/Core/Snowflake.cpp
        src/Discord/Voice/Socks5UdpAssociation.cpp)

    target_link_libraries(TestProxy PRIVATE Qt::Core Qt::Gui Qt::Widgets Qt::Network Qt::Test)
    target_include_directories(TestProxy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ProxyTests COMMAND TestProxy)

    qt_add_executable(TestVoicePayloads tests/tst_VoicePayloads.cpp src/Core/OrderedJson.cpp
        src/Core/Snowflake.cpp)
    target_link_libraries(TestVoicePayloads PRIVATE Qt::Core Qt::Network Qt::Test)
    target_include_directories(TestVoicePayloads PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME VoicePayloadTests COMMAND TestVoicePayloads)

    qt_add_executable(TestStreams tests/tst_Streams.cpp src/Core/Streams/StreamManager.cpp
        src/Core/Streams/StreamPreviews.cpp src/Discord/StreamKey.cpp src/Core/Logging.cpp
        src/Core/OrderedJson.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestStreams PRIVATE Qt::Core Qt::Gui Qt::Network Qt::Test)
    target_include_directories(TestStreams PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME StreamTests COMMAND TestStreams)

    qt_add_executable(TestSendQuality tests/tst_SendQuality.cpp src/Core/Video/SendQuality.cpp src/Core/Video/SendRateController.cpp)
    target_link_libraries(TestSendQuality PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestSendQuality PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME SendQualityTests COMMAND TestSendQuality)

    qt_add_executable(TestDecodeDemand tests/tst_DecodeDemand.cpp src/Core/Media/DecodeDemand.cpp)
    target_link_libraries(TestDecodeDemand PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestDecodeDemand PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME DecodeDemandTests COMMAND TestDecodeDemand)

    qt_add_executable(TestCallTiles tests/tst_CallTiles.cpp src/UI/Call/CallTileModel.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestCallTiles PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestCallTiles PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME CallTilesTests COMMAND TestCallTiles)

    qt_add_executable(TestCalls tests/tst_Calls.cpp src/Core/Calls/CallStore.cpp
        src/Core/OrderedJson.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestCalls PRIVATE Qt::Core Qt::Gui Qt::Network Qt::Test)
    target_include_directories(TestCalls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME CallTests COMMAND TestCalls)

    if(ENABLE_VOICE)
        # a test of the media engine, with what it takes of the program beside it
        function(add_media_test name ctest_name)
            qt_add_executable(${name} ${ARGN})
            target_link_libraries(${name} PRIVATE acheron_media Qt::Test)
            target_compile_definitions(${name} PRIVATE NOMINMAX)
            add_test(NAME ${ctest_name} COMMAND ${name})
        endfunction()

        add_media_test(TestRtp RtpTests tests/tst_Rtp.cpp)
        add_media_test(TestOpusEncoder OpusEncoderTests tests/tst_OpusEncoder.cpp)
        add_media_test(TestUdpTransport UdpTransportTests tests/tst_UdpTransport.cpp)
        add_media_test(TestSinkWants SinkWantsTests tests/tst_SinkWants.cpp)
        add_media_test(TestVoiceClient VoiceClientTests tests/tst_VoiceClient.cpp)

        if(ACHERON_HAVE_VIDEO)
            add_media_test(TestLoopbackCapture LoopbackCaptureTests tests/tst_LoopbackCapture.cpp)
            add_media_test(TestH264Sps H264SpsTests tests/tst_H264Sps.cpp)
            add_media_test(TestRtpVideo RtpVideoTests tests/tst_RtpVideo.cpp)
            add_media_test(TestVideoDecode VideoDecodeTests tests/tst_VideoDecode.cpp)
            add_media_test(TestVideoSend VideoSendTests tests/tst_VideoSend.cpp)
            add_media_test(TestSendSideEstimator SendSideEstimatorTests tests/tst_SendSideEstimator.cpp)
            add_media_test(TestVideoEncode VideoEncodeTests tests/tst_VideoEncode.cpp)
            add_media_test(TestCameraCapture CameraCaptureTests tests/tst_CameraCapture.cpp)
            add_media_test(TestVideoSurface VideoSurfaceTests tests/tst_VideoSurface.cpp)
            if(UNIX AND NOT APPLE)
                add_media_test(TestX11Capture X11CaptureTests tests/tst_X11Capture.cpp)
                add_media_test(TestPortalCapture PortalCaptureTests tests/tst_PortalCapture.cpp)
            endif()
        endif()
    endif()

    qt_add_executable(TestEmojiGrid tests/tst_EmojiGrid.cpp
        src/UI/Emoji/EmojiGridView.cpp src/UI/Emoji/EmojiGlyphs.cpp src/UI/Emoji/EmojiPainting.cpp
        src/UI/Emoji/PaintedImages.cpp
        src/Core/ImageManager.cpp src/Core/AnimatedImageCache.cpp src/Core/ApngDecoder.cpp
        src/Core/LottieDecoder.cpp src/Core/ProxyConfig.cpp src/Core/Snowflake.cpp src/Core/Logging.cpp
        src/Core/Theme/Manager.cpp src/Core/Theme/Fonts.cpp src/Core/Theme/Stylesheet.cpp
        src/Core/Theme/Tokens.cpp src/Core/Theme/Icons.cpp resources.qrc)
    target_link_libraries(TestEmojiGrid PRIVATE Qt::Core Qt::Gui Qt::Widgets Qt::Network Qt::Svg Qt::Test)
    target_include_directories(TestEmojiGrid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME EmojiGridTests COMMAND TestEmojiGrid)

    qt_add_executable(TestCdnUrls tests/tst_CdnUrls.cpp src/Discord/CdnUrls.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestCdnUrls PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestCdnUrls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME CdnUrlTests COMMAND TestCdnUrls)

    qt_add_executable(TestEmbeds tests/tst_Embeds.cpp src/Discord/CdnUrls.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestEmbeds PRIVATE Qt::Core Qt::Gui Qt::Test)
    target_include_directories(TestEmbeds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME EmbedTests COMMAND TestEmbeds)

    qt_add_executable(TestFavoriteGifs tests/tst_FavoriteGifs.cpp src/Core/Gifs/FavoriteGifRules.cpp src/UI/Gifs/GifMasonryLayout.cpp
        src/Discord/CdnUrls.cpp src/Core/Snowflake.cpp)
    target_link_libraries(TestFavoriteGifs PRIVATE Qt::Core Qt::Test)
    target_include_directories(TestFavoriteGifs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME FavoriteGifTests COMMAND TestFavoriteGifs)

    if(TARGET ffmpeg)
        qt_add_executable(TestClips tests/tst_Clips.cpp src/Core/Media/ClipDecoder.cpp src/Core/Media/ClipPlayer.cpp src/Core/Media/RgbScaler.cpp)
        target_link_libraries(TestClips PRIVATE Qt::Core Qt::Gui Qt::Test ffmpeg)
        target_compile_definitions(TestClips PRIVATE ACHERON_HAVE_FFMPEG)
        target_include_directories(TestClips PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        add_test(NAME ClipTests COMMAND TestClips)

        qt_add_executable(TestMediaInput tests/tst_MediaInput.cpp src/Core/Media/RgbScaler.cpp src/Core/Media/InputConfinement.cpp src/Core/Logging.cpp)
        target_link_libraries(TestMediaInput PRIVATE Qt::Core Qt::Gui Qt::Network Qt::Test ffmpeg)
        target_compile_definitions(TestMediaInput PRIVATE ACHERON_HAVE_FFMPEG)
        target_include_directories(TestMediaInput PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        add_test(NAME MediaInputTests COMMAND TestMediaInput)

        qt_add_executable(TestApng tests/tst_Apng.cpp src/Core/ApngDecoder.cpp)
        target_link_libraries(TestApng PRIVATE Qt::Core Qt::Gui Qt::Test ffmpeg)
        target_compile_definitions(TestApng PRIVATE ACHERON_HAVE_FFMPEG)
        target_include_directories(TestApng PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        add_test(NAME ApngTests COMMAND TestApng)
    endif()
endif()

set(_translated_targets ${PROJECT_NAME})
if(TARGET acheron_media)
    list(APPEND _translated_targets acheron_media)
endif()
qt_add_translations(${PROJECT_NAME} SOURCE_TARGETS ${_translated_targets})

if(NOT MSVC AND QT_VERSION_MAJOR LESS 6)
    get_property(_qt5_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_qt5_target ${_qt5_targets})
        get_target_property(_qt5_target_type ${_qt5_target} TYPE)
        if(_qt5_target_type STREQUAL "EXECUTABLE" OR _qt5_target STREQUAL "acheron_media")
            target_compile_options(${_qt5_target} PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/qt5_compat.h")
        endif()
    endforeach()
endif()

if (MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL>)
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:/ZI> $<$<NOT:$<CONFIG:Debug>>:/Zi>)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)

    if(QT_VERSION_MAJOR LESS 6)
        target_compile_options(${PROJECT_NAME} PRIVATE
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/qt5_compat.h")
    endif()

    if(TARGET acheron_media)
        target_compile_options(acheron_media PRIVATE $<$<CONFIG:Debug>:/ZI> $<$<NOT:$<CONFIG:Debug>>:/Zi> /utf-8)
        if(QT_VERSION_MAJOR LESS 6)
            target_compile_options(acheron_media PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/qt5_compat.h")
        endif()
    endif()

    if(BUILD_TESTS)
        set(_msvc_test_targets TestMarkdown TestUnicodeEmoji TestOrderedJson TestFrecency TestProto TestProxy TestEmojiGrid TestCdnUrls TestEmbeds TestFavoriteGifs TestVoicePayloads TestStreams TestCalls TestDecodeDemand TestCallTiles TestSendQuality)
        if(TARGET TestApng)
            list(APPEND _msvc_test_targets TestApng TestClips TestMediaInput)
        endif()
        if(TARGET TestRtp)
            list(APPEND _msvc_test_targets TestRtp TestOpusEncoder TestUdpTransport TestSinkWants TestVoiceClient)
        endif()
        if(TARGET TestRtpVideo)
            list(APPEND _msvc_test_targets TestRtpVideo TestVideoDecode TestVideoSurface TestVideoSend TestSendSideEstimator TestVideoEncode TestCameraCapture TestLoopbackCapture TestH264Sps)
        endif()
        foreach(test_target ${_msvc_test_targets})
            target_link_options(${test_target} PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL>)
            target_compile_options(${test_target} PRIVATE $<$<CONFIG:Debug>:/ZI> $<$<NOT:$<CONFIG:Debug>>:/Zi>)
            target_compile_options(${test_target} PRIVATE /utf-8)
            if(QT_VERSION_MAJOR LESS 6)
                target_compile_options(${test_target} PRIVATE
                    "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/qt5_compat.h")
            endif()
        endforeach()
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
        Qt::Svg
        Qt::Network
        Qt::Sql
        Qt::Concurrent
        CURL::libcurl
    PRIVATE
        qt${QT_VERSION_MAJOR}keychain
        OpenSSL::Crypto
)
# a static OpenSSL must not be what the system's libssl finds when Qt or another library loads
# it: the linker would otherwise export it from the program, ahead of the libcrypto libssl needs
if(UNIX AND NOT APPLE AND OPENSSL_CRYPTO_LIBRARY MATCHES "\\.a$")
    target_link_options(${PROJECT_NAME} PRIVATE "LINKER:--exclude-libs,libcrypto.a:libssl.a")
endif()
if(ENABLE_VOICE)
    target_link_libraries(${PROJECT_NAME} PRIVATE acheron_media)

    if(ENABLE_RNNOISE)
        target_link_libraries(${PROJECT_NAME} PRIVATE rnnoise)
        target_compile_definitions(${PROJECT_NAME} PRIVATE ACHERON_HAVE_RNNOISE)
    endif()
endif()
if(ENABLE_VOICE OR TARGET ffmpeg)
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniaudio")
    target_compile_definitions(${PROJECT_NAME} PRIVATE ACHERON_HAVE_MINIAUDIO)
endif()
if(TARGET ffmpeg)
    target_link_libraries(${PROJECT_NAME} PRIVATE ffmpeg)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ACHERON_HAVE_FFMPEG)
endif()
if(ACHERON_HAVE_VIDEO)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ACHERON_HAVE_VIDEO)
endif()
if(TARGET lottie)
    target_link_libraries(${PROJECT_NAME} PRIVATE lottie)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ACHERON_HAVE_RLOTTIE)
endif()
if(TARGET Qt::ZlibPrivate)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt::ZlibPrivate)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE QtZlibCompat)
endif()
target_precompile_headers(${PROJECT_NAME} PUBLIC src/pch.h)

include(CheckCXXSymbolExists)
get_target_property(_curl_incs CURL::libcurl INTERFACE_INCLUDE_DIRECTORIES)
if(_curl_incs)
    set(CMAKE_REQUIRED_INCLUDES ${_curl_incs})
endif()
set(_curl_saved_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_cxx_symbol_exists(curl_easy_impersonate "curl/curl.h" HAS_CURL_IMPERSONATE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_curl_saved_target_type}")
unset(CMAKE_REQUIRED_INCLUDES)

if(HAS_CURL_IMPERSONATE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE IS_CURL_IMPERSONATE)
elseif(ALLOW_PLAIN_CURL)
    message(WARNING "curl_easy_impersonate not found, building against plain libcurl")
else()
    message(FATAL_ERROR
        "curl_easy_impersonate not found in the curl headers at:\n"
        "  ${_curl_incs}\n"
        "Point CURL_INCLUDE_DIR at curl-impersonate's headers, or pass "
        "-DALLOW_PLAIN_CURL=ON to build against plain libcurl anyway.")
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

if(FFMPEG_RUNTIME_DLLS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "Copying ffmpeg runtime libraries to build directory"
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
    install(FILES acheron.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME io.github.ouwou.acheron.svg
    )
endif()
