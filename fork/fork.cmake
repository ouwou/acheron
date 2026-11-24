# Fork additions to the acheron target (see fork/README.md). Included from CMakeLists.txt
# just before qt_add_executable(), so it only appends to PROJECT_SOURCES and sets source
# properties; keeping it in one file keeps the upstream edit to a single line.

list(APPEND PROJECT_SOURCES
    fork/fork.qrc
    fork/AboutPage.cpp
    fork/AboutPage.hpp
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
