# Optional ffmpeg/libav discovery for video playback.
#
# Normalizes whatever is available into a single INTERFACE target named `ffmpeg`,
# the same way rnnoise.cmake does. ENABLE_FFMPEG is a statement of intent: if it is
# on and ffmpeg cannot be found, configuration fails rather than quietly producing
# a client with no video support. -DENABLE_FFMPEG=OFF is the way to opt out, and
# then the target is never defined, ACHERON_HAVE_FFMPEG stays undefined, and video
# attachments keep their existing open-in-browser behaviour.

if(NOT ENABLE_FFMPEG)
    return()
endif()

set(FFMPEG_RUNTIME_DIR "" CACHE INTERNAL "Directory holding ffmpeg shared libraries, if any")

# Deliberately no find_package(FFMPEG). Upstream ffmpeg ships pkg-config files,
# not a CMake config package, so find_package can only ever match some third
# party's Find module. Qt6 bundles one, and it reports bare library names like
# "avcodec.lib" rather than absolute paths, which the linker cannot resolve.

# 1. pkg-config, which is how Linux and homebrew present it. Skipped under MSVC:
#    pkg-config emits Unix toolchain flags like -lavcodec, so an MSYS2 or vcpkg
#    pkgconf appearing on PATH would hand link.exe something it cannot parse.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT MSVC)
    pkg_check_modules(FFMPEG_PC QUIET IMPORTED_TARGET
        libavcodec libavformat libavutil libswscale libswresample)
    if(TARGET PkgConfig::FFMPEG_PC)
        add_library(ffmpeg INTERFACE)
        target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG_PC)
        message(STATUS "Using ffmpeg from pkg-config")
        return()
    endif()
endif()

# 2. a prebuilt dev package, or a vcpkg install, on CMAKE_PREFIX_PATH. This is the
#    Windows route and the one the CI uses; such packages ship neither a CMake
#    config nor .pc files.
#
#    find_path and find_library cache a NOTFOUND result just as eagerly as a hit,
#    so a lookup that failed once would stay failed even after CMAKE_PREFIX_PATH is
#    corrected. Clearing the misses first makes "fix the path, reconfigure" work
#    without having to blow away the whole build directory.
if(NOT FFMPEG_INCLUDE_DIR)
    unset(FFMPEG_INCLUDE_DIR CACHE)
endif()
find_path(FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    PATH_SUFFIXES include
)

set(_ffmpeg_libraries "")
set(_ffmpeg_missing "")
foreach(_ffmpeg_component avcodec avformat avutil swscale swresample)
    if(NOT FFMPEG_${_ffmpeg_component}_LIBRARY)
        unset(FFMPEG_${_ffmpeg_component}_LIBRARY CACHE)
    endif()
    find_library(FFMPEG_${_ffmpeg_component}_LIBRARY
        NAMES ${_ffmpeg_component} lib${_ffmpeg_component}
        PATH_SUFFIXES lib
    )
    if(FFMPEG_${_ffmpeg_component}_LIBRARY)
        list(APPEND _ffmpeg_libraries "${FFMPEG_${_ffmpeg_component}_LIBRARY}")
    else()
        list(APPEND _ffmpeg_missing ${_ffmpeg_component})
    endif()
endforeach()

if(FFMPEG_INCLUDE_DIR AND NOT _ffmpeg_missing)
    add_library(ffmpeg INTERFACE)
    target_include_directories(ffmpeg INTERFACE "${FFMPEG_INCLUDE_DIR}")
    target_link_libraries(ffmpeg INTERFACE ${_ffmpeg_libraries})
    message(STATUS "Using ffmpeg from ${FFMPEG_INCLUDE_DIR}")

    # shared prebuilds keep their DLLs in <prefix>/bin; remember it so the main
    # CMakeLists can stage them next to the executable
    get_filename_component(_ffmpeg_prefix "${FFMPEG_INCLUDE_DIR}" DIRECTORY)
    if(WIN32 AND IS_DIRECTORY "${_ffmpeg_prefix}/bin")
        set(FFMPEG_RUNTIME_DIR "${_ffmpeg_prefix}/bin" CACHE INTERNAL
            "Directory holding ffmpeg shared libraries, if any" FORCE)
    endif()
    return()
endif()

if(NOT FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR
        "ENABLE_FFMPEG is on but ffmpeg was not found. Video playback needs ffmpeg 5.1 or newer: "
        "point CMAKE_PREFIX_PATH at a prebuilt ffmpeg prefix (an LGPL shared build is enough), or "
        "install the libavcodec/libavformat/libavutil/libswscale/libswresample development "
        "packages. Configure with -DENABLE_FFMPEG=OFF to build without video support.")
endif()

message(FATAL_ERROR
    "ffmpeg headers were found at ${FFMPEG_INCLUDE_DIR} but these libraries were not: "
    "${_ffmpeg_missing}. Video playback needs all of avcodec, avformat, avutil, swscale and "
    "swresample. Configure with -DENABLE_FFMPEG=OFF to build without video support.")
