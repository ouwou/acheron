cmake_minimum_required(VERSION 3.16)
project(acheron LANGUAGES CXX)

include(qt.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTORCC ON)

enable_testing()

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
qt_standard_project_setup(I18N_TRANSLATED_LANGUAGES en)

set(PROJECT_SOURCES
    resources.qrc
    src/main.cpp
    src/Core/Snowflake.cpp
    src/Core/ClientInstance.cpp
    src/Core/Session.cpp
    src/Core/Logging.cpp
    src/Core/MessageManager.cpp
    src/Core/ImageManager.cpp
    src/Core/AttachmentCache.cpp
    src/Core/UserManager.cpp
    src/Core/TypingTracker.cpp
    src/Core/Markdown/Parser.cpp
    src/UI/MainWindow.cpp
    src/UI/AccountsWindow.cpp
    src/UI/AccountsModel.cpp
    src/UI/TypingIndicator.cpp
    src/UI/ElidedLabel.cpp
    src/UI/ImageViewer.cpp
    src/UI/ChannelList/ChannelDelegate.cpp
    src/UI/ChannelList/ChannelTreeModel.cpp
    src/UI/Chat/ChatModel.cpp
    src/UI/Chat/ChatDelegate.cpp
    src/UI/Chat/ChatView.cpp
    src/UI/Chat/ChatLayout.cpp
    src/UI/Dialogs/AddAccountDialog.cpp
    src/UI/Dialogs/BasePopup.cpp
    src/UI/Dialogs/ConfirmPopup.cpp
    src/UI/Input/MessageInput.cpp
    src/Storage/DatabaseManager.cpp
    src/Storage/BaseRepository.cpp
    src/Storage/AccountRepository.cpp
    src/Storage/GuildRepository.cpp
    src/Storage/ChannelRepository.cpp
    src/Storage/MessageRepository.cpp
    src/Storage/UserRepository.cpp
    src/Storage/MemberRepository.cpp
    src/Discord/Client.cpp
    src/Discord/Gateway.cpp
    src/Discord/IngestThread.cpp
    src/Discord/HttpClient.cpp
    src/Discord/CurlUtils.cpp
    src/Discord/ClientIdentity.cpp
    src/Proto/ProtoReader.cpp
    src/Proto/UserSettings.cpp
    src/App.cpp
)

qt_add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

qt_add_executable(TestMarkdown tests/tst_Markdown.cpp src/Core/Markdown/Parser.cpp)
target_link_libraries(TestMarkdown PRIVATE Qt::Core Qt::Test)
target_include_directories(TestMarkdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME MarkdownTests COMMAND TestMarkdown)

qt_add_translations(${PROJECT_NAME})

if (MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL>)
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:/ZI> $<$<NOT:$<CONFIG:Debug>>:/Zi>)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)

    target_link_options(TestMarkdown PRIVATE $<$<CONFIG:Debug>:/INCREMENTAL>)
    target_compile_options(TestMarkdown PRIVATE $<$<CONFIG:Debug>:/ZI> $<$<NOT:$<CONFIG:Debug>>:/Zi>)
    target_compile_options(TestMarkdown PRIVATE /utf-8)
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
)
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
