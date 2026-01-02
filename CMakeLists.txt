cmake_minimum_required(VERSION 3.16)
project(acheron LANGUAGES CXX)

include(qt.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTORCC ON)

enable_testing()

find_package(CURL REQUIRED)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Network Sql Test)
find_package(Qt${QT_VERSION_MAJOR}
    COMPONENTS
        Core
        Gui
        Widgets
        Network
        Sql
        Concurrent
        Test
)
qt_standard_project_setup()

set(PROJECT_SOURCES
    resources.qrc
    src/main.cpp
    src/Core/Snowflake.cpp
    src/Core/ClientInstance.cpp
    src/Core/Session.cpp
    src/Core/Logging.cpp
    src/Core/MessageManager.cpp
    src/Core/ImageManager.cpp
    src/Core/Markdown/Parser.cpp
    src/UI/MainWindow.cpp
    src/UI/AccountsWindow.cpp
    src/UI/AccountsModel.cpp
    src/UI/ChannelList/ChannelDelegate.cpp
    src/UI/ChannelList/ChannelTreeModel.cpp
    src/UI/Chat/ChatModel.cpp
    src/UI/Chat/ChatDelegate.cpp
    src/UI/Chat/ChatView.cpp
    src/UI/Chat/ChatLayout.cpp
    src/UI/Dialogs/AddAccountDialog.cpp
    src/UI/Input/MessageInput.cpp
    src/Storage/DatabaseManager.cpp
    src/Storage/BaseRepository.cpp
    src/Storage/AccountRepository.cpp
    src/Storage/GuildRepository.cpp
    src/Storage/ChannelRepository.cpp
    src/Storage/MessageRepository.cpp
    src/Discord/Client.cpp
    src/Discord/Gateway.cpp
    src/Discord/IngestThread.cpp
    src/Discord/HttpClient.cpp
    src/App.cpp
)

qt_add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

qt_add_executable(TestMarkdown tests/tst_Markdown.cpp src/Core/Markdown/Parser.cpp)
target_link_libraries(TestMarkdown PRIVATE Qt::Core Qt::Test)
target_include_directories(TestMarkdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME MarkdownTests COMMAND TestMarkdown)

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
