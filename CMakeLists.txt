cmake_minimum_required(VERSION 3.16)
project(acheron LANGUAGES CXX)

include(qt.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTORCC ON)

option(BUILD_TESTS "Build test targets" ON)
option(ACHERON_ENABLE_DAVE "Enable DAVE (Discord Audio/Video E2E Encryption) support" ON)

if(BUILD_TESTS)
    enable_testing()
endif()

find_package(CURL REQUIRED)

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

# libdave (Discord Audio/Video E2E Encryption)
if(ACHERON_ENABLE_DAVE)
    message(STATUS "DAVE support enabled")
    set(TESTING OFF CACHE BOOL "" FORCE)
    set(PERSISTENT_KEYS OFF CACHE BOOL "" FORCE)
    add_subdirectory(vendor/libdave/cpp EXCLUDE_FROM_ALL)
else()
    message(STATUS "DAVE support disabled")
endif()

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
    src/Core/MurmurHash3.cpp
    src/Core/EmojiSegmenter.cpp
    src/Core/Markdown/Parser.cpp
    src/UI/MainWindow.cpp
    src/UI/TypingIndicator.cpp
    src/UI/SlowModeIndicator.cpp
    src/UI/ConnectionBanner.cpp
    src/UI/ElidedLabel.cpp
    src/UI/ImageViewer.cpp
    src/UI/Accounts/AccountsWindow.cpp
    src/UI/Accounts/AccountsModel.cpp
    src/UI/ChannelList/ChannelDelegate.cpp
    src/UI/ChannelList/ChannelTreeModel.cpp
    src/UI/ChannelList/ChannelFilterProxyModel.cpp
    src/UI/ChannelList/ChannelTreeView.cpp
    src/UI/TabBar/TabBar.cpp
    src/UI/Chat/ChatModel.cpp
    src/UI/Chat/ChatDelegate.cpp
    src/UI/Chat/ChatView.cpp
    src/UI/Chat/ChatLayout.cpp
    src/UI/Dialogs/AddAccountDialog.cpp
    src/UI/Dialogs/BasePopup.cpp
    src/UI/Dialogs/ConfirmPopup.cpp
    src/UI/MemberList/MemberListModel.cpp
    src/UI/MemberList/MemberListDelegate.cpp
    src/UI/MemberList/MemberListView.cpp
    src/UI/Input/MessageInput.cpp
    src/Storage/DatabaseManager.cpp
    src/Storage/BaseRepository.cpp
    src/Storage/AccountRepository.cpp
    src/Storage/GuildRepository.cpp
    src/Storage/ChannelRepository.cpp
    src/Storage/MessageRepository.cpp
    src/Storage/UserRepository.cpp
    src/Storage/MemberRepository.cpp
    src/Storage/RoleRepository.cpp
    src/Discord/Client.cpp
    src/Discord/Gateway.cpp
    src/Discord/IngestThread.cpp
    src/Discord/HttpClient.cpp
    src/Discord/CurlUtils.cpp
    src/Discord/ClientIdentity.cpp
    src/Proto/ProtoReader.cpp
    src/Proto/UserSettings.cpp
    src/Core/TokenStore.cpp
    src/App.cpp
)

qt_add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE vendor/emoji-segmenter)

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
)
if(TARGET Qt::ZlibPrivate)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt::ZlibPrivate)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE QtZlibCompat)
endif()
if(ACHERON_ENABLE_DAVE)
    target_link_libraries(${PROJECT_NAME} PRIVATE libdave)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ACHERON_DAVE_ENABLED)
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
