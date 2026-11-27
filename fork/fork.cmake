# Fork additions to the acheron target (see fork/README.md). Included from CMakeLists.txt
# just before qt_add_executable(), so it only appends to PROJECT_SOURCES and sets source
# properties; keeping it in one file keeps the upstream edit to a single line.

list(APPEND PROJECT_SOURCES
    fork/fork.qrc
    fork/AboutPage.cpp
    fork/AboutPage.hpp
    fork/UpdateChecker.cpp
    fork/UpdateChecker.hpp
)
if(WIN32)
    list(APPEND PROJECT_SOURCES fork/acheron.rc)
endif()

# Build information for Settings > About: from GitHub Actions when built there,
# otherwise from the local git checkout.
set(ACHERON_FORK_COMMIT "$ENV{GITHUB_SHA}")
if(NOT ACHERON_FORK_COMMIT)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE ACHERON_FORK_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
endif()
string(TIMESTAMP ACHERON_FORK_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)

set_source_files_properties(fork/AboutPage.cpp PROPERTIES
    # its defines differ from the rest of the target, so keep it out of the shared PCH
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_DEFINITIONS "ACHERON_FORK_COMMIT=\"${ACHERON_FORK_COMMIT}\";ACHERON_FORK_REPO=\"$ENV{GITHUB_REPOSITORY}\";ACHERON_FORK_RUN_NUMBER=\"$ENV{GITHUB_RUN_NUMBER}\";ACHERON_FORK_RUN_ID=\"$ENV{GITHUB_RUN_ID}\";ACHERON_FORK_WORKFLOW=\"$ENV{GITHUB_WORKFLOW}\";ACHERON_FORK_BUILD_TIME=\"${ACHERON_FORK_BUILD_TIME}\";ACHERON_FORK_BUILD_CONFIG=\"$<CONFIG>\""
)

# acheron-updater (fork/updater): finds and installs nightly builds, run by Acheron (fork/UpdateChecker).
# Built next to acheron.exe, so every build ships it. Only Qt Core and Qt Network, which Acheron already
# ships; windowless (no console window when Acheron runs it), but still a plain main().
add_executable(acheron-updater WIN32 fork/updater/main.cpp)
target_link_libraries(acheron-updater PRIVATE Qt::Core Qt::Network)
set_target_properties(acheron-updater PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
if(MSVC)
    target_compile_definitions(acheron-updater PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_options(acheron-updater PRIVATE /ENTRY:mainCRTStartup)
endif()
